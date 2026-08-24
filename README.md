# libddcci

# THIS LIBRARY IS FULLY WRITTEN BY GROK BUILD
## I AM NEVER AGAIN TRYING TO WRITE A DDC/CI LIBRARY AS IT WAS PAINFUL LAST TIME I TRIED SO THIS SHOULD BE ENOUGH

A small C library for talking to monitors over **DDC/CI** on Linux I2C
(`/dev/i2c-*`). It can:

1. **Find every connected display** (DRM connectors first, then leftover I2C buses)
2. **Tell you if the panel speaks DDC/CI** (I2C slave `0x37`)
3. **Resolve the VCP addresses for brightness and contrast** when the monitor
   actually implements them

Brightness is almost always MCCS opcode `0x10` (luminance). A few panels only
expose backlight (`0x13`) or backlight-white (`0x6B`). Contrast is `0x12` when
present. The library checks the capabilities string *and* a live GetVCP, because
those two sources disagree on a surprising number of monitors.

## Quick start

```c
#include "ddcci.h"
#include <stdio.h>

int main(void)
{
    ddcci_info *list = NULL;
    size_t n = 0, i;

    if (ddcci_find_displays(&list, &n) != DDCCI_OK)
        return 1;

    for (i = 0; i < n; i++) {
        ddcci_info *d = &list[i];
        printf("%s  %s %s  DDC/CI %s\n",
               d->path,
               d->edid.manufacturer, d->edid.model,
               d->ddc_supported ? "yes" : "no");

        if (d->brightness.present)
            printf("  brightness  VCP 0x%02X  (%u / %u)\n",
                   d->brightness.opcode, d->brightness.current, d->brightness.maximum);
        if (d->contrast.present)
            printf("  contrast    VCP 0x%02X  (%u / %u)\n",
                   d->contrast.opcode, d->contrast.current, d->contrast.maximum);
    }
    ddcci_free_info_list(list);
    return 0;
}
```

Build the example against the static library:

```sh
make
sudo ./build/ddcci-probe
```

Link your own program with `-lddcci` after `make install`, or compile
`src/*.c` directly into your binary.

## Permissions

DDC/CI needs **read/write** on the adapter node:

```sh
sudo usermod -aG i2c "$USER"
# log out and back in
# or, for a one-shot:
sudo ./build/ddcci-probe
```

Without that, EDID can still be read from sysfs (`/sys/class/drm/*/edid`) but
the library cannot probe slave `0x37`.

## How discovery works

1. Walk `/sys/class/drm/card*-*` connectors with `status=connected`.
2. Read EDID from sysfs; follow the `ddc` symlink to the I2C adapter.
3. Open `/dev/i2c-N`, read EDID at **0x50** if needed, then probe DDC/CI at **0x37**.
4. Scan remaining `/sys/class/i2c-dev` nodes (skipping SMBus adapters) for
   anything that answers at 0x50 or 0x37.

Laptop **eDP** panels almost always implement EDID and almost never implement
DDC/CI. Those show up as `ddc_supported = false`.

## DDC/CI packet (host → display)

| Byte | Value        | Meaning                                      |
|------|--------------|----------------------------------------------|
| —    | `0x6E`       | I2C write address (`0x37 << 1`), not in buf  |
| 0    | `0x51`       | Host virtual address                         |
| 1    | `0x80 \| n`  | Length of the following data bytes           |
| 2…   | data         | Command + payload                            |
| last | XOR          | `0x6E` XOR every payload byte                |

GetVCP brightness: `51 82 01 10 AC`  
SetVCP contrast 80: `51 84 03 12 00 50 FA`

Replies are checksummed with initial `0x50` over the whole received frame
(including the checksum byte) and must XOR to `0`.

## Layout

```
include/ddcci.h          public API
src/ddcci.c              open / query / get / set
src/ddcci_parse.c        checksum, packets, EDID, capabilities
src/ddcci_i2c.c          linux i2c-dev (I2C_RDWR + write/read fallback)
src/ddcci_enum.c         DRM + /dev/i2c-* discovery
examples/ddcci-probe.c   CLI
tests/test_ddcci.c       protocol / EDID / caps unit tests
```

`make test` does not need a monitor.

## API surface

| Function | Role |
|----------|------|
| `ddcci_find_displays` | Enumerate every display |
| `ddcci_has_ddc` / `info.ddc_supported` | DDC/CI present? |
| `ddcci_find_brightness` / `ddcci_find_contrast` | Resolve VCP addresses |
| `ddcci_get_vcp` / `ddcci_set_vcp` | Read or write any opcode |
| `ddcci_get_capabilities` | Raw MCCS capabilities string |
| `ddcci_parse_edid` / `ddcci_parse_capabilities` | Pure parsers, no I2C |

Handles are not thread-safe. Distinct `ddcci_display *` values may be used
concurrently.

## License

MIT
