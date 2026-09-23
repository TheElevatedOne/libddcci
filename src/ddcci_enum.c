#define _GNU_SOURCE

#include "ddcci_priv.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DDCCI_MAX_DISPLAYS 64
#define DDCCI_MAX_BUS_NOTES 128

static void trim_nl(char *s)
{
    size_t n;

    if (!s)
        return;
    n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = '\0';
}

static int read_text(const char *path, char *buf, size_t n)
{
    FILE *f;
    size_t r;

    if (!path || !buf || n == 0)
        return -1;
    f = fopen(path, "r");
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
    FILE *f;
    size_t r;

    f = fopen(path, "rb");
    if (!f)
        return -1;
    r = fread(buf, 1, n, f);
    fclose(f);
    return (ssize_t)r;
}

int ddcci_i2c_number_in(const char *s)
{
    const char *p;
    int bus = -1;

    if (!s)
        return -1;
    /* The last i2c-<number> is the device. A parent adapter earlier in the
     * path (…/i2c-3/i2c-8) is not the bus to open. "i2c-dev" has no number.
     * "DP-3" has none either: that digit is a connector index. */
    for (p = strstr(s, "i2c-"); p; p = strstr(p + 4, "i2c-")) {
        int n;

        if (sscanf(p + 4, "%d", &n) != 1 || n < 0)
            continue;
        bus = n;
    }
    return bus;
}

int ddcci_bus_from_devnode(const char *path)
{
    const char *base;

    if (!path)
        return -1;
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return ddcci_i2c_number_in(base);
}

static void fill_adapter_name(int bus, char *dst, size_t n)
{
    char path[160];

    if (!dst || n == 0)
        return;
    dst[0] = '\0';
    if (bus < 0)
        return;

    snprintf(path, sizeof(path), "/sys/class/i2c-adapter/i2c-%d/name", bus);
    if (read_text(path, dst, n) > 0)
        return;
    snprintf(path, sizeof(path), "/sys/class/i2c-dev/i2c-%d/name", bus);
    if (read_text(path, dst, n) > 0)
        return;
    snprintf(path, sizeof(path), "/sys/bus/i2c/devices/i2c-%d/name", bus);
    (void)read_text(path, dst, n);
}

static void connector_short(const char *sysname, char *dst, size_t n)
{
    const char *dash;

    if (!dst || n == 0)
        return;
    dash = sysname ? strchr(sysname, '-') : NULL;
    snprintf(dst, n, "%s", (dash && dash[1]) ? dash + 1 : (sysname ? sysname : ""));
}

static int name_prefers_aux(const char *s)
{
    if (!s || !s[0])
        return 0;
    /* DRM connector index, not an I2C adapter number. */
    if (strncmp(s, "DP-", 3) == 0 || strncmp(s, "eDP-", 4) == 0)
        return 1;
    if (strncmp(s, "USB-", 4) == 0)
        return 1;
    return 0;
}

static int connector_prefers_aux(const char *sysname)
{
    char short_name[64];

    if (name_prefers_aux(sysname))
        return 1;
    connector_short(sysname, short_name, sizeof(short_name));
    return name_prefers_aux(short_name);
}

void ddcci_order_connector_buses(const char *connector,
                                 int ddc_bus, const char *ddc_name,
                                 int child_bus, const char *child_name,
                                 int *primary, int *secondary)
{
    int ddc_aux, child_aux;

    if (!primary || !secondary)
        return;
    *primary = -1;
    *secondary = -1;
    if (ddc_bus < 0)
        ddc_bus = -1;
    if (child_bus < 0)
        child_bus = -1;
    if (ddc_bus >= 0 && ddc_bus == child_bus)
        child_bus = -1;

    if (ddc_bus < 0 && child_bus < 0)
        return;
    if (ddc_bus < 0) {
        *primary = child_bus;
        return;
    }
    if (child_bus < 0) {
        *primary = ddc_bus;
        return;
    }

    ddc_aux = ddc_name && ddcci_ascii_icontains(ddc_name, "aux");
    child_aux = child_name && ddcci_ascii_icontains(child_name, "aux");

    if (connector_prefers_aux(connector)) {
        /* Two different adapters. The one whose name contains "aux" is the
         * DisplayPort AUX channel. With no names, the i2c-* child of the
         * connector is that channel on amdgpu; the ddc symlink is the hw bus. */
        if (ddc_aux && !child_aux) {
            *primary = ddc_bus;
            *secondary = child_bus;
            return;
        }
        *primary = child_bus;
        *secondary = ddc_bus;
        return;
    }

    *primary = ddc_bus;
    *secondary = child_bus;
}

/* ddc_bus: "ddc" symlink target. child_bus: i2c-* entry in the connector
 * directory (the AUX adapter when it is not the symlink target). */
static void buses_from_connector_sys(const char *base, int *ddc_bus, int *child_bus)
{
    char path[512];
    char link[512];
    DIR *dir;
    struct dirent *de;
    ssize_t n;

    *ddc_bus = -1;
    *child_bus = -1;

    snprintf(path, sizeof(path), "%s/ddc", base);
    n = readlink(path, link, sizeof(link) - 1);
    if (n > 0 && (size_t)n < sizeof(link) - 1) {
        link[n] = '\0';
        *ddc_bus = ddcci_i2c_number_in(link);
    }
    if (*ddc_bus < 0) {
        dir = opendir(path);
        if (dir) {
            while ((de = readdir(dir)) != NULL) {
                if (strncmp(de->d_name, "i2c-", 4) != 0)
                    continue;
                *ddc_bus = ddcci_i2c_number_in(de->d_name);
                if (*ddc_bus >= 0)
                    break;
            }
            closedir(dir);
        }
    }

    dir = opendir(base);
    if (!dir)
        return;
    while ((de = readdir(dir)) != NULL) {
        int bus;

        if (strncmp(de->d_name, "i2c-", 4) != 0)
            continue;
        bus = ddcci_i2c_number_in(de->d_name);
        if (bus < 0)
            continue;
        if (*child_bus < 0 || (*child_bus == *ddc_bus && bus != *ddc_bus))
            *child_bus = bus;
    }
    closedir(dir);
}

static void connector_bus_candidates(const char *base, const char *sysname,
                                     int *primary, int *secondary)
{
    int ddc_bus = -1, child_bus = -1;
    char ddc_name[128];
    char child_name[128];

    buses_from_connector_sys(base, &ddc_bus, &child_bus);
    fill_adapter_name(ddc_bus, ddc_name, sizeof(ddc_name));
    fill_adapter_name(child_bus, child_name, sizeof(child_name));
    ddcci_order_connector_buses(sysname, ddc_bus, ddc_name, child_bus, child_name,
                                primary, secondary);
}

static int edid_on_bus_matches(int bus, const uint8_t *edid, size_t n)
{
    ddcci_display *d = NULL;
    ddcci_edid got;

    if (bus < 0 || !edid || n < DDCCI_EDID_LEN_MIN)
        return 0;
    if (ddcci_open(bus, &d) != DDCCI_OK)
        return 0;
    if (ddcci_read_edid_i2c(d, &got) != DDCCI_OK || got.len < DDCCI_EDID_LEN_MIN) {
        ddcci_close(d);
        return 0;
    }
    ddcci_close(d);
    return memcmp(got.raw, edid, DDCCI_EDID_LEN_MIN) == 0;
}

static int bus_answers_ddc(int bus)
{
    ddcci_display *d = NULL;
    int ok;

    if (bus < 0 || ddcci_open(bus, &d) != DDCCI_OK)
        return 0;
    ok = ddcci_has_ddc(d) ? 1 : 0;
    ddcci_close(d);
    return ok;
}

/* primary was ordered first. When both exist, keep the one whose EDID matches
 * the connector, else the one where slave 0x37 answers. Neither match: primary,
 * so a panel with EDID but no DDC/CI (eDP) stays on the AUX adapter. */
static int choose_connector_bus(int primary, int secondary,
                                const uint8_t *sysfs_edid, size_t edid_n)
{
    if (primary < 0)
        return secondary;
    if (secondary < 0 || secondary == primary)
        return primary;

    if (sysfs_edid && edid_n >= DDCCI_EDID_LEN_MIN) {
        if (edid_on_bus_matches(primary, sysfs_edid, edid_n))
            return primary;
        if (edid_on_bus_matches(secondary, sysfs_edid, edid_n))
            return secondary;
    }
    if (bus_answers_ddc(primary))
        return primary;
    if (bus_answers_ddc(secondary))
        return secondary;
    return primary;
}

typedef struct {
    int bus;
    bool connected;
} bus_note;

static int note_index(const bus_note *notes, int n, int bus)
{
    int i;

    for (i = 0; i < n; i++) {
        if (notes[i].bus == bus)
            return i;
    }
    return -1;
}

static void note_bus(bus_note *notes, int *n, int bus, bool connected);

static void note_i2c_children(const char *dirpath, bus_note *notes, int *nnotes,
                               bool connected)
{
    DIR *dir;
    struct dirent *de;

    dir = opendir(dirpath);
    if (!dir)
        return;
    while ((de = readdir(dir)) != NULL) {
        int bus;

        if (strncmp(de->d_name, "i2c-", 4) != 0)
            continue;
        bus = ddcci_i2c_number_in(de->d_name);
        /* "i2c-dev" is a directory, not an adapter number. */
        if (bus >= 0)
            note_bus(notes, nnotes, bus, connected);
    }
    closedir(dir);
}

static void note_bus(bus_note *notes, int *n, int bus, bool connected)
{
    int i;

    if (bus < 0 || *n >= DDCCI_MAX_BUS_NOTES)
        return;
    i = note_index(notes, *n, bus);
    if (i < 0) {
        notes[*n].bus = bus;
        notes[*n].connected = connected;
        (*n)++;
        return;
    }
    if (connected)
        notes[i].connected = true;
}

typedef struct {
    ddcci_info *items;
    size_t n;
    size_t cap;
} info_vec;

static ddcci_status_t vec_push(info_vec *v, const ddcci_info *item)
{
    ddcci_info *grow;

    if (v->n >= DDCCI_MAX_DISPLAYS)
        return DDCCI_OK;
    if (v->n == v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 8;

        if (nc > DDCCI_MAX_DISPLAYS)
            nc = DDCCI_MAX_DISPLAYS;
        grow = realloc(v->items, nc * sizeof(*grow));
        if (!grow)
            return DDCCI_ERR_NOMEM;
        v->items = grow;
        v->cap = nc;
    }
    v->items[v->n++] = *item;
    return DDCCI_OK;
}

static ddcci_status_t probe_bus(ddcci_info *info)
{
    ddcci_display *d = NULL;
    ddcci_info got;
    ddcci_edid edid;
    char connector[64];
    char adapter[128];
    bool edid_ok;
    int bus;
    ddcci_status_t st;

    if (info->bus < 0 || info->path[0] == '\0') {
        info->accessible = false;
        return DDCCI_OK;
    }

    bus = info->bus;
    edid_ok = info->edid_ok;
    edid = info->edid;
    snprintf(connector, sizeof(connector), "%s", info->connector);
    snprintf(adapter, sizeof(adapter), "%s", info->adapter_name);

    st = ddcci_open(bus, &d);
    if (st != DDCCI_OK) {
        info->accessible = false;
        info->bus = bus;
        snprintf(info->path, sizeof(info->path), "/dev/i2c-%d", bus);
        if (connector[0])
            snprintf(info->connector, sizeof(info->connector), "%s", connector);
        if (adapter[0] && info->adapter_name[0] == '\0')
            snprintf(info->adapter_name, sizeof(info->adapter_name), "%s", adapter);
        if (edid_ok) {
            info->edid = edid;
            info->edid_ok = true;
        }
        return DDCCI_OK;
    }

    if (connector[0])
        snprintf(d->connector, sizeof(d->connector), "%s", connector);
    if (adapter[0] && d->adapter_name[0] == '\0')
        snprintf(d->adapter_name, sizeof(d->adapter_name), "%s", adapter);
    if (edid_ok) {
        d->info.edid = edid;
        d->info.edid_ok = true;
        d->edid_loaded = true;
    }

    st = ddcci_query(d, &got);
    ddcci_close(d);
    if (st == DDCCI_ERR_NOMEM)
        return st;
    if (st != DDCCI_OK) {
        /* Keep the sysfs identity. One flaky panel must not fail the scan. */
        info->accessible = true;
        info->bus = bus;
        snprintf(info->path, sizeof(info->path), "/dev/i2c-%d", bus);
        return DDCCI_OK;
    }
    *info = got;
    return DDCCI_OK;
}

static int match_rank(const char *sysname, const char *want, int connected)
{
    char short_name[64];
    int score = 0;

    if (!sysname || !want)
        return 0;
    if (strcmp(sysname, want) == 0)
        score = 3;
    else {
        connector_short(sysname, short_name, sizeof(short_name));
        if (strcmp(short_name, want) == 0)
            score = 2;
    }
    if (score == 0)
        return 0;
    return (score << 1) | (connected ? 1 : 0);
}

int ddcci_bus_from_connector(const char *drm_connector, char *short_name, size_t short_n)
{
    DIR *dir;
    struct dirent *de;
    int best_rank = 0;
    int best_primary = -1;
    int best_secondary = -1;
    int best_connected = 0;
    char best_short[64];
    char best_base[512];
    uint8_t edid[DDCCI_EDID_LEN_MAX];
    ssize_t edid_n;

    best_short[0] = '\0';
    best_base[0] = '\0';
    if (short_name && short_n)
        short_name[0] = '\0';
    if (!drm_connector || !drm_connector[0])
        return -1;

    dir = opendir("/sys/class/drm");
    if (!dir)
        return -1;

    while ((de = readdir(dir)) != NULL) {
        char base[512];
        char status[32];
        char path[540];
        int connected = 0;
        int primary = -1, secondary = -1;
        int rank;

        if (de->d_name[0] == '.' || strchr(de->d_name, '-') == NULL)
            continue;
        rank = match_rank(de->d_name, drm_connector, 0);
        if (rank == 0)
            continue;

        snprintf(base, sizeof(base), "/sys/class/drm/%s", de->d_name);
        snprintf(path, sizeof(path), "%s/status", base);
        if (read_text(path, status, sizeof(status)) > 0 &&
            strcmp(status, "connected") == 0)
            connected = 1;
        rank = match_rank(de->d_name, drm_connector, connected);
        connector_bus_candidates(base, de->d_name, &primary, &secondary);
        if (primary < 0)
            continue;
        if (rank > best_rank) {
            best_rank = rank;
            best_primary = primary;
            best_secondary = secondary;
            best_connected = connected;
            snprintf(best_base, sizeof(best_base), "%s", base);
            connector_short(de->d_name, best_short, sizeof(best_short));
        }
    }
    closedir(dir);

    if (best_primary < 0)
        return -1;
    if (short_name && short_n)
        snprintf(short_name, short_n, "%s", best_short);
    /* A disconnected port is not probed. Two adapters on a live port are:
     * the ddc symlink alone is the wrong node for native DisplayPort. */
    if (!best_connected || best_secondary < 0)
        return best_primary;

    {
        char path[540];

        snprintf(path, sizeof(path), "%s/edid", best_base);
        edid_n = read_bin(path, edid, sizeof(edid));
    }
    if (edid_n < (ssize_t)DDCCI_EDID_LEN_MIN)
        edid_n = 0;
    return choose_connector_bus(best_primary, best_secondary,
                                edid_n ? edid : NULL, (size_t)edid_n);
}

static void find_connector_for_bus(int bus, char *dst, size_t n)
{
    DIR *dir;
    struct dirent *de;
    char fallback[64];

    if (!dst || n == 0)
        return;
    dst[0] = '\0';
    fallback[0] = '\0';
    dir = opendir("/sys/class/drm");
    if (!dir)
        return;

    while ((de = readdir(dir)) != NULL) {
        char base[512];
        char path[540];
        char status[32];

        if (de->d_name[0] == '.' || strchr(de->d_name, '-') == NULL)
            continue;
        snprintf(base, sizeof(base), "/sys/class/drm/%s", de->d_name);
        {
            int ddc_bus = -1, child_bus = -1;

            buses_from_connector_sys(base, &ddc_bus, &child_bus);
            if (ddc_bus != bus && child_bus != bus)
                continue;
        }
        connector_short(de->d_name, fallback, sizeof(fallback));
        snprintf(path, sizeof(path), "%s/status", base);
        if (read_text(path, status, sizeof(status)) > 0 &&
            strcmp(status, "connected") == 0) {
            snprintf(dst, n, "%s", fallback);
            closedir(dir);
            return;
        }
    }
    closedir(dir);
    if (fallback[0])
        snprintf(dst, n, "%s", fallback);
}

void ddcci_note_identity(ddcci_display *d)
{
    if (!d)
        return;
    if (d->bus < 0)
        d->bus = ddcci_bus_from_devnode(d->path);
    if (d->adapter_name[0] == '\0')
        fill_adapter_name(d->bus, d->adapter_name, sizeof(d->adapter_name));
    if (d->connector[0] == '\0' && d->bus >= 0)
        find_connector_for_bus(d->bus, d->connector, sizeof(d->connector));
}

static ddcci_status_t scan_drm(info_vec *v, bus_note *notes, int *nnotes)
{
    DIR *dir;
    struct dirent *de;
    ddcci_status_t st = DDCCI_OK;

    dir = opendir("/sys/class/drm");
    if (!dir)
        return DDCCI_OK;

    while ((de = readdir(dir)) != NULL) {
        char base[512];
        char path[540];
        char status[32];
        ddcci_info info;
        uint8_t edid[DDCCI_EDID_LEN_MAX];
        ssize_t n = -1;
        int connected;

        if (de->d_name[0] == '.' || strchr(de->d_name, '-') == NULL)
            continue;

        snprintf(base, sizeof(base), "/sys/class/drm/%s", de->d_name);
        snprintf(path, sizeof(path), "%s/status", base);
        if (read_text(path, status, sizeof(status)) < 0)
            continue;
        connected = strcmp(status, "connected") == 0;

        ddcci_info_reset(&info);
        {
            int primary = -1, secondary = -1;

            /* primary is the first adapter to try, not "whatever the ddc
             * symlink says". On AMD DisplayPort that symlink is the non-AUX
             * hw bus (often numbered like the connector) and does not carry
             * DDC/CI. The i2c-* child is the AUX adapter that does.
             * Both are noted so the fallback scan does not open the loser. */
            connector_bus_candidates(base, de->d_name, &primary, &secondary);
            note_bus(notes, nnotes, primary, connected);
            note_bus(notes, nnotes, secondary, connected);
            note_i2c_children(base, notes, nnotes, connected);
            if (!connected)
                continue;

            snprintf(path, sizeof(path), "%s/edid", base);
            n = read_bin(path, edid, sizeof(edid));
            info.bus = choose_connector_bus(primary, secondary,
                                            n >= (ssize_t)DDCCI_EDID_LEN_MIN ? edid : NULL,
                                            n > 0 ? (size_t)n : 0);
        }

        connector_short(de->d_name, info.connector, sizeof(info.connector));
        if (info.bus >= 0)
            snprintf(info.path, sizeof(info.path), "/dev/i2c-%d", info.bus);
        fill_adapter_name(info.bus, info.adapter_name, sizeof(info.adapter_name));

        if (n >= (ssize_t)DDCCI_EDID_LEN_MIN &&
            ddcci_parse_edid(edid, (size_t)n, &info.edid) == DDCCI_OK)
            info.edid_ok = true;

        if (!info.edid_ok && info.bus < 0)
            continue;

        st = probe_bus(&info);
        if (st != DDCCI_OK)
            break;
        if (!info.edid_ok && !info.ddc_supported && !info.accessible)
            continue;
        /* Connected panel with EDID but no DDC (typical laptop eDP) is kept. */
        if (!info.edid_ok && !info.ddc_supported)
            continue;
        st = vec_push(v, &info);
        if (st != DDCCI_OK)
            break;
    }
    closedir(dir);
    return st;
}

static int already_listed(const info_vec *v, int bus)
{
    size_t i;

    for (i = 0; i < v->n; i++) {
        if (v->items[i].bus == bus)
            return 1;
    }
    return 0;
}

static ddcci_status_t scan_i2c_fallback(info_vec *v, const bus_note *notes, int nnotes)
{
    DIR *dir;
    struct dirent *de;
    ddcci_status_t st = DDCCI_OK;

    dir = opendir("/sys/class/i2c-dev");
    if (!dir)
        return DDCCI_OK;

    while ((de = readdir(dir)) != NULL) {
        int bus;
        ddcci_info info;

        if (strncmp(de->d_name, "i2c-", 4) != 0)
            continue;
        bus = ddcci_i2c_number_in(de->d_name);
        if (bus < 0)
            continue;
        /* DRM already owns this adapter. Disconnected ports are not probed;
         * connected ones were handled in scan_drm. */
        if (note_index(notes, nnotes, bus) >= 0)
            continue;
        if (already_listed(v, bus))
            continue;

        ddcci_info_reset(&info);
        info.bus = bus;
        snprintf(info.path, sizeof(info.path), "/dev/i2c-%d", bus);
        fill_adapter_name(bus, info.adapter_name, sizeof(info.adapter_name));
        /* SMBus is motherboard sensors. "bit bus OEM" is an AMD display
         * engine bus with no connector behind it. */
        if (ddcci_ascii_icontains(info.adapter_name, "smbus") ||
            ddcci_ascii_icontains(info.adapter_name, "bit bus oem"))
            continue;

        st = probe_bus(&info);
        if (st != DDCCI_OK)
            break;
        if (!info.edid_ok && !info.ddc_supported)
            continue;
        st = vec_push(v, &info);
        if (st != DDCCI_OK)
            break;
    }
    closedir(dir);
    return st;
}

ddcci_status_t ddcci_enum_displays(ddcci_info **list, size_t *count)
{
    info_vec v;
    bus_note notes[DDCCI_MAX_BUS_NOTES];
    int nnotes = 0;
    ddcci_status_t st;

    if (!list || !count)
        return DDCCI_ERR_INVALID_ARG;

    memset(&v, 0, sizeof(v));
    memset(notes, 0, sizeof(notes));
    st = scan_drm(&v, notes, &nnotes);
    if (st == DDCCI_OK)
        st = scan_i2c_fallback(&v, notes, nnotes);
    if (st != DDCCI_OK) {
        free(v.items);
        return st;
    }
    *list = v.items;
    *count = v.n;
    return DDCCI_OK;
}
