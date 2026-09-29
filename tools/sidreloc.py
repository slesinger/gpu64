#!/usr/bin/env python3
"""
sidreloc.py - move a PSID tune to another page, and prove the move.

The game's title tunes arrive as PSID files with fixed load addresses, and
Quake.sid's $1000 is the middle of the game's code. A player routine has
no source to reassemble, so it is relocated the way sidreloc does it: run
it, and watch which bytes end up being used as the high byte of an
address.

  * An executed instruction whose absolute operand points into the tune
    has its operand's high byte moved.
  * A value that reaches the high byte of an indirect pointer (a (zp),y
    or (zp,x) access, a JMP (ind)), the high byte of a self-modified
    operand, or the high byte of an RTS return address is traced back to
    the tune byte (or immediate operand) it was loaded from, and that
    byte is moved.

Provenance travels through loads, stores, register transfers, the stack,
INC/DEC and the ALU (so "lda hi / adc #0 / sta ptr+1" still points back
at hi). Only bytes whose value lies inside the tune's pages are touched,
and the move is by whole pages, so low bytes never change.

The proof is the point: the relocated tune and the original are run side
by side for --frames play calls, and every SID register write of every
frame must match. A path the tune never took in that time cannot be
proved, so run it for longer than the song's loop.

Usage:
  tools/sidreloc.py IN.sid OUT.bin --page=0xB0 [--frames=20000]

OUT.bin is a C64 .prg-style image: two bytes of load address, then the
tune. init and play move by the same number of pages.
"""
import struct
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                'prgsim'))
from cpu6502 import Cpu6502                                  # noqa: E402

# opcode -> (kind, mode). kind: 'ld' A/X/Y, 'st' A/X/Y, 'alu' (reads mem
# into A), 'rmw' (read-modify-write on memory), 'jmp', 'jsr', or 'other'.
# Only what provenance needs is classified; everything else is 'other'.
MODES = {}


def _m(codes, kind, reg, mode):
    for c in codes:
        MODES[c] = (kind, reg, mode)


_m([0xA9], 'ld', 'a', 'imm'); _m([0xA5], 'ld', 'a', 'zp')
_m([0xB5], 'ld', 'a', 'zpx'); _m([0xAD], 'ld', 'a', 'abs')
_m([0xBD], 'ld', 'a', 'absx'); _m([0xB9], 'ld', 'a', 'absy')
_m([0xA1], 'ld', 'a', 'indx'); _m([0xB1], 'ld', 'a', 'indy')
_m([0xA2], 'ld', 'x', 'imm'); _m([0xA6], 'ld', 'x', 'zp')
_m([0xB6], 'ld', 'x', 'zpy'); _m([0xAE], 'ld', 'x', 'abs')
_m([0xBE], 'ld', 'x', 'absy')
_m([0xA0], 'ld', 'y', 'imm'); _m([0xA4], 'ld', 'y', 'zp')
_m([0xB4], 'ld', 'y', 'zpx'); _m([0xAC], 'ld', 'y', 'abs')
_m([0xBC], 'ld', 'y', 'absx')
_m([0x85], 'st', 'a', 'zp'); _m([0x95], 'st', 'a', 'zpx')
_m([0x8D], 'st', 'a', 'abs'); _m([0x9D], 'st', 'a', 'absx')
_m([0x99], 'st', 'a', 'absy'); _m([0x81], 'st', 'a', 'indx')
_m([0x91], 'st', 'a', 'indy')
_m([0x86], 'st', 'x', 'zp'); _m([0x96], 'st', 'x', 'zpy')
_m([0x8E], 'st', 'x', 'abs')
_m([0x84], 'st', 'y', 'zp'); _m([0x94], 'st', 'y', 'zpx')
_m([0x8C], 'st', 'y', 'abs')
for base in (0x69, 0xE9, 0x29, 0x09, 0x49):     # ADC SBC AND ORA EOR
    _m([base], 'alu', 'a', 'imm'); _m([base - 4], 'alu', 'a', 'zp')
    _m([base + 12], 'alu', 'a', 'zpx'); _m([base + 4], 'alu', 'a', 'abs')
    _m([base + 20], 'alu', 'a', 'absx'); _m([base + 16], 'alu', 'a', 'absy')
    _m([base - 8], 'alu', 'a', 'indx'); _m([base + 8], 'alu', 'a', 'indy')
for c, mode in ((0xC9, 'imm'), (0xC5, 'zp'), (0xD5, 'zpx'), (0xCD, 'abs'),
                (0xDD, 'absx'), (0xD9, 'absy'), (0xC1, 'indx'),
                (0xD1, 'indy'), (0xE0, 'imm'), (0xE4, 'zp'), (0xEC, 'abs'),
                (0xC0, 'imm'), (0xC4, 'zp'), (0xCC, 'abs'), (0x24, 'zp'),
                (0x2C, 'abs')):
    _m([c], 'cmp', None, mode)
for base in (0xE6, 0xC6, 0x06, 0x46, 0x26, 0x66):   # INC DEC ASL LSR ROL ROR
    _m([base], 'rmw', None, 'zp'); _m([base + 0x10], 'rmw', None, 'zpx')
    _m([base + 8], 'rmw', None, 'abs'); _m([base + 0x18], 'rmw', None, 'absx')
_m([0x4C], 'jmp', None, 'abs'); _m([0x6C], 'jmpi', None, 'ind')
_m([0x20], 'jsr', None, 'abs')

ABS_MODES = ('abs', 'absx', 'absy', 'ind')


class Tracer:
    def __init__(self, load, body):
        self.lo, self.hi = load, load + len(body)
        self.plo, self.phi = load >> 8, (self.hi - 1) >> 8
        self.mem = bytearray(65536)
        self.mem[load:self.hi] = body
        self.orig = bytes(body)
        self.sid = []
        self.cpu = Cpu6502(self.rd, self.wr)
        self.pa = self.px = self.py = frozenset()
        self.pm = {}                     # address -> provenance of its byte
        self.stack = {}
        self.marks = set()               # tune offsets to move
        self.code = set()                # executed opcode addresses

    def rd(self, a):
        return self.mem[a]

    def wr(self, a, v):
        if 0xD400 <= a < 0xD420:
            self.sid.append((a - 0xD400, v))
        self.mem[a] = v

    def intune(self, a):
        return self.lo <= a < self.hi

    def inpages(self, v):
        return self.plo <= v <= self.phi

    def prov(self, a):
        if a in self.pm:
            return self.pm[a]
        return frozenset([a]) if self.intune(a) else frozenset()

    def mark(self, provs):
        for a in provs:
            if self.intune(a) and self.inpages(self.orig[a - self.lo]):
                self.marks.add(a - self.lo)

    def ea(self, mode, pc):
        c, m = self.cpu, self.mem
        op1 = m[(pc + 1) & 0xFFFF]
        w = op1 | (m[(pc + 2) & 0xFFFF] << 8)
        if mode == 'imm':
            return (pc + 1) & 0xFFFF
        if mode == 'zp':
            return op1
        if mode == 'zpx':
            return (op1 + c.x) & 0xFF
        if mode == 'zpy':
            return (op1 + c.y) & 0xFF
        if mode == 'abs':
            return w
        if mode == 'absx':
            return (w + c.x) & 0xFFFF
        if mode == 'absy':
            return (w + c.y) & 0xFFFF
        if mode == 'indx':
            z = (op1 + c.x) & 0xFF
            self.mark(self.prov((z + 1) & 0xFF))
            return m[z] | (m[(z + 1) & 0xFF] << 8)
        if mode == 'indy':
            self.mark(self.prov((op1 + 1) & 0xFF))
            return ((m[op1] | (m[(op1 + 1) & 0xFF] << 8)) + c.y) & 0xFFFF
        if mode == 'ind':
            self.mark(self.prov((w + 1) & 0xFFFF))
            return w
        return None

    def step(self):
        c, m = self.cpu, self.mem
        pc = c.pc
        op = m[pc]
        self.code.add(pc)
        kind, reg, mode = MODES.get(op, ('other', None, None))
        if mode in ABS_MODES:
            hiaddr = (pc + 2) & 0xFFFF
            target = m[(pc + 1) & 0xFFFF] | (m[hiaddr] << 8)
            if self.intune(target) or self.inpages(m[hiaddr]):
                # The operand itself, and whatever wrote it if it was
                # self-modified.
                if hiaddr in self.pm:
                    self.mark(self.pm[hiaddr])
                if self.intune(target):
                    self.mark([hiaddr])
        ea = self.ea(mode, pc) if mode else None
        if kind == 'ld':
            p = self.prov(ea)
            setattr(self, 'p' + reg, p)
        elif kind == 'st':
            self.pm[ea] = getattr(self, 'p' + reg)
        elif kind == 'alu':
            self.pa = self.pa | (self.prov(ea) if mode != 'imm' else
                                 frozenset([ea]))
        elif op == 0xAA:
            self.px = self.pa
        elif op == 0xA8:
            self.py = self.pa
        elif op == 0x8A:
            self.pa = self.px
        elif op == 0x98:
            self.pa = self.py
        elif op == 0x48:
            self.stack[c.sp] = self.pa
        elif op == 0x68:
            self.pa = self.stack.get((c.sp + 1) & 0xFF, frozenset())
        elif op == 0x60:
            # RTS: the high byte of the return address.
            self.mark(self.stack.get((c.sp + 2) & 0xFF, frozenset()))
        elif op == 0x20:
            self.stack[c.sp] = frozenset()
            self.stack[(c.sp - 1) & 0xFF] = frozenset()
        c.step()

    def call(self, pc, a=0):
        c = self.cpu
        c.a = a
        c.sp = 0xFD
        c.push(0xFF)
        c.push(0xFE)
        self.stack[0xFD] = self.stack[0xFC] = frozenset()
        c.pc = pc
        n = 0
        while c.pc != 0xFFFF:
            self.step()
            n += 1
            if n > 2_000_000:
                raise RuntimeError('runaway at $%04X' % c.pc)
        out = self.sid
        self.sid = []
        return out


def parse(path):
    d = open(path, 'rb').read()
    ver, off, load, init, play, songs, start = struct.unpack('>HHHHHHH',
                                                            d[4:18])
    body = d[off:]
    if load == 0:
        load = body[0] | (body[1] << 8)
        body = body[2:]
    return load, init, play, start, bytes(body)


def run(load, init, play, body, frames, song):
    t = Tracer(load, body)
    frames_out = [t.call(init, song)]
    for _ in range(frames):
        frames_out.append(t.call(play))
    return t, frames_out


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('--')]
    opts = dict(a[2:].split('=', 1) for a in argv[1:] if a.startswith('--'))
    if len(args) != 2 or 'page' not in opts:
        print(__doc__)
        return 2
    page = int(opts['page'], 0)
    frames = int(opts.get('frames', '20000'))
    load, init, play, start, body = parse(args[0])
    delta = page - (load >> 8)
    t, ref = run(load, init, play, body, frames, start - 1)
    moved = bytearray(body)
    for off in sorted(t.marks):
        moved[off] = (moved[off] + delta) & 0xFF
    nload = load + delta * 256
    _, got = run(nload, init + delta * 256, play + delta * 256, bytes(moved),
                 frames, start - 1)
    bad = [i for i in range(len(ref)) if ref[i] != got[i]]
    print('%s: $%04X-$%04X -> $%04X-$%04X, %d bytes moved, %d frames, '
          'init $%04X play $%04X' %
          (args[0], load, load + len(body) - 1, nload,
           nload + len(body) - 1, len(t.marks), frames,
           init + delta * 256, play + delta * 256))
    if bad:
        print('FAIL: SID writes differ from frame %d (%d frames differ)' %
              (bad[0], len(bad)))
        return 1
    print('PASS: every SID write of every frame matches the original')
    with open(args[1], 'wb') as f:
        f.write(bytes([nload & 0xFF, nload >> 8]) + bytes(moved))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
