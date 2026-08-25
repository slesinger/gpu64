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

## Testing the 3D renderer without hardware

Most of milestone 6 does not need the Pi. The maths, the rasteriser, the
colormap builder and the mesh parsers depend on nothing but
`<circle/types.h>`, and `tools/hostsim` compiles those firmware sources
unchanged with a native g++ against a stubbed header:

```
make -C tools/hostsim run          # writes tools/hostsim/out/*.ppm
```

It renders a turning box, a z-buffer interpenetration case, a near-plane
straddle, the flat-colour path, and `out/prgpreview.ppm` — the exact scene
`Source/TestPRG/gpu64_3d_cube.a` asks for, from the same code the firmware
runs. **That file is the expected result of the hardware test.** Hold the HDMI
output against it: any difference is the firmware's plumbing — the blob pull,
the palette, the page flip, the cache clean — and not the pipeline.

Every per-image checksum is printed, so a change that was meant to be
cosmetic and was not is visible without opening a viewer.

Do this before a bench trip, not after. The first run of the sim found four
transposed signs in the Euler matrix, a bug that presents as a perspective
artefact rather than as a maths error and would have cost a whole session to
chase on hardware.

## Testing the class 2 raster core without hardware

The same trick, differently aimed. `Source/Firmware/gpu64_raster_core.cpp`
is the whole of class 2's pixel work and depends on nothing but
`<circle/types.h>`, so `tools/rastercheck` compiles it natively and renders
randomised scenarios through it -- and through the independent reference
model in `tools/prgsim/gpu64model.py`, which was written from
[api_design.md](api_design.md) rather than from the firmware. Then it
compares all 64000 pixels and the batch counters.

```
make -C tools/rastercheck run            # 400 scenarios
python3 tools/rastercheck/check.py -n 5000 --seed 3
```

A disagreement is a finding either way round: either the firmware does not
do what the document says, or the document does not say what the firmware
does. Both are worth knowing, and both are far cheaper to find here.

The scenarios deliberately concentrate on the edges a PRG can only sample a
few of -- columns starting above the view (so `v` must be advanced across
the clipped rows), negative `dv`, `u` past the texture width, spans naming a
non-power-of-two texture, sprites clipped by `clipY0`/`clipY1`, masking and
lighting. `DRAW_WALLS` scenarios add a random camera and segments biased to
straddle the near plane, plus an illegal camera one time in ten. Do this
before a bench trip: the first run found `DRAW_SPRITE` not counting itself
in the stats block at all.

Wall scenarios matter here more than anywhere else in class 2, because
`DRAW_WALLS` is the only opcode whose output is arithmetic rather than a
copy. Two people writing a perspective projection from the same paragraph
will disagree about a rounding or a sign, and the disagreement will be one
wrong pixel on one column at one angle. That is not findable at a bench, and
it is findable here in a second -- which is why the model's divisions go
through an `idiv()` that truncates toward zero the way C does, rather than
Python's flooring `//`.

## The demonstration programs

`Source/Demos/gpu64_demo_*.a` is nine developer-facing programs -- `hello`,
`palette`, `sprites`, `bounce`, `rotate`, `matrix`, `raycast`, `walls` --
indexed in
[demos.md](demos.md). They assert nothing and reach no verdict; they are what
a developer copies from, and on the bench they are a fast visual smoke test
of the whole API surface. `hello` is the go/no-go: if it draws, the cartridge
is wired up and the menu is in REU mode.

```
tools/demos.sh              # assemble, run on the PC in both display cases
                            # and render Source/Demos/out/<name>.ppm
```

Those PPMs are the expected HDMI output. Compare against them before
concluding that hardware is wrong -- the same argument as `tools/hostsim` and
the suite below, and it paid the same way: the first six passed on hardware
in a single round. `raycast` has been on the bench and blinks -- see the
progress tracker, section 8b. `walls` is newer still and has never run on
hardware.

**Run a demo long before believing it.** `runsim.py` reports the STOP key
pressed after four polls, and a demo polls it once a frame, so the default
run is about five frames. That is not a check:

```
python3 tools/prgsim/runsim.py Source/Demos/gpu64_demo_walls.prg \
        --demo --stop-after=400 --frame-log --max=2000000000
```

`--frame-log` prints one line per page flip -- the frame number, the drawn
page's non-zero pixel count and whatever the demo has written on the C64's
status rows -- and `--ppm-frame=N:PATH` writes the page being flipped away at
frame N. This is what a demo that misbehaves only after a while looks like on
a PC, and `vice-validation-run-length` is the note about why four frames was
never going to find one.

Deploy by copying the `.prg` files into `RAD_PRG/` on the card. RAD reads
those at launch time, so unlike the kernel image they never go stale.

## The API conformance suite

`Source/TestPRG/gpu64_test_*.a` is a ten-program suite that checks class 0
and class 2 against [api_design.md](api_design.md) and prints its own
verdict. It exists
so that a bench session starts from a known-good baseline instead of from a
demo that looks about right.

| Program | Covers |
|---|---|
| `gpu64_test_system` | `NOP`, `GET_INFO` field by field, flag-byte validation, and **every error code class 0 can produce** |
| `gpu64_test_draw` | `CLEAR`, `SET_PIXEL`, `LINE`, `RECT`, `RECT_FILL` — verified by reading the framebuffer back, plus clipping at both corners |
| `gpu64_test_blit` | `BLIT`, `BLIT_KEYED`, `READ_RECT`, the blob descriptor rules, an REU-space source, and a 16000-byte round trip compared byte for byte |
| `gpu64_test_math` | `$80`–`$86`, 8.8 fixed point: products, rounding on both signs, saturation, inverse, and `SINGULAR` leaving the destination untouched |
| `gpu64_test_float` | `$90`–`$96`, IEEE 754: the same shapes, as the control for the fixed-point rounding cases |
| `gpu64_test_pages` | `SET_DRAW_PAGE`, both forms of `PAGE_FLIP`, `VBLANK_ARM`/`ACK`/`SYNC` — and it adapts to a display with no frame clock |
| `gpu64_test_raster` | Class 2 end to end: the view rectangle, texture upload rules, both batch kinds read back pixel by pixel, clipping with `v` advanced across the dropped rows, rejected records against `RASTER_STATS`, the batch checksum passing and failing, `DRAW_SPRITE` scaling and clipping, and the colormap |
| `gpu64_test_walls` | `SET_CAMERA`'s three refusals, and `DRAW_WALLS` against geometry worked out by hand: a flat-on wall at a known distance, each of its four edges asserted from both sides, a back face and a wall behind the camera counted as rejections, and `CAM_PAINT` filling the ceiling and floor of the columns a wall covers and no others |
| `gpu64_test_sectors` | `SET_SECTORS`' three refusals, and `DRAW_SECTORS` against geometry worked out by hand: a one-sided wall's four edges from both sides, a portal's upper band, window and lower band at their exact rows, and four assertions on the **per-pixel** depth buffer -- a far wall visible through the window, losing to the near wall's lower band in the rows they share, at its own width, and unchanged when the two records are sent in the other order. Those four are the ones a per-column depth buffer would fail |
| `gpu64_test_things` | `DRAW_THINGS` against the same hand-worked geometry: a billboard's four screen edges at a known distance, the five malformed records that are rejected and the well-formed one that clips away and is not, a thing behind a wall and a thing in front of one, two things in either order, `THING_NODEPTH`, and the four quadrants of a 2x2 texture plus `FLIPX` and the mask key. The wall assertions are the ones that matter: they only pass if `DRAW_SECTORS` and `DRAW_THINGS` really share one depth buffer |

Each prints one line per assertion — a name and either `OK` or `F<hex>` — and
a verdict line at the bottom. **The fail code is the byte that was wrong**,
almost always the `ERRCODE` that came back, so a red line is usually readable
without opening the source. The codes that are not an `ERRCODE` are `$E1` (two
buffers differed), `$E2` (a region was not uniformly filled), `$E3` (a byte
was wrong and happened to be zero), `$E4` (a value was in range when it should
not have been, or a timed wait expired) and `$FE` — **a command returned OK
where an error was demanded**, which is the important one: an error path that
never fires is not a tested error path.

Results stay on screen until RUN/STOP, which also clears it so BASIC's
`READY.` cannot land on a result line.

### Desk-check before the bench

```
tools/testprg.sh                # assemble all ten and desk-check them
tools/testprg.sh -v system      # ...and print the screen it produced
```

`tools/prgsim/` is a 6502 core plus a reference model of the class 0 API
written from `api_design.md`. `testprg.sh` runs each program against it and
requires `VERDICT PASS`, in both display modes — with a frame clock and
without.

The direction matters. The model is built from the same document the suite
asserts against, so **a failure on the PC means the TEST is wrong** and a
failure on hardware means the **firmware** is. Getting the first kind out of
the way costs a second here and a session at the bench, which is the same
argument as `tools/hostsim` above — and it paid the same way: the first run
caught two transposed row/column offsets in the blit clipping expectations,
which on hardware would have looked exactly like a clipping defect in the
firmware.

The model is not a second implementation to be trusted over the firmware. It
is a second opinion. Where the two disagree, one of them is wrong and the
disagreement is the finding.

### Hardware results (2026-08-24)

The suite has been run on real hardware. 174 assertions, and every failure it
found was a real defect:

| Program | Result |
|---|---|
| `gpu64_test_system` | 39 / 39 |
| `gpu64_test_draw` | 36 / 36 |
| `gpu64_test_float` | 26 / 26 |
| `gpu64_test_pages` | 24 / 24 |
| `gpu64_test_math` | 30 / 30 (3 red before the `fixWrite()` fix) |
| `gpu64_test_blit` | 22 / 24 — `BIG EXACT` and `REU EXACT`, both open |

`gpu64_test_math` is the case worth remembering. `runsim.py
--firmware-rounding` predicted, on a PC, that exactly `ROUND NEG`,
`MUL MINUS ONE` and `SCALE NEG` would go red and nothing else. Hardware
produced exactly those three. The cause was `fixWrite()` biasing the
accumulator by ±128 and then arithmetic-shifting right by 8: the shift
floors, so on a negative accumulator the bias landed twice and every
negative 8.8 product came back one LSB further from zero than it should
(`-1.0 * 1.0` returned `$FEFF`, −1.00391, instead of `$FF00`). Positive
products were unaffected, which is why it survived a hardware-verified
milestone — a one-LSB error on negatives only is invisible in a rendered
frame. Fixed by biasing and dividing toward zero rather than shifting;
confirmed green on hardware.

**Two failures are still open**, both in the bulk-transfer path and both
reporting `$E1`:

- `BIG EXACT` — 8000 bytes to the framebuffer and back. First mismatch at
  offset `$1CE1` (7393), expected `$06`, got `$5C`.
- `REU EXACT` — a tile stashed into REU space through the REU's own
  controller and blitted from `space = 1`. First mismatch at offset 0,
  expected `$10`, got `$A9`.

Both commands returned OK, so the dispatches were accepted and the data did
not survive. This is the path CLAUDE.md flags as carrying both known REU
defects. `kitCmp` now also reports the total number of differing bytes,
which is what separates a single dropped byte (this project has a known drop
floor) from a stream that shifted.

**Anything else red is a new finding.** And a bench run showing a single
otherwise inexplicable glitch should be weighed against the known dropped
register write — roughly 1 transfer in 35000, always at the start of a
transfer — before it is attributed to anything here.

### Reading a bulk-transfer failure

`BIG` and `REU` each report three numbers beyond the verdict:

```
xxx DIFF AT    offset of the first mismatching byte
xxx DIFF N     how many bytes differ in total
xxx WANT/GOT   the expected byte and the actual one
```

`DIFF N` is the one that decides what happened. `0001` is a single corrupted
byte. A count that runs to the end of the buffer is a shifted stream, which
means the transfer lost or gained a byte rather than corrupting one — a
different bug with a different fix. Without the count, both report `$E1`.

`BIG` runs the same 8000 bytes through the API three times, and the three
passes differ in exactly one thing each so that the offsets can be compared:

| pass | source | framebuffer rect | destination |
|------|--------|------------------|-------------|
| `BIG`  | `$4000` | (0,100) 320x25 | `$6000` |
| `BIG2` | *(no new upload — re-reads the same framebuffer)* | same | `$6001` |
| `BIG3` | `$4001` | same | `$6000` |

The framebuffer base is a multiple of 256 in all three, so buffer index *k*
sits at framebuffer page offset *k* throughout; only the C64 addresses move.

* `BIG2` failing at the same *k* as `BIG` — the **destination** write address
  does not select the damaged byte.
* `BIG3` failing at the same *k* — the **source** read address does not
  either, and since `BIG3` is a fresh upload, the damage is chosen anew every
  transfer.
* `BIG3` failing one lower — the source address does select it.

`BIG2` alone cannot separate much: it re-reads a framebuffer that was
uploaded once, so if the upload is what corrupted it, every readback repeats
the same bytes faithfully and the results match whatever the cause. That is
why `BIG3` uploads again.

### What the three passes found (2026-08-25) -- FIXED, hardware-confirmed

`BIG3` came back **clean on both runs** while `BIG` failed on both: a second,
warm upload of the same 8000 bytes is correct, the first, cold one is not.
And every mismatch ever recorded -- eleven of them across five runs -- sits
beyond offset **1024**, the lowest at 1057.

1024 was `nWarm` in `gpu64_blobRead()`. The burst warmed the first 1024 bytes
of its destination and nothing after, so past that window every 64th store
landed on a cold cache line, the core had to read-allocate it from an SDRAM
the VideoCore is also driving, and the stall outlasted the C64 cycle the
burst was riding. The byte sampled was then whatever the bus had moved on to.
`gpu64_warmBuffer()` now warms the whole length, with real loads rather than
an advisory `prfm` -- `prfm` may be dropped by the hardware, and this is the
one place where "mostly warmed" and "warmed" differ by a corrupted byte.
`BIG2` showed no damage beyond `BIG`'s for the same reason it could not: the
upload had just warmed that staging buffer for the readback to re-read.

Two readings died on the way here, both artefacts of the test data: "the
wrong byte is always `$5C`, the page's last byte", and then "failures cluster
at page offset `$E1`". `kitSeq`'s original add-and-xor step was a bijection
on the byte, so its period was exactly **256, the same as a page**: every
byte value lived at one fixed page offset, which made "the value is `$06`"
and "the page offset is `$E1`" the same statement. `kitSeq` is now a Galois
LFSR of period 255, coprime with 256, and the clustering did not survive.

Confirmed on hardware the same day: with `src:cf073b3a` all three passes
report `0000` across the board, and the other five programs stayed green --
`gpu64_blobWrite()` took the same change and every test uses it.

One failure stays outside this account, and it is **not** the drop floor.
`REU EXACT` -- and once `BLIT EXACT` -- has failed on three of seven runs, on
a **64-byte** transfer. 64 bytes is inside any warm window, so the cold-line
defect cannot reach it; and three in seven is orders of magnitude above the
~1/35000 register-write drop rate. `gpu64_probe_reu` was written to measure
it: see below.

### gpu64_probe_raster -- the class 2 frame loop

`Source/TestPRG/gpu64_probe_raster.a`. Written for the raycast blink and
useful for any class 2 batch defect that only shows up over frames. 300
frames in about six seconds, three phases differing in exactly one axis:

| Phase | Records | Flip | Isolates |
|---|---|---|---|
| `BIG` | 320 | yes | the demo's own frame shape |
| `NOFL` | 320 | no | whether the fault is in the page flip |
| `SML` | 40 | yes | whether the fault scales with the size of the pull |

Nothing in it is random and nothing in it moves: the same checksummed batch
is drawn every frame and the expected pixels are a constant, so a screen
that is right on frame 1 and wrong on frame 40 has no explanation other than
the frame loop. Each phase counts four things separately, and which one
moves is the answer:

- `ERR` -- a dispatch did not return OK. The batch carries a checksum, so a
  pull that arrived damaged appears here as `BAD ARGS` ($04) rather than as
  garbage. This separates "the records did not arrive" from everything else.
- `STAT` -- OK returned, but `RASTER_STATS` does not report the batch that
  was sent. The records arrived and the parse disagreed with what was
  written.
- `PIX` -- stats perfect and the pixels still are not there. A 32-pixel
  strip is read back with `READ_RECT` every frame and compared to the colour
  the records asked for.
- `1ST` -- the first frame of the phase on which anything went wrong, so a
  late onset is visible as a number rather than as a feeling.

All zero on all three phases is clean. `PIX 0064` on `BIG` with `ERR 0000`
and `STAT 0000` means every frame drew nothing while claiming to have drawn
everything -- which is what the raycast demo looks like.

### gpu64_probe_reu

`REU EXACT` does three things at once, and any of them could be at fault: it
stashes a tile into REU space using the REU's **own** controller (ten
consecutive writes to `$df01`-`$df08`, exactly the register-write pressure
the `$df00`-`$df0a` decode path is known to be fragile under), blits it out
of space 1, then reads the framebuffer back. The probe repeats each part 64
times and reports `FAILS` / `1ST` / `OFF` / `W/G` per phase:

* **RAW** — stash and fetch back with the REU controller alone, no gpu64
  command involved. Red here means the defect is in RAD's REU emulation and
  gpu64 is only the messenger.
* **API** — the whole `REU EXACT` path repeated, stash included.
* **ONCE** — one stash, then 64 blit-and-read cycles. Clean here but red in
  API pins it on the stash rather than on reading REU memory.

`1ST = 0000` with `FAILS = 0001` would be the notable case: only the very
first transfer after start-up affected, which is all a single-stash program
like `gpu64_test_blit` can ever see.

64 iterations is deliberately modest — each writes ten REU registers, and
heavy REU register traffic is the documented way to latch the emulation into
total transfer failure. `FAILS` jumping to a large number from some `1ST`
onward is that latch, not this defect, and needs a power cycle.

**First result (2026-08-25): all three phases clean, 0 failures in 192
iterations.** That is not a null result, it is a constraint. If `REU EXACT`
failed as an independent event at anything like the observed 3-in-7 rate, a
run of 192 would be all but certain to catch it, and it caught none. So the
failure is **not** a property of the REU-space path repeated in isolation —
not the controller stash, not the space-1 blit, not the readback. What
remains is either something about the state `gpu64_test_blit` is in when it
reaches that line, some twenty commands in, or an event rarer than three runs
in seven made it look.

Cheapest things to check next: whether the framebuffer rectangle `REU EXACT`
reads back at (180,100) overlaps anything an earlier check in the same
program painted — the probe used (0,0) and could not have seen that — and
whether the probe stays clean with those twenty-odd commands replayed ahead
of it.

`tools/prgsim/runsim.py --alias-drop=OFF[,OFF...]` injects a byte-level fault
at chosen offsets, so the suite's diagnostic lines can be read against a
known answer on a PC before a bench run is spent on them.

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
