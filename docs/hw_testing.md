# Building and testing the firmware on real hardware

## Building

```
tools/build.sh                        # build external/Circle/app/Firmware/kernel8.img
SDCARD=/path/to/mounted/sdcard tools/build.sh   # build + deploy (see "Deploying to the right kernel")
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
  after deploying the new firmware, power off/on the RPi (or the C64, if the
  RAD is powered from the expansion port) to boot into it.

- **On-screen logging as the console**: with UART unavailable, the debug
  channel is `logger->Write()`, tee'd to HDMI via
  [`Source/Firmware/tee_device.h`](../Source/Firmware/tee_device.h) --
  `CTeeDevice` fans every log line out to both `m_Serial` (works on Tier 1,
  a no-op on Tier 2) and `CHDMIConsole` (works on both). **Important:**
  neither plain `CSerialDevice::Write()` nor `CScreenDevice::Write()`
  (Circle's own text console) can be used for this once RAD calls
  `DisableIRQs()`, which happens early in `CRAD::Run()` and stays disabled
  for essentially the rest of the program's life (needed for cycle-precise
  C64 bus timing). With `REALTIME` defined (`Source/Firmware/Circle/sysconfig.h`),
  both of those classes check `CurrentExecutionLevel()` and silently drop
  the write whenever the IRQ mask bit is set -- Circle's DAIF-based check
  can't distinguish "really inside an interrupt handler" from "IRQs are
  just globally masked", so on-screen logging via the normal console
  appears to work at boot and then silently goes dark the moment
  `DisableIRQs()` runs, with no hang and no error. `CHDMIConsole` sidesteps
  this entirely by blitting text directly via `SetPixel()` (using Circle's
  `CCharGenerator` font data), which has no such guard -- same mechanism
  `showTestPattern()` already relies on. It only flushes the cache range for
  the screen row(s) it actually touched (not the whole framebuffer) --
  doing a full-framebuffer flush per call was cheap the first time (boot)
  but expensive enough to blow the per-rasterline timing budget on any
  later call during live operation, which showed up as bus corruption
  (garbled characters) in testing before this was narrowed down.
  Layout: a reserved box top-left (`GPU_OUTPUT_BOX_W` x `GPU_OUTPUT_BOX_H`)
  for the eventual upscaled C64-passthrough image, log text in the column to
  its right using the full screen height.

### Two hardware bring-up gotchas that cost real debugging time

**1. `KERNEL_MAX_SIZE` too small for RAD's real `.bss` -- total silent hang,
zero diagnostic output.** RAD's static buffers (`printOutputFile`, `filesAll`,
`vsf`, `mempool`, `sort`, `previewImage`, etc.) add up to ~98MB, well past
Circle's stock 64MB `KERNEL_MAX_SIZE` default. Circle places the kernel
stack, exception stacks, and MMU translation tables at fixed offsets
computed from `MEM_KERNEL_START + KERNEL_MAX_SIZE` (`memorymap.h`) -- with
the old 64MB cap those landed *inside* RAD's real `.bss` range, so crt0's
`.bss`-zeroing at boot silently corrupted the stack/page tables before a
single instruction of application code (not even the earliest possible
serial log line) could run. Fixed by raising `KERNEL_MAX_SIZE` to 160MB in
[`Source/Firmware/Circle/sysconfig.h`](../Source/Firmware/Circle/sysconfig.h).
This required a full clean rebuild of Circle's base `lib/` (not just the
app) -- the offsets are baked into `libcircle.a` at compile time, and
`tools/build.sh`'s dependency tracking doesn't catch a `sysconfig.h` change
for that base build, so bumping this again would need a manual
`rm -f lib/*.o lib/*.d lib/*.a` + rebuild.

**2. `armstub=rad-prefetch.bin` must be uncommented in `config.txt`.**
[`Source/Firmware/ARM STUB/rad-prefetch.S`](<../Source/Firmware/ARM STUB/rad-prefetch.S>)
is a custom armstub (runs at EL3, before Circle's kernel even loads) that
disables L1 data prefetching, tunes L2 cache read/write latency, and
disables cache-coherency broadcast (`SMPEN`) -- all things that otherwise
introduce unpredictable timing jitter into RAD's cycle-counted bus-hijack
loop (`RESTART_CYCLE_COUNTER`/`WAIT_UP_TO_CYCLE`, raw `PMCCNTR_EL0` reads).
With the stock RPi armstub (i.e. this line commented out, which is how this
particular SD card's `config.txt` was found), the picture and RAD menu are
unstable/garbled even though the hardware itself is fine -- confirmed by
reproducing perfectly stable behavior with the *stock, non-gpu64* firmware
once this line was commented out, and stable behavior returning the moment
it was uncommented again. This looked exactly like a hardware bus-timing
problem (bad solder joint, undervoltage, bus contention) and cost real time
to rule out as one -- check this line first next time.

### Deploying to the right kernel

The Pi's bootloader loads whatever the SD card's `config.txt` names in its
`kernel=` line. RAD's cards set `kernel=kernel_rad.img`, **not** the
`kernel8.img` that Circle's build produces. `tools/build.sh` originally copied
to `kernel8.img`, so every deploy wrote a file the Pi never read and the
hardware silently kept booting the previous firmware.

This cost three consecutive hardware test rounds during milestone 3: a bug fix
and two sets of newly added diagnostics all "produced no output", because none
of them were ever on the machine. Test PRGs on the card *did* update normally
(RAD reads those at launch time), which is what made it look like a firmware
bug rather than a deployment one. The decisive tell was a log containing two
lines that bracket a third, newly added line that never appeared -- no single
build can do that, so the running image had to be older than the source.

Two guards exist now:

- `tools/build.sh` parses `config.txt`'s `kernel=` line, deploys under that
  name, and `cmp`-verifies the copy afterwards.
- Every boot logs `Run: bc0 build <git-describe> src:<digest>` as its first
  line, and `build.sh` prints the same id after deploying. If the two do not
  match, nothing else in the log says anything about the current source.

The build id is a git description plus a digest of the firmware sources,
deliberately not a wall-clock timestamp -- a timestamp would differ on every
invocation and force a rebuild and re-flash even when nothing changed.

### Reading the on-screen log

`CHDMIConsole` (tee_device.h) is a **ring**, not a scrolling console: when it
reaches the bottom of the column it continues from the top, overwriting the
oldest lines. A blank one-row gap is cleared ahead of the write head so the
wrap point is visible -- without it, a full column reads exactly like a frozen
log, which is how it was first reported from hardware.

Keep logging out of hot paths. Each line does glyph rendering via `SetPixel`
plus a cache clean, and anything logged from inside `reuUsingPolling()` runs
with the C64 free-running and no bus access being serviced. The mirror logs
once, on its first snapshot, for exactly this reason.

### Configuring what RAD starts in

`RAD/rad.cfg`'s `STARTUP` line decides what happens at power-on:
`MENU` opens RAD's file browser; `REU128K`/`REU1M`/... boot straight into REU
emulation at that size, with the C64 arriving at READY and gpu64's mirror
already polling, no menu navigation needed. The menu is then reachable only
via the RAD button. There is no startup option that also loads a `.reu` image
file -- the startup path initialises a blank REU.

Note the menu's `T` key cycles a separate `meType` state (REU / GeoRAM /
None) that is independent of the REU size setting and of whether an image is
mounted. Only REU reaches `reuUsingPolling()`, so neither gpu64's trigger nor
its mirror can fire in the other two.

### Open item

**RAD's Mahoney-technique digi music (SID `$D418` writes once per raster
line, see `rad_hijack.cpp`) is silent on real hardware, even though
`music.wav` loads and converts correctly** -- confirmed via the on-screen
log: `readFile()` returns the full 3219278-byte file, the raw bytes start
with the `RIFF` header as expected, and the post-`convertWAV2RAW_inplace()`
samples are real PCM data centered near 0x80, not blank/silent. The
equalizer animates and the C64's audio-out is confirmed connected and
otherwise working (original, non-gpu64 firmware plays music fine on this
same hardware). So the write path has good data -- the open question is
whether the SID register writes are reaching the chip correctly, or
whether `SIDType`/`supportDAC` auto-detection (`detectSID()`,
`rad_hijack.cpp` ~line 2585) is picking the wrong playback branch.

**Update:** `detectSID()` now logs the detected `SIDType`/`hasSIDKick`/
`supportDAC` values on-screen the first time it runs -- not yet run on
hardware to see the actual numbers, that's still the next step. But reading
the surrounding code while adding that log turned up a likely root cause:
`SIDType` (a file-static `u8` in `rad_hijack.cpp`, zero-initialized) is
**never assigned anywhere** in this vendored firmware -- it's only ever
compared against, never set. That means `mahoneyLUT` (assigned from
`SIDType == (6581 & 255) ? lookup6581 : lookup8580` right after the
detection block) always resolves to `lookup8580`, regardless of which chip
is actually on the board. Mahoney's technique needs the chip-correct
nonlinearity table, so if this hardware has a 6581, silently getting the
8580 table would plausibly produce exactly this symptom (SID writes with
good data, audio-out confirmed working, still silent/wrong) -- on top of,
or instead of, the already-understood `SIDType == 0 && !supportDAC`
fallback path being taken whenever no SIDKick is present (that part is
working as designed). Next step once on hardware again: check the new log
line's `SIDType` value (expected: always 0, confirming this), then check
whether RAD's actual upstream/original (non-gpu64) firmware has real 6581
vs. 8580 detection logic that just isn't present in this vendored copy --
if so, port it in; if this vendored copy never had it, that's a
pre-existing upstream gap unrelated to gpu64's changes.

[project_description.md](project_description.md#io-address-space-allocation)
already flags that the Ultimate's (and any real REU's) IO2 decode needs
confirming on hardware -- that's the first thing to check once milestone 2's
trigger PRG exists, using whichever of the two tiers above matches what's
plugged in at the time.
