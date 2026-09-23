/*
 * ddcci-probe — list displays, or read and write VCP features.
 *
 *   ddcci-probe                         every connected output
 *   ddcci-probe --bus 7                 one adapter, full snapshot
 *   ddcci-probe --connector DP-1 --get brightness
 *   ddcci-probe --bus 7 --set brightness 40 --save
 *   ddcci-probe --bus 7 --set 0x10 40
 *
 * Needs read/write on /dev/i2c-N (root, or the i2c group).
 * DDCCI_SLEEP_MULTIPLIER or --sleep-scale adjusts protocol waits.
 */

#define _GNU_SOURCE

#include "ddcci.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_feature(const char *name, const ddcci_feature *f)
{
    if (!f->present) {
        printf("  %-11s  not available\n", name);
        return;
    }
    printf("  %-11s  VCP 0x%02X", name, f->opcode);
    if (f->from_probe)
        printf("  %u / %u", f->current, f->maximum);
    else
        printf("  (listed in capabilities, GetVCP did not confirm)");
    if (f->from_caps && f->from_probe)
        printf("  [caps+probe]");
    else if (f->from_caps)
        printf("  [caps]");
    else if (f->from_probe)
        printf("  [probe]");
    printf("\n");
}

static void print_info(size_t index, const ddcci_info *d)
{
    printf("Display %zu", index);
    if (d->path[0])
        printf("  %s", d->path);
    if (d->connector[0])
        printf("  [%s]", d->connector);
    printf("\n");

    if (d->adapter_name[0])
        printf("  Adapter      %s\n", d->adapter_name);

    if (d->edid_ok) {
        printf("  EDID         %s", d->edid.manufacturer);
        if (d->edid.model[0])
            printf(" %s", d->edid.model);
        if (d->edid.serial[0])
            printf("  s/n %s", d->edid.serial);
        printf("  (EDID %u.%u", d->edid.version_major, d->edid.version_minor);
        if (!d->edid.checksum_ok)
            printf(", checksum bad");
        printf(")\n");
    } else {
        printf("  EDID         not readable\n");
    }

    if (!d->accessible) {
        printf("  DDC/CI       unknown (cannot open %s — load i2c-dev, or use root / the i2c group)\n",
               d->path[0] ? d->path : "/dev/i2c-*");
        return;
    }

    if (!d->ddc_supported) {
        printf("  DDC/CI       not supported (no slave at 0x37)\n");
        return;
    }

    printf("  DDC/CI       supported");
    if (d->mccs_major)
        printf("  MCCS %u.%u", d->mccs_major, d->mccs_minor);
    printf("\n");

    print_feature("Brightness", &d->brightness);
    print_feature("Contrast", &d->contrast);

    if (d->n_vcp) {
        size_t i;

        printf("  VCP list     ");
        for (i = 0; i < d->n_vcp; i++)
            printf("%s%02X", i ? " " : "", d->vcp_opcodes[i]);
        printf("\n");
    }
}

static int probe_all(void)
{
    ddcci_info *list = NULL;
    size_t n = 0, i;
    ddcci_status_t st = ddcci_find_displays(&list, &n);

    if (st != DDCCI_OK) {
        fprintf(stderr, "ddcci-probe: %s\n", ddcci_strerror(st));
        return 1;
    }

    printf("found %zu display%s\n", n, n == 1 ? "" : "s");
    for (i = 0; i < n; i++) {
        printf("\n");
        print_info(i, &list[i]);
    }
    ddcci_free_info_list(list);
    return 0;
}

static int probe_open(ddcci_display *d)
{
    ddcci_info info;
    ddcci_status_t st = ddcci_query(d, &info);

    if (st != DDCCI_OK) {
        fprintf(stderr, "ddcci-probe: query: %s\n", ddcci_strerror(st));
        return 1;
    }
    print_info(0, &info);
    return 0;
}

static void usage(FILE *fp)
{
    fprintf(fp,
            "usage: ddcci-probe [options]\n"
            "\n"
            "  With no read/write options, print every display (or one --bus / --connector).\n"
            "\n"
            "  --bus N                 use /dev/i2c-N\n"
            "  --connector NAME        DRM connector (DP-1, HDMI-A-2, card0-eDP-1, ...)\n"
            "  --get brightness|contrast|0xNN\n"
            "  --set brightness N      resolve 0x10, else 0x13, else 0x6B, then write N\n"
            "  --set contrast N\n"
            "  --set 0xNN N            write an opcode directly (one I2C transaction)\n"
            "  --save                  MCCS Save Current Settings after the writes\n"
            "  --sleep-scale F         wait multiplier for this process (0.1 .. 8, default 1)\n"
            "  -h, --help\n"
            "\n"
            "  Environment: DDCCI_SLEEP_MULTIPLIER (same knob, read when a display is opened).\n");
}

enum {
    FEAT_RAW = 0,
    FEAT_BRIGHTNESS = 1,
    FEAT_CONTRAST = 2
};

static int parse_feature(const char *s, int *named, uint8_t *op)
{
    char *end = NULL;
    unsigned long v;

    if (strcmp(s, "brightness") == 0) {
        *named = FEAT_BRIGHTNESS;
        *op = DDCCI_VCP_BRIGHTNESS;
        return 0;
    }
    if (strcmp(s, "contrast") == 0) {
        *named = FEAT_CONTRAST;
        *op = DDCCI_VCP_CONTRAST;
        return 0;
    }
    if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
        return -1;
    errno = 0;
    v = strtoul(s, &end, 16);
    if (errno || end == s || *end || v > 0xFFul)
        return -1;
    *named = FEAT_RAW;
    *op = (uint8_t)v;
    return 0;
}

static int parse_u16(const char *s, unsigned *out)
{
    char *end = NULL;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, 0);
    if (errno || !s[0] || end == s || *end || v > 65535ul)
        return -1;
    *out = (unsigned)v;
    return 0;
}

static int parse_bus(const char *s, int *bus)
{
    char *end = NULL;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || !s[0] || end == s || *end || v < 0 || v > 4096)
        return -1;
    *bus = (int)v;
    return 0;
}

#define MAX_OPS 8

struct set_op {
    int named;
    uint8_t op;
    unsigned value;
};

struct get_op {
    int named;
    uint8_t op;
};

/* Connected DRM connectors, not I2C bus numbers. "DP-3" is the third
 * DisplayPort on the card; ddcci_open_connector picks the adapter that
 * actually carries DDC/CI (the AUX child on AMD, not the ddc symlink).
 * Several outputs are narrowed with one Get VCP each — eDP NACKs, a DDC
 * panel answers. Returns 0 with *out open, 1 if an error was printed,
 * -1 if DRM has nothing to open, -2 if ambiguous. */
static int connected_connectors(char names[][256], int max, int *overflow)
{
    DIR *dir;
    struct dirent *de;
    int n = 0;

    *overflow = 0;
    dir = opendir("/sys/class/drm");
    if (!dir)
        return 0;

    while ((de = readdir(dir)) != NULL) {
        char path[576], status[32];
        FILE *f;
        size_t r;

        if (de->d_name[0] == '.' || strchr(de->d_name, '-') == NULL)
            continue;
        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", de->d_name);
        f = fopen(path, "r");
        if (!f)
            continue;
        r = fread(status, 1, sizeof(status) - 1, f);
        fclose(f);
        status[r] = '\0';
        if (strncmp(status, "connected", 9) != 0)
            continue;
        if (n == max) {
            *overflow = 1;
            break;
        }
        snprintf(names[n], sizeof(names[n]), "%s", de->d_name);
        n++;
    }
    closedir(dir);
    return n;
}

static int open_sole_drm(ddcci_display **out)
{
    char names[16][256];
    int overflow = 0;
    int n, i, opened = 0, hits = 0, io_fail = 0;
    ddcci_status_t io_st = DDCCI_OK;
    const char *io_name = NULL;
    ddcci_display *hit = NULL;

    n = connected_connectors(names, 16, &overflow);
    if (overflow)
        return -2;
    if (n == 0)
        return -1;
    if (n == 1) {
        ddcci_status_t st = ddcci_open_connector(names[0], out);

        if (st == DDCCI_OK)
            return 0;
        if (st == DDCCI_ERR_NO_DEVICE)
            return -1;
        fprintf(stderr, "ddcci-probe: open %s: %s\n", names[0], ddcci_strerror(st));
        return 1;
    }

    for (i = 0; i < n; i++) {
        ddcci_display *d = NULL;
        ddcci_status_t st = ddcci_open_connector(names[i], &d);

        if (st == DDCCI_ERR_NO_DEVICE)
            continue;
        if (st != DDCCI_OK) {
            if (!io_fail) {
                io_fail = 1;
                io_st = st;
                io_name = names[i];
            }
            continue;
        }
        opened++;
        if (ddcci_has_ddc(d)) {
            hits++;
            if (hit)
                ddcci_close(hit);
            hit = d;
        } else {
            ddcci_close(d);
        }
    }
    if (hits == 1) {
        *out = hit;
        return 0;
    }
    if (hit)
        ddcci_close(hit);
    /* Every open failed (almost always permissions). Report that, not "none". */
    if (opened == 0 && io_fail) {
        fprintf(stderr, "ddcci-probe: open %s: %s\n", io_name, ddcci_strerror(io_st));
        return 1;
    }
    if (opened == 0)
        return -1;
    return -2;
}

static int open_target(int bus, const char *connector, int explicit_target,
                       ddcci_display **out)
{
    ddcci_status_t st;

    if (bus >= 0 && connector) {
        fprintf(stderr, "ddcci-probe: pass only one of --bus and --connector\n");
        return 2;
    }
    if (bus >= 0) {
        st = ddcci_open(bus, out);
        if (st != DDCCI_OK) {
            fprintf(stderr, "ddcci-probe: open /dev/i2c-%d: %s\n",
                    bus, ddcci_strerror(st));
            return 1;
        }
        return 0;
    }
    if (connector) {
        st = ddcci_open_connector(connector, out);
        if (st != DDCCI_OK) {
            fprintf(stderr, "ddcci-probe: open %s: %s\n",
                    connector, ddcci_strerror(st));
            return 1;
        }
        return 0;
    }
    if (explicit_target)
        return 2;

    /* Prefer the connected DRM connector. The library resolves the I2C
     * adapter; the digits in "DP-3" are not a bus number. Fall back to a
     * full scan only when DRM names no bus. */
    {
        int drm = open_sole_drm(out);

        if (drm == 0)
            return 0;
        if (drm == 1)
            return 1;
        if (drm == -2) {
            fprintf(stderr,
                    "ddcci-probe: more than one connected output — pass --bus or --connector\n");
            return 1;
        }
    }

    {
        ddcci_info *list = NULL;
        size_t n = 0, i, hits = 0, which = 0;

        st = ddcci_find_displays(&list, &n);
        if (st != DDCCI_OK) {
            fprintf(stderr, "ddcci-probe: %s\n", ddcci_strerror(st));
            return 1;
        }
        for (i = 0; i < n; i++) {
            if (list[i].ddc_supported && list[i].bus >= 0) {
                which = i;
                hits++;
            }
        }
        if (hits != 1) {
            fprintf(stderr,
                    "ddcci-probe: %s — pass --bus or --connector\n",
                    hits == 0 ? "no DDC/CI display found" : "more than one DDC/CI display");
            for (i = 0; i < n; i++)
                fprintf(stderr, "  %s %s\n",
                        list[i].path[0] ? list[i].path : "(no bus)",
                        list[i].connector);
            ddcci_free_info_list(list);
            return 1;
        }
        bus = list[which].bus;
        ddcci_free_info_list(list);
        st = ddcci_open(bus, out);
        if (st != DDCCI_OK) {
            fprintf(stderr, "ddcci-probe: open /dev/i2c-%d: %s\n",
                    bus, ddcci_strerror(st));
            return 1;
        }
        return 0;
    }
}

static int resolve_named(ddcci_display *d, int named, uint8_t *op)
{
    ddcci_feature f;
    ddcci_status_t st;

    if (named == FEAT_BRIGHTNESS)
        st = ddcci_find_brightness(d, &f);
    else if (named == FEAT_CONTRAST)
        st = ddcci_find_contrast(d, &f);
    else
        return 0;
    if (st != DDCCI_OK) {
        fprintf(stderr, "ddcci-probe: %s: %s\n",
                named == FEAT_BRIGHTNESS ? "brightness" : "contrast",
                ddcci_strerror(st));
        return 1;
    }
    *op = f.opcode;
    return 0;
}

static int do_ops(ddcci_display *d, const struct set_op *sets, int nset,
                  const struct get_op *gets, int nget, int do_save)
{
    int i;

    for (i = 0; i < nset; i++) {
        uint8_t op = sets[i].op;
        ddcci_status_t st;

        if (sets[i].named != FEAT_RAW && resolve_named(d, sets[i].named, &op) != 0)
            return 1;
        st = ddcci_set_vcp(d, op, (uint16_t)sets[i].value);
        if (st != DDCCI_OK) {
            fprintf(stderr, "ddcci-probe: set 0x%02X: %s\n", op, ddcci_strerror(st));
            return 1;
        }
        printf("set 0x%02X = %u\n", op, sets[i].value);
    }

    for (i = 0; i < nget; i++) {
        ddcci_feature f;
        ddcci_status_t st;
        uint8_t op = gets[i].op;
        const char *label = "VCP";

        if (gets[i].named == FEAT_BRIGHTNESS) {
            st = ddcci_find_brightness(d, &f);
            label = "brightness";
        } else if (gets[i].named == FEAT_CONTRAST) {
            st = ddcci_find_contrast(d, &f);
            label = "contrast";
        } else {
            st = ddcci_get_vcp(d, op, &f);
        }
        if (st != DDCCI_OK) {
            fprintf(stderr, "ddcci-probe: get %s: %s\n", label, ddcci_strerror(st));
            return 1;
        }
        printf("%s  VCP 0x%02X  %u / %u\n", label, f.opcode, f.current, f.maximum);
    }

    if (do_save) {
        ddcci_status_t st = ddcci_save_settings(d);

        if (st != DDCCI_OK) {
            fprintf(stderr, "ddcci-probe: save: %s\n", ddcci_strerror(st));
            return 1;
        }
        printf("saved current settings\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    int bus = -1;
    const char *connector = NULL;
    double scale = -1.0;
    int do_save = 0;
    int nset = 0, nget = 0;
    int i;
    struct set_op sets[MAX_OPS];
    struct get_op gets[MAX_OPS];
    int list_only;
    ddcci_display *d = NULL;
    int rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--bus") == 0 && i + 1 < argc) {
            if (parse_bus(argv[++i], &bus) != 0) {
                fprintf(stderr, "ddcci-probe: bad bus '%s'\n", argv[i]);
                return 2;
            }
            continue;
        }
        if (strcmp(argv[i], "--connector") == 0 && i + 1 < argc) {
            connector = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--sleep-scale") == 0 && i + 1 < argc) {
            char *end = NULL;

            errno = 0;
            scale = strtod(argv[++i], &end);
            if (errno || end == argv[i] || *end || scale <= 0.0) {
                fprintf(stderr, "ddcci-probe: bad sleep scale '%s'\n", argv[i]);
                return 2;
            }
            continue;
        }
        if (strcmp(argv[i], "--save") == 0) {
            do_save = 1;
            continue;
        }
        if (strcmp(argv[i], "--get") == 0 && i + 1 < argc) {
            if (nget >= MAX_OPS || parse_feature(argv[++i], &gets[nget].named, &gets[nget].op) != 0) {
                fprintf(stderr, "ddcci-probe: bad --get (brightness, contrast, or 0xNN)\n");
                return 2;
            }
            nget++;
            continue;
        }
        if (strcmp(argv[i], "--set") == 0 && i + 2 < argc) {
            unsigned value;

            if (nset >= MAX_OPS ||
                parse_feature(argv[i + 1], &sets[nset].named, &sets[nset].op) != 0 ||
                parse_u16(argv[i + 2], &value) != 0) {
                fprintf(stderr, "ddcci-probe: bad --set (brightness|contrast|0xNN value)\n");
                return 2;
            }
            sets[nset].value = value;
            nset++;
            i += 2;
            continue;
        }
        fprintf(stderr, "ddcci-probe: unknown argument '%s' (try --help)\n", argv[i]);
        return 2;
    }

    if (scale > 0.0) {
        char buf[32];

        snprintf(buf, sizeof(buf), "%.4f", scale);
        if (setenv("DDCCI_SLEEP_MULTIPLIER", buf, 1) != 0) {
            fprintf(stderr, "ddcci-probe: setenv failed\n");
            return 1;
        }
    }

    list_only = (nset == 0 && nget == 0 && !do_save);
    if (list_only && bus < 0 && !connector)
        return probe_all();

    if (list_only) {
        rc = open_target(bus, connector, 1, &d);
        if (rc != 0)
            return rc;
        if (scale > 0.0)
            ddcci_set_sleep_scale(d, scale);
        rc = probe_open(d);
        ddcci_close(d);
        return rc;
    }

    rc = open_target(bus, connector, 0, &d);
    if (rc != 0)
        return rc;
    if (scale > 0.0)
        ddcci_set_sleep_scale(d, scale);
    rc = do_ops(d, sets, nset, gets, nget, do_save);
    ddcci_close(d);
    return rc;
}
