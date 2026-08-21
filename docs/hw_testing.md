# Building and testing the firmware on real hardware

## Building

```
tools/build.sh                        # build external/Circle/app/Firmware/kernel8.img
SDCARD=/path/to/mounted/sdcard tools/build.sh   # build + copy kernel8.img onto it
```

The script clones [Circle](https://github.com/rsta2/circle) (pinned to `Step44.3`,
the version this Rules.mk/sysconfig.h were written for) into `external/Circle`
on first run, overlays this repo's Circle config onto it, builds the Circle
libraries gpu64 links against, then builds `Source/Firmware`. Re-runs are
incremental (only rebuilds what changed) and normally take well under a
second. `external/` is gitignored -- it's fetched/rebuilt, not checked in.

**Toolchain**: you need a bare-metal `aarch64-none-elf-*` cross compiler,
*not* `aarch64-linux-gnu-*` -- the latter's headers (glibc) collide with
Circle's freestanding ones and produce a wall of "conflicting declaration"
errors. If your distro doesn't package `aarch64-none-elf-gcc`, the
[xPack AArch64 Embedded GCC](https://github.com/xpack-dev-tools/aarch64-none-elf-gcc-xpack/releases)
prebuilt tarball works out of the box; extract it and put its `bin/` on
`PATH`. `tools/build.sh` looks for `${PREFIX64}gcc` (default
`aarch64-none-elf-`) and fails fast with a clear message if it's missing.

### What had to be fixed to get a clean build

The vendored `Source/Firmware` didn't compile as-is against a standards-strict
modern compiler; these are now fixed in the tree (see git history for the
mechanical details):

- `reuUsingPolling()`'s declaration didn't match its definition/call sites
  (missing `int step` parameter).
- A handful of `strstr(...) > 0` pointer/int comparisons (dirscan.cpp,
  rad_iecdevice.cpp) -- always illegal in C++, apparently tolerated by
  whatever compiler this was last built with.
- No `toupper`/`strupr` declarations visible outside helpers.cpp (that file
  already defines its own freestanding versions -- this is a no-libc build,
  there's no `<ctype.h>` to fall back on).
- `rad_iecdevice.o` was missing from the Makefile's `OBJS` entirely.
- FatFs' `FF_FS_RPATH` was off, but `helpers.cpp` calls `f_chdir()` --
  `tools/build.sh` flips it on in the Circle checkout.
- Two missing library dependencies (`lib/usb`, `lib/input` -- pulled in by
  `rad_iecdevice.cpp`'s USB-serial IEC device support) added to the Makefile.
- `rad_iecdevice.cpp` `#include`s `Printer/drv-nl10.h`, the IECBuddy
  dot-matrix-printer emulation driver -- **not present in this repo or in
  upstream frntc/RAD**, and irrelevant to gpu64. It's now gated behind
  `RAD_IEC_PRINTER_SUPPORT` (undefined by default); printing is a no-op, all
  other IECBuddy functionality (file sync, disk swapping) is unaffected. If
  you get hold of that driver, define the macro and add the file back.

None of this touches gpu64-specific behavior -- it's exactly what milestone 1
asked for: get the *existing* RAD image to build.

## Testing on hardware

The friction the progress tracker calls out -- "physically move the SD card
to the RPi" -- has two different answers depending on which stage of
development you're at, because **GPIO14/15 (the only UART pins broken out on
the 40-pin header) are physically wired into the cartridge's bus latches**
(`OE_Dx` = GPIO14, `LATCH_A0` = GPIO15, see
[gpio_defs.h](../Source/Firmware/gpio_defs.h)). That rules out Circle's
built-in serial/USB bootloader (`doc/bootloader.txt`, the "Flashy" tool) for
any build that's actually plugged into a C64 -- it needs those exact two pins
for a USB-serial adapter.

### Tier 1 -- bare RPi on the bench (no C64 needed)

For pure firmware bring-up (does it boot, does the menu render, does REU
emulation work against nothing) you don't need the cartridge at all: a bare
RPi 3A+/Zero 2, HDMI monitor, and a 3.3V USB-serial adapter wired to GPIO14/15
gives you Circle's serial bootloader for real edit-build-flash-see cycles
with **no SD card handling per iteration**:

1. One-time: build Circle's bootloader kernel (`external/Circle/boot`, `make
   all`, follow `doc/bootloader.txt`) onto an SD card that stays in the Pi
   permanently.
2. Each iteration: `tools/build.sh` builds `kernel8.img`, then Circle's flash
   tool pushes it over the serial link and the Pi runs it immediately -- no
   SD access, no physical trip.
3. `CSerialDevice` logger output comes back over the same link, so you get a
   real console, not just on-screen text.

This is the right loop for early gpu64 milestones (2 and 3 in particular)
before bus-sniffing/C64 interaction needs to be verified for real.

### Tier 2 -- seated in a live C64 (GPIO14/15 unavailable)

Once you're testing against the real bus, use:

- **A USB microSD card reader with a built-in cable** (5–6 foot cable is ideal)
  plugged into your PC's USB port permanently, with the female socket dangling
  on your desk. When you want to redeploy, power off the RPi, mount the card
  from the reader on your PC, run `SDCARD=/mount/point tools/build.sh`, then
  power the RPi back on. This is the actual fix for "physically move the card"
  — the card never moves, you just toggle the RPi's power and re-mount from
  your desk. `SDCARD=/mount/point tools/build.sh` makes the whole
  rebuild+redeploy a single command.
  
  (Why a reader with cable instead of an extension on the RPi side? The RPi's
  microSD slot is female, and USB readers are also female, so you can't daisy-chain
  them. A long-cable reader is simpler and avoids the signal-integrity issues that
  plague flimsy ribbon extensions between the two devices.)

- **Power-cycle to reboot**: Circle doesn't support hot-reloading a new kernel;
  after copying the new `kernel8.img`, power off/on the RPi (or the C64, if the
  RAD is powered from the expansion port) to boot into it.

- **On-screen logging as the console**: with UART unavailable, RAD's own
  `CLogger`-to-HDMI-screen output (already wired up in `rad_main.cpp` /
  `c64screen.h`) is the debug channel. Anything you'd normally `printf` to a
  serial console, log there instead.

### Open item

[project_description.md](project_description.md#io-address-space-allocation)
already flags that the Ultimate's (and any real REU's) IO2 decode needs
confirming on hardware -- that's the first thing to check once milestone 2's
trigger PRG exists, using whichever of the two tiers above matches what's
plugged in at the time.
