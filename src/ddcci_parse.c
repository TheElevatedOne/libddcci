#define _GNU_SOURCE

#include "ddcci_priv.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void ddcci_sleep_ms(unsigned ms)
{
    struct timespec ts;

    if (ms == 0)
        return;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) {
        if (errno != EINTR)
            break;
    }
}

long long ddcci_mono_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
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
    if (!frame || n < 2)
        return false;
    /* VESA: seed 0x50, XOR every received byte including the checksum → 0. */
    return ddcci_xor(DDCCI_HOST_XOR, frame, n) == 0;
}

bool ddcci_frame_split(const uint8_t *buf, size_t n, size_t *flen, bool *is_null)
{
    size_t declared, need;

    if (flen)
        *flen = 0;
    if (is_null)
        *is_null = false;
    if (!buf || n < 3)
        return false;
    if (buf[0] != DDCCI_DISP_WRITE || (buf[1] & DDCCI_LENGTH_FLAG) == 0)
        return false;

    declared = (size_t)(buf[1] & 0x7Fu);
    need = 2u + declared + 1u;
    if (need > n || !ddcci_read_checksum_ok(buf, need))
        return false;
    if (flen)
        *flen = need;
    if (is_null)
        *is_null = (declared == 0);
    return true;
}

static size_t pack_frame(const uint8_t *data, size_t n, uint8_t *out)
{
    size_t i;

    /* Callers pass an 8-byte buffer. 3 + n must fit; Set VCP uses n = 4. */
    if (!data || !out || n > 5)
        return 0;

    out[0] = DDCCI_HOST_ADDR;
    out[1] = (uint8_t)(DDCCI_LENGTH_FLAG | n);
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
    if (f)
        memset(f, 0, sizeof(*f));
}

void ddcci_info_reset(ddcci_info *info)
{
    if (!info)
        return;
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

bool ddcci_ascii_icontains(const char *hay, const char *needle)
{
    size_t n;

    if (!hay || !needle || !needle[0])
        return false;
    n = strlen(needle);
    for (; *hay; hay++) {
        size_t i;

        for (i = 0; i < n; i++) {
            unsigned char a = (unsigned char)hay[i];
            unsigned char b = (unsigned char)needle[i];

            if (a == 0)
                return false;
            if (a >= 'A' && a <= 'Z')
                a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = (unsigned char)(b - 'A' + 'a');
            if (a != b)
                break;
        }
        if (i == n)
            return true;
    }
    return false;
}

unsigned ddcci_default_scale_num(void)
{
    const char *env = getenv("DDCCI_SLEEP_MULTIPLIER");
    char *end = NULL;
    double x;

    if (!env || !env[0])
        return 100;
    x = strtod(env, &end);
    if (end == env || x <= 0.0)
        return 100;
    if (x < 0.1)
        x = 0.1;
    if (x > 8.0)
        x = 8.0;
    return (unsigned)(x * 100.0 + 0.5);
}

unsigned ddcci_scale_ms(const struct ddcci_display *d, unsigned base_ms)
{
    unsigned num, v;

    if (base_ms == 0)
        return 0;
    num = (d && d->scale_num) ? d->scale_num : 100u;
    v = (unsigned)(((unsigned long)base_ms * num) / 100ul);
    if (v == 0)
        v = 1;
    return v;
}

ddcci_status_t ddcci_caps_append(char **buf, size_t *len, size_t *cap,
                                 const uint8_t *data, size_t dlen, bool *done)
{
    size_t use = 0;
    size_t i;
    char *grow;

    if (!buf || !len || !cap || !done)
        return DDCCI_ERR_INVALID_ARG;
    *done = false;
    if (dlen && !data)
        return DDCCI_ERR_INVALID_ARG;

    for (i = 0; i < dlen; i++) {
        if (data[i] == 0) {
            *done = true;
            break;
        }
        use++;
    }

    if (use == 0) {
        if (*buf && *len < *cap)
            (*buf)[*len] = '\0';
        return DDCCI_OK;
    }

    if (*len + use + 1 > *cap) {
        size_t nc = *cap ? *cap : 256u;

        while (nc < *len + use + 1) {
            if (nc > 4096u)
                return DDCCI_ERR_NOMEM;
            nc *= 2u;
        }
        grow = realloc(*buf, nc);
        if (!grow)
            return DDCCI_ERR_NOMEM;
        *buf = grow;
        *cap = nc;
    }
    memcpy(*buf + *len, data, use);
    *len += use;
    (*buf)[*len] = '\0';
    return DDCCI_OK;
}

static ddcci_status_t classify_bad_frame(const uint8_t *frame, size_t n)
{
    size_t declared, need;

    if (!frame || n < 3)
        return DDCCI_ERR_PARSE;
    if (frame[0] == DDCCI_DISP_WRITE && (frame[1] & DDCCI_LENGTH_FLAG) != 0) {
        declared = (size_t)(frame[1] & 0x7Fu);
        need = 2u + declared + 1u;
        if (need <= n && !ddcci_read_checksum_ok(frame, need))
            return DDCCI_ERR_CHECKSUM;
    }
    return DDCCI_ERR_PARSE;
}

ddcci_status_t ddcci_unpack_getvcp(const uint8_t *frame, size_t n,
                                   uint8_t opcode, ddcci_feature *out)
{
    size_t flen = 0;
    size_t payload;
    bool is_null = false;
    const uint8_t *p;

    if (!frame || !out)
        return DDCCI_ERR_INVALID_ARG;
    ddcci_feature_clear(out);
    out->opcode = opcode;

    if (!ddcci_frame_split(frame, n, &flen, &is_null))
        return classify_bad_frame(frame, n);
    if (is_null)
        return DDCCI_ERR_TIMEOUT;

    payload = (size_t)(frame[1] & 0x7Fu);
    p = frame + 2;
    if (p[0] != DDCCI_CMD_GETVCP_REP)
        return DDCCI_ERR_PARSE;
    /* A short "unsupported" reply is still a real answer, not a null message. */
    if (payload >= 2 && p[1] == DDCCI_RC_UNSUPPORTED) {
        if (payload >= 3 && p[2] != opcode)
            return DDCCI_ERR_PARSE;
        return DDCCI_ERR_UNSUPPORTED;
    }
    if (payload < 8)
        return DDCCI_ERR_PARSE;
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

ddcci_status_t ddcci_unpack_caps_any(const uint8_t *frame, size_t n,
                                     const uint8_t **data, size_t *dlen,
                                     uint16_t *got_off)
{
    size_t flen = 0;
    size_t payload;
    bool is_null = false;

    if (!frame || !data || !dlen || !got_off)
        return DDCCI_ERR_INVALID_ARG;
    *data = NULL;
    *dlen = 0;
    *got_off = 0;

    if (!ddcci_frame_split(frame, n, &flen, &is_null))
        return classify_bad_frame(frame, n);
    if (is_null)
        return DDCCI_ERR_TIMEOUT;

    payload = (size_t)(frame[1] & 0x7Fu);
    if (payload < 3 || frame[2] != DDCCI_CMD_CAPS_REP)
        return DDCCI_ERR_PARSE;

    *got_off = (uint16_t)((frame[3] << 8) | frame[4]);
    *data = frame + 5;
    *dlen = payload - 3;
    return DDCCI_OK;
}

ddcci_status_t ddcci_unpack_caps(const uint8_t *frame, size_t n,
                                 uint16_t expect_off,
                                 const uint8_t **data, size_t *dlen)
{
    uint16_t got = 0;
    ddcci_status_t st;

    st = ddcci_unpack_caps_any(frame, n, data, dlen, &got);
    if (st != DDCCI_OK)
        return st;
    if (got != expect_off) {
        *data = NULL;
        *dlen = 0;
        return DDCCI_ERR_PARSE;
    }
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

static char mfg_letter(unsigned v)
{
    if (v >= 1 && v <= 26)
        return (char)('A' + (int)v - 1);
    return '?';
}

static void edid_copy_text(char *dst, size_t dstsz, const uint8_t *src, size_t n)
{
    size_t i, o = 0;

    for (i = 0; i < n && o + 1 < dstsz; i++) {
        unsigned char c = src[i];

        if (c == 0x0A || c == 0x00)
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
        /* 0xFC is the monitor name and wins over 0xFE (unspecified text). */
        if (type == 0xFC)
            edid_copy_text(out->model, sizeof(out->model), desc + 5, 13);
        else if (type == 0xFF && out->serial[0] == '\0')
            edid_copy_text(out->serial, sizeof(out->serial), desc + 5, 13);
        else if (type == 0xFE && out->model[0] == '\0')
            edid_copy_text(out->model, sizeof(out->model), desc + 5, 13);
    }
}

ddcci_status_t ddcci_parse_edid(const uint8_t *raw, size_t len, ddcci_edid *out)
{
    uint16_t mfg;
    size_t copy;
    size_t block;

    if (!raw || !out)
        return DDCCI_ERR_INVALID_ARG;
    if (len < DDCCI_EDID_LEN_MIN || !edid_header_ok(raw))
        return DDCCI_ERR_PARSE;

    memset(out, 0, sizeof(*out));
    copy = len > DDCCI_EDID_LEN_MAX ? DDCCI_EDID_LEN_MAX : len;
    /* Keep only whole 128-byte blocks so a short tail cannot fail the sum. */
    copy -= copy % 128u;
    if (copy < DDCCI_EDID_LEN_MIN)
        return DDCCI_ERR_PARSE;
    memcpy(out->raw, raw, copy);
    out->len = copy;
    out->checksum_ok = true;
    for (block = 0; block < copy; block += 128u) {
        if (!edid_block_sum_ok(raw + block))
            out->checksum_ok = false;
    }

    mfg = (uint16_t)((raw[8] << 8) | raw[9]);
    out->manufacturer[0] = mfg_letter((mfg >> 10) & 0x1Fu);
    out->manufacturer[1] = mfg_letter((mfg >> 5) & 0x1Fu);
    out->manufacturer[2] = mfg_letter(mfg & 0x1Fu);
    out->manufacturer[3] = '\0';

    out->product_code = (uint16_t)(raw[10] | (raw[11] << 8));
    out->serial_number = (uint32_t)raw[12]
                       | ((uint32_t)raw[13] << 8)
                       | ((uint32_t)raw[14] << 16)
                       | ((uint32_t)raw[15] << 24);
    out->week = raw[16];
    out->year = (uint16_t)(1990u + raw[17]);
    out->version_major = raw[18];
    out->version_minor = raw[19];
    edid_parse_descriptors(raw, out);
    return DDCCI_OK;
}

static char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

static const char *find_tag(const char *s, const char *tag)
{
    size_t n = strlen(tag);
    const char *p;

    for (p = s; *p; p++) {
        size_t i;
        char before;

        for (i = 0; i < n; i++) {
            if (ascii_lower(p[i]) != ascii_lower(tag[i]))
                break;
        }
        if (i != n)
            continue;
        before = (p == s) ? '\0' : p[-1];
        if (!isalnum((unsigned char)before) && p[n] == '(')
            return p + n + 1;
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
    if (out->brightness.opcode == 0) {
        if (ddcci_info_has_vcp(out, DDCCI_VCP_BRIGHTNESS))
            out->brightness.opcode = DDCCI_VCP_BRIGHTNESS;
        else if (ddcci_info_has_vcp(out, DDCCI_VCP_BACKLIGHT))
            out->brightness.opcode = DDCCI_VCP_BACKLIGHT;
        else if (ddcci_info_has_vcp(out, DDCCI_VCP_BACKLIGHT_WHITE))
            out->brightness.opcode = DDCCI_VCP_BACKLIGHT_WHITE;
    }
    out->brightness.from_caps = out->brightness.opcode != 0 &&
        ddcci_info_has_vcp(out, out->brightness.opcode);
    if (out->brightness.from_caps)
        out->brightness.present = true;

    if (out->contrast.opcode == 0 &&
        ddcci_info_has_vcp(out, DDCCI_VCP_CONTRAST))
        out->contrast.opcode = DDCCI_VCP_CONTRAST;
    out->contrast.from_caps = out->contrast.opcode != 0 &&
        ddcci_info_has_vcp(out, out->contrast.opcode);
    if (out->contrast.from_caps)
        out->contrast.present = true;
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
