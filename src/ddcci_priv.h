#ifndef DDCCI_PRIV_H
#define DDCCI_PRIV_H

#include "ddcci.h"

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define DDCCI_HOST_ADDR      0x51u  /* virtual host address in the write payload */
#define DDCCI_HOST_XOR       0x50u  /* reply checksum seed (host address, R/W cleared) */
#define DDCCI_DISP_WRITE     0x6Eu  /* 0x37 << 1, included in both checksums */
#define DDCCI_DISP_READ      0x6Fu
#define DDCCI_LENGTH_FLAG    0x80u

#define DDCCI_CMD_GETVCP     0x01u
#define DDCCI_CMD_GETVCP_REP 0x02u
#define DDCCI_CMD_SETVCP     0x03u
#define DDCCI_CMD_CAPS       0xF3u
#define DDCCI_CMD_CAPS_REP   0xE3u

#define DDCCI_RC_NO_ERROR    0x00u
#define DDCCI_RC_UNSUPPORTED 0x01u

/* Unscaled milliseconds. The spec budgets ~40 ms before a Get VCP reply and
 * ~50 ms before the next command after Set VCP. These start lower and move
 * up when a panel answers with the null message. */
#define DDCCI_GET_WAIT_MS     30u
#define DDCCI_CAPS_WAIT_MS    35u
#define DDCCI_WAIT_MIN_MS     15u
#define DDCCI_WAIT_MAX_MS     70u
#define DDCCI_GET_BUDGET_MS   80u
#define DDCCI_CAPS_BUDGET_MS  110u
#define DDCCI_NULL_STEP_MS    10u
#define DDCCI_SET_GAP_MS      50u
#define DDCCI_SAVE_GAP_MS     200u
#define DDCCI_EDID_GAP_MS     20u
#define DDCCI_RETRY_MAX       3
#define DDCCI_READ_SPIN_MAX   8

#define DDCCI_READ_GET        16u   /* Get VCP reply is 11 bytes */
#define DDCCI_READ_CAPS       80u   /* typical fragment is ~38; 80 covers the rest */
#define DDCCI_READ_MAX        132u
#define DDCCI_FRAME_MAX       DDCCI_READ_MAX
#define DDCCI_EDID_CHUNK      16u   /* safe on DP-AUX and SMBus-style adapters */

#define DDCCI_KIND_GET        0
#define DDCCI_KIND_CAPS       1
#define DDCCI_KIND_SET        2
#define DDCCI_KIND_SAVE       3

struct ddcci_display {
    int  fd;
    int  bus;
    char path[64];
    char connector[64];
    char adapter_name[128];

    bool ddc_checked;     /* definitive: a frame arrived, or the slave NACKed */
    bool ddc_ok;

    bool caps_loaded;     /* negative result cached */
    bool caps_retryable;  /* last attempt was a timeout / I/O error */
    char *caps;
    size_t caps_len;

    bool info_loaded;
    bool edid_loaded;
    bool brightness_known;
    bool contrast_known;
    bool brightness_fresh; /* snapshot current matches the last Get/Set */
    bool contrast_fresh;
    ddcci_info info;

    unsigned scale_num;    /* 100 = 1.00x */
    unsigned get_wait_ms;  /* unscaled adaptive write-to-read delay */
    unsigned caps_wait_ms;
    unsigned get_streak;
    unsigned caps_streak;
    long long next_cmd_ms; /* monotonic; 0 means "no wait pending" */
    int use_rdwr;          /* 0 unknown, 1 yes, -1 adapter has no I2C_RDWR */
    unsigned edid_chunk;   /* 0 = default; remembered after a short-read fallback */
};

void ddcci_sleep_ms(unsigned ms);
long long ddcci_mono_ms(void);

uint8_t ddcci_xor(uint8_t init, const uint8_t *p, size_t n);
uint8_t ddcci_write_checksum(const uint8_t *payload, size_t n);
bool    ddcci_read_checksum_ok(const uint8_t *frame, size_t n);

/* Complete checksummed reply inside buf. *flen excludes padding.
 * *is_null is the DDC null message (slave not ready yet). */
bool ddcci_frame_split(const uint8_t *buf, size_t n, size_t *flen, bool *is_null);

size_t ddcci_pack_getvcp(uint8_t opcode, uint8_t out[8]);
size_t ddcci_pack_setvcp(uint8_t opcode, uint16_t value, uint8_t out[8]);
size_t ddcci_pack_caps(uint16_t offset, uint8_t out[8]);

ddcci_status_t ddcci_unpack_getvcp(const uint8_t *frame, size_t n,
                                   uint8_t opcode, ddcci_feature *out);
ddcci_status_t ddcci_unpack_caps(const uint8_t *frame, size_t n,
                                 uint16_t expect_off,
                                 const uint8_t **data, size_t *dlen);
ddcci_status_t ddcci_unpack_caps_any(const uint8_t *frame, size_t n,
                                     const uint8_t **data, size_t *dlen,
                                     uint16_t *got_off);

/* Append a capabilities fragment, stopping at the first NUL.
 * *done is set when that NUL was present. */
ddcci_status_t ddcci_caps_append(char **buf, size_t *len, size_t *cap,
                                 const uint8_t *data, size_t dlen, bool *done);

void ddcci_info_reset(ddcci_info *info);
void ddcci_feature_clear(ddcci_feature *f);
void ddcci_add_vcp(ddcci_info *info, uint8_t opcode);

bool ddcci_ascii_icontains(const char *hay, const char *needle);
unsigned ddcci_default_scale_num(void);
unsigned ddcci_scale_ms(const struct ddcci_display *d, unsigned base_ms);

int ddcci_i2c_open(const char *path, int *err_out);
ddcci_status_t ddcci_transact(struct ddcci_display *d, const uint8_t *w, size_t wn,
                              uint8_t *r, size_t rcap, size_t read_n, size_t *rgot,
                              int kind);
ddcci_status_t ddcci_read_edid_i2c(struct ddcci_display *d, ddcci_edid *out);

int  ddcci_bus_from_devnode(const char *path);
void ddcci_note_identity(struct ddcci_display *d);
/* Match a DRM short or full name. Writes the short name. Returns the bus or -1.
 * The number in "DP-3" is not an I2C bus. When the connector names two
 * adapters, the one that carries this link's DDC/CI is returned. */
int  ddcci_bus_from_connector(const char *drm_connector, char *short_name, size_t short_n);

/* Last "i2c-<number>" in s, or -1. */
int  ddcci_i2c_number_in(const char *s);

/* ddc_bus is the "ddc" symlink. child_bus is an "i2c-*" entry in the
 * connector directory. Either may be -1; names may be NULL.
 * DisplayPort, eDP, and USB-C try the AUX adapter first — on amdgpu the
 * symlink points at the non-AUX hw bus, which does not speak DDC/CI for a
 * native DP link. HDMI, DVI, and VGA try the symlink first.
 * *secondary is -1 when there is no other adapter to try. */
void ddcci_order_connector_buses(const char *connector,
                                 int ddc_bus, const char *ddc_name,
                                 int child_bus, const char *child_name,
                                 int *primary, int *secondary);

ddcci_status_t ddcci_get_vcp_raw(struct ddcci_display *d, uint8_t opcode,
                                 ddcci_feature *out);
ddcci_status_t ddcci_enum_displays(ddcci_info **list, size_t *count);

#endif
