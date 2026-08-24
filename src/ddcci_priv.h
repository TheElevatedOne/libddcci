#ifndef DDCCI_PRIV_H
#define DDCCI_PRIV_H

#include "ddcci.h"

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define DDCCI_HOST_ADDR      0x51u  /* DDC/CI host virtual address (write) */
#define DDCCI_HOST_XOR       0x50u  /* checksum init for replies */
#define DDCCI_DISP_WRITE     0x6Eu  /* 0x37 << 1 */
#define DDCCI_DISP_READ      0x6Fu
#define DDCCI_LENGTH_FLAG    0x80u

#define DDCCI_CMD_GETVCP     0x01u
#define DDCCI_CMD_GETVCP_REP 0x02u
#define DDCCI_CMD_SETVCP     0x03u
#define DDCCI_CMD_CAPS       0xF3u
#define DDCCI_CMD_CAPS_REP   0xE3u

#define DDCCI_RC_NO_ERROR    0x00u
#define DDCCI_RC_UNSUPPORTED 0x01u

#define DDCCI_WAIT_GET_MS    40
#define DDCCI_WAIT_SET_MS    50
#define DDCCI_WAIT_CAPS_MS   50
#define DDCCI_RETRIES        3

#define DDCCI_FRAME_MAX      128

struct ddcci_display {
    int  fd;
    int  bus;
    char path[64];
    char connector[64];
    char adapter_name[128];
    bool ddc_checked;
    bool ddc_ok;
    bool caps_loaded;
    char *caps;
    size_t caps_len;
    bool info_loaded;
    ddcci_info info;
};

void ddcci_sleep_ms(unsigned ms);

uint8_t ddcci_xor(uint8_t init, const uint8_t *p, size_t n);
uint8_t ddcci_write_checksum(const uint8_t *payload, size_t n);
bool    ddcci_read_checksum_ok(const uint8_t *frame, size_t n);

/* Pack host→display payloads (no 0x6E; first byte is 0x51). Returns length. */
size_t ddcci_pack_getvcp(uint8_t opcode, uint8_t out[8]);
size_t ddcci_pack_setvcp(uint8_t opcode, uint16_t value, uint8_t out[8]);
size_t ddcci_pack_caps(uint16_t offset, uint8_t out[8]);

ddcci_status_t ddcci_unpack_getvcp(const uint8_t *frame, size_t n,
                                   uint8_t opcode, ddcci_feature *out);
ddcci_status_t ddcci_unpack_caps(const uint8_t *frame, size_t n,
                                 uint16_t expect_off,
                                 const uint8_t **data, size_t *dlen);

void ddcci_info_reset(ddcci_info *info);
void ddcci_feature_clear(ddcci_feature *f);
void ddcci_add_vcp(ddcci_info *info, uint8_t opcode);

int ddcci_i2c_open(const char *path);
ddcci_status_t ddcci_i2c_write(int fd, uint8_t addr, const uint8_t *buf, size_t n);
ddcci_status_t ddcci_i2c_read(int fd, uint8_t addr, uint8_t *buf, size_t n, size_t *got);
ddcci_status_t ddcci_transact(ddcci_display *d, const uint8_t *w, size_t wn,
                              uint8_t *r, size_t rmax, size_t *rgot,
                              unsigned wait_ms);

ddcci_status_t ddcci_probe_ddc(ddcci_display *d);
ddcci_status_t ddcci_load_caps(ddcci_display *d);
ddcci_status_t ddcci_read_edid_i2c(int fd, ddcci_edid *out);

ddcci_status_t ddcci_enum_displays(ddcci_info **list, size_t *count);

#endif
