#define _GNU_SOURCE

#include "ddcci_priv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void ddcci_sleep_ms(unsigned ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}

uint8_t ddcci_xor(uint8_t init, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        init ^= p[i];
    return init;
}

uint8_t ddcci_write_checksum(const uint8_t *payload, size_t n)
{
    return ddcci_xor(DDCCI_DISP_WRITE, payload, n);
}

bool ddcci_read_checksum_ok(const uint8_t *frame, size_t n)
{
    if (n < 2)
        return false;
    /* VESA: init 0x50, XOR every received byte including checksum → 0 */
    return ddcci_xor(DDCCI_HOST_XOR, frame, n) == 0;
}

static size_t pack_frame(const uint8_t *data, size_t n, uint8_t *out)
{
    size_t i;

    out[0] = DDCCI_HOST_ADDR;
    out[1] = (uint8_t)(DDCCI_LENGTH_FLAG | (n & 0x0Fu));
    for (i = 0; i < n; i++)
        out[2 + i] = data[i];
    out[2 + n] = ddcci_write_checksum(out, 2 + n);
    return 3 + n;
}

size_t ddcci_pack_getvcp(uint8_t opcode, uint8_t out[8])
{
    uint8_t data[2];

    data[0] = DDCCI_CMD_GETVCP;
    data[1] = opcode;
    return pack_frame(data, 2, out);
}

size_t ddcci_pack_setvcp(uint8_t opcode, uint16_t value, uint8_t out[8])
{
    uint8_t data[4];

    data[0] = DDCCI_CMD_SETVCP;
    data[1] = opcode;
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)(value & 0xFFu);
    return pack_frame(data, 4, out);
}

size_t ddcci_pack_caps(uint16_t offset, uint8_t out[8])
{
    uint8_t data[3];

    data[0] = DDCCI_CMD_CAPS;
    data[1] = (uint8_t)(offset >> 8);
    data[2] = (uint8_t)(offset & 0xFFu);
    return pack_frame(data, 3, out);
}

void ddcci_feature_clear(ddcci_feature *f)
{
    memset(f, 0, sizeof(*f));
}

void ddcci_info_reset(ddcci_info *info)
{
    memset(info, 0, sizeof(*info));
    info->bus = -1;
}

void ddcci_add_vcp(ddcci_info *info, uint8_t opcode)
{
    size_t i;

    if (!info)
        return;
    for (i = 0; i < info->n_vcp; i++) {
        if (info->vcp_opcodes[i] == opcode)
            return;
    }
    if (info->n_vcp < DDCCI_VCP_MAX)
        info->vcp_opcodes[info->n_vcp++] = opcode;
}

bool ddcci_info_has_vcp(const ddcci_info *info, uint8_t opcode)
{
    size_t i;

    if (!info)
        return false;
    for (i = 0; i < info->n_vcp; i++) {
        if (info->vcp_opcodes[i] == opcode)
            return true;
    }
    return false;
}

ddcci_status_t ddcci_unpack_getvcp(const uint8_t *frame, size_t n,
                                   uint8_t opcode, ddcci_feature *out)
{
    size_t payload;
    const uint8_t *p;

    if (!frame || !out)
        return DDCCI_ERR_INVALID_ARG;
    ddcci_feature_clear(out);
    out->opcode = opcode;

    if (n < 3)
        return DDCCI_ERR_PARSE;
    if (!ddcci_read_checksum_ok(frame, n))
        return DDCCI_ERR_CHECKSUM;
    if (frame[0] != DDCCI_DISP_WRITE)
        return DDCCI_ERR_PARSE;

    payload = (size_t)(frame[1] & 0x7Fu);
    if (payload == 0)
        return DDCCI_ERR_UNSUPPORTED;
    if (2 + payload + 1 > n)
        return DDCCI_ERR_PARSE;

    p = frame + 2;
    if (p[0] != DDCCI_CMD_GETVCP_REP)
        return DDCCI_ERR_PARSE;
    if (payload < 8)
        return DDCCI_ERR_PARSE;
    if (p[1] == DDCCI_RC_UNSUPPORTED)
        return DDCCI_ERR_UNSUPPORTED;
    if (p[1] != DDCCI_RC_NO_ERROR)
        return DDCCI_ERR_PARSE;
    if (p[2] != opcode)
        return DDCCI_ERR_PARSE;

    out->present = true;
    out->from_probe = true;
    out->type = p[3];
    out->maximum = (uint16_t)((p[4] << 8) | p[5]);
    out->current = (uint16_t)((p[6] << 8) | p[7]);
    return DDCCI_OK;
}

ddcci_status_t ddcci_unpack_caps(const uint8_t *frame, size_t n,
                                 uint16_t expect_off,
                                 const uint8_t **data, size_t *dlen)
{
    size_t payload;
    uint16_t off;

    if (!frame || !data || !dlen)
        return DDCCI_ERR_INVALID_ARG;
    *data = NULL;
    *dlen = 0;

    if (n < 3)
        return DDCCI_ERR_PARSE;
    if (!ddcci_read_checksum_ok(frame, n))
        return DDCCI_ERR_CHECKSUM;
    if (frame[0] != DDCCI_DISP_WRITE)
        return DDCCI_ERR_PARSE;

    payload = (size_t)(frame[1] & 0x7Fu);
    if (2 + payload + 1 > n)
        return DDCCI_ERR_PARSE;
    if (payload < 3)
        return DDCCI_ERR_PARSE;
    if (frame[2] != DDCCI_CMD_CAPS_REP)
        return DDCCI_ERR_PARSE;

    off = (uint16_t)((frame[3] << 8) | frame[4]);
    if (off != expect_off)
        return DDCCI_ERR_PARSE;

    *data = frame + 5;
    *dlen = payload - 3;
    return DDCCI_OK;
}

static bool edid_header_ok(const uint8_t *raw)
{
    static const uint8_t magic[8] = {
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
    };
    return memcmp(raw, magic, 8) == 0;
}

static bool edid_block_sum_ok(const uint8_t *block)
{
    unsigned sum = 0;
    size_t i;

    for (i = 0; i < 128; i++)
        sum += block[i];
    return (sum & 0xFFu) == 0;
}

static void edid_copy_text(char *dst, size_t dstsz, const uint8_t *src, size_t n)
{
    size_t i, o = 0;

    for (i = 0; i < n && o + 1 < dstsz; i++) {
        unsigned char c = src[i];
        if (c == 0x0A)
            break;
        if (c < 0x20 || c > 0x7E)
            continue;
        dst[o++] = (char)c;
    }
    while (o > 0 && dst[o - 1] == ' ')
        o--;
    dst[o] = '\0';
}

static void edid_parse_descriptors(const uint8_t *raw, ddcci_edid *out)
{
    int d;

    for (d = 0; d < 4; d++) {
        const uint8_t *desc = raw + 54 + d * 18;
        uint8_t type;

        if (desc[0] || desc[1] || desc[2])
            continue;
        type = desc[3];
        if (type == 0xFC && out->model[0] == '\0')
            edid_copy_text(out->model, sizeof(out->model), desc + 5, 13);
        else if (type == 0xFF && out->serial[0] == '\0')
            edid_copy_text(out->serial, sizeof(out->serial), desc + 5, 13);
    }
}

ddcci_status_t ddcci_parse_edid(const uint8_t *raw, size_t len, ddcci_edid *out)
{
    uint16_t mfg;
    size_t copy;

    if (!raw || !out)
        return DDCCI_ERR_INVALID_ARG;
    if (len < DDCCI_EDID_LEN_MIN)
        return DDCCI_ERR_PARSE;
    if (!edid_header_ok(raw))
        return DDCCI_ERR_PARSE;

    memset(out, 0, sizeof(*out));
    copy = len > DDCCI_EDID_LEN_MAX ? DDCCI_EDID_LEN_MAX : len;
    memcpy(out->raw, raw, copy);
    out->len = copy;
    out->checksum_ok = edid_block_sum_ok(raw);

    mfg = (uint16_t)((raw[8] << 8) | raw[9]);
    out->manufacturer[0] = (char)(((mfg >> 10) & 0x1F) + 'A' - 1);
    out->manufacturer[1] = (char)(((mfg >> 5) & 0x1F) + 'A' - 1);
    out->manufacturer[2] = (char)((mfg & 0x1F) + 'A' - 1);
    out->manufacturer[3] = '\0';

    out->product_code = (uint16_t)(raw[10] | (raw[11] << 8));
    out->serial_number = (uint32_t)raw[12]
                       | ((uint32_t)raw[13] << 8)
                       | ((uint32_t)raw[14] << 16)
                       | ((uint32_t)raw[15] << 24);
    out->week = raw[16];
    out->year = (uint16_t)(1990 + raw[17]);
    out->version_major = raw[18];
    out->version_minor = raw[19];

    edid_parse_descriptors(raw, out);
    return DDCCI_OK;
}

static const char *find_tag(const char *s, const char *tag)
{
    size_t n = strlen(tag);
    const char *p = s;

    while ((p = strstr(p, tag)) != NULL) {
        char before = (p == s) ? '\0' : p[-1];
        if (!isalnum((unsigned char)before) && p[n] == '(')
            return p + n + 1;
        p += n;
    }
    return NULL;
}

static void parse_vcp_body(const char *body, ddcci_info *out)
{
    const char *p = body;
    int depth = 1;

    while (*p && depth > 0) {
        if (*p == '(') {
            depth++;
            p++;
            continue;
        }
        if (*p == ')') {
            depth--;
            p++;
            continue;
        }
        if (depth == 1 && isxdigit((unsigned char)*p)) {
            char tmp[8];
            size_t k = 0;
            unsigned long v;

            while (isxdigit((unsigned char)*p) && k + 1 < sizeof(tmp))
                tmp[k++] = *p++;
            tmp[k] = '\0';
            v = strtoul(tmp, NULL, 16);
            if (v <= 0xFFu)
                ddcci_add_vcp(out, (uint8_t)v);
            continue;
        }
        p++;
    }
}

static void parse_mccs_ver(const char *body, ddcci_info *out)
{
    unsigned maj = 0, min = 0;
    const char *p = body;

    while (*p && *p != ')' && !isdigit((unsigned char)*p))
        p++;
    if (sscanf(p, "%u.%u", &maj, &min) >= 1) {
        out->mccs_major = (uint8_t)maj;
        out->mccs_minor = (uint8_t)min;
    }
}

static void apply_known_features_from_caps(ddcci_info *out)
{
    if (ddcci_info_has_vcp(out, DDCCI_VCP_BRIGHTNESS)) {
        out->brightness.from_caps = true;
        out->brightness.present = true;
        if (out->brightness.opcode == 0)
            out->brightness.opcode = DDCCI_VCP_BRIGHTNESS;
    } else if (ddcci_info_has_vcp(out, DDCCI_VCP_BACKLIGHT)) {
        out->brightness.from_caps = true;
        out->brightness.present = true;
        if (out->brightness.opcode == 0)
            out->brightness.opcode = DDCCI_VCP_BACKLIGHT;
    } else if (ddcci_info_has_vcp(out, DDCCI_VCP_BACKLIGHT_WHITE)) {
        out->brightness.from_caps = true;
        out->brightness.present = true;
        if (out->brightness.opcode == 0)
            out->brightness.opcode = DDCCI_VCP_BACKLIGHT_WHITE;
    }

    if (ddcci_info_has_vcp(out, DDCCI_VCP_CONTRAST)) {
        out->contrast.from_caps = true;
        out->contrast.present = true;
        if (out->contrast.opcode == 0)
            out->contrast.opcode = DDCCI_VCP_CONTRAST;
    }
}

ddcci_status_t ddcci_parse_capabilities(const char *caps, ddcci_info *out)
{
    const char *body;

    if (!caps || !out)
        return DDCCI_ERR_INVALID_ARG;

    out->n_vcp = 0;
    memset(out->vcp_opcodes, 0, sizeof(out->vcp_opcodes));
    out->mccs_major = 0;
    out->mccs_minor = 0;
    out->brightness.from_caps = false;
    out->contrast.from_caps = false;

    body = find_tag(caps, "vcp");
    if (body)
        parse_vcp_body(body, out);

    body = find_tag(caps, "mccs_ver");
    if (body)
        parse_mccs_ver(body, out);

    apply_known_features_from_caps(out);
    return DDCCI_OK;
}

const char *ddcci_strerror(ddcci_status_t st)
{
    switch (st) {
    case DDCCI_OK:              return "success";
    case DDCCI_ERR_INVALID_ARG: return "invalid argument";
    case DDCCI_ERR_NOMEM:       return "out of memory";
    case DDCCI_ERR_IO:          return "I2C I/O error";
    case DDCCI_ERR_NO_DEVICE:   return "no such display";
    case DDCCI_ERR_NO_DDC:      return "display does not support DDC/CI";
    case DDCCI_ERR_CHECKSUM:    return "DDC/CI checksum mismatch";
    case DDCCI_ERR_TIMEOUT:     return "timeout waiting for DDC/CI reply";
    case DDCCI_ERR_UNSUPPORTED: return "VCP feature not supported";
    case DDCCI_ERR_PARSE:       return "malformed DDC/CI or EDID data";
    case DDCCI_ERR_BUSY:        return "I2C bus busy";
    default:                    return "unknown error";
    }
}
