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

## Reading order

1. **[getting-started.md](getting-started.md)** — register map, what a
   command costs the C64, issuing a command, blob descriptors, the ten demo
   programs and how to build/run them. Start here.
2. **[class0-2d-reference.md](class0-2d-reference.md)** — system, whole
   surface, primitives, palette, blit, info/health blocks, matrix and
   vector ops.
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
8. **[examples.md](examples.md)** — three complete, short programs covering
   the common shapes a gpu64 program takes.
9. **[known-gaps.md](known-gaps.md)** — a usability audit from the
   perspective of someone shipping a real game: what's missing, what's easy
   to get wrong, what to size a level around.

Each file closes with a "See also" line pointing at its `project/`
counterpart, where the rationale and bring-up history live, for anyone who
wants to go deeper than "how do I call this."
