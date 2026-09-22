# libddcci

![Static Badge](https://img.shields.io/badge/Vibecoded-Grok-black?style=for-the-badge&labelColor=%236F0E82)

A small C library for **DDC/CI** (VESA MCCS) on Linux. It talks to a monitor's
I2C slave `0x37` through `/dev/i2c-*`: discover displays, test whether DDC/CI
is actually implemented, and read or write VCP features — brightness and
contrast in particular.

DDC/CI is a slow, half-duplex protocol. Most of the time is mandatory waiting,
not CPU work. The library keeps that wait as short as a given panel allows,
and it does not issue a transaction it does not need. Reading one VCP is one
round trip. Dumping the capabilities string is many, and nothing can pipeline
them.

There are **no libraries to link besides libc**. The kernel UAPI headers
provide `linux/i2c.h` and `linux/i2c-dev.h`. `i2c-tools` is optional and is
not used at build time; install it for the `i2c` group and the udev rule that
opens `/dev/i2c-*`.

## What you need installed

Build tools and the kernel userspace headers:

| Distro | Build | Recommended (permissions, not linked) |
| --- | --- | --- |
| Debian, Ubuntu | `sudo apt-get install build-essential linux-libc-dev pkg-config` | `sudo apt-get install i2c-tools` |
| Fedora, RHEL | `sudo dnf install gcc make kernel-headers pkgconf-pkg-config` | `sudo dnf install i2c-tools` |
| Arch | `sudo pacman -S --needed base-devel` | `sudo pacman -S i2c-tools` |

`base-devel` on Arch pulls in `linux-api-headers`. On Debian the header
package is `linux-libc-dev`. On Fedora it is `kernel-headers`.

The device nodes come from the `i2c-dev` module. Load it and make that stick:

```sh
sudo modprobe i2c-dev
echo i2c-dev | sudo tee /etc/modules-load.d/i2c-dev.conf
```

Access is read/write, not read-only. `i2c-tools` ships a udev rule that puts
`/dev/i2c-*` in group `i2c` mode `0660`. After installing it:

```sh
sudo usermod -aG i2c "$USER"
# log out and back in
```

Until that works, run the probe tool as root. Laptop **eDP** panels almost
always expose EDID and almost never implement DDC/CI. External monitors on
HDMI, DisplayPort, and USB-C usually do.

## Build and install

```sh
make            # static lib, shared lib, ddcci-probe, libddcci.pc
make test       # protocol / EDID / capabilities tests; no monitor and no root
make install PREFIX=/usr/local
sudo ldconfig   # if the dynamic linker does not already search /usr/local/lib
```

Useful make variables: `PREFIX`, `DESTDIR`, `LIBDIR`, `CC`, `CFLAGS`, `LDFLAGS`.
`make WERROR=1` turns warnings into errors. `make uninstall` removes what
`install` wrote. The shared library soname is `libddcci.so.1`.

```sh
gcc -o app app.c $(pkg-config --cflags --libs libddcci)
```

`make test` also links a tiny program both statically (`-lddcci`) and against
`libddcci.so`, and checks that internal symbols are not exported.

`ddcci-probe` in the build tree is linked directly from the object files, so
`./build/ddcci-probe` does not need `LD_LIBRARY_PATH`. The installed binary is
the same kind of self-contained tool. Your own programs should use the shared
library via pkg-config.

## Probe tool

```sh
ddcci-probe                              # every connected output
ddcci-probe --bus 7                      # full snapshot of /dev/i2c-7
ddcci-probe --connector DP-1 --get brightness
ddcci-probe --bus 7 --set brightness 40 --save
ddcci-probe --bus 7 --set 0x12 50        # one write, no capabilities read
ddcci-probe --sleep-scale 0.5 --bus 7 --get brightness
```

`--set brightness` tries VCP `0x10`, then `0x13`, then `0x6B`, and writes the
one the panel actually answers. `--set 0xNN` does not probe; it is a single
I2C write. `--save` sends MCCS "Save Current Settings" (`0x0C`). Many panels
forget a Set VCP across power-off until that is sent.

With no `--bus` or `--connector`, a read or write uses the only connected
output that speaks DDC/CI. An internal eDP port is skipped after a quick NACK
instead of a capabilities dump. If two external monitors both answer, pass
`--bus` or `--connector`.

## Library

```c
#include <ddcci.h>
#include <stdio.h>

int main(void)
{
    ddcci_display *d = NULL;
    ddcci_feature bri;

    if (ddcci_open(7, &d) != DDCCI_OK)
        return 1;

    /* One Get VCP when 0x10 works. Capabilities are not read. */
    if (ddcci_find_brightness(d, &bri) == DDCCI_OK) {
        unsigned mid = bri.maximum / 2;
        ddcci_set_vcp(d, bri.opcode, (uint16_t)mid);
        ddcci_save_settings(d);          /* returns after the write */
    }

    ddcci_close(d);
    return 0;
}
```

`ddcci_set_vcp()` returns as soon as the write is acknowledged. The spec's
settle time (~50 ms, ~200 ms after a save) is waited at the start of the
*next* command on that handle, and only for whatever time is still left.

`ddcci_find_brightness()` and `ddcci_find_contrast()` always read a live
value: the first call resolves the opcode, later calls are one Get VCP.
`ddcci_get_vcp()` is the same for an opcode you already know.

`ddcci_query()` and `ddcci_find_displays()` are the slow calls. They read the
capabilities string (one round trip per fragment, often 16–32 bytes of text)
so they can report every VCP code. Capabilities are cached for the life of
the handle. The snapshot's brightness and contrast currents are refreshed
after `ddcci_set_vcp()` touches those opcodes; they are not re-read on every
query. Poll with `ddcci_get_vcp()` or `ddcci_find_brightness()`.

```c
ddcci_info *list = NULL;
size_t n = 0, i;

if (ddcci_find_displays(&list, &n) != DDCCI_OK)
    return 1;
for (i = 0; i < n; i++) {
    printf("%s  %s %s  DDC/CI %s\n",
           list[i].path, list[i].edid.manufacturer, list[i].edid.model,
           list[i].ddc_supported ? "yes" : "no");
}
ddcci_free_info_list(list);
```

A handle is not safe to share across threads. Two handles may be used at the
same time only on different I2C buses. Each transaction takes an exclusive
`flock` on the device node so a write and its reply cannot be interleaved
with another process. Do not poke the same bus from a second handle, or from
another program that does not lock, during a call.

### API

| Function | Role |
| --- | --- |
| `ddcci_find_displays` | Connected outputs, EDID, capabilities, brightness, contrast |
| `ddcci_open` / `ddcci_open_path` / `ddcci_open_connector` | Open one bus. The connector form does not scan other displays |
| `ddcci_close` | Close and free. NULL is safe |
| `ddcci_query` | Cached snapshot; refreshes currents after a set |
| `ddcci_has_ddc` | Slave `0x37` answered a real DDC/CI frame. A timeout is not cached |
| `ddcci_find_brightness` / `ddcci_find_contrast` | Resolve the opcode and read the live value |
| `ddcci_get_vcp` / `ddcci_set_vcp` | Any opcode |
| `ddcci_save_settings` | VCP `0x0C` = 1 |
| `ddcci_get_capabilities` | Capabilities text. Caller `free`s it. A timeout can be retried |
| `ddcci_set_sleep_scale` | Per-handle wait multiplier |
| `ddcci_parse_edid` / `ddcci_parse_capabilities` | Pure parsers, no I2C |
| `ddcci_strerror` | Status text |

`ddcci_feature.from_probe` means Get VCP succeeded. `from_caps` means that
opcode appears in the capabilities string. Panels disagree with themselves;
a feature can be only one of the two. Brightness prefers `0x10` (luminance),
then `0x13` (backlight), then `0x6B` (backlight white). Contrast is `0x12`.

`ddcci_parse_capabilities()` replaces the opcode list and the MCCS version
from the string. It does not clear a value previously read with Get VCP, and
it will not change a feature opcode that is already non-zero.

Status codes are `DDCCI_OK` (0) and negative `DDCCI_ERR_*` values. `DDCCI_ERR_NO_DDC`
means the slave NACKed or never answered a DDC frame. `DDCCI_ERR_UNSUPPORTED`
means the panel replied that this VCP does not exist. `DDCCI_ERR_TIMEOUT` means
the panel kept sending the DDC null message ("not ready") or never produced a
checksummed frame. A null message is not reported as unsupported.

## Why it is still not instant

The VESA spec budgets about 40 ms before a Get VCP reply and about 50 ms
before the next command after a Set VCP. On a 100 kHz DDC bus a short reply
is another 1–2 ms. Capabilities are a multi-part read: the panel chooses the
fragment size, and the host cannot pipeline the next offset.

What the library does about that:

- Get VCP reads 16 bytes, not a 128-byte buffer. At 100 kHz that is the
  difference between ~1 ms and ~12 ms on the wire, and it avoids adapters
  that reject an oversized transfer.
- The write-to-read wait starts at 30 ms (35 ms for capabilities) and moves.
  Two clean replies in a row shave it, down to 15 ms. A null message — the
  panel's "not ready" frame — is re-read after a short extra wait instead of
  being treated as a missing feature. If the panel needed the extra wait, the
  next command starts later so it does not pay the retry again.
- Set VCP and Save do not block the caller for the settle time. The next
  command on that handle waits only the remainder.
- `DDCCI_SLEEP_MULTIPLIER` (or `ddcci_set_sleep_scale()`) scales every wait.
  `1` is the default. `0.5` is worth trying on a panel you know is quick;
  `2` or more is for a panel that corrupts replies. The clamp is 0.1 to 8.
- Brightness and contrast resolution does not read capabilities unless every
  candidate opcode answers "unsupported".
- Discovery reads EDID from sysfs when the DRM connector already has it, and
  it does not open I2C buses that belong to disconnected DRM outputs. SMBus
  adapters are skipped; probing them can disturb motherboard sensors.
- EDID over I2C, when sysfs does not have it, is a repeated-start write of the
  offset plus a read, in 16-byte chunks. A separate write and a separate read
  drops the EEPROM's address pointer on a lot of adapters. The chunk shrinks
  if the adapter refuses 16 bytes, and that size is reused for the rest of
  the block.
- `I2C_TIMEOUT` and `I2C_RETRIES` are not set. Those ioctls change the
  adapter's global timeout inside the kernel, for every process on that bus.

A full capabilities string of a few hundred bytes is commonly a few tenths of
a second and can be longer on a panel that returns 8-byte fragments. That is
the protocol. Use `ddcci_get_vcp()` when you only need one number.

## How a transaction is framed

Host to display, I2C address `0x37` (8-bit write address `0x6E` is not part of
the buffer; it is included in the checksum):

| Byte | Value | Meaning |
| --- | --- | --- |
| 0 | `0x51` | Virtual host address |
| 1 | `0x80 \| n` | Length of the following data bytes |
| 2… | data | Command + payload |
| last | XOR | `0x6E` XOR every byte of the message |

Get VCP brightness: `51 82 01 10 AC`  
Set VCP contrast to 80: `51 84 03 12 00 50 FA`

Replies are checksummed with seed `0x50` over the whole received frame,
including the checksum byte. The XOR is 0 when the frame is intact. The first
payload byte is the source address `0x6E`. A reply of `6E 80 <checksum>` is a
null message: the slave is there, and it is not ready.

I2C map on the DDC link: `0x50` EDID EEPROM, `0x37` DDC/CI.

## Discovery

1. Walk `/sys/class/drm/card*-*` and remember every connector's I2C bus.
2. For `status=connected`, read EDID from sysfs and follow the `ddc` symlink
   (or an `i2c-*` child, on kernels that expose it that way) to `/dev/i2c-N`.
3. Open that node, probe DDC/CI, and read capabilities.
4. Adapters DRM does not reference are scanned too. Skipped without an open:
   SMBus (motherboard sensors), AMD "i2c bit bus OEM" adapters, disconnected
   DRM ports, and the aux `i2c-*` child of a connector whose `ddc` symlink
   already names the bus to use.

`ddcci_open_connector("DP-1")` or `"card0-HDMI-A-1"` resolves the bus from
sysfs and opens it. It does not probe the other connectors. A short name that
matches two cards prefers the connected one.

## Troubleshooting

**No `/dev/i2c-*`.** `sudo modprobe i2c-dev`. The DRM connector can exist in
sysfs without the device node.

**`DDCCI_ERR_IO` on open.** Permissions. Group `i2c`, or root. The header
files being installed does not grant access to the device.

**EDID is fine, DDC/CI is not.** Normal for laptop panels. The library reports
`ddc_supported = false` and still returns the EDID from sysfs.

**Checksum errors or null-message timeouts.** Raise the scale:
`DDCCI_SLEEP_MULTIPLIER=2`, or `ddcci_set_sleep_scale(d, 2.0)`. Some panels
need this only for the capabilities read.

**A panel is quick and discovery feels sluggish.** `DDCCI_SLEEP_MULTIPLIER=0.5`.
If replies start failing, move it back up. The per-handle adaptive wait will
also climb on its own after a null message.

**Brightness write seems to succeed and then the OSD snaps back, or the value
dies at power-off.** Call `ddcci_save_settings()`.

**Two programs fighting over the bus.** This library holds `flock` for each
transaction, including the wait between the write and the read. A program
that issues raw I2C without locking can still land in that window between
commands. Use one handle per bus.

**Enumeration shows DDC unsupported on a monitor that works a moment later.**
A timeout during a scan is reported as unsupported so one stuck panel does
not fail the whole list. Open that bus and call `ddcci_get_vcp()` or
`ddcci_query()` again; a timeout is not sticky on the handle.

## Layout

```
include/ddcci.h           public API
src/ddcci.c               open, query, get, set, capabilities assembly
src/ddcci_parse.c         checksums, packets, EDID, capabilities parser
src/ddcci_i2c.c           I2C_RDWR, flock, adaptive waits, chunked EDID
src/ddcci_enum.c          DRM + leftover /dev/i2c-* discovery
src/libddcci.map          shared-library symbol versions
examples/ddcci-probe.c    command-line tool
tests/test_ddcci.c        protocol, EDID, and capabilities tests
```

## License

MIT. See [LICENSE](LICENSE).
