# gpu64 command API design

API reference for the C64→gpu64 command protocol over IO2. This document
describes the wire protocol only — status, history, and hardware test
results live in [progress_tracker.md](progress_tracker.md); the continuous
render loop, multicore execution, and persistent GPU-side resources
(textures, meshes) are a separate, later concern, covered in
[milestone6_3d_design.md](milestone6_3d_design.md). Companion to
[project_description.md](project_description.md) (IO address space
allocation, target hardware configs, the three operating modes) and
[bus_access_design.md](bus_access_design.md) (why cycle-by-cycle sniffing
doesn't fit this hardware).

Scope: milestone 4's 2D command set (clear/fill, pixel, line, rect, blit,
palette, page flip) plus general-purpose math/matrix ops. Everything here
executes synchronously, in the existing single core-0 bus-watch loop —
immediate-mode dispatch, the same model `showTestPattern()` already proves
on hardware. No independent render loop or second core is required for
this scope; see milestone6_3d_design.md for where and why that changes.

## Philosophy

- **IO2 carries commands only, never bulk payload.** A command that needs
  data (e.g. a blit's source bytes) passes a *reference* — where the data
  already lives (C64 RAM or REU) and how long it is — not the data itself
  byte-by-byte through a register. gpu64 pulls the referenced bytes via a
  DMA burst, the same technique milestone 3's mirror snapshot already
  proved on hardware, just parameterized instead of hardcoded to screen RAM.
- **A payload-carrying command causes one bounded DMA-held burst.** Per
  [project_description.md](project_description.md#operating-modes), the C64
  is never DMA-halted for anything but a brief burst, in any gpu64 mode.
  The burst's duration is proportional to the payload's `len` — a known,
  C64-computable cost, not an open-ended stall.

## Address space

Settled in [project_description.md](project_description.md#io-address-space-allocation):
gpu64 owns **$DF0B–$DFFF, 245 bytes** of IO2. IO1 ($DE00–$DEFF) stays
reserved/untouched (needed for target config #3, a real hardware REU sharing
the bus). REU keeps $DF00–$DF0A.

Implementation notes for widening the decode beyond REU's own:

- The *current* `reuUsingPolling()` dispatch masks `IO_ADDRESS & 0x1f` —
  that's REU's own internal decode (11 registers fit in 5 bits), not a
  hardware limit; the GPIO latch delivers the full 8-bit address. gpu64's
  own dispatch must decode the full byte, not inherit that mask, or the
  usable space silently shrinks to ~21 aliased addresses.
- **Existing landmine on the read path**: `rad_reu.cpp`'s REU register read
  handler indexes `((u8*)&reu.status)[addr]` with `addr` masked to 5 bits —
  a read from anywhere in the gpu64 range as currently masked walks off the
  end of `REUSTATE` and returns whatever's there. Widening the decode must
  not inherit this; gpu64's own registers need their own bounds-checked read
  path, not REU's.
- Widening the write-side `switch(addr)` to a full 8 bits puts a bigger
  jump table/compare chain inside the cycle-critical write path — keep it
  branch-cheap.

## Memory model (this scope)

Two source spaces for a DMA-referencing command:

- **C64 RAM** — 64KB.
- **REU** — up to 16MB, independent C64-side expansion memory. Not
  RPi-resident; pulling from it costs a real DMA burst, same as C64 RAM.

The destination for this scope is always **the existing framebuffer**
(the same `GPU_OUTPUT_BOX`-backed buffer `showTestPattern()`/`showMirror()`
already draw into) — there is no persistent, ID-addressed GPU-side resource
store yet. That's introduced in milestone6_3d_design.md alongside textures
and meshes.

**Decided**: a DMA read sees whatever C64 memory banking (`$01`) is in
effect at the moment gpu64 samples it — same convention
`gpu64_mirrorSnapshot()` already relies on to reach `$D800` colour RAM.
There is no separate bank-select field. Revisit only if a concrete use case
needs to read through a banking configuration other than the CPU's current
one.

## Register map ($DF0B–$DFFF)

| Offset | Name | Dir | Purpose |
|---|---|---|---|
| $DF0B | `CMD_HI` | W | Class selector. **Sticky** — persists until changed, defaults to 0 (2D + math/system) on reset. Writing it alone triggers nothing. |
| $DF0C | `CMD_LO` | W | Opcode within the current class; write triggers dispatch using whatever `ID`/`ARG` are currently staged |
| $DF0D | `STATUS` | R | bit0 busy, bit1 error, bit2 vblank-pending, bit3 vblank-IRQ-armed, rest TBD |
| $DF0E | `ERRCODE` | R | Set on error (`BAD_OPCODE`, `OUT_OF_RANGE`, ...); cleared on next successful `CMD_LO` |
| $DF0F–$DF10 | `ID` | W | Resource ID, 16-bit — unused in this scope, reserved for milestone 6 |
| $DF11–$DF20 | `ARG0`–`ARG15` | W | Generic 16-byte argument block, layout defined per-command (see below) |
| $DF21–$DFFF | — | — | Reserved for growth |

**Protocol**: write `CMD_HI` only when changing class (it persists — the
common case, staying in class 0, never touches it again after the first
write). Stage `ID`/`ARG` in any order, any number of writes, then a single
`STA` to `CMD_LO` fires dispatch. Matches REU's own existing protocol shape
in this firmware (stage address/length, then write `COMMAND` with the
execute bit).

**Unknown opcode**: `CMD_LO` value with no defined meaning in the current
class → `ERRCODE = BAD_OPCODE`, no dispatch, no DMA pull. Never silently
ignored.

**`ARG`/`ID` staleness**: these registers are write-only with no implicit
clear. Each opcode's spec (see Opcode table, TBD) must state exactly how
many of its bytes it reads; core 0 must not read past that count. A command
that reads more than its spec says would silently inherit bytes an earlier,
unrelated command left behind — the same failure shape as the
`gpu64ApiActive` bug (see progress_tracker.md's milestone 3 section) one
register layer up. This is a hard requirement on the opcode table, not
optional polish.

### ARG block convention for DMA-referencing commands

Not a fixed register layout — each command defines its own use of the 16
`ARG` bytes — but a command that references a DMA payload (e.g. blit) uses
a 6-byte **blob descriptor** shape within it:

| Bytes | Field | Purpose |
|---|---|---|
| 1 | space | 0 = C64 RAM, 1 = REU |
| 3 | addr | 24-bit offset within that space (covers REU's full 16MB; C64 RAM just uses the low 16 bits) |
| 2 | len | byte count (max 65535 — plenty for this scope: a 320x200 framebuffer is 64000 bytes, an 80x50 PETSCII screen is 4000) |

**Decided**: `len = 0` is a no-op (not an error). `addr + len` exceeding the
source space's size is `ERRCODE = OUT_OF_RANGE` — no silent clamping or
wraparound (unlike REU's own legacy wrap behavior, which gpu64 has no
compatibility reason to replicate).

A single-blob command (e.g. a blit) uses one descriptor (6 bytes), leaving
10 bytes for command-specific fields (destination x/y, width/height, ...).
Commands needing more than one blob (mesh upload, etc.) are out of this
scope — see milestone6_3d_design.md.

## Cycle-predictability

Core 0's write handler must never do anything with *unbounded* or
*contention-dependent* latency — no allocation, no blocking locks, no
unbounded cache work. A DMA pull's cost scales with payload length, which
is a known, C64-computable cost — not the kind of unpredictability being
avoided here.

The destination framebuffer region a blit writes into needs the same cache
handling `showMirror()` already does for its rows (explicit clean after
writing, not relying on eventual eviction) — cache state at the
*destination*, not just the source pull, affects how long a burst actually
takes.

## vblank signaling

Confirmed available and already proven on this hardware: `IRQ_OUT`/`bIRQ_OUT`
is a real GPIO line into the CBTD3861 transceiver, and REU's own emulation
already drives it (`rad_reu.cpp:605,618`) for REU's completion-interrupt
feature. gpu64 reuses the same mechanism for vblank:

- `STATUS.vblank-pending` bit for simple polling (BASIC-friendly, no IRQ
  handler needed).
- An IRQ pulse on vblank, gated by a C64-settable "arm" bit
  (`STATUS.vblank-IRQ-armed`), for programs that want a real interrupt
  instead of polling.

**Decided: stays armed, auto-rearms.** Like a raster IRQ, the handler
acknowledges on entry; re-arming automatically for the next frame avoids
needing an explicit re-arm write every frame. A C64-settable disarm (back to
polling, or off entirely) stays available.

## Opcode space

`CMD_HI` selects the class: 0 = 2D + math/matrix + system (this scope), 1 =
3D (milestone6_3d_design.md), 2+ reserved for later (e.g. a future WiFi
API). 256 opcodes per class.

**Class 0 — 2D + math + system** (minimum viable set, carried over from
progress_tracker.md): clear/fill, set pixel, line, rect (filled/outline),
blit-by-reference, palette select, page flip; vector/matrix ops (add,
multiply, transform, inverse, ...); status/error queries.

Math/matrix ops live in class 0 rather than getting their own class — likely
used every frame (camera/transform updates once milestone 6 exists), so
they belong alongside 2D on the register C64 code will write to most.

To fill in next: full opcode table, one row per command, with its exact
`ARG` byte layout and the byte count core 0 must enforce (see the ARG
staleness requirement above) — texture-adjacent commands wait for
milestone 6, but the pure-2D set above should get its table next.

## Open questions

1. Full class-0 opcode table — exact `ARG` layout and enforced byte count
   per command.
2. `ERRCODE` enum — needs the opcode table to enumerate against.
