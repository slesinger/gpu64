# gpu64 — working agreement for implementation agents

gpu64 is a Raspberry Pi 3A+ bare-metal cartridge (RAD + Circle 44.3) that gives
a Commodore 64 a second, independent HDMI screen and a command API to draw on
it. `reuUsingPolling()` in `Source/Firmware/rad_reu.cpp` is the cycle-critical
loop that samples the C64 bus on core 0; almost every hard-won rule below
exists because something in or near that loop was violated.

Read [project/progress_tracker.md](project/progress_tracker.md) for status and
[docs/api_design.md](docs/api_design.md) for the API surface.

## The bus is not reliable — design for it

**This is the constraint most likely to bite work in progress.** A register
write from the C64 is occasionally not sampled by the polling loop: roughly
**1 in 35000 REU transfers** and **1 in 180000 gpu64 command writes**. There is
no retry and no error — the write simply never happened. It is pre-existing, it
reproduces with all 3D code compiled out, and it is not going to be fixed
before the API grows.

Consequences that are binding on new work:

1. **Never assume a register write landed.** Any multi-write protocol needs to
   be able to detect that it didn't.
2. **Commands must carry a sequence number** with a gap detector on the
   firmware side, so a dropped argument write becomes a reported error instead
   of a command silently executing on a half-written argument block.
3. **Any bulk upload that goes through REU DMA needs a checksum or a length
   readback.** That path carries both known defects.
4. **When a bench run shows one otherwise-inexplicable glitch, suspect this
   floor first** — not your code.

There is a second, much larger defect: under extra *REU* register write
pressure the emulation latches into 100% transfer failure. It lives in the
`$df00-$df0a` decode path, which resets `reu.pl`/`reu.pl2` and calls
`reuPrefetch()` on **every** write. The gpu64 register window `$df0b-$df21`
does neither, which is why command traffic has never reproduced it. **Do not
add per-write work to either decode path.**

Full detail and the continuation handle (`io2-sampling`) are in the memory
note `gpu64-io2-sampling-reliability`.

## Rules for anything reachable from the polling loop

Every milestone-4 hardware bug was one of these, and none was visible in a
build or in code reading.

1. **The C64 free-runs through the loop.** Anything the loop is too busy to
   sample is lost outright. Long work must hold the bus (`CLR_GPIO(bDMA_OUT)`),
   which is why a gpu64 command halts the C64.
2. **Release DMA on a cycle boundary**, never wherever the work finished. Get
   this wrong and damage accumulates — the program survives its first commands
   and derails later.
3. **`WAIT_FOR_VIC_HALFCYCLE` alone is not a sync.** Precede it with
   `WAIT_FOR_CPU_HALFCYCLE`. It falls straight through when the C64 is
   *already* in the VIC half, and then the whole entry sequence degenerates:
   `RESTART_CYCLE_COUNTER` anchors mid-half-cycle, `TIMING_TRIGGER_DMA` has
   already elapsed, and the bus is taken at an undefined phase. So before
   opening a hold, check which half-cycle the **call site** is in — the
   polling loop calls into its helpers from inside the VIC half. This has now
   been the bug twice: milestone 4 in the blob helpers, 2026-08-27 in
   `gpu64_vsyncCommitFlip()`, which cost days of chasing a phantom power
   fault.
4. **"Preloaded at start-up" is not durable.** A single `CLEAR` evicts it.
   Cycle-critical code reachable from a command must warm its own i-cache at
   the point of use. Cold-fails / warm-passes is the signature.
5. **Exception to 4: warm from upstream when the point of use is unprotected.**
   A 2KB preload running in the loop *before* the bus is held is the delay it
   was meant to prevent. `gpu64_apiWarmPollingLoop()` and `gpu64_vsyncWarmCommit`
   are the two worked examples.
6. **Work added ahead of the bus sampling is exposed**, and how much depends on
   which mode it runs in. Judge loop additions by that, not by their cost.

Also: **RAD's low-level macros do not parenthesise their arguments.** Never
pass an expression containing `?:`, `+` or `%`.

## Multicore

The milestone 6a load ladder settled this in eighteen rounds; do not re-derive
it.

- **Core 1 may write at most 7 consecutive cache lines (448 bytes) before
  yielding.** Design to **256 bytes (4 lines)** for margin — one 320-byte
  scanline per yield is inside the limit.
- Burst length is the *only* axis. Write bandwidth (free to 64 MB/s),
  working-set size, L2 capacity and duty cycle are all non-constraints.
- **Never poll an MMIO register from another core** — any MMIO spin breaks core
  0's bus timing. Use `CNTVCT_EL0`, not the BCM system timer.
- **Inside `reuUsingPolling()` and anything it reaches with the bus still
  free-running, core 0 must never read a cache line another core writes.**
  Separate cache lines are not sufficient; removing the read is what produced
  a zero noise floor. This is a statement about that loop's per-C64-cycle
  deadline, not a general memory-model rule — 6a's own instrument only ever
  measured loop-pass elongation there. Code that runs with the C64 DMA-halted
  (a command dispatch, the log path) has no such deadline and already reads
  core-1-written state safely in shipped, hardware-verified form
  (`logGpu64_3dStats()` in `gpu64_api.cpp`, the ring's own cached-tail
  fallback). Conflating the two cost a full design round on Stage 15 — see
  `project/gap_filling_plan.md`'s Stage 15 section.
- `STNP` and `DC ZVA` were both tried as escapes and both fail.

## Testing

- **Bench time is the user's scarce resource.** Optimise instruments for
  *events per minute*, not statistical power. A test that needs two hours needs
  redesigning — the accelerated bench found the same rate 16x faster.
- **Verify on the host or in VICE before asking for a hardware run.**
  `tools/hostsim` compiles the renderer natively and renders to PPM;
  `tools/hostsim/out/prgpreview.ppm` is the expected HDMI output.
- **VICE runs must be several hundred frames.** A 96-frame desk check missed a
  defect whose onset was frame 98.
- **Poison destination buffers so a miss has a fingerprint**, and log per
  event — a sum and a maximum cannot recover a distribution.
- **A clean timing metric is necessary, not sufficient.** The ladder's `L=`
  counter read `0/0` on rungs that were killing the C64. The C64 staying alive
  is the only ground truth.
- Deploying: `SDCARD=<mountpoint> tools/build.sh`. The card boots
  **`kernel_rad.img`** per `config.txt`, not `kernel8.img` — check the boot
  log's build id, because stale-firmware deploys have silently wasted whole
  test rounds.
- In the RAD menu, press **T until it reads REU**. Without it
  `reuUsingPolling()` never runs and none of gpu64 executes at all.
- 64tass: **never use `--nostart`** — it strips the `$0801` header silently and
  the program simply never runs.

## Docs

`docs/` is for the API's users; `project/` is for whoever works on gpu64.

- `docs/api_design.md` — only what a developer needs to *use* the API.
- `project/milestone*_design.md` — rationale and as-built decisions.
- `project/progress_tracker.md` — status and campaign history.

Keep them separated that way.
