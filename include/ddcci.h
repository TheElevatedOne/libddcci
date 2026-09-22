/*
 * libddcci — DDC/CI (VESA MCCS) over Linux I2C.
 *
 * Discovers displays, talks to slave 0x37, and reads or writes VCP features
 * such as brightness (usually opcode 0x10) and contrast (0x12). A few panels
 * expose backlight as 0x13 or 0x6B instead of 0x10.
 *
 * The bus is slow by specification: every Get VCP spends tens of milliseconds
 * waiting on the panel, and the capabilities string is many round trips.
 * Get/set of a known opcode is one transaction. Walking capabilities is not.
 * See the README for timing, permissions, and build requirements.
 *
 * A ddcci_display is not safe to share across threads. Two handles may be
 * used concurrently only when they are different I2C buses.
 */

#ifndef DDCCI_H
#define DDCCI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(DDCCI_BUILD) && defined(__GNUC__)
#pragma GCC visibility push(default)
#endif

#define DDCCI_VERSION_MAJOR 1
#define DDCCI_VERSION_MINOR 1
#define DDCCI_VERSION_PATCH 0
#define DDCCI_VERSION_STRING "0.2.0"

/* 7-bit I2C slave addresses */
#define DDCCI_ADDR_DDC 0x37u
#define DDCCI_ADDR_EDID 0x50u

/* Well-known MCCS VCP opcodes */
#define DDCCI_VCP_BRIGHTNESS 0x10u
#define DDCCI_VCP_CONTRAST 0x12u
#define DDCCI_VCP_BACKLIGHT 0x13u
#define DDCCI_VCP_SAVE 0x0Cu /* Save Current Settings */
#define DDCCI_VCP_BACKLIGHT_WHITE 0x6Bu
#define DDCCI_VCP_VERSION 0xDFu

/* ddcci_feature.type */
#define DDCCI_VCP_TYPE_CONTINUOUS 0x00u
#define DDCCI_VCP_TYPE_MOMENTARY 0x01u

#define DDCCI_EDID_LEN_MIN 128
#define DDCCI_EDID_LEN_MAX 256
#define DDCCI_VCP_MAX 256

typedef enum ddcci_status {
  DDCCI_OK = 0,
  DDCCI_ERR_INVALID_ARG = -1,
  DDCCI_ERR_NOMEM = -2,
  DDCCI_ERR_IO = -3,
  DDCCI_ERR_NO_DEVICE = -4,
  DDCCI_ERR_NO_DDC = -5,
  DDCCI_ERR_CHECKSUM = -6,
  DDCCI_ERR_TIMEOUT = -7,
  DDCCI_ERR_UNSUPPORTED = -8,
  DDCCI_ERR_PARSE = -9,
  DDCCI_ERR_BUSY = -10,
} ddcci_status_t;

typedef struct ddcci_display ddcci_display;

typedef struct ddcci_edid {
  uint8_t raw[DDCCI_EDID_LEN_MAX];
  size_t len;
  bool checksum_ok;
  char manufacturer[4]; /* 3-letter PNP ID, e.g. "DEL" */
  char model[14];
  char serial[14];
  uint16_t product_code;
  uint32_t serial_number;
  uint8_t week;
  uint16_t year; /* 1990 + EDID year byte */
  uint8_t version_major;
  uint8_t version_minor;
} ddcci_edid;

typedef struct ddcci_feature {
  uint8_t opcode; /* VCP code, 0 if not present */
  bool present;
  bool from_caps;  /* listed in the capabilities string */
  bool from_probe; /* Get VCP Feature succeeded */
  uint8_t type;    /* DDCCI_VCP_TYPE_CONTINUOUS or MOMENTARY */
  uint16_t current;
  uint16_t maximum;
} ddcci_feature;

typedef struct ddcci_info {
  int bus;            /* I2C adapter number, -1 if unknown */
  char path[64];      /* "/dev/i2c-N", empty if unopened */
  char connector[64]; /* DRM name, e.g. "DP-1", may be empty */
  char adapter_name[128];
  bool accessible; /* /dev/i2c-N could be opened */
  bool edid_ok;
  bool ddc_supported; /* slave 0x37 speaks DDC/CI */
  ddcci_edid edid;
  uint8_t mccs_major; /* 0 if unknown */
  uint8_t mccs_minor;
  ddcci_feature brightness;
  ddcci_feature contrast;
  uint8_t vcp_opcodes[DDCCI_VCP_MAX];
  size_t n_vcp;
} ddcci_info;

/* ---------- discovery ---------- */

/*
 * Probe connected DRM connectors, then I2C adapters that DRM does not own.
 * SMBus adapters and disconnected DRM ports are not probed.
 * On success *list is NULL when *count is 0. Free with ddcci_free_info_list().
 * This reads each panel's capabilities string, so it is the slow call.
 */
ddcci_status_t ddcci_find_displays(ddcci_info **list, size_t *count);
void ddcci_free_info_list(ddcci_info *list);

bool ddcci_info_has_vcp(const ddcci_info *info, uint8_t opcode);

/* ---------- open / close ---------- */

ddcci_status_t ddcci_open(int bus, ddcci_display **out);
ddcci_status_t ddcci_open_path(const char *dev_path, ddcci_display **out);

/* drm_connector is a DRM name ("DP-1", "HDMI-A-1") or a full sysfs name
 * ("card0-DP-1"). Does not probe other displays. */
ddcci_status_t ddcci_open_connector(const char *drm_connector,
                                    ddcci_display **out);
void ddcci_close(ddcci_display *d);

/* ---------- per-display queries ---------- */

/*
 * Snapshot of EDID, capabilities, brightness, and contrast.
 * Capabilities are fetched once per open. Brightness and contrast currents
 * are refreshed if ddcci_set_vcp() changed them after the snapshot.
 * For a live reading of one opcode, use ddcci_get_vcp() or
 * ddcci_find_brightness().
 */
ddcci_status_t ddcci_query(ddcci_display *d, ddcci_info *info);

/* True only if slave 0x37 answered a DDC/CI frame. A bus timeout is not cached.
 */
bool ddcci_has_ddc(ddcci_display *d);

/*
 * Resolve brightness / contrast and read the live value.
 * Brightness tries 0x10, then 0x13, then 0x6B. Contrast is 0x12.
 * The common case is a single Get VCP — capabilities are not read unless
 * every candidate answers "unsupported".
 * Later calls re-read the resolved opcode (one transaction).
 */
ddcci_status_t ddcci_find_brightness(ddcci_display *d, ddcci_feature *out);
ddcci_status_t ddcci_find_contrast(ddcci_display *d, ddcci_feature *out);

ddcci_status_t ddcci_get_vcp(ddcci_display *d, uint8_t opcode,
                             ddcci_feature *out);

/* Write a VCP value. Returns after the I2C write; the mandatory settle
 * delay is applied before the next command on this handle, not here. */
ddcci_status_t ddcci_set_vcp(ddcci_display *d, uint8_t opcode, uint16_t value);

/* MCCS Save Current Settings (opcode 0x0C, value 1). Many panels drop
 * Set VCP changes on power-off until this is sent. */
ddcci_status_t ddcci_save_settings(ddcci_display *d);

/*
 * Scale protocol waits on this handle. 1.0 is the default (already below
 * the 1998 spec ceilings, then trimmed per panel). Useful range is about
 * 0.2 to 4. 0.1 and 8.0 are the clamps. The same knob is the environment
 * variable DDCCI_SLEEP_MULTIPLIER, read when the handle is opened.
 */
void ddcci_set_sleep_scale(ddcci_display *d, double scale);

/*
 * Raw capabilities string, NUL-terminated. Caller frees *ascii with free().
 * A transient failure can be retried; a panel with no capabilities string
 * is remembered for the life of the handle.
 */
ddcci_status_t ddcci_get_capabilities(ddcci_display *d, char **ascii,
                                      size_t *len);

/* ---------- parsers (no I2C) ---------- */

ddcci_status_t ddcci_parse_edid(const uint8_t *raw, size_t len,
                                ddcci_edid *out);

/*
 * Fill VCP opcodes, MCCS version, and brightness/contrast from_caps from a
 * capabilities string. Replaces the opcode list and MCCS version. Does not
 * clear from_probe, current, or maximum. A non-zero feature opcode is kept;
 * from_caps is then true only if that opcode appears in the string.
 */
ddcci_status_t ddcci_parse_capabilities(const char *caps, ddcci_info *out);

const char *ddcci_strerror(ddcci_status_t st);

#if defined(DDCCI_BUILD) && defined(__GNUC__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* DDCCI_H */
