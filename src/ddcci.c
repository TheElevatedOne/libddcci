#define _GNU_SOURCE

#include "ddcci_priv.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const uint8_t BRI_OPS[] = {
    DDCCI_VCP_BRIGHTNESS,
    DDCCI_VCP_BACKLIGHT,
    DDCCI_VCP_BACKLIGHT_WHITE
};
static const uint8_t CON_OPS[] = { DDCCI_VCP_CONTRAST };

static ddcci_display *display_new(void)
{
    ddcci_display *d = calloc(1, sizeof(*d));

    if (!d)
        return NULL;
    d->fd = -1;
    d->bus = -1;
    d->scale_num = ddcci_default_scale_num();
    d->get_wait_ms = DDCCI_GET_WAIT_MS;
    d->caps_wait_ms = DDCCI_CAPS_WAIT_MS;
    ddcci_info_reset(&d->info);
    return d;
}

static ddcci_status_t display_open_path(ddcci_display *d, const char *path)
{
    int err = 0;

    d->fd = ddcci_i2c_open(path, &err);
    if (d->fd < 0) {
        if (err == ENOENT || err == ENXIO || err == ENODEV)
            return DDCCI_ERR_NO_DEVICE;
        return DDCCI_ERR_IO;
    }
    snprintf(d->path, sizeof(d->path), "%s", path);
    d->bus = ddcci_bus_from_devnode(path);
    ddcci_note_identity(d);
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
    char short_name[64];
    int bus;
    ddcci_status_t st;

    if (!drm_connector || !out)
        return DDCCI_ERR_INVALID_ARG;

    short_name[0] = '\0';
    bus = ddcci_bus_from_connector(drm_connector, short_name, sizeof(short_name));
    if (bus < 0)
        return DDCCI_ERR_NO_DEVICE;

    st = ddcci_open(bus, out);
    if (st != DDCCI_OK)
        return st;
    if (short_name[0])
        snprintf((*out)->connector, sizeof((*out)->connector), "%s", short_name);
    return DDCCI_OK;
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

void ddcci_set_sleep_scale(ddcci_display *d, double scale)
{
    if (!d)
        return;
    if (scale < 0.1)
        scale = 0.1;
    if (scale > 8.0)
        scale = 8.0;
    d->scale_num = (unsigned)(scale * 100.0 + 0.5);
    if (d->scale_num == 0)
        d->scale_num = 1;
}

ddcci_status_t ddcci_get_vcp_raw(ddcci_display *d, uint8_t opcode, ddcci_feature *out)
{
    uint8_t w[8];
    uint8_t r[DDCCI_READ_MAX];
    size_t wn, got = 0;
    ddcci_status_t st;

    if (!d || d->fd < 0 || !out)
        return DDCCI_ERR_INVALID_ARG;
    if (d->ddc_checked && !d->ddc_ok)
        return DDCCI_ERR_NO_DDC;

    wn = ddcci_pack_getvcp(opcode, w);
    if (wn == 0)
        return DDCCI_ERR_INVALID_ARG;
    st = ddcci_transact(d, w, wn, r, sizeof(r), DDCCI_READ_GET, &got, DDCCI_KIND_GET);
    if (st != DDCCI_OK)
        return st;
    return ddcci_unpack_getvcp(r, got, opcode, out);
}

static ddcci_status_t probe_ops(ddcci_display *d, ddcci_feature *slot,
                                const uint8_t *ops, size_t nops, bool *known)
{
    size_t i;

    for (i = 0; i < nops; i++) {
        ddcci_feature got;
        ddcci_status_t st;
        bool in_caps = ddcci_info_has_vcp(&d->info, ops[i]);

        st = ddcci_get_vcp_raw(d, ops[i], &got);
        if (st == DDCCI_OK) {
            bool from_caps = in_caps || (slot->from_caps && slot->opcode == ops[i]);

            *slot = got;
            slot->opcode = ops[i];
            slot->from_caps = from_caps;
            slot->from_probe = true;
            slot->present = true;
            if (known)
                *known = true;
            return DDCCI_OK;
        }
        if (st == DDCCI_ERR_NO_DDC || st == DDCCI_ERR_NO_DEVICE) {
            if (known)
                *known = true;
            return DDCCI_ERR_NO_DDC;
        }
        if (st == DDCCI_ERR_UNSUPPORTED) {
            if (in_caps && !slot->from_probe) {
                slot->opcode = ops[i];
                slot->from_caps = true;
                slot->present = true;
            }
            continue;
        }
        /* Timeout or a bus error: do not cache, so the caller can retry. */
        return st;
    }

    if (known)
        *known = true;
    return slot->present ? DDCCI_OK : DDCCI_ERR_UNSUPPORTED;
}

static void preserve_mccs(ddcci_display *d, uint8_t maj, uint8_t min)
{
    if (d->info.mccs_major == 0 && maj != 0) {
        d->info.mccs_major = maj;
        d->info.mccs_minor = min;
    }
}

static ddcci_status_t load_caps(ddcci_display *d)
{
    char *buf = NULL;
    size_t cap = 0, len = 0;
    unsigned offset = 0;
    int fragments = 0;
    int mismatch_left = 1;
    uint8_t prev[DDCCI_READ_MAX];
    size_t prev_len = 0;
    unsigned prev_off = 0xFFFFFFFFu;

    if (!d)
        return DDCCI_ERR_INVALID_ARG;
    if (d->caps_loaded && !d->caps_retryable)
        return d->caps ? DDCCI_OK : DDCCI_ERR_UNSUPPORTED;
    if (d->ddc_checked && !d->ddc_ok)
        return DDCCI_ERR_NO_DDC;

    free(d->caps);
    d->caps = NULL;
    d->caps_len = 0;
    d->caps_loaded = false;
    d->caps_retryable = false;

    while (offset < 4096u && fragments < 64) {
        uint8_t w[8];
        uint8_t r[DDCCI_READ_MAX];
        const uint8_t *data = NULL;
        size_t dlen = 0, got = 0, wn, step, i;
        uint16_t got_off = 0;
        bool done = false;
        ddcci_status_t st;

        wn = ddcci_pack_caps((uint16_t)offset, w);
        st = ddcci_transact(d, w, wn, r, sizeof(r), DDCCI_READ_CAPS, &got,
                            DDCCI_KIND_CAPS);
        if (st != DDCCI_OK) {
            if (len > 0)
                break;
            free(buf);
            if (st == DDCCI_ERR_TIMEOUT || st == DDCCI_ERR_BUSY ||
                st == DDCCI_ERR_IO || st == DDCCI_ERR_CHECKSUM) {
                d->caps_retryable = true;
                return st;
            }
            d->caps_loaded = true;
            return st;
        }

        st = ddcci_unpack_caps_any(r, got, &data, &dlen, &got_off);
        if (st != DDCCI_OK) {
            if (len > 0)
                break;
            free(buf);
            if (st == DDCCI_ERR_TIMEOUT) {
                d->caps_retryable = true;
                return st;
            }
            d->caps_loaded = true;
            return DDCCI_ERR_UNSUPPORTED;
        }

        if (dlen == 0)
            break;

        /* Panel ignored the offset and replayed the start of the string. */
        if (got_off == 0 && offset != 0)
            break;

        if (got_off != offset) {
            if (mismatch_left > 0) {
                mismatch_left--;
                continue;
            }
        } else {
            mismatch_left = 1;
        }

        if (prev_len == dlen && got_off == prev_off &&
            memcmp(prev, data, dlen) == 0)
            break;
        prev_off = got_off;
        prev_len = dlen < sizeof(prev) ? dlen : sizeof(prev);
        memcpy(prev, data, prev_len);

        step = dlen;
        for (i = 0; i < dlen; i++) {
            if (data[i] == 0) {
                step = i + 1;
                break;
            }
        }

        st = ddcci_caps_append(&buf, &len, &cap, data, dlen, &done);
        if (st != DDCCI_OK) {
            free(buf);
            return st;
        }
        if (offset + step >= 4096u || step == 0)
            break;
        offset += (unsigned)step;
        fragments++;
        if (done)
            break;
    }

    d->caps_loaded = true;
    d->caps_retryable = false;
    if (!buf || len == 0) {
        free(buf);
        return DDCCI_ERR_UNSUPPORTED;
    }
    d->caps = buf;
    d->caps_len = len;
    return DDCCI_OK;
}

static ddcci_status_t note_caps(ddcci_display *d)
{
    uint8_t maj, min;
    ddcci_status_t st;

    st = load_caps(d);
    if (st != DDCCI_OK)
        return st;
    maj = d->info.mccs_major;
    min = d->info.mccs_minor;
    st = ddcci_parse_capabilities(d->caps, &d->info);
    preserve_mccs(d, maj, min);
    return st;
}

static ddcci_status_t refresh_slot(ddcci_display *d, ddcci_feature *slot, bool *fresh)
{
    ddcci_feature got;
    bool from_caps;
    ddcci_status_t st;

    if (!slot->from_probe || slot->opcode == 0) {
        *fresh = true;
        return DDCCI_OK;
    }
    from_caps = slot->from_caps;
    st = ddcci_get_vcp_raw(d, slot->opcode, &got);
    if (st != DDCCI_OK)
        return st;
    *slot = got;
    slot->from_caps = from_caps;
    *fresh = true;
    return DDCCI_OK;
}

static void read_version_if_needed(ddcci_display *d)
{
    ddcci_feature ver;

    if (d->info.mccs_major != 0 || !d->ddc_ok)
        return;
    if (ddcci_get_vcp_raw(d, DDCCI_VCP_VERSION, &ver) == DDCCI_OK) {
        d->info.mccs_major = (uint8_t)(ver.current >> 8);
        d->info.mccs_minor = (uint8_t)(ver.current & 0xFFu);
    }
}

static void fill_identity(ddcci_display *d)
{
    d->info.bus = d->bus;
    snprintf(d->info.path, sizeof(d->info.path), "%s", d->path);
    snprintf(d->info.connector, sizeof(d->info.connector), "%s", d->connector);
    snprintf(d->info.adapter_name, sizeof(d->info.adapter_name), "%s", d->adapter_name);
    d->info.accessible = d->fd >= 0;
}

static ddcci_status_t ensure_edid(ddcci_display *d)
{
    if (d->edid_loaded || d->fd < 0)
        return DDCCI_OK;
    if (ddcci_read_edid_i2c(d, &d->info.edid) == DDCCI_OK) {
        d->info.edid_ok = true;
        d->edid_loaded = true;
    }
    return DDCCI_OK;
}

static ddcci_status_t load_info(ddcci_display *d)
{
    ddcci_status_t st;

    if (d->info_loaded)
        return DDCCI_OK;

    fill_identity(d);
    (void)ensure_edid(d);

    if (!d->brightness_known) {
        st = probe_ops(d, &d->info.brightness, BRI_OPS, sizeof(BRI_OPS),
                       &d->brightness_known);
        if (st == DDCCI_ERR_NO_DDC) {
            d->info.ddc_supported = false;
            d->brightness_fresh = true;
            d->info_loaded = true;
            return DDCCI_OK;
        }
        if (st != DDCCI_OK && st != DDCCI_ERR_UNSUPPORTED)
            return st;
        d->brightness_fresh = true;
    } else if (!d->brightness_fresh) {
        st = refresh_slot(d, &d->info.brightness, &d->brightness_fresh);
        if (st == DDCCI_ERR_NO_DDC) {
            d->info.ddc_supported = false;
            d->info_loaded = true;
            return DDCCI_OK;
        }
        if (st != DDCCI_OK && st != DDCCI_ERR_UNSUPPORTED)
            return st;
    }

    if (d->ddc_checked && !d->ddc_ok) {
        d->info.ddc_supported = false;
        d->info_loaded = true;
        return DDCCI_OK;
    }
    if (!d->ddc_ok) {
        /* Brightness was unsupported-or-ok only if ddc_ok was set by transact.
         * If we somehow got here without a frame, don't claim support. */
        d->info.ddc_supported = false;
        d->info_loaded = true;
        return DDCCI_OK;
    }

    d->info.ddc_supported = true;

    st = note_caps(d);
    if (st == DDCCI_ERR_NOMEM)
        return st;
    /* A missing or slow capabilities string does not fail the snapshot. */

    if (!d->contrast_known) {
        st = probe_ops(d, &d->info.contrast, CON_OPS, sizeof(CON_OPS),
                       &d->contrast_known);
        if (st == DDCCI_OK || st == DDCCI_ERR_UNSUPPORTED)
            d->contrast_fresh = true;
        else if (st == DDCCI_ERR_NO_DDC)
            d->info.ddc_supported = false;
        else
            d->contrast_known = false; /* transient: allow a later retry */
    } else if (!d->contrast_fresh) {
        (void)refresh_slot(d, &d->info.contrast, &d->contrast_fresh);
    }

    read_version_if_needed(d);
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

    /* Snapshot stays cached, except currents invalidated by ddcci_set_vcp(). */
    if (d->info.ddc_supported && d->brightness_known && !d->brightness_fresh)
        (void)refresh_slot(d, &d->info.brightness, &d->brightness_fresh);
    if (d->info.ddc_supported && d->contrast_known && !d->contrast_fresh)
        (void)refresh_slot(d, &d->info.contrast, &d->contrast_fresh);

    *info = d->info;
    return DDCCI_OK;
}

bool ddcci_has_ddc(ddcci_display *d)
{
    ddcci_feature f;
    ddcci_status_t st;

    if (!d || d->fd < 0)
        return false;
    if (d->ddc_checked)
        return d->ddc_ok;

    /* One Get VCP. A NACK is cached as "no DDC"; a timeout is not, so the
     * next call tries again instead of burning a second opcode first. */
    st = ddcci_get_vcp_raw(d, DDCCI_VCP_BRIGHTNESS, &f);
    return st == DDCCI_OK || st == DDCCI_ERR_UNSUPPORTED;
}

static ddcci_status_t resolve_feature(ddcci_display *d, ddcci_feature *slot,
                                      bool *known, bool *fresh,
                                      const uint8_t *ops, size_t nops,
                                      ddcci_feature *out)
{
    ddcci_status_t st;

    if (*known) {
        if (d->ddc_checked && !d->ddc_ok)
            return DDCCI_ERR_NO_DDC;
        if (slot->from_probe && slot->opcode != 0) {
            st = refresh_slot(d, slot, fresh);
            if (st != DDCCI_OK)
                return st;
        }
        *out = *slot;
        if (!slot->present)
            return DDCCI_ERR_UNSUPPORTED;
        return DDCCI_OK;
    }

    st = probe_ops(d, slot, ops, nops, known);
    if (st == DDCCI_OK) {
        *fresh = true;
        d->info.ddc_supported = true;
        *out = *slot;
        return DDCCI_OK;
    }
    if (st == DDCCI_ERR_NO_DDC) {
        d->info.ddc_supported = false;
        *fresh = true;
        ddcci_feature_clear(out);
        return DDCCI_ERR_NO_DDC;
    }
    if (st != DDCCI_ERR_UNSUPPORTED) {
        *known = false;
        return st;
    }

    /* Every candidate said unsupported. Capabilities are the last resort
     * and the only reason this path reads more than a few transactions. */
    if (!slot->present) {
        ddcci_status_t caps = note_caps(d);

        if (caps == DDCCI_ERR_NOMEM)
            return caps;
        if (caps == DDCCI_ERR_NO_DDC)
            return DDCCI_ERR_NO_DDC;
    }

    *fresh = true;
    *out = *slot;
    if (d->ddc_ok)
        d->info.ddc_supported = true;
    return slot->present ? DDCCI_OK : DDCCI_ERR_UNSUPPORTED;
}

ddcci_status_t ddcci_find_brightness(ddcci_display *d, ddcci_feature *out)
{
    if (!d || !out)
        return DDCCI_ERR_INVALID_ARG;
    return resolve_feature(d, &d->info.brightness, &d->brightness_known,
                           &d->brightness_fresh, BRI_OPS, sizeof(BRI_OPS), out);
}

ddcci_status_t ddcci_find_contrast(ddcci_display *d, ddcci_feature *out)
{
    if (!d || !out)
        return DDCCI_ERR_INVALID_ARG;
    return resolve_feature(d, &d->info.contrast, &d->contrast_known,
                           &d->contrast_fresh, CON_OPS, sizeof(CON_OPS), out);
}

ddcci_status_t ddcci_get_vcp(ddcci_display *d, uint8_t opcode, ddcci_feature *out)
{
    ddcci_feature got;
    ddcci_status_t st;

    if (!d || !out)
        return DDCCI_ERR_INVALID_ARG;

    st = ddcci_get_vcp_raw(d, opcode, &got);
    if (st != DDCCI_OK)
        return st;

    if (d->info.brightness.opcode == opcode)
        got.from_caps = d->info.brightness.from_caps;
    else if (d->info.contrast.opcode == opcode)
        got.from_caps = d->info.contrast.from_caps;
    *out = got;

    if (d->info.brightness.opcode == opcode && d->info.brightness.present) {
        bool from_caps = d->info.brightness.from_caps;

        d->info.brightness = got;
        d->info.brightness.from_caps = from_caps;
        d->brightness_fresh = true;
    }
    if (d->info.contrast.opcode == opcode && d->info.contrast.present) {
        bool from_caps = d->info.contrast.from_caps;

        d->info.contrast = got;
        d->info.contrast.from_caps = from_caps;
        d->contrast_fresh = true;
    }
    return DDCCI_OK;
}

static ddcci_status_t set_vcp_kind(ddcci_display *d, uint8_t opcode,
                                   uint16_t value, int kind)
{
    uint8_t w[8];
    size_t wn;
    ddcci_status_t st;

    if (!d)
        return DDCCI_ERR_INVALID_ARG;
    wn = ddcci_pack_setvcp(opcode, value, w);
    if (wn == 0)
        return DDCCI_ERR_INVALID_ARG;

    st = ddcci_transact(d, w, wn, NULL, 0, 0, NULL, kind);
    if (st != DDCCI_OK)
        return st;

    if (d->info.brightness.present && d->info.brightness.opcode == opcode) {
        d->info.brightness.current = value;
        d->brightness_fresh = false;
    }
    if (d->info.contrast.present && d->info.contrast.opcode == opcode) {
        d->info.contrast.current = value;
        d->contrast_fresh = false;
    }
    return DDCCI_OK;
}

ddcci_status_t ddcci_set_vcp(ddcci_display *d, uint8_t opcode, uint16_t value)
{
    return set_vcp_kind(d, opcode, value, DDCCI_KIND_SET);
}

ddcci_status_t ddcci_save_settings(ddcci_display *d)
{
    return set_vcp_kind(d, DDCCI_VCP_SAVE, 1, DDCCI_KIND_SAVE);
}

ddcci_status_t ddcci_get_capabilities(ddcci_display *d, char **ascii, size_t *len)
{
    ddcci_status_t st;
    char *copy;

    if (!d || !ascii)
        return DDCCI_ERR_INVALID_ARG;

    st = load_caps(d);
    if (st != DDCCI_OK)
        return st;
    copy = malloc(d->caps_len + 1);
    if (!copy)
        return DDCCI_ERR_NOMEM;
    memcpy(copy, d->caps, d->caps_len);
    copy[d->caps_len] = '\0';
    *ascii = copy;
    if (len)
        *len = d->caps_len;
    return DDCCI_OK;
}
