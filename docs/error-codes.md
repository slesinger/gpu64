# Error codes

`ERRCODE` (`$DF0E`) is written by **every** dispatch, success or failure —
there is no code path that leaves it stale. `STATUS` bit1 is exactly
`ERRCODE != OK`, so a caller that only wants pass/fail can check one bit
instead of decoding the value.

| Code | Name | Meaning |
|---|---|---|
| $00 | `OK` | Command completed normally. |
| $01 | `BAD_OPCODE` | `CMD_LO` named an opcode not defined for the current `CMD_HI` class. |
| $02 | `BAD_CLASS` | `CMD_HI` named a class not compiled into this firmware build (see the info block's implemented-classes bitmap), or — for a batch op — a class-2 record referenced a batch kind the build doesn't support. |
| $03 | `OUT_OF_RANGE` | An address, length, index, or dimension argument would read or write outside the space it names. |
| $04 | `BAD_ARGS` | An argument combination is structurally invalid independent of range — e.g. a zero matrix dimension. |
| $05 | `SINGULAR` | `MAT_INVERSE` was asked to invert a matrix with no inverse. |
| $06 | `UNSUPPORTED` | The opcode is defined but not available right now — e.g. a vblank opcode when the boot-time frame period measurement failed, or a class-1 opcode not yet built (`LOOP_START`/`SCENE_COMMIT` as of this writing). |
| $07 | `BUSY` | A previous asynchronous operation on the same resource hasn't finished. |
| $08 | `OUT_OF_MEMORY` | A resource table (textures, meshes, nodes, ...) is full. |
| $09 | (reserved) | |
| $0A | `BAD_ID` | A resource id argument doesn't name a live resource. |
| $0C | `WORKER_TIMEOUT` | Class-1-specific: core 0 gave up waiting on core 1's render worker after a generous worst-case window. Designed to be unreachable in normal operation — if you see it, something is genuinely stuck, not just slow. |

See also: [class2-raster-reference.md](class2-raster-reference.md) for the
handful of class-2-specific codes and situations layered on top of this
shared table.
