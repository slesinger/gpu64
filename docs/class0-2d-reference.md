# Class 0 — 2D and system reference

This is what a byte means for `CMD_HI = 0`: framebuffer drawing, palette,
blitting, and the system/info/matrix opcodes every gpu64 program touches
regardless of which higher class it also uses. Class 0 is always compiled
in.

See [getting-started.md](getting-started.md) for the register map and how a
command is issued, and [error-codes.md](error-codes.md) for the codes every
opcode below can return.

## System — $00–$0F

| Op | Name | ARG bytes | Does |
|---|---|---|---|
| $00 | `NOP` | none | Dispatches and returns `OK`. Useful as a liveness check. |
| $01 | `RESET_STATE` | none | Resets palette, draw page, border, and vblank arm state to power-on defaults. Does not clear the framebuffer. |
| $02 | `VBLANK_ARM` | none | Arms the vblank IRQ line. May halt up to a frame if a vblank is due imminently — see [vblank-and-animation.md](vblank-and-animation.md). |
| $03 | `VBLANK_ACK` | none | Releases the armed IRQ line. Must follow every `VBLANK_ARM` that fired. |
| $04 | `SET_DRAW_PAGE` | 1: page index | Selects which framebuffer page subsequent draw opcodes write to. `OUT_OF_RANGE` past `GPU64_FB_PAGES` (see the info block). |
| $05 | `PAGE_FLIP` | 1: 0 = now, 1 = at next vblank | Makes the draw page visible and hands you a fresh page to draw into. With `ARG0 = 0` the swap is immediate. With `ARG0 = 1` it lands at the next vblank, `STATUS` bit0 (busy) stays set until it does, and a second deferred flip while one is pending returns `BUSY` and changes nothing. **Either way the draw page moves as soon as the command returns** — see [vblank-and-animation.md](vblank-and-animation.md). `UNSUPPORTED` for `ARG0 = 1` if the boot-time frame period measurement failed. |
| $06 | `GET_INFO` | 0-5: dest descriptor | Writes the 16-byte info block (below) to the given destination. |
| $07 | `LOG_ENABLE` | 1: 0/1 | Turns firmware-side debug logging on or off. Has no effect on drawing. |
| $08 | `SET_BORDER` | 1: colour | Sets the HDMI border colour. Overridden automatically by the sticky under-voltage indicator — see the health block below. |
| $09 | `VBLANK_SYNC` | none | Blocks until the next vblank. May halt up to a full frame. `UNSUPPORTED` if the boot-time frame period measurement failed. |
| $0A | `GET_HEALTH` | 0-5: dest descriptor | Writes the 12-byte health block (below) to the given destination. |

## Whole surface — $10–$1F

| Op | Name | ARG bytes | Does |
|---|---|---|---|
| $10 | `CLEAR` | 1: colour | Fills the current draw page with one colour. |
| $14 | `UPLOAD_POLYS` | 10: 0-5 blob descriptor, 6-7 count, 8-9 first | Loads `count` 16-byte polygon records — the same record format `DRAW_POLYS` takes — into the **resident world** table starting at slot `first`, so a level bigger than one transfer arrives as several commands. Max 2048 slots; `len` must equal `count * 16`, and `first + count` past 2048 is `BAD_ARGS`. `count = 0` with `first = 0` drops the pool; `count = 0` with any other `first` does nothing. Slots may be rewritten in place; the pool survives everything except `RASTER_RESET`. This is a class 0 opcode, but the table it fills is only drawn by class 2's `DRAW_WORLD` ($27) — see [class2-raster-reference.md](class2-raster-reference.md). |

## Primitives — $20–$2F

| Op | Name | ARG bytes | Does |
|---|---|---|---|
| $20 | `SET_PIXEL` | 4: x(2), y(2), colour(1) — 5 total | Sets one pixel. Off-page coordinates are silently clipped, not an error. |
| $21 | `LINE` | x0,y0,x1,y1,colour | Bresenham line. Clipped to the page. |
| $22 | `RECT` | x,y,w,h,colour | Outline only. Clipped. |
| $23 | `RECT_FILL` | x,y,w,h,colour | Filled. Clipped. |

## Palette — $30–$3F

| Op | Name | ARG bytes | Does |
|---|---|---|---|
| $30 | `PAL_SET` | 1: index, 3: r,g,b | Sets one palette entry. |
| $31 | `PAL_LOAD` | 0-5: source descriptor | Loads up to 256 entries (768 bytes, 3 per entry) from RAM or REU. |

## Blit — $40–$4F

| Op | Name | ARG bytes | Does |
|---|---|---|---|
| $40 | `BLIT` | dest x,y + 0-5 source descriptor + w,h | Opaque rectangular copy from C64 RAM or REU into the draw page. |
| $41 | `BLIT_KEYED` | as `BLIT`, +1: key colour | As `BLIT`, but pixels equal to the key colour are skipped — source stays transparent there. |
| $42 | `READ_RECT` | source x,y,w,h + 0-5 dest descriptor | Copies a rectangle *out of* the draw page into C64 RAM or REU. Pairs with `BLIT`/`BLIT_KEYED` for a save-under-sprite/restore idiom. |

## Info block

Written by `GET_INFO` ($06), 16 bytes:

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 | `"$G64$"` magic (4 ASCII bytes) |
| 4 | 1 | API version |
| 5 | 2 | Framebuffer width |
| 7 | 2 | Framebuffer height |
| 9 | 1 | Bits per pixel |
| 10 | 1 | `GPU64_FB_PAGES` — number of framebuffer pages available for `SET_DRAW_PAGE` |
| 11 | 1 | Implemented-classes bitmap (bit N set = class N is compiled into this build) |
| 12 | 2 | Border width, height |
| 14 | 2 | Frame period, in the same units `VBLANK_SYNC` extrapolates from; 0 if the boot-time measurement failed |

## Health block

Written by `GET_HEALTH` ($0A), 12 bytes. The Pi reports its own
throttle/temperature state; a sticky under-voltage condition survives even
a hang, because it's read out of hardware state, not firmware state.

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 | Throttle word — see bit table below |
| 4 | 4 | Core temperature, millidegrees C |
| 8 | 4 | Reserved |

Throttle word bits:

| Bit | Meaning |
|---|---|
| 0 | Under-voltage detected, now |
| 1 | ARM frequency capped, now |
| 2 | Currently throttled |
| 3 | Soft temperature limit active, now |
| 16 | Under-voltage has occurred since boot (**sticky**) |
| 17 | ARM frequency capping has occurred since boot (sticky) |
| 18 | Throttling has occurred since boot (sticky) |
| 19 | Soft temperature limit has occurred since boot (sticky) |

**Bit 16 set is a verdict, not a hint.** If the sticky under-voltage bit is
set, something in the power delivery to that Pi was marginal at some point
in the session — HDMI corruption, an intermittent hang, or a raster glitch
reported around that time should be re-examined as a power problem before
anything else. gpu64 automatically forces the HDMI border red for the rest
of the session the first time bit 16 latches, independent of whatever
`SET_BORDER` last asked for — this is deliberate and is not a bug in
`SET_BORDER`.

## Matrix and vector ops — $80–$9F

A small linear-algebra coprocessor, useful for anything transforming
vertices before handing them to class 1/2, or for a game doing its own math
gpu64 can do faster. Two parallel op sets, same opcode offsets within each
half:

- **Fixed** — $80–$8F. Elements are 8.8 fixed point, 2 bytes each.
- **Float** — $90–$9F. Elements are IEEE754 single precision, 4 bytes each.

| Offset | Name | ARG bytes | Does |
|---|---|---|---|
| +$00 | `MAT_MUL` | dims + 2 compact descriptors (operands) + 1 compact descriptor (dest) | `dest = a * b`. Matrix dimensions from ARG bytes; `a`'s columns must equal `b`'s rows. |
| +$01 | `MAT_ADD` | dims + 2 operand descriptors + 1 dest descriptor | Elementwise. Operand dimensions must match. |
| +$02 | `MAT_SUB` | as `MAT_ADD` | Elementwise. |
| +$03 | `MAT_SCALE` | dims + 1 operand descriptor + scalar + 1 dest descriptor | Elementwise multiply by a scalar. |
| +$04 | `MAT_TRANSPOSE` | dims + 1 operand descriptor + 1 dest descriptor | |
| +$05 | `MAT_IDENTITY` | dim (square) + 1 dest descriptor | Writes an n×n identity matrix. |
| +$06 | `MAT_INVERSE` | dim (square, n≤64) + 1 operand descriptor + 1 dest descriptor | Gaussian elimination with partial pivoting (double precision internally, regardless of fixed/float mode, for numerical stability). `SINGULAR` if the matrix has no inverse. `OUT_OF_RANGE` for n>64. |

Matrices are row-major. A matrix operand is addressed with a **compact**
descriptor (space + addr, no length — the op's dimension arguments imply
the byte count). Any dimension of 0 is `BAD_ARGS`. An operand or
destination whose implied extent runs past its space is `OUT_OF_RANGE`. All
results are written back to their destination before the `CMD_LO` write
that started the command returns.

A vector is a 1×N (or N×1) matrix — there is no separate vector opcode set.
`MAT_MUL` with a 1-row or 1-column operand is how a vertex or a batch of
vertices gets transformed; a single call is limited to 255 vertices.

`$A0`–`$FF` in class 0 are undefined and return `BAD_OPCODE`.

See also: [class2-raster-reference.md](class2-raster-reference.md) for
`UPLOAD_POLYS`'s polygon record format and `DRAW_WORLD`, and
[project/milestone4_2d_api_design.md](../project/milestone4_2d_api_design.md)
for class 0's original design rationale.
