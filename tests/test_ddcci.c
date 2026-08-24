#define _GNU_SOURCE

#include "ddcci.h"
#include "ddcci_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;
static int g_passed;

#define EXPECT(cond, msg)                                                      \
    do {                                                                       \
        if (cond) {                                                            \
            g_passed++;                                                        \
        } else {                                                               \
            g_failed++;                                                        \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);      \
        }                                                                      \
    } while (0)

#define EXPECT_EQ_U(a, b, msg)                                                 \
    do {                                                                       \
        unsigned long _a = (unsigned long)(a);                                 \
        unsigned long _b = (unsigned long)(b);                                 \
        if (_a == _b) {                                                        \
            g_passed++;                                                        \
        } else {                                                               \
            g_failed++;                                                        \
            fprintf(stderr, "FAIL %s:%d  %s  (%lu != %lu)\n",                  \
                    __FILE__, __LINE__, msg, _a, _b);                          \
        }                                                                      \
    } while (0)

static void test_strerror(void)
{
    EXPECT(ddcci_strerror(DDCCI_OK) != NULL, "strerror ok");
    EXPECT(strstr(ddcci_strerror(DDCCI_ERR_NO_DDC), "DDC") != NULL, "no-ddc text");
}

static void test_write_checksum_getvcp(void)
{
    uint8_t pkt[8];
    size_t n = ddcci_pack_getvcp(0x10, pkt);

    /* 51 82 01 10 AC   — XOR with leading 0x6E is 0 */
    EXPECT_EQ_U(n, 5, "getvcp frame len");
    EXPECT_EQ_U(pkt[0], 0x51, "host addr");
    EXPECT_EQ_U(pkt[1], 0x82, "length");
    EXPECT_EQ_U(pkt[2], 0x01, "getvcp opcode");
    EXPECT_EQ_U(pkt[3], 0x10, "vcp");
    EXPECT_EQ_U(pkt[4], 0xAC, "checksum 6E^51^82^01^10");
    EXPECT_EQ_U(ddcci_write_checksum(pkt, 4), 0xAC, "write checksum helper");
}

static void test_write_checksum_setvcp(void)
{
    uint8_t pkt[8];
    size_t n = ddcci_pack_setvcp(0x12, 0x0050, pkt);

    /* 6E 51 84 03 12 00 50 → chk 0xFA */
    EXPECT_EQ_U(n, 7, "setvcp frame len");
    EXPECT_EQ_U(pkt[0], 0x51, "host");
    EXPECT_EQ_U(pkt[1], 0x84, "length 4");
    EXPECT_EQ_U(pkt[2], 0x03, "setvcp");
    EXPECT_EQ_U(pkt[3], 0x12, "contrast");
    EXPECT_EQ_U(pkt[4], 0x00, "value hi");
    EXPECT_EQ_U(pkt[5], 0x50, "value lo");
    EXPECT_EQ_U(pkt[6], 0xFA, "checksum");
}

static void test_pack_caps(void)
{
    uint8_t pkt[8];
    size_t n = ddcci_pack_caps(0x0020, pkt);
    uint8_t expect_chk = (uint8_t)(0x6E ^ 0x51 ^ 0x83 ^ 0xF3 ^ 0x00 ^ 0x20);

    EXPECT_EQ_U(n, 6, "caps frame len");
    EXPECT_EQ_U(pkt[2], 0xF3, "caps opcode");
    EXPECT_EQ_U(pkt[3], 0x00, "off hi");
    EXPECT_EQ_U(pkt[4], 0x20, "off lo");
    EXPECT_EQ_U(pkt[5], expect_chk, "caps checksum");
}

static void test_unpack_getvcp(void)
{
    /* Brightness 45/100, type continuous.
     * frame: 6E 88 02 00 10 00 00 64 00 2D ED */
    uint8_t frame[] = {
        0x6E, 0x88, 0x02, 0x00, 0x10, 0x00, 0x00, 0x64, 0x00, 0x2D, 0xED
    };
    ddcci_feature f;
    ddcci_status_t st;

    EXPECT(ddcci_read_checksum_ok(frame, sizeof(frame)), "reply checksum");
    st = ddcci_unpack_getvcp(frame, sizeof(frame), 0x10, &f);
    EXPECT_EQ_U(st, DDCCI_OK, "unpack status");
    EXPECT(f.present && f.from_probe, "present+probe");
    EXPECT_EQ_U(f.opcode, 0x10, "opcode");
    EXPECT_EQ_U(f.current, 45, "current");
    EXPECT_EQ_U(f.maximum, 100, "maximum");
    EXPECT_EQ_U(f.type, 0, "continuous");
}

static void test_unpack_unsupported(void)
{
    /* Result code 0x01 — VCP not supported. Checksum over 0x50. */
    uint8_t frame[11] = {
        0x6E, 0x88, 0x02, 0x01, 0x99, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t cs = ddcci_xor(0x50, frame, 10);
    ddcci_feature f;
    ddcci_status_t st;

    frame[10] = cs;
    EXPECT(ddcci_read_checksum_ok(frame, 11), "synth checksum");
    st = ddcci_unpack_getvcp(frame, 11, 0x99, &f);
    EXPECT_EQ_U(st, DDCCI_ERR_UNSUPPORTED, "unsupported vcp");
}

static void test_unpack_bad_checksum(void)
{
    uint8_t frame[] = {
        0x6E, 0x88, 0x02, 0x00, 0x10, 0x00, 0x00, 0x64, 0x00, 0x2D, 0x00
    };
    ddcci_feature f;
    ddcci_status_t st = ddcci_unpack_getvcp(frame, sizeof(frame), 0x10, &f);
    EXPECT_EQ_U(st, DDCCI_ERR_CHECKSUM, "bad checksum");
}

static void test_unpack_caps(void)
{
    /* Reply: 6E | len | E3 | offH offL | "vcp" | chk
     * payload = E3 00 00 'v' 'c' 'p'  → 6 bytes, length = 0x86 */
    uint8_t frame[10];
    const uint8_t *data = NULL;
    size_t dlen = 0;
    ddcci_status_t st;

    frame[0] = 0x6E;
    frame[1] = 0x86;
    frame[2] = 0xE3;
    frame[3] = 0x00;
    frame[4] = 0x00;
    frame[5] = 'v';
    frame[6] = 'c';
    frame[7] = 'p';
    frame[8] = ddcci_xor(0x50, frame, 8);

    st = ddcci_unpack_caps(frame, 9, 0, &data, &dlen);
    EXPECT_EQ_U(st, DDCCI_OK, "caps unpack");
    EXPECT_EQ_U(dlen, 3, "caps data len");
    EXPECT(data && memcmp(data, "vcp", 3) == 0, "caps data");
}

static void test_caps_parse_typical(void)
{
    const char *caps =
        "(prot(monitor)type(LCD)model(U2720Q)cmds(01 02 03 07 0C E3 F3)"
        "vcp(02 04 05 08 10 12 14(05 08 0B 0C) 16 18 1A 52 60(0F 11 12) "
        "AC AE B6 C0 C6 C8 C9 D6(01 04 05) DC DF)"
        "mswhql(1)mccs_ver(2.1))";
    ddcci_info info;

    ddcci_info_reset(&info);
    EXPECT_EQ_U(ddcci_parse_capabilities(caps, &info), DDCCI_OK, "parse caps");
    EXPECT_EQ_U(info.mccs_major, 2, "mccs major");
    EXPECT_EQ_U(info.mccs_minor, 1, "mccs minor");
    EXPECT(ddcci_info_has_vcp(&info, 0x10), "has brightness 10");
    EXPECT(ddcci_info_has_vcp(&info, 0x12), "has contrast 12");
    EXPECT(ddcci_info_has_vcp(&info, 0x14), "has 14");
    EXPECT(ddcci_info_has_vcp(&info, 0x05), "05 is a top-level opcode");
    /* 05 appears both as a top-level VCP and nested under 14. That's fine.
     * Nested 0B and 0C must NOT be added as VCP opcodes. */
    EXPECT(!ddcci_info_has_vcp(&info, 0x0B), "nested 0B is not a VCP opcode");
    EXPECT(!ddcci_info_has_vcp(&info, 0x0C), "nested 0C is not a VCP opcode");
    EXPECT(!ddcci_info_has_vcp(&info, 0x0F), "nested 0F under 60 is not an opcode");
    EXPECT(ddcci_info_has_vcp(&info, 0x60), "has 60");
    EXPECT(ddcci_info_has_vcp(&info, 0xDF), "has DF");
    EXPECT(info.brightness.present && info.brightness.opcode == 0x10, "bri 10");
    EXPECT(info.brightness.from_caps, "bri from caps");
    EXPECT(info.contrast.present && info.contrast.opcode == 0x12, "con 12");
}

static void test_caps_parse_backlight_fallback(void)
{
    const char *caps = "(vcp(02 13 16 DF)mccs_ver(2.2))";
    ddcci_info info;

    ddcci_info_reset(&info);
    EXPECT_EQ_U(ddcci_parse_capabilities(caps, &info), DDCCI_OK, "parse");
    EXPECT(!ddcci_info_has_vcp(&info, 0x10), "no 10");
    EXPECT(info.brightness.present && info.brightness.opcode == 0x13,
           "brightness falls back to 0x13");
    EXPECT(!info.contrast.present, "no contrast");
    EXPECT_EQ_U(info.mccs_major, 2, "mccs 2");
    EXPECT_EQ_U(info.mccs_minor, 2, "mccs .2");
}

static void test_caps_parse_white_backlight(void)
{
    const char *caps = "(prot(monitor)vcp(6B)mccs_ver(2.0))";
    ddcci_info info;

    ddcci_info_reset(&info);
    EXPECT_EQ_U(ddcci_parse_capabilities(caps, &info), DDCCI_OK, "parse");
    EXPECT(info.brightness.present && info.brightness.opcode == 0x6B,
           "brightness falls back to 0x6B");
}

static void test_caps_parse_empty(void)
{
    ddcci_info info;
    ddcci_info_reset(&info);
    EXPECT_EQ_U(ddcci_parse_capabilities("(prot(monitor))", &info), DDCCI_OK, "empty");
    EXPECT_EQ_U(info.n_vcp, 0, "no vcps");
    EXPECT(!info.brightness.present && !info.contrast.present, "no features");
}

static void make_edid(uint8_t *edid, const char *mfg, const char *model,
                      const char *serial)
{
    uint16_t m;
    unsigned sum = 0;
    int i;

    memset(edid, 0, 128);
    edid[0] = 0x00;
    edid[1] = edid[2] = edid[3] = edid[4] = edid[5] = edid[6] = 0xFF;
    edid[7] = 0x00;

    m = (uint16_t)(((mfg[0] - 'A' + 1) << 10)
                   | ((mfg[1] - 'A' + 1) << 5)
                   | (mfg[2] - 'A' + 1));
    edid[8] = (uint8_t)(m >> 8);
    edid[9] = (uint8_t)(m & 0xFF);
    edid[10] = 0x34;
    edid[11] = 0x12; /* product 0x1234 */
    edid[12] = 0x78;
    edid[13] = 0x56;
    edid[14] = 0x34;
    edid[15] = 0x12; /* serial 0x12345678 */
    edid[16] = 12;
    edid[17] = 34; /* year 2024 */
    edid[18] = 1;
    edid[19] = 4;

    edid[54 + 3] = 0xFC;
    memcpy(edid + 54 + 5, model, strlen(model));
    edid[54 + 5 + strlen(model)] = 0x0A;

    edid[72 + 3] = 0xFF;
    memcpy(edid + 72 + 5, serial, strlen(serial));
    edid[72 + 5 + strlen(serial)] = 0x0A;

    for (i = 0; i < 127; i++)
        sum += edid[i];
    edid[127] = (uint8_t)((256 - (sum & 0xFF)) & 0xFF);
}

static void test_edid_parse(void)
{
    uint8_t raw[128];
    ddcci_edid e;
    ddcci_status_t st;

    make_edid(raw, "DEL", "U2720Q", "ABCDEF");
    st = ddcci_parse_edid(raw, 128, &e);
    EXPECT_EQ_U(st, DDCCI_OK, "edid parse");
    EXPECT(e.checksum_ok, "checksum");
    EXPECT(strcmp(e.manufacturer, "DEL") == 0, "mfg DEL");
    EXPECT(strcmp(e.model, "U2720Q") == 0, "model");
    EXPECT(strcmp(e.serial, "ABCDEF") == 0, "serial text");
    EXPECT_EQ_U(e.product_code, 0x1234, "product");
    EXPECT_EQ_U(e.serial_number, 0x12345678u, "serial num");
    EXPECT_EQ_U(e.week, 12, "week");
    EXPECT_EQ_U(e.year, 2024, "year");
    EXPECT_EQ_U(e.version_major, 1, "edid major");
    EXPECT_EQ_U(e.version_minor, 4, "edid minor");
}

static void test_edid_bad_header(void)
{
    uint8_t raw[128];
    ddcci_edid e;

    memset(raw, 0, sizeof(raw));
    EXPECT_EQ_U(ddcci_parse_edid(raw, 128, &e), DDCCI_ERR_PARSE, "bad header");
}

static void test_edid_short(void)
{
    uint8_t raw[16];
    ddcci_edid e;
    memset(raw, 0, sizeof(raw));
    EXPECT_EQ_U(ddcci_parse_edid(raw, 16, &e), DDCCI_ERR_PARSE, "too short");
}

static void test_null_args(void)
{
    EXPECT_EQ_U(ddcci_parse_edid(NULL, 128, NULL), DDCCI_ERR_INVALID_ARG, "edid null");
    EXPECT_EQ_U(ddcci_parse_capabilities(NULL, NULL), DDCCI_ERR_INVALID_ARG, "caps null");
    EXPECT_EQ_U(ddcci_find_displays(NULL, NULL), DDCCI_ERR_INVALID_ARG, "find null");
    EXPECT_EQ_U(ddcci_open(-1, NULL), DDCCI_ERR_INVALID_ARG, "open null");
}

int main(void)
{
    test_strerror();
    test_write_checksum_getvcp();
    test_write_checksum_setvcp();
    test_pack_caps();
    test_unpack_getvcp();
    test_unpack_unsupported();
    test_unpack_bad_checksum();
    test_unpack_caps();
    test_caps_parse_typical();
    test_caps_parse_backlight_fallback();
    test_caps_parse_white_backlight();
    test_caps_parse_empty();
    test_edid_parse();
    test_edid_bad_header();
    test_edid_short();
    test_null_args();

    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
