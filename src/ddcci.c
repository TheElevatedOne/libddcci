#define _GNU_SOURCE

#include "ddcci_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ddcci_display *display_new(void)
{
    ddcci_display *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->fd = -1;
    d->bus = -1;
    ddcci_info_reset(&d->info);
    return d;
}

static ddcci_status_t display_open_path(ddcci_display *d, const char *path)
{
    int bus;

    snprintf(d->path, sizeof(d->path), "%s", path);
    d->fd = ddcci_i2c_open(path);
    if (d->fd < 0)
        return DDCCI_ERR_IO;

    bus = -1;
    if (sscanf(path, "/dev/i2c-%d", &bus) == 1)
        d->bus = bus;
    return DDCCI_OK;
}

ddcci_status_t ddcci_find_displays(ddcci_info **list, size_t *count)
{
    return ddcci_enum_displays(list, count);
}

void ddcci_free_info_list(ddcci_info *list)
{
    free(list);
}

ddcci_status_t ddcci_open(int bus, ddcci_display **out)
{
    char path[64];

    if (bus < 0 || !out)
        return DDCCI_ERR_INVALID_ARG;
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
    return ddcci_open_path(path, out);
}

ddcci_status_t ddcci_open_path(const char *dev_path, ddcci_display **out)
{
    ddcci_display *d;
    ddcci_status_t st;

    if (!dev_path || !out)
        return DDCCI_ERR_INVALID_ARG;

    d = display_new();
    if (!d)
        return DDCCI_ERR_NOMEM;
    st = display_open_path(d, dev_path);
    if (st != DDCCI_OK) {
        free(d);
        return st;
    }
    *out = d;
    return DDCCI_OK;
}

ddcci_status_t ddcci_open_connector(const char *drm_connector, ddcci_display **out)
{
    ddcci_info *list = NULL;
    size_t n = 0, i;
    ddcci_status_t st;

    if (!drm_connector || !out)
        return DDCCI_ERR_INVALID_ARG;

    st = ddcci_find_displays(&list, &n);
    if (st != DDCCI_OK)
        return st;

    st = DDCCI_ERR_NO_DEVICE;
    for (i = 0; i < n; i++) {
        if (strcmp(list[i].connector, drm_connector) == 0) {
            if (list[i].bus >= 0)
                st = ddcci_open(list[i].bus, out);
            else
                st = DDCCI_ERR_IO;
            if (st == DDCCI_OK)
                snprintf((*out)->connector, sizeof((*out)->connector),
                         "%s", drm_connector);
            break;
        }
    }
    ddcci_free_info_list(list);
    return st;
}

void ddcci_close(ddcci_display *d)
{
    if (!d)
        return;
    if (d->fd >= 0)
        close(d->fd);
    free(d->caps);
    free(d);
}

ddcci_status_t ddcci_probe_ddc(ddcci_display *d)
{
    uint8_t w[8], r[DDCCI_FRAME_MAX];
    size_t wn, got = 0;
    ddcci_status_t st;
    static const uint8_t probes[] = { DDCCI_VCP_VERSION, DDCCI_VCP_BRIGHTNESS };

    if (!d || d->fd < 0)
        return DDCCI_ERR_INVALID_ARG;

    if (d->ddc_checked)
        return d->ddc_ok ? DDCCI_OK : DDCCI_ERR_NO_DDC;

    d->ddc_checked = true;
    d->ddc_ok = false;

    /* A valid DDC/CI frame (including a Null Message or unsupported VCP)
     * means the slave at 0x37 speaks the protocol.  A NAK / I/O error does
     * not — typical of eDP laptop panels that only implement EDID at 0x50. */
    for (size_t i = 0; i < sizeof(probes); i++) {
        ddcci_feature f;
        wn = ddcci_pack_getvcp(probes[i], w);
        got = 0;
        st = ddcci_transact(d, w, wn, r, sizeof(r), &got, DDCCI_WAIT_GET_MS);
        if (st == DDCCI_ERR_NO_DEVICE || st == DDCCI_ERR_IO || st == DDCCI_ERR_BUSY)
            continue;
        if (st == DDCCI_OK && got >= 3 && r[0] == DDCCI_DISP_WRITE) {
            d->ddc_ok = true;
            if (ddcci_unpack_getvcp(r, got, probes[i], &f) == DDCCI_OK
                && probes[i] == DDCCI_VCP_VERSION) {
                d->info.mccs_major = (uint8_t)(f.current >> 8);
                d->info.mccs_minor = (uint8_t)(f.current & 0xFFu);
            }
            return DDCCI_OK;
        }
        /* Checksum failed but we got *something* from 0x37: still DDC-ish.
         * Be conservative and keep trying the next probe. */
    }
    return DDCCI_ERR_NO_DDC;
}

ddcci_status_t ddcci_load_caps(ddcci_display *d)
{
    uint16_t offset = 0;
    char *buf = NULL;
    size_t cap = 0, len = 0;
    int empty_streak = 0;

    if (!d)
        return DDCCI_ERR_INVALID_ARG;
    if (d->caps_loaded)
        return d->caps ? DDCCI_OK : DDCCI_ERR_UNSUPPORTED;

    if (ddcci_probe_ddc(d) != DDCCI_OK)
        return DDCCI_ERR_NO_DDC;

    /* Fragments are ≤32 bytes; 4 KiB is well beyond any real caps string. */
    while (offset < 4096 && empty_streak < 2) {
        uint8_t w[8], r[DDCCI_FRAME_MAX];
        size_t wn, got = 0;
        const uint8_t *data = NULL;
        size_t dlen = 0;
        ddcci_status_t st;
        char *grow;

        wn = ddcci_pack_caps(offset, w);
        st = ddcci_transact(d, w, wn, r, sizeof(r), &got, DDCCI_WAIT_CAPS_MS);
        if (st != DDCCI_OK)
            break;
        st = ddcci_unpack_caps(r, got, offset, &data, &dlen);
        if (st != DDCCI_OK)
            break;
        if (dlen == 0) {
            empty_streak++;
            break;
        }
        if (len + dlen + 1 > cap) {
            size_t nc = cap ? cap * 2 : 256;
            while (nc < len + dlen + 1)
                nc *= 2;
            grow = realloc(buf, nc);
            if (!grow) {
                free(buf);
                return DDCCI_ERR_NOMEM;
            }
            buf = grow;
            cap = nc;
        }
        memcpy(buf + len, data, dlen);
        len += dlen;
        buf[len] = '\0';
        offset = (uint16_t)(offset + dlen);
        empty_streak = 0;
    }

    d->caps_loaded = true;
    if (!buf || len == 0) {
        free(buf);
        return DDCCI_ERR_UNSUPPORTED;
    }
    d->caps = buf;
    d->caps_len = len;
    return DDCCI_OK;
}

static void resolve_feature(ddcci_display *d, ddcci_feature *slot,
                            const uint8_t *candidates, size_t ncand)
{
    size_t i;
    ddcci_feature f;
    ddcci_status_t st;

    for (i = 0; i < ncand; i++) {
        uint8_t op = candidates[i];
        bool in_caps = ddcci_info_has_vcp(&d->info, op);

        st = ddcci_get_vcp(d, op, &f);
        if (st == DDCCI_OK) {
            bool from_caps = in_caps || slot->from_caps;
            *slot = f;
            slot->from_caps = from_caps;
            slot->from_probe = true;
            slot->present = true;
            slot->opcode = op;
            return;
        }
        if (in_caps && !slot->present) {
            slot->opcode = op;
            slot->from_caps = true;
            slot->present = true;
        }
    }
}

static ddcci_status_t load_info(ddcci_display *d)
{
    if (!d)
        return DDCCI_ERR_INVALID_ARG;
    if (d->info_loaded)
        return DDCCI_OK;

    d->info.bus = d->bus;
    snprintf(d->info.path, sizeof(d->info.path), "%s", d->path);
    snprintf(d->info.connector, sizeof(d->info.connector), "%s", d->connector);
    d->info.accessible = d->fd >= 0;

    if (d->fd >= 0) {
        if (ddcci_read_edid_i2c(d->fd, &d->info.edid) == DDCCI_OK)
            d->info.edid_ok = true;
    }

    if (ddcci_probe_ddc(d) == DDCCI_OK) {
        static const uint8_t bri[] = {
            DDCCI_VCP_BRIGHTNESS,
            DDCCI_VCP_BACKLIGHT,
            DDCCI_VCP_BACKLIGHT_WHITE
        };
        static const uint8_t con[] = { DDCCI_VCP_CONTRAST };

        d->info.ddc_supported = true;
        if (ddcci_load_caps(d) == DDCCI_OK && d->caps)
            (void)ddcci_parse_capabilities(d->caps, &d->info);

        resolve_feature(d, &d->info.brightness, bri, sizeof(bri));
        resolve_feature(d, &d->info.contrast, con, sizeof(con));
    }

    d->info_loaded = true;
    return DDCCI_OK;
}

ddcci_status_t ddcci_query(ddcci_display *d, ddcci_info *info)
{
    ddcci_status_t st;

    if (!d || !info)
        return DDCCI_ERR_INVALID_ARG;
    st = load_info(d);
    if (st != DDCCI_OK)
        return st;
    *info = d->info;
    return DDCCI_OK;
}

bool ddcci_has_ddc(ddcci_display *d)
{
    if (!d)
        return false;
    if (ddcci_probe_ddc(d) != DDCCI_OK)
        return false;
    return d->ddc_ok;
}

ddcci_status_t ddcci_find_brightness(ddcci_display *d, ddcci_feature *out)
{
    ddcci_status_t st;

    if (!d || !out)
        return DDCCI_ERR_INVALID_ARG;
    st = load_info(d);
    if (st != DDCCI_OK)
        return st;
    if (!d->info.ddc_supported)
        return DDCCI_ERR_NO_DDC;
    *out = d->info.brightness;
    return out->present ? DDCCI_OK : DDCCI_ERR_UNSUPPORTED;
}

ddcci_status_t ddcci_find_contrast(ddcci_display *d, ddcci_feature *out)
{
    ddcci_status_t st;

    if (!d || !out)
        return DDCCI_ERR_INVALID_ARG;
    st = load_info(d);
    if (st != DDCCI_OK)
        return st;
    if (!d->info.ddc_supported)
        return DDCCI_ERR_NO_DDC;
    *out = d->info.contrast;
    return out->present ? DDCCI_OK : DDCCI_ERR_UNSUPPORTED;
}

ddcci_status_t ddcci_get_vcp(ddcci_display *d, uint8_t opcode, ddcci_feature *out)
{
    uint8_t w[8], r[DDCCI_FRAME_MAX];
    size_t wn, got = 0;
    ddcci_status_t st;

    if (!d || !out)
        return DDCCI_ERR_INVALID_ARG;
    if (ddcci_probe_ddc(d) != DDCCI_OK)
        return DDCCI_ERR_NO_DDC;

    wn = ddcci_pack_getvcp(opcode, w);
    st = ddcci_transact(d, w, wn, r, sizeof(r), &got, DDCCI_WAIT_GET_MS);
    if (st != DDCCI_OK)
        return st;
    return ddcci_unpack_getvcp(r, got, opcode, out);
}

ddcci_status_t ddcci_set_vcp(ddcci_display *d, uint8_t opcode, uint16_t value)
{
    uint8_t w[8];
    size_t wn;

    if (!d)
        return DDCCI_ERR_INVALID_ARG;
    if (ddcci_probe_ddc(d) != DDCCI_OK)
        return DDCCI_ERR_NO_DDC;

    wn = ddcci_pack_setvcp(opcode, value, w);
    return ddcci_transact(d, w, wn, NULL, 0, NULL, DDCCI_WAIT_SET_MS);
}

ddcci_status_t ddcci_get_capabilities(ddcci_display *d, char **ascii, size_t *len)
{
    ddcci_status_t st;
    char *copy;

    if (!d || !ascii)
        return DDCCI_ERR_INVALID_ARG;

    st = ddcci_load_caps(d);
    if (st != DDCCI_OK)
        return st;
    copy = strdup(d->caps ? d->caps : "");
    if (!copy)
        return DDCCI_ERR_NOMEM;
    *ascii = copy;
    if (len)
        *len = d->caps_len;
    return DDCCI_OK;
}
