# gpu64 API reference

gpu64 turns a Raspberry Pi 3A+ cartridge into a second, independent HDMI
screen for the Commodore 64 — not a VIC-II overlay, but a separate display
(think C128's 80-column screen) that the 6502 drives through a small
command API mapped into IO2 (`$DF0B`-`$DFFF`). A program writes arguments
into `ARG0`-`ARG15`, then writes an opcode to `CMD_LO`; that write halts the
C64 until gpu64 finishes and writes `ERRCODE`. Everything in this directory
documents that API from the point of view of someone writing a C64 program
against it — a game, a demo, a CAD-style tool. Nothing here covers how
gpu64 itself is built, tested, or debugged; that material lives in
[`project/`](../project/) instead.

Opcodes are grouped into **classes**, selected by `CMD_HI`:

- **Class 0** — 2D drawing, palette, blit, system/info, and the matrix
  coprocessor. Always compiled in; every program uses it.
- **Class 1** — the retained 3D scene graph (meshes, nodes, render ops on
  core 1). gpu64's forward path for 3D as of the 2026-08-28 decision.
- **Class 2** — the earlier first-person raster layer (walls, sectors,
  things, per-frame polygon batches). Frozen but still working; superseded
  by class 1 for new work.

## The v1 surface

As of 2026-09-11 the API below is **frozen for v1**: opcodes, argument
layouts and error codes will not change meaning, and anything added later
takes a new opcode number rather than a new argument on an existing one.
Three things are deliberately outside that freeze, and a program should not
be written as if they were settled:

- **Free-running mode** (`LOOP_START` with `ARG0` = 1) answers `UNSUPPORTED`
  and is staged. Handshake mode is the whole of v1's loop. `LOOP_START`
  itself is optional in handshake mode — `SCENE_COMMIT` starts a stopped
  loop — so it survives v1 only as the place you will eventually ask for
  free-running.
- **Class 2** is frozen, not removed. It still works; it gets no new
  opcodes.
- **Visibility** (PVS, frustum culling) is not in v1 at all. Size levels
  accordingly — see [known-gaps.md](known-gaps.md) and
  [level-scale-visibility.md](level-scale-visibility.md).

Two v1 rules are not optional for a program that runs for more than a few
seconds, because the IO2 bus drops roughly one register write in tens of
thousands and a retained scene remembers every one of them: refresh your
node state on a rotation ([state-refresh.md](state-refresh.md)), and fence
every readback with `SET_DMA_WINDOW` before you use one.

## Reading order

1. **[getting-started.md](getting-started.md)** — register map, what a
   command costs the C64, issuing a command, blob descriptors, the eleven
   demo programs and how to build/run them. Start here.
2. **[class0-2d-reference.md](class0-2d-reference.md)** — system, whole
   surface, primitives, palette, blit, the 80x50 text mode, info/health
   blocks, matrix and vector ops.
3. **[class1-3d-mesh-reference.md](class1-3d-mesh-reference.md)** — the
   retained scene graph: meshes, nodes, render ops, current build status.
4. **[class2-raster-reference.md](class2-raster-reference.md)** — the
   frozen raster layer: walls, sectors, things, per-frame polygon batches.
5. **[level-scale-visibility.md](level-scale-visibility.md)** — a frozen,
   historical requirements gap (PVS/frustum culling for level-scale
   worlds), kept for reference, not a live roadmap.
6. **[vblank-and-animation.md](vblank-and-animation.md)** — syncing to the
   frame and the tear-free double-buffering idiom.
7. **[error-codes.md](error-codes.md)** — the shared `ERRCODE` table.
8. **[state-refresh.md](state-refresh.md)** — keeping a retained scene
   correct: why a lost argument write is permanent, and the bounded-lifetime
   refresh ring that closes it. Read this before shipping a game.
9. **[examples.md](examples.md)** — three complete, short programs covering
   the common shapes a gpu64 program takes.
10. **[known-gaps.md](known-gaps.md)** — a usability audit from the
   perspective of someone shipping a real game: what's missing, what's easy
   to get wrong, what to size a level around.

Each file closes with a "See also" line pointing at its `project/`
counterpart, where the rationale and bring-up history live, for anyone who
wants to go deeper than "how do I call this."
