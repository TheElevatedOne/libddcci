#define _GNU_SOURCE

#include "ddcci_priv.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void trim_nl(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = '\0';
}

static int read_text(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    size_t r;

    if (!f)
        return -1;
    r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = '\0';
    trim_nl(buf);
    return r > 0 ? (int)r : 0;
}

static ssize_t read_bin(const char *path, uint8_t *buf, size_t n)
{
    FILE *f = fopen(path, "rb");
    size_t r;

    if (!f)
        return -1;
    r = fread(buf, 1, n, f);
    fclose(f);
    return (ssize_t)r;
}

static bool name_is_smbus(const char *name)
{
    return name && strcasestr(name, "smbus") != NULL;
}

static int parse_i2c_n(const char *s)
{
    const char *p = s;
    int bus = -1;

    if (!s)
        return -1;
    p = strstr(s, "i2c-");
    if (!p)
        p = s;
    else
        p += 4;
    if (sscanf(p, "%d", &bus) != 1)
        return -1;
    return bus;
}

static void fill_adapter_name(int bus, char *dst, size_t n)
{
    char path[128];

    dst[0] = '\0';
    if (bus < 0)
        return;
    snprintf(path, sizeof(path), "/sys/class/i2c-adapter/i2c-%d/name", bus);
    if (read_text(path, dst, n) >= 0)
        return;
    snprintf(path, sizeof(path), "/sys/class/i2c-dev/i2c-%d/name", bus);
    (void)read_text(path, dst, n);
}

static void drm_connector_short(const char *sysname, char *dst, size_t n)
{
    const char *dash = strchr(sysname, '-');
    const char *src = dash ? dash + 1 : sysname;

    snprintf(dst, n, "%.*s", (int)(n - 1), src);
}

typedef struct {
    ddcci_info *items;
    size_t n;
    size_t cap;
} info_vec;

static ddcci_status_t vec_push(info_vec *v, const ddcci_info *item)
{
    ddcci_info *grow;

    if (v->n == v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 8;
        grow = realloc(v->items, nc * sizeof(*grow));
        if (!grow)
            return DDCCI_ERR_NOMEM;
        v->items = grow;
        v->cap = nc;
    }
    v->items[v->n++] = *item;
    return DDCCI_OK;
}

static bool already_have_bus(const info_vec *v, int bus)
{
    size_t i;

    if (bus < 0)
        return false;
    for (i = 0; i < v->n; i++) {
        if (v->items[i].bus == bus)
            return true;
    }
    return false;
}

static void probe_bus_into(ddcci_info *info)
{
    ddcci_display d;
    ddcci_status_t st;

    memset(&d, 0, sizeof(d));
    d.fd = -1;
    d.bus = info->bus;
    snprintf(d.path, sizeof(d.path), "%s", info->path);

    if (info->bus < 0 || info->path[0] == '\0') {
        info->accessible = false;
        return;
    }

    d.fd = ddcci_i2c_open(info->path);
    if (d.fd < 0) {
        info->accessible = false;
        return;
    }
    info->accessible = true;

    if (!info->edid_ok) {
        if (ddcci_read_edid_i2c(d.fd, &info->edid) == DDCCI_OK)
            info->edid_ok = true;
    }

    st = ddcci_probe_ddc(&d);
    if (st == DDCCI_OK && d.ddc_ok) {
        info->ddc_supported = true;
        (void)ddcci_load_caps(&d);
        if (d.caps)
            (void)ddcci_parse_capabilities(d.caps, info);

        /* Confirm brightness / contrast with GetVCP — some panels omit them
         * from the capabilities string (and a few list them but NAK GetVCP). */
        {
            static const uint8_t bri[] = {
                DDCCI_VCP_BRIGHTNESS,
                DDCCI_VCP_BACKLIGHT,
                DDCCI_VCP_BACKLIGHT_WHITE
            };
            size_t i;
            ddcci_feature f;
            uint8_t w[8], r[DDCCI_FRAME_MAX];
            size_t wn, got;

            for (i = 0; i < sizeof(bri); i++) {
                wn = ddcci_pack_getvcp(bri[i], w);
                got = 0;
                if (ddcci_transact(&d, w, wn, r, sizeof(r), &got, DDCCI_WAIT_GET_MS) != DDCCI_OK)
                    continue;
                if (ddcci_unpack_getvcp(r, got, bri[i], &f) != DDCCI_OK)
                    continue;
                ddcci_add_vcp(info, bri[i]);
                if (!info->brightness.from_probe) {
                    bool from_caps = info->brightness.from_caps;
                    uint8_t prev = info->brightness.opcode;
                    info->brightness = f;
                    info->brightness.from_caps = from_caps || (prev == bri[i]);
                    info->brightness.present = true;
                    info->brightness.from_probe = true;
                    info->brightness.opcode = bri[i];
                    /* Prefer 0x10: stop once we have it. */
                    if (bri[i] == DDCCI_VCP_BRIGHTNESS)
                        break;
                }
            }

            wn = ddcci_pack_getvcp(DDCCI_VCP_CONTRAST, w);
            got = 0;
            if (ddcci_transact(&d, w, wn, r, sizeof(r), &got, DDCCI_WAIT_GET_MS) == DDCCI_OK
                && ddcci_unpack_getvcp(r, got, DDCCI_VCP_CONTRAST, &f) == DDCCI_OK) {
                bool from_caps = info->contrast.from_caps;
                info->contrast = f;
                info->contrast.from_caps = from_caps;
                info->contrast.present = true;
                info->contrast.from_probe = true;
                info->contrast.opcode = DDCCI_VCP_CONTRAST;
                ddcci_add_vcp(info, DDCCI_VCP_CONTRAST);
            }
        }

        if (info->mccs_major == 0) {
            ddcci_feature ver;
            uint8_t w[8], r[DDCCI_FRAME_MAX];
            size_t wn = ddcci_pack_getvcp(DDCCI_VCP_VERSION, w);
            size_t got = 0;
            if (ddcci_transact(&d, w, wn, r, sizeof(r), &got, DDCCI_WAIT_GET_MS) == DDCCI_OK
                && ddcci_unpack_getvcp(r, got, DDCCI_VCP_VERSION, &ver) == DDCCI_OK) {
                info->mccs_major = (uint8_t)(ver.current >> 8);
                info->mccs_minor = (uint8_t)(ver.current & 0xFFu);
            }
        }
        free(d.caps);
    } else {
        info->ddc_supported = false;
    }

    close(d.fd);
}

static void scan_drm(info_vec *v)
{
    DIR *dir;
    struct dirent *de;

    dir = opendir("/sys/class/drm");
    if (!dir)
        return;

    while ((de = readdir(dir)) != NULL) {
        char base[400], path[480], status[32];
        ddcci_info info;
        uint8_t edid[DDCCI_EDID_LEN_MAX];
        ssize_t n;
        char link[256];
        ssize_t ln;

        if (de->d_name[0] == '.')
            continue;
        /* Connectors are "cardN-NAME"; skip the card device itself. */
        if (strchr(de->d_name, '-') == NULL)
            continue;

        snprintf(base, sizeof(base), "/sys/class/drm/%s", de->d_name);
        snprintf(path, sizeof(path), "%s/status", base);
        if (read_text(path, status, sizeof(status)) < 0)
            continue;
        if (strcmp(status, "connected") != 0)
            continue;

        ddcci_info_reset(&info);
        drm_connector_short(de->d_name, info.connector, sizeof(info.connector));

        snprintf(path, sizeof(path), "%s/edid", base);
        n = read_bin(path, edid, sizeof(edid));
        if (n >= DDCCI_EDID_LEN_MIN
            && ddcci_parse_edid(edid, (size_t)n, &info.edid) == DDCCI_OK)
            info.edid_ok = true;

        snprintf(path, sizeof(path), "%s/ddc", base);
        ln = readlink(path, link, sizeof(link) - 1);
        if (ln > 0) {
            link[ln] = '\0';
            info.bus = parse_i2c_n(link);
            if (info.bus >= 0)
                snprintf(info.path, sizeof(info.path), "/dev/i2c-%d", info.bus);
        }
        fill_adapter_name(info.bus, info.adapter_name, sizeof(info.adapter_name));

        if (!info.edid_ok && info.bus < 0)
            continue;

        probe_bus_into(&info);

        if (!info.edid_ok && !info.ddc_supported)
            continue;

        (void)vec_push(v, &info);
    }
    closedir(dir);
}

static void scan_i2c_dev(info_vec *v)
{
    DIR *dir;
    struct dirent *de;

    dir = opendir("/sys/class/i2c-dev");
    if (!dir)
        return;

    while ((de = readdir(dir)) != NULL) {
        int bus;
        ddcci_info info;

        if (strncmp(de->d_name, "i2c-", 4) != 0)
            continue;
        bus = parse_i2c_n(de->d_name);
        if (bus < 0 || already_have_bus(v, bus))
            continue;

        ddcci_info_reset(&info);
        info.bus = bus;
        snprintf(info.path, sizeof(info.path), "/dev/i2c-%d", bus);
        fill_adapter_name(bus, info.adapter_name, sizeof(info.adapter_name));

        if (name_is_smbus(info.adapter_name))
            continue;

        probe_bus_into(&info);
        if (!info.edid_ok && !info.ddc_supported)
            continue;
        (void)vec_push(v, &info);
    }
    closedir(dir);
}

ddcci_status_t ddcci_enum_displays(ddcci_info **list, size_t *count)
{
    info_vec v;

    if (!list || !count)
        return DDCCI_ERR_INVALID_ARG;

    memset(&v, 0, sizeof(v));
    scan_drm(&v);
    scan_i2c_dev(&v);

    *list = v.items;
    *count = v.n;
    return DDCCI_OK;
}
