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
| $05 | `PAGE_FLIP` | 1: 0 = now, 1 = at next vblank | Makes the draw page visible and hands you a fresh page to draw into. With `ARG0 = 0` the swap is immediate. With `ARG0 = 1` it lands at the next vblank, `STATUS` bit0 (busy) stays set until it does, and a second deferred flip while one is pending returns `BUSY` and changes nothing. **Either way the draw page moves as soon as the command returns** — see [vblank-and-animation.md](vblank-and-animation.md). `UNSUPPORTED` for `ARG0 = 1` if the boot-time frame period measurement failed, and `UNSUPPORTED` in either form while text mode is up (there is only one page there). |
| $06 | `GET_INFO` | 0-5: dest descriptor | Writes the 16-byte info block (below) to the given destination. |
| $07 | `LOG_ENABLE` | 1: 0/1 | Turns firmware-side debug logging on or off. Has no effect on drawing. |
| $08 | `SET_BORDER` | 1: colour | Sets the HDMI border colour. Overridden automatically by the sticky under-voltage indicator — see the health block below. |
| $09 | `VBLANK_SYNC` | none | Blocks until the next vblank. May halt up to a full frame. `UNSUPPORTED` if the boot-time frame period measurement failed. |
| $0A | `GET_HEALTH` | 0-5: dest descriptor | Writes the health block (below) to the given destination — 12 bytes, or up to 48 with the diagnostic counters. |

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

## Text mode — $50–$5F

A second display **mode**, not a layer over the graphics one. `TEXT_MODE 1`
re-programs the display to 640×400 and turns it into 80 columns by 50 rows
of 8×8 glyphs taken from the Commodore 64's own character ROM; `TEXT_MODE 0`
puts the 320×200 paged framebuffer back. Only one of the two exists at a
time:

- While text mode is up there is no framebuffer page. Every drawing opcode
  above still returns `OK`, but writes nowhere, `PAGE_FLIP` returns
  `UNSUPPORTED`, and **every class 1 and class 2 command returns
  `UNSUPPORTED`** before it runs.
- `GET_INFO` answers for the live mode: in text mode it reports 640×400,
  `GPU64_FB_PAGES` = 1, and a 64×72 border. Reading `pages = 1` is how a
  program learns `PAGE_FLIP` is unavailable without trying it.
- `SET_BORDER`, `GET_HEALTH`, `VBLANK_ARM`/`ACK`/`SYNC` and the matrix ops
  all work normally.

The model is the C64's: three parallel 4000-byte planes — **screen codes**,
**foreground colour**, **background colour** — indexed row-major, so cell
`row * 80 + col`. Screen code **bit 7 is reverse video**, exactly as at
`$0400`. What the C64 does not have, and this does, is a per-cell
*background* colour, with all 256 palette entries available in both colour
planes.

| Op | Name | ARG bytes | Does |
|---|---|---|---|
| $50 | `TEXT_MODE` | 1: 0 = graphics, 1 = text | Switches the display and repaints from the planes. Asking for the mode you are already in returns `OK` and costs nothing. `BAD_ARGS` above 1; `BUSY` if a deferred `PAGE_FLIP` is still pending; `UNSUPPORTED` if the display could not be re-programmed. Drains any queued class 1 render first — this is a Stage 15b observation point. |
| $51 | `TEXT_CLEAR` | 1: fg, 1: bg | All 4000 cells to a space in those two colours. |
| $52 | `TEXT_PUT` | col, row, code, fg, bg | One cell. Off-grid is clipped, not an error — the same contract `SET_PIXEL` has. |
| $53 | `TEXT_WRITE` | 0-5: source descriptor, 6: col, 7: row, 8: fg, 9: bg, 10: flags | One line of text. `len` is clamped to 80 and the line is truncated at the right-hand edge, never wrapped. A `row`/`col` off the grid draws nothing and is not an error. Flags bit 0 = the payload is ASCII and gets translated to screen codes for the character set currently up; clear it to pass screen codes straight through, which is the only way to reach the graphics half of the ROM or to set bit 7. |
| $54 | `TEXT_UPLOAD` | 0-5: source descriptor, 6: plane, 7-8: first cell (u16) | Copies `len` bytes into one plane starting at cell `first`, leaving the other two planes alone. Plane 0 = screen codes, 1 = foreground, 2 = background. `BAD_ARGS` for a plane above 2; `OUT_OF_RANGE` if `first ≥ 4000` or `first + len > 4000` — checked before anything is written, so a caller that gets the arithmetic wrong is told rather than left with a partial upload. This is the cheap way to paint: a whole plane is one command. |
| $55 | `TEXT_CHARSET` | 1: 0 or 1 | Picks the half of the character ROM the whole screen is drawn with: 0 = upper case and graphics, 1 = lower case and upper case. Repaints; the planes do not change. `BAD_ARGS` above 1. |
| $56 | `TEXT_SCROLL` | 1: rows, 2: fg, bg | Scrolls the **whole screen** up by `rows`, filling the rows that appear at the bottom with spaces in the given colours. |
| $57 | `TEXT_FILL` | col (s8), row (s8), w, h, code, fg, bg | A rectangle of cells, all three planes. `col` and `row` are signed, so a rectangle may start off the left or top edge; the result is clipped. |
| $58 | `TEXT_REFRESH` | none | Repaints all 50 rows from the planes. Only needed after something outside the API has disturbed the display. |

`$59`–`$5F` are undefined and return `BAD_OPCODE`.

**The plane commands work with the graphics screen still up.** They update
the planes either way and only repaint when text mode is the live mode, so a
program can compose an entire text screen and have it appear complete at the
`TEXT_MODE 1` that follows. `RESET_STATE` ($01) resets the planes too — every
cell a space, light blue on blue, charset 1.

**Text mode is sticky across programs.** A program that leaves it up hands
the next one a display its drawing commands cannot reach. The demo runtime's
`dmInit` issues an unconditional `TEXT_MODE 0` for exactly this reason, and
anything else that starts by assuming a framebuffer should do the same.

## Info block

Written by `GET_INFO` ($06), 16 bytes:

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 | `"$G64$"` magic (4 ASCII bytes) |
| 4 | 1 | API version |
| 5 | 2 | Framebuffer width (640 in text mode) |
| 7 | 2 | Framebuffer height (400 in text mode) |
| 9 | 1 | Bits per pixel |
| 10 | 1 | `GPU64_FB_PAGES` — number of framebuffer pages available for `SET_DRAW_PAGE`; 1 in text mode, where `PAGE_FLIP` is `UNSUPPORTED` |
| 11 | 1 | Implemented-classes bitmap (bit N set = class N is compiled into this build) |
| 12 | 2 | Border width, height (64×72 in text mode) |
| 14 | 2 | Frame period, in the same units `VBLANK_SYNC` extrapolates from; 0 if the boot-time measurement failed |

## Health block

Written by `GET_HEALTH` ($0A). The Pi reports its own throttle/temperature
state; a sticky under-voltage condition survives even a hang, because it's
read out of hardware state, not firmware state.

Ask for 12 bytes and you get the block below. Ask for 20, 36 or 48 and you
additionally get the diagnostic counters described under "Diagnostic
extension"; any other `len` ≥ 12 is rounded *down* to the next of those four
sizes, so a program written against the 12-byte block keeps working
unchanged.

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 | Throttle word, current and sticky bits — see bit table below |
| 4 | 4 | Throttle word as latched since boot (sticky bits only) |
| 8 | 2 | Core temperature, tenths of a degree C |
| 10 | 2 | Highest core temperature seen this session, tenths of a degree C |

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

### Diagnostic extension

Everything past byte 12 is an instrument, not a feature: 16-bit saturating
counters that exist so a bench program can read the firmware's own view of
itself without turning the log on and changing what it is measuring. They
are stable enough to write a test against and are not part of the drawing
contract.

| Offset | Size | Contents |
|---|---|---|
| 12 | 8 | Flip path: drain timeouts, post-full timeouts, slow flips, flips posted |
| 20 | 6 | DMA hold spacing: smallest gap, smallest dispatch→commit gap, ARM cycles per C64 cycle — all in raw ARM cycles |
| 26 | 10 | Hold-gap histogram buckets 0-2, dispatch→commit asserts under 4 C64 cycles, holds opened |
| 36 | 4 | Hold gate: gates armed, gates fired. Equal is the healthy steady state |
| 40 | 2 | Hold gate: worst wait from arm to hold, in C64 cycles |
| 42 | 2 | Hold gate: passes an armed gate could not fire on |
| 44 | 4 | Hold gate: `CMD_LO` writes deferred as read-modify-write, and deferred dispatches that ran |

Bytes 44-47 are 0 for every program that writes `CMD_LO` with a plain `sta`,
which is every program in this tree. A non-zero value there means something
is doing `inc $DF0C` or an indexed store whose dummy read lands in `$DFxx` —
safe, and handled, but worth knowing about.

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
