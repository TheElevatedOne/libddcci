/*
 * libddcci — DDC/CI over I2C (Linux)
 *
 * Discover connected displays, test DDC/CI support, and resolve the
 * VCP opcodes used for brightness and contrast.
 *
 * Typical I2C map on a DDC link:
 *   0x50  EDID EEPROM
 *   0x37  DDC/CI (VESA MCCS) slave
 *
 * Standard MCCS opcodes (always confirm per display — not every panel
 * implements them, and a few use a backlight opcode instead of 0x10):
 *   0x10  Luminance (brightness)
 *   0x12  Contrast
 *   0x13  Backlight control
 *   0x6B  Backlight level: white
 *   0xDF  VCP version
 */

#ifndef DDCCI_H
#define DDCCI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define DDCCI_VERSION_MAJOR 1
#define DDCCI_VERSION_MINOR 0
#define DDCCI_VERSION_PATCH 0
#define DDCCI_VERSION_STRING "1.0.0"

/* 7-bit I2C slave addresses */
#define DDCCI_ADDR_DDC  0x37u
#define DDCCI_ADDR_EDID 0x50u

/* Well-known MCCS VCP opcodes */
#define DDCCI_VCP_BRIGHTNESS       0x10u
#define DDCCI_VCP_CONTRAST         0x12u
#define DDCCI_VCP_BACKLIGHT        0x13u
#define DDCCI_VCP_BACKLIGHT_WHITE  0x6Bu
#define DDCCI_VCP_VERSION          0xDFu

#define DDCCI_EDID_LEN_MIN 128
#define DDCCI_EDID_LEN_MAX 256
#define DDCCI_VCP_MAX      256

typedef enum ddcci_status {
    DDCCI_OK              = 0,
    DDCCI_ERR_INVALID_ARG = -1,
    DDCCI_ERR_NOMEM       = -2,
    DDCCI_ERR_IO          = -3,
    DDCCI_ERR_NO_DEVICE   = -4,
    DDCCI_ERR_NO_DDC      = -5,
    DDCCI_ERR_CHECKSUM    = -6,
    DDCCI_ERR_TIMEOUT     = -7,
    DDCCI_ERR_UNSUPPORTED = -8,
    DDCCI_ERR_PARSE       = -9,
    DDCCI_ERR_BUSY        = -10,
} ddcci_status_t;

typedef struct ddcci_display ddcci_display;

typedef struct ddcci_edid {
    uint8_t  raw[DDCCI_EDID_LEN_MAX];
    size_t   len;
    bool     checksum_ok;
    char     manufacturer[4];  /* 3-letter PNP ID, e.g. "DEL" */
    char     model[14];
    char     serial[14];
    uint16_t product_code;
    uint32_t serial_number;
    uint8_t  week;
    uint16_t year;             /* 1990-based manufacture year decoded */
    uint8_t  version_major;
    uint8_t  version_minor;
} ddcci_edid;

typedef struct ddcci_feature {
    uint8_t  opcode;     /* VCP address (0 if not present) */
    bool     present;    /* capabilities and/or GetVCP say it exists */
    bool     from_caps;  /* listed in the MCCS capabilities string */
    bool     from_probe; /* GetVCP Feature succeeded */
    uint8_t  type;       /* 0 = continuous (set parameter), 1 = momentary */
    uint16_t current;
    uint16_t maximum;
} ddcci_feature;

typedef struct ddcci_info {
    int      bus;                 /* I2C adapter number, -1 if unknown */
    char     path[64];            /* "/dev/i2c-N", empty if unopened */
    char     connector[64];       /* DRM name, e.g. "DP-1", may be empty */
    char     adapter_name[128];
    bool     accessible;          /* /dev/i2c-N could be opened */
    bool     edid_ok;
    bool     ddc_supported;       /* slave 0x37 speaks DDC/CI */
    ddcci_edid edid;
    uint8_t  mccs_major;          /* 0 if unknown */
    uint8_t  mccs_minor;
    ddcci_feature brightness;
    ddcci_feature contrast;
    uint8_t  vcp_opcodes[DDCCI_VCP_MAX];
    size_t   n_vcp;
} ddcci_info;

/* ---------- discovery ---------- */

/*
 * Probe DRM connectors and /dev/i2c-* adapters.  Returns a heap array
 * of displays that have a readable EDID and/or a talking DDC/CI slave.
 * On success *list is never NULL when *count > 0.  Free with
 * ddcci_free_info_list().
 */
ddcci_status_t ddcci_find_displays(ddcci_info **list, size_t *count);
void           ddcci_free_info_list(ddcci_info *list);

bool ddcci_info_has_vcp(const ddcci_info *info, uint8_t opcode);

/* ---------- open / close ---------- */

ddcci_status_t ddcci_open(int bus, ddcci_display **out);
ddcci_status_t ddcci_open_path(const char *dev_path, ddcci_display **out);
ddcci_status_t ddcci_open_connector(const char *drm_connector, ddcci_display **out);
void           ddcci_close(ddcci_display *d);

/* ---------- per-display queries ---------- */

/* Fill *info (and cache capabilities / feature addresses). */
ddcci_status_t ddcci_query(ddcci_display *d, ddcci_info *info);

/* True if I2C 0x37 answered a DDC/CI request. */
bool ddcci_has_ddc(ddcci_display *d);

/*
 * Resolve the VCP address used for brightness / contrast.
 * present=false and opcode=0 if the panel does not expose the feature.
 * Brightness prefers 0x10, then 0x13, then 0x6B.
 * Contrast is 0x12 when available.
 */
ddcci_status_t ddcci_find_brightness(ddcci_display *d, ddcci_feature *out);
ddcci_status_t ddcci_find_contrast(ddcci_display *d, ddcci_feature *out);

ddcci_status_t ddcci_get_vcp(ddcci_display *d, uint8_t opcode, ddcci_feature *out);
ddcci_status_t ddcci_set_vcp(ddcci_display *d, uint8_t opcode, uint16_t value);

/*
 * Fetch the raw ASCII capabilities string (NUL-terminated).
 * Caller frees *ascii with free().
 */
ddcci_status_t ddcci_get_capabilities(ddcci_display *d, char **ascii, size_t *len);

/* ---------- parsers (pure, no I2C; useful in tests) ---------- */

ddcci_status_t ddcci_parse_edid(const uint8_t *raw, size_t len, ddcci_edid *out);

/*
 * Parse a capabilities string into VCP opcodes, MCCS version, and the
 * brightness/contrast from_caps flags.  Leaves probe fields untouched.
 */
ddcci_status_t ddcci_parse_capabilities(const char *caps, ddcci_info *out);

const char *ddcci_strerror(ddcci_status_t st);

#ifdef __cplusplus
}
#endif

#endif /* DDCCI_H */
