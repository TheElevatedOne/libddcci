/*
 * ddcci-probe — list connected displays, DDC/CI support, and the VCP
 * addresses used for brightness and contrast.
 *
 * Usage:
 *   ddcci-probe              # every adapter / DRM connector
 *   ddcci-probe --bus 7      # a single /dev/i2c-7
 *
 * Needs read/write on /dev/i2c-N (root, or membership of the `i2c` group).
 */

#include "ddcci.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        printf("  DDC/CI       unknown (cannot open %s — try root / i2c group)\n",
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

static int probe_bus(int bus)
{
    ddcci_display *d = NULL;
    ddcci_info info;
    ddcci_status_t st;

    st = ddcci_open(bus, &d);
    if (st != DDCCI_OK) {
        fprintf(stderr, "ddcci-probe: open /dev/i2c-%d: %s\n",
                bus, ddcci_strerror(st));
        return 1;
    }
    st = ddcci_query(d, &info);
    if (st != DDCCI_OK) {
        fprintf(stderr, "ddcci-probe: query: %s\n", ddcci_strerror(st));
        ddcci_close(d);
        return 1;
    }
    print_info(0, &info);
    ddcci_close(d);
    return 0;
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                    "usage: ddcci-probe [--bus N]\n"
                    "  list displays, DDC/CI support, brightness/contrast VCP addresses\n");
            return 0;
        }
        if (strcmp(argv[i], "--bus") == 0 && i + 1 < argc) {
            return probe_bus(atoi(argv[++i]));
        }
        fprintf(stderr, "ddcci-probe: unknown argument '%s' (try --help)\n", argv[i]);
        return 2;
    }
    return probe_all();
}
