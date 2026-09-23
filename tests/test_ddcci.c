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
    EXPECT_EQ_U(ddcci_save_settings(NULL), DDCCI_ERR_INVALID_ARG, "save null");
    EXPECT_EQ_U(ddcci_set_vcp(NULL, 0x10, 1), DDCCI_ERR_INVALID_ARG, "set null");
    ddcci_set_sleep_scale(NULL, 0.25);
    ddcci_close(NULL);
    ddcci_free_info_list(NULL);
}

static void test_null_message_and_padding(void)
{
    uint8_t null_frame[8] = {0x6E, 0x80, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t padded[16] = {
        0x6E, 0x88, 0x02, 0x00, 0x10, 0x00, 0x00, 0x64, 0x00, 0x2D, 0xED,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    uint8_t no_flag[11] = {
        0x6E, 0x08, 0x02, 0x00, 0x10, 0x00, 0x00, 0x64, 0x00, 0x2D, 0x00
    };
    ddcci_feature f;
    size_t flen = 0;
    bool is_null = false;

    null_frame[2] = ddcci_xor(DDCCI_HOST_XOR, null_frame, 2);
    EXPECT(ddcci_frame_split(null_frame, sizeof(null_frame), &flen, &is_null),
           "null frame splits");
    EXPECT(is_null, "null flag");
    EXPECT_EQ_U(flen, 3, "null length");
    /* A null message means "not ready", not "this VCP does not exist". */
    EXPECT_EQ_U(ddcci_unpack_getvcp(null_frame, sizeof(null_frame), 0x10, &f),
                DDCCI_ERR_TIMEOUT, "null is not unsupported");
    EXPECT(!f.present, "null feature stays clear");

    flen = 0;
    is_null = true;
    EXPECT(ddcci_frame_split(padded, sizeof(padded), &flen, &is_null), "padded split");
    EXPECT(!is_null, "padded is not null");
    EXPECT_EQ_U(flen, 11, "padded trimmed");
    EXPECT_EQ_U(ddcci_unpack_getvcp(padded, sizeof(padded), 0x10, &f),
                DDCCI_OK, "padded unpack");
    EXPECT_EQ_U(f.current, 45, "padded current");
    EXPECT_EQ_U(f.maximum, 100, "padded max");

    no_flag[10] = ddcci_xor(DDCCI_HOST_XOR, no_flag, 10);
    EXPECT(!ddcci_frame_split(no_flag, sizeof(no_flag), &flen, &is_null),
           "length flag required");
    EXPECT_EQ_U(ddcci_unpack_getvcp(no_flag, sizeof(no_flag), 0x10, &f),
                DDCCI_ERR_PARSE, "missing length flag");
}

static void test_caps_append_stops_at_nul(void)
{
    char *buf = NULL;
    size_t len = 0, cap = 0;
    bool done = false;
    const uint8_t frag[] = {'a', 'b', 'c', 0, 'X', 'Y', 'Z'};

    EXPECT_EQ_U(ddcci_caps_append(&buf, &len, &cap, frag, sizeof(frag), &done),
                DDCCI_OK, "append");
    EXPECT(done, "saw nul");
    EXPECT_EQ_U(len, 3, "stopped before garbage");
    EXPECT(buf && strcmp(buf, "abc") == 0, "abc only");
    free(buf);

    buf = NULL;
    len = 0;
    cap = 0;
    done = true;
    EXPECT_EQ_U(ddcci_caps_append(&buf, &len, &cap, (const uint8_t *)"hello", 5, &done),
                DDCCI_OK, "append hello");
    EXPECT(!done, "no nul yet");
    EXPECT_EQ_U(ddcci_caps_append(&buf, &len, &cap, (const uint8_t *)"!\0", 2, &done),
                DDCCI_OK, "append bang");
    EXPECT(done && buf && strcmp(buf, "hello!") == 0, "joined");
    free(buf);
}

static void test_caps_case_and_probe_fields(void)
{
    ddcci_info info;

    ddcci_info_reset(&info);
    info.brightness.opcode = 0x10;
    info.brightness.current = 40;
    info.brightness.maximum = 100;
    info.brightness.from_probe = true;
    info.brightness.present = true;
    info.mccs_major = 2;
    info.mccs_minor = 4;

    EXPECT_EQ_U(ddcci_parse_capabilities("(vcp(12)mccs_ver(3.0))", &info),
                DDCCI_OK, "parse keeps probe");
    EXPECT_EQ_U(info.brightness.opcode, 0x10, "opcode kept");
    EXPECT_EQ_U(info.brightness.current, 40, "current kept");
    EXPECT_EQ_U(info.brightness.maximum, 100, "max kept");
    EXPECT(info.brightness.from_probe, "from_probe kept");
    EXPECT(!info.brightness.from_caps, "0x10 is not in this string");
    EXPECT(info.contrast.present && info.contrast.opcode == 0x12, "contrast from caps");
    EXPECT(info.contrast.from_caps && !info.contrast.from_probe, "contrast caps only");
    EXPECT_EQ_U(info.mccs_major, 3, "string replaces mccs");
    EXPECT_EQ_U(info.mccs_minor, 0, "mccs minor from string");
    EXPECT(!ddcci_info_has_vcp(&info, 0x10), "list replaced");
    EXPECT(ddcci_info_has_vcp(&info, 0x12), "12 listed");

    ddcci_info_reset(&info);
    info.brightness.opcode = 0x10;
    info.brightness.current = 7;
    info.brightness.from_probe = true;
    info.brightness.present = true;
    EXPECT_EQ_U(ddcci_parse_capabilities("(VCP(10 12)MCCS_VER(2.2))", &info),
                DDCCI_OK, "case insensitive tags");
    EXPECT(info.brightness.from_caps && info.brightness.from_probe, "both sources");
    EXPECT_EQ_U(info.brightness.current, 7, "current still kept");
    EXPECT(info.contrast.present && info.contrast.opcode == 0x12, "contrast 12");
    EXPECT_EQ_U(info.mccs_major, 2, "mccs 2");
    EXPECT_EQ_U(info.mccs_minor, 2, "mccs .2");
}

static void edid_fix_checksum(uint8_t *edid)
{
    unsigned sum = 0;
    int i;

    edid[127] = 0;
    for (i = 0; i < 127; i++)
        sum += edid[i];
    edid[127] = (uint8_t)((256u - (sum & 0xFFu)) & 0xFFu);
}

static void test_edid_text_and_mfg(void)
{
    uint8_t raw[128];
    ddcci_edid e;
    size_t nlen;

    make_edid(raw, "DEL", "U2720Q", "ABCDEF");
    nlen = strlen("U2720Q");
    raw[54 + 5 + nlen] = 0x00;
    raw[54 + 5 + nlen + 1] = 'X';
    raw[54 + 5 + nlen + 2] = 'X';
    edid_fix_checksum(raw);
    EXPECT_EQ_U(ddcci_parse_edid(raw, 128, &e), DDCCI_OK, "nul name parses");
    EXPECT(strcmp(e.model, "U2720Q") == 0, "nul stops the name");

    make_edid(raw, "DEL", "U2720Q", "ABCDEF");
    raw[54 + 3] = 0xFE;
    edid_fix_checksum(raw);
    EXPECT_EQ_U(ddcci_parse_edid(raw, 128, &e), DDCCI_OK, "fe parses");
    EXPECT(strcmp(e.model, "U2720Q") == 0, "0xFE used when no name descriptor");

    make_edid(raw, "DEL", "U2720Q", "ABCDEF");
    raw[54 + 3] = 0xFE;
    raw[90] = raw[91] = raw[92] = raw[94] = 0;
    raw[93] = 0xFC;
    memcpy(raw + 95, "FC-MODEL", 8);
    raw[95 + 8] = 0x0A;
    edid_fix_checksum(raw);
    EXPECT_EQ_U(ddcci_parse_edid(raw, 128, &e), DDCCI_OK, "fc overrides fe");
    EXPECT(strcmp(e.model, "FC-MODEL") == 0, "monitor name wins");

    make_edid(raw, "DEL", "U2720Q", "ABCDEF");
    raw[8] = 0;
    raw[9] = 0;
    edid_fix_checksum(raw);
    EXPECT_EQ_U(ddcci_parse_edid(raw, 128, &e), DDCCI_OK, "bad mfg still parses");
    EXPECT(strcmp(e.manufacturer, "???") == 0, "invalid PNP letters");
}

static void test_bus_is_not_connector_index(void)
{
    int primary = -2, secondary = -2;

    EXPECT_EQ_U(ddcci_i2c_number_in(
                    "../../../../devices/pci0000:00/0000:03:00.0/i2c-3/i2c-8"),
                8, "nested path uses the last i2c-N");
    EXPECT_EQ_U(ddcci_i2c_number_in("../../card0/card0-DP-3/i2c-8"), 8,
                "DP-3 in the path is not the bus");
    EXPECT(ddcci_i2c_number_in("../../card0/card0-DP-3/ddc") < 0,
           "connector index alone is not an i2c number");
    EXPECT(ddcci_i2c_number_in("i2c-dev") < 0, "i2c-dev is not a bus");
    EXPECT_EQ_U(ddcci_i2c_number_in("i2c-8"), 8, "plain adapter name");

    ddcci_order_connector_buses("card0-DP-3",
                                3, "AMDGPU DM i2c hw bus 2",
                                8, "AMDGPU DM aux hw bus 2",
                                &primary, &secondary);
    EXPECT_EQ_U(primary, 8, "native DP tries the aux child first");
    EXPECT_EQ_U(secondary, 3, "hw bus is only the fallback");

    ddcci_order_connector_buses("DP-3", 3, "", 8, "", &primary, &secondary);
    EXPECT_EQ_U(primary, 8, "DP-3 with no adapter names still prefers the child");
    EXPECT_EQ_U(secondary, 3, "unnamed hw bus stays the fallback");

    ddcci_order_connector_buses("card1-eDP-1",
                                4, "AMDGPU DM i2c hw bus 0",
                                7, "AMDGPU DM aux hw bus 0",
                                &primary, &secondary);
    EXPECT_EQ_U(primary, 7, "eDP prefers aux");

    ddcci_order_connector_buses("card0-USB-C-1",
                                2, "AMDGPU DM i2c hw bus 1",
                                9, "AMDGPU DM aux hw bus 1",
                                &primary, &secondary);
    EXPECT_EQ_U(primary, 9, "USB-C prefers aux");

    ddcci_order_connector_buses("card0-HDMI-A-1",
                                5, "AMDGPU DM i2c hw bus 0",
                                -1, NULL,
                                &primary, &secondary);
    EXPECT_EQ_U(primary, 5, "HDMI follows the ddc symlink");
    EXPECT(secondary < 0, "HDMI with one adapter has no fallback");

    ddcci_order_connector_buses("card0-HDMI-A-1",
                                5, "AMDGPU DM i2c hw bus 0",
                                9, "AMDGPU DM aux hw bus 0",
                                &primary, &secondary);
    EXPECT_EQ_U(primary, 5, "HDMI keeps the symlink when a child also exists");
    EXPECT_EQ_U(secondary, 9, "HDMI child is only a fallback");

    ddcci_order_connector_buses("card0-DP-1",
                                8, "AMDGPU DM aux hw bus 1",
                                8, "AMDGPU DM aux hw bus 1",
                                &primary, &secondary);
    EXPECT_EQ_U(primary, 8, "ddc symlink and child are the same adapter");
    EXPECT(secondary < 0, "the same bus is not tried twice");

    ddcci_order_connector_buses("card0-DP-1",
                                6, "AMDGPU DM aux hw bus 0",
                                7, "AMDGPU DM i2c hw bus 0",
                                &primary, &secondary);
    EXPECT_EQ_U(primary, 6, "an aux name on the symlink wins");
    EXPECT_EQ_U(secondary, 7, "the non-aux adapter is the fallback");
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
    test_null_message_and_padding();
    test_caps_append_stops_at_nul();
    test_caps_case_and_probe_fields();
    test_edid_text_and_mfg();
    test_bus_is_not_connector_index();

    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
