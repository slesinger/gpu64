# gpu64 API reference

The C64→gpu64 command protocol over IO2: everything a C64 programmer needs
to drive gpu64's framebuffer.

Everything on this page is implemented. Vblank included — it shipped after
milestone 4, and the one thing that can still take it away is a display
gpu64 could not measure a frame period from at boot; read the frame period
out of `GET_INFO` to find out (see [vblank](#vblank)). See
[progress_tracker.md](progress_tracker.md) for hardware test status.

Why the protocol looks the way it does is in
[milestone4_2d_api_design.md](milestone4_2d_api_design.md); status and
hardware test results are in [progress_tracker.md](progress_tracker.md).
Class 1 (3D) is specified in
[milestone6_3d_design.md](milestone6_3d_design.md).

---

## Framebuffer

| Property | Value |
|---|---|
| Resolution | 320 x 200 |
| Format | 8 bits per pixel — one byte per pixel, a palette index |
| Page size | 64000 bytes |
| Pages | 2 (front / back), hardware-swapped |
| Palette | 256 entries, 24-bit RGB — any 256 colours out of 16.7M. Index 255 is reserved for the on-screen log. |
| Origin | top-left, +y down |
| Border | 32 px left/right, 36 px top/bottom, around the 320x200 surface |

Draw ops write to the **draw page**; the **visible page** is what the HDMI
output shows. Both are page 0 at reset, so a program that ignores paging
draws straight to the visible screen. `SET_DRAW_PAGE` and `PAGE_FLIP` give
you tear-free double buffering: draw the whole frame into the back page,
then flip.

### Border

Around the drawing surface is a **border**, exactly as on the C64: a frame
32 pixels wide at the sides and 36 tall at top and bottom, which no draw op
can reach and which `SET_BORDER` ($08) paints in one palette index. It is
black at reset.

The border is a property of the display, not of a page, so `PAGE_FLIP` never
changes it — set it once and it stays. That also means `SET_BORDER` is
comparatively expensive: it repaints the frame on both pages. Set it when it
changes, not once per frame.

Coordinates are **signed 16-bit**. Drawing ops **clip** to the 320x200
page — geometry partly or wholly offscreen is normal and never sets an
error. `w`/`h` fields are unsigned 16-bit; a zero `w` or `h` is a no-op.

At reset, palette entries 0–15 hold the standard C64 colours (indices match
the C64's own colour numbering), 16–254 are black, and 255 is white. The
framebuffer contents at reset are undefined — `CLEAR` first.

### Index 255 and the on-screen log

gpu64 draws its own diagnostic log over your framebuffer in **palette index
255**, one colour, glyph pixels only — the space around each glyph is left
untouched, so whatever you drew shows through. The log is **on at reset**;
hidden automatically the moment your first command succeeds; turn it back on with `LOG_ENABLE` ($07) when you want it.

Nothing stops you using index 255 as an ordinary drawing colour, or
repointing it with `PAL_SET` — the log just draws in whatever colour that
entry holds, and becomes invisible if you set it to match your background.

## Register map

gpu64 owns **$DF0B–$DFFF** in IO2. REU keeps $DF00–$DF0A; IO1
($DE00–$DEFF) is untouched.

| Address | Name | Dir | Purpose |
|---|---|---|---|
| $DF0B | `CMD_HI` | W | Class selector. **Sticky** — persists until changed, 0 at reset. Writing it alone triggers nothing. |
| $DF0C | `CMD_LO` | W | Opcode within the current class. **Writing it fires the command**, using whatever `ID`/`ARG` are staged. |
| $DF0D | `STATUS` | R | bit0 busy, bit1 error, bit2 vblank-pending, bit3 vblank-IRQ-armed |
| $DF0E | `ERRCODE` | R | Result of the last dispatch (see [Error codes](#error-codes)) |
| $DF0F–$DF10 | `ID` | W | Resource ID, 16-bit — unused in class 0, reserved for class 1 |
| $DF11–$DF20 | `ARG0`–`ARG15` | W | 16-byte argument block, layout defined per opcode |
| $DF21–$DFFF | — | — | Reserved |

### What a command costs the C64

**The write to `CMD_LO` halts the C64 until the command finishes.** gpu64
holds the bus for the whole dispatch, exactly the way an REU transfer does,
and the CPU resumes at the next instruction with nothing missed. You do not
need to poll, wait, or space your writes out — staging `ARG` bytes and
firing `CMD_LO` back to back always works.

The halt is bounded by the command's own work: a `SET_PIXEL` is trivial, a
`CLEAR` writes 64000 bytes, a blit also moves its payload across the bus.
Nothing is unbounded, and nothing else in the API halts the CPU for longer
than a normal register access.

If you have raster-timed code running, treat a gpu64 command the way you
would treat an REU transfer: it steals the cycles it needs.

### Issuing a command

1. Write `CMD_HI` **only when changing class**. Staying in class 0 (the
   normal case) means never writing it after the first time.
2. Stage `ARG` bytes in any order, any number of writes.
3. Write `CMD_LO` — this dispatches.
4. Optionally read `ERRCODE`.

`ARG` registers are write-only and are **not** cleared between commands.
Every opcode reads exactly the byte count listed in its table row and never
looks past it, so leftovers from a previous command cannot leak in — but
that also means you must stage every byte an opcode reads, every time.

Class 0 commands complete before the `CMD_LO` write returns, except a
vblank-deferred `PAGE_FLIP`. Poll `STATUS` bit0 (busy) if you used one.

Two commands are slower than their work suggests, because they wait for the
display rather than for gpu64: `VBLANK_SYNC`, and `VBLANK_ARM` when arming.
Both can halt the C64 for up to one frame. They are setup and housekeeping
commands — do not put either in a per-frame loop.

### Blob descriptor

Bulk data never crosses the register window byte-by-byte. A command that
needs data passes a 6-byte **blob descriptor** — where the bytes already
live — and gpu64 fetches them itself:

| Bytes | Field | Purpose |
|---|---|---|
| 1 | `space` | 0 = C64 RAM, 1 = REU |
| 3 | `addr` | 24-bit offset in that space (C64 RAM uses the low 16 bits) |
| 2 | `len` | byte count, max 65535 |

Multi-byte fields are little-endian (low byte first), as a 6502 expects.

The same descriptor is also used as a **destination**: commands that produce
data (`GET_INFO`, `READ_RECT`, every math op) don't return it through a
register window — they write it back into the memory you point them at, and
the write lands before the `CMD_LO` write returns. So a matrix multiply is
"here are the two operands, here is where the result goes", and you read the
result straight out of your own RAM.

- `len = 0` is a no-op, not an error.
- `addr + len` past the end of the space → `OUT_OF_RANGE`. No clamping, no
  wraparound.
- The fetch sees whatever memory banking (`$01`) is in effect at that
  moment. There is no bank-select field.
- Cost: the transfer's time is proportional to `len` — a 64000-byte blit
  costs more than a 256-byte one, and a command with both a source and a
  destination pays for both. See "What a command costs the C64" below.

#### Compact descriptor

Math opcodes use a 4-byte **compact descriptor** instead — `space` (1) plus
24-bit `addr` (3), with no `len`, because the byte count is already implied
by the operand's dimensions and element size. Bounds are checked against
that computed count exactly as if you had written it out.

## Class 0 opcodes — 2D, system, math

Set `CMD_HI = 0` (the reset default). `ARG` offsets below are relative to
`ARG0` ($DF11). "Bytes" is exactly how many `ARG` bytes the opcode reads.

### System — $00–$0F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $00 | `NOP` | 0 | — | Nothing. Liveness probe: sets `ERRCODE = OK`. |
| $01 | `RESET_STATE` | 0 | — | `ERRCODE = OK`, vblank IRQ disarmed, vblank-pending cleared, draw page = visible page = 0. Leaves framebuffer contents and palette alone. |
| $02 | `VBLANK_ARM` | 1 | `ARG0`: 0 = disarm, 1 = arm | Arms the auto-rearming vblank IRQ. Reflected in `STATUS` bit3. Arming also re-syncs the frame clock, so it costs up to one frame of halt — see [vblank](#vblank). |
| $03 | `VBLANK_ACK` | 0 | — | Clears `STATUS` bit2 (vblank-pending). Poll-loop programs write this after seeing the bit; IRQ handlers write it on entry. |
| $04 | `SET_DRAW_PAGE` | 1 | `ARG0`: 0 or 1 | Selects the page subsequent draw ops write to. |
| $05 | `PAGE_FLIP` | 1 | `ARG0`: 0 = now, 1 = at next vblank | Makes the draw page visible and the old visible page the draw page. With `ARG0 = 1` the swap happens at the next vblank and `STATUS` bit0 (busy) stays set until it lands; asking for a second deferred flip while one is still pending returns `BUSY` and changes nothing. |
| $06 | `GET_INFO` | 6 | `ARG0-5` destination descriptor | Writes the 16-byte info block (see below) to your memory. `len` must be ≥ 16. |
| $07 | `LOG_ENABLE` | 1 | `ARG0`: 0 = off, 1 = on | Shows or hides gpu64's on-screen log overlay. On at reset, but **the first successful command of a session hides it automatically** so firmware text does not land on your output; `LOG_ENABLE` itself is exempt, so call it whenever you actually want the log. Turning it on also prints two `FLIP` lines giving what this session's page flips cost, in microseconds. |
| $08 | `SET_BORDER` | 1 | `ARG0`: palette index | Paints the border around the drawing surface. Black at reset. See [Border](#border). |
| $09 | `VBLANK_SYNC` | 0 | — | Re-syncs the frame clock against a real vertical sync. Costs up to one frame of halt; occasional housekeeping for a long-running program, not a per-frame call. See [vblank](#vblank). |

### Whole surface — $10–$1F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $10 | `CLEAR` | 1 | `ARG0`: colour index | Fills the entire draw page. |

### Primitives — $20–$2F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $20 | `SET_PIXEL` | 5 | `ARG0-1` x, `ARG2-3` y, `ARG4` colour | One pixel. |
| $21 | `LINE` | 9 | `ARG0-1` x0, `ARG2-3` y0, `ARG4-5` x1, `ARG6-7` y1, `ARG8` colour | Line, both endpoints inclusive. |
| $22 | `RECT` | 9 | `ARG0-1` x, `ARG2-3` y, `ARG4-5` w, `ARG6-7` h, `ARG8` colour | 1-pixel outline; `x,y` is the top-left corner, `w`/`h` include the outline. |
| $23 | `RECT_FILL` | 9 | as `$22` | Filled rectangle. |

### Palette — $30–$3F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $30 | `PAL_SET` | 4 | `ARG0` index, `ARG1` r, `ARG2` g, `ARG3` b | Sets one entry from 24-bit RGB. |
| $31 | `PAL_LOAD` | 8 | `ARG0-5` blob descriptor, `ARG6` first index, `ARG7` count | Fetches `count` RGB triples into entries `first .. first+count-1`. `count = 0` is a no-op. `len` must equal `count * 3` and `first + count` must be ≤ 256, else `BAD_ARGS`. |

### Blit — $40–$4F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $40 | `BLIT` | 14 | `ARG0-5` blob descriptor, `ARG6-7` dstX, `ARG8-9` dstY, `ARG10-11` w, `ARG12-13` h | Fetches `w * h` bytes of 8bpp palette indices, row-major with stride `w`, into the draw page at `dstX,dstY`. Clipped like any drawing op. `len` must equal `w * h`, else `BAD_ARGS`. |
| $41 | `BLIT_KEYED` | 15 | as `$40`, plus `ARG14` key index | Same, but source pixels equal to `key` leave the destination untouched — the sprite case. |
| $42 | `READ_RECT` | 14 | `ARG0-5` destination descriptor, `ARG6-7` srcX, `ARG8-9` srcY, `ARG10-11` w, `ARG12-13` h | The reverse of `BLIT`: copies a `w * h` rectangle of the **draw page** back into your memory, row-major with stride `w`. The rectangle must lie entirely within the page (this one does **not** clip) else `BAD_ARGS`; `len` must equal `w * h`. |

### Info block

`GET_INFO` writes 16 bytes:

| Offset | Size | Contents |
|---|---|---|
| 0 | 3 | `"G64"` |
| 3 | 1 | API version, major |
| 4 | 1 | API version, minor |
| 5 | 2 | framebuffer width (320) |
| 7 | 2 | framebuffer height (200) |
| 9 | 1 | bits per pixel (8) |
| 10 | 1 | number of pages (2) |
| 11 | 1 | bitmap of implemented classes: bit0 = class 0, bit1 = class 1 (3D) |
| 12 | 1 | border width, each side (32) |
| 13 | 1 | border height, each side (36) |
| 14 | 2 | measured frame period, microseconds — **0 means the frame clock never calibrated**, and every vblank feature will answer `UNSUPPORTED` |

## Matrix and vector ops — $80–$9F

Two parallel sets of the same operations, differing only in element format:

- **$80–$8F — fixed point.** Elements are signed 16-bit **8.8** fixed point,
  little-endian: `$0100` = 1.0, `$FF00` = −1.0, `$0080` = 0.5. Range
  ±127.99, resolution 1/256. **2 bytes per element.** Products accumulate at
  full width and come back to 8.8 **rounded half away from zero and
  saturated** — a value past the range clamps to ±127.99 rather than
  wrapping.
- **$90–$9F — floating point.** Elements are 32-bit IEEE 754 single
  precision, little-endian. **4 bytes per element.**

Layouts are identical between the two sets; only the implied byte counts
differ. The low nibble picks the operation, so `$80`/`$90` are both
`MAT_MUL`, `$81`/`$91` both `MAT_ADD`, and so on.

Matrices are **row-major**, dimensions are given as bytes 1–255, and
operands are addressed with the 4-byte [compact descriptor](#compact-descriptor)
— no `len`, since dimensions and element size imply it. A dimension of 0 is
`BAD_ARGS`; an operand or result running past the end of its space, or a
result larger than 65535 bytes, is `OUT_OF_RANGE`.

Results are written back to the memory the destination descriptor points at,
before the `CMD_LO` write returns. Source and destination may be in
different spaces (compute from REU, write back to C64 RAM). Overlapping
source and destination is undefined.

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $80 / $90 | `MAT_MUL` | 15 | `ARG0` m, `ARG1` k, `ARG2` n, `ARG3-6` A, `ARG7-10` B, `ARG11-14` C | C(m x n) = A(m x k) · B(k x n) |
| $81 / $91 | `MAT_ADD` | 14 | `ARG0` m, `ARG1` n, `ARG2-5` A, `ARG6-9` B, `ARG10-13` C | C = A + B, elementwise, all m x n |
| $82 / $92 | `MAT_SUB` | 14 | as `$81` | C = A − B |
| $83 / $93 | `MAT_SCALE` | 12 fixed / 14 float | `ARG0` m, `ARG1` n, `ARG2-5` A, `ARG6-9` C, then the scalar inline: `ARG10-11` (8.8) or `ARG10-13` (float) | C = A · scalar |
| $84 / $94 | `MAT_TRANSPOSE` | 10 | `ARG0` m, `ARG1` n, `ARG2-5` A, `ARG6-9` C | C(n x m) = A(m x n) transposed |
| $85 / $95 | `MAT_IDENTITY` | 5 | `ARG0` n, `ARG1-4` C | C(n x n) = identity |
| $86 / $96 | `MAT_INVERSE` | 9 | `ARG0` n, `ARG1-4` A, `ARG5-8` C | C = A⁻¹ (n x n), **n ≤ 64** (larger is `BAD_ARGS`). A singular matrix sets `SINGULAR` and writes nothing. The elimination runs in double precision whatever the element format is, so an 8.8 operand keeps its precision through the inversion. |

A vector is just a matrix with one row or one column: transforming a
4-vector by a 4x4 matrix is `MAT_MUL` with m=4, k=4, n=1, and transforming
a batch of vertices is the same call with n = however many. Every dimension
is a single byte, so a batch is at most 255 vertices per call — split larger
sets across calls, advancing the operand and result addresses.

Everything not listed above — including $A0–$FF — is undefined: writing it
sets `ERRCODE = BAD_OPCODE` and does nothing else.

## vblank

Two ways to sync to the display, both driven by the same signal:

- **Polling**: read `STATUS` bit2. Set at each vblank, cleared by
  `VBLANK_ACK`. No IRQ handler needed — usable from BASIC.
- **Interrupt**: `VBLANK_ARM` with `ARG0 = 1` raises the cartridge IRQ line
  once per vblank. It stays armed and re-arms itself, like a raster IRQ —
  acknowledge with `VBLANK_ACK` on handler entry. `VBLANK_ARM` with
  `ARG0 = 0` returns to polling.

`VBLANK_ACK` is also what releases the IRQ line, so a handler that forgets
it gets exactly one interrupt.

### What the vblank signal actually is

gpu64 does not ask the display where the raster is — doing that means a
mailbox call to the VideoCore, which blocks for up to a frame and would
stall the bus-watch loop. Instead the frame period is **measured once at
boot** and extrapolated from a free-running microsecond timer. Both clocks
come off the same crystal, so the extrapolation holds, but it is not exact:

- Every vblank lands a small **constant offset** after the display's true
  one, because the boot measurement carries the mailbox's own latency. It is
  consistent frame to frame, which is what tear-free flipping needs.
- It **drifts** over long runs — on the order of a millisecond per several
  minutes. A program that animates for a long time should call
  `VBLANK_SYNC` occasionally (once a minute is plenty) to re-pin it.
  `VBLANK_ARM` does this for you when you arm, so a program that arms at
  start-up begins accurate.

If the boot measurement failed, there is no signal at all: `GET_INFO`
reports a frame period of 0, `STATUS` bit2 never sets, and `VBLANK_ARM(1)`,
`PAGE_FLIP(1)` and `VBLANK_SYNC` all return `UNSUPPORTED`. Immediate
`PAGE_FLIP` still works, so a program can fall back to flipping untimed.

### Tear-free animation

The intended shape, and what
[`gpu64_vblank_demo.a`](../Source/TestPRG/gpu64_vblank_demo.a) does:

1. `SET_DRAW_PAGE` 1 once, at the start. Every `PAGE_FLIP` after that swaps
   the pages for you, so the draw page is always the one that is not
   visible.
2. Wait for `STATUS` bit2, then `VBLANK_ACK`.
3. Draw the frame.
4. `PAGE_FLIP` with `ARG0 = 1`.
5. Wait for `STATUS` bit0 to clear — the flip has landed.

Step 5 is the only place gpu64 makes the C64 wait on something other than
its own work, and it is a spin on a register read, not a halt: the C64 is
free to do anything else instead.

## Error codes

`ERRCODE` is written by **every** dispatch, including successful ones, so a
stale value can never be mistaken for a fresh failure. `STATUS` bit1 is
simply `ERRCODE != OK`.

| Value | Name | Meaning |
|---|---|---|
| $00 | `OK` | Success. |
| $01 | `BAD_OPCODE` | No such opcode in the current class. |
| $02 | `BAD_CLASS` | `CMD_HI` selects a class this firmware doesn't implement. |
| $03 | `OUT_OF_RANGE` | A blob descriptor's `addr + len` runs past the end of its space, or `space` isn't 0 or 1. |
| $04 | `BAD_ARGS` | Arguments inconsistent for an otherwise valid opcode: `len` not matching `w * h` or `count * 3`, a page or flag byte outside its defined values, a palette range past 256, a matrix dimension of 0, a `READ_RECT` rectangle outside the page. |
| $05 | `SINGULAR` | `MAT_INVERSE` on a matrix with no inverse. Nothing was written to the destination. |
| $06 | `UNSUPPORTED` | The feature is not available on this hardware right now. In practice this means the frame clock did not calibrate at boot, which takes every vblank feature with it. |
| $07 | `BUSY` | A vblank-deferred `PAGE_FLIP` is still waiting for its frame boundary. The pending flip is untouched; poll `STATUS` bit0 and retry. |

A failed dispatch does nothing: no drawing, no data fetch, no state change.

## Examples

Clear the screen to blue (index 6), then draw a filled white box:

```asm
CMD_LO  = $df0c
ARG0    = $df11

        lda #6
        sta ARG0
        lda #$10        ; CLEAR
        sta CMD_LO

        lda #<100
        sta ARG0
        lda #>100
        sta ARG0+1      ; x = 100
        lda #<50
        sta ARG0+2
        lda #>50
        sta ARG0+3      ; y = 50
        lda #<80
        sta ARG0+4
        lda #>80
        sta ARG0+5      ; w = 80
        lda #<40
        sta ARG0+6
        lda #>40
        sta ARG0+7      ; h = 40
        lda #1
        sta ARG0+8      ; colour = white
        lda #$23        ; RECT_FILL
        sta CMD_LO
```

Blit a 32x32 sprite from C64 RAM at $C000 into the back page, then flip on
vblank:

```asm
        lda #0
        sta ARG0        ; space = C64 RAM
        lda #$00
        sta ARG0+1
        lda #$c0
        sta ARG0+2
        lda #$00
        sta ARG0+3      ; addr = $00c000
        lda #<1024
        sta ARG0+4
        lda #>1024
        sta ARG0+5      ; len = 32*32
        ; dstX, dstY, w, h in ARG6..ARG13 ...
        lda #$40        ; BLIT
        sta CMD_LO

        lda #1
        sta ARG0
        lda #$05        ; PAGE_FLIP at next vblank
        sta CMD_LO

wait    lda STATUS      ; bit0 clears when the flip lands
        and #$01
        bne wait
```

Multiply a 4x4 matrix at $2000 by a 4x1 vector at $2040, both 8.8 fixed
point, leaving the transformed vector at $2080 — all in C64 RAM:

```asm
        lda #4
        sta ARG0        ; m = 4
        sta ARG0+1      ; k = 4
        lda #1
        sta ARG0+2      ; n = 1

        lda #0
        sta ARG0+3      ; A: space = C64 RAM
        lda #$00
        sta ARG0+4
        lda #$20
        sta ARG0+5
        lda #$00
        sta ARG0+6      ; A: addr = $002000

        lda #0
        sta ARG0+7      ; B: space = C64 RAM
        lda #$40
        sta ARG0+8
        lda #$20
        sta ARG0+9
        lda #$00
        sta ARG0+10     ; B: addr = $002040

        lda #0
        sta ARG0+11     ; C: space = C64 RAM
        lda #$80
        sta ARG0+12
        lda #$20
        sta ARG0+13
        lda #$00
        sta ARG0+14     ; C: addr = $002080

        lda #$80        ; MAT_MUL, 8.8 fixed point
        sta CMD_LO      ; result is in $2080..$2087 when this returns
```

The float32 version is the same code with `#$90` in the last `lda` — and
16 bytes of result instead of 8.
