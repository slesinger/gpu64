#!/usr/bin/env python3
"""
Run a gpu64 conformance-suite .prg against the reference model and print
the C64 screen it produced.

    tools/prgsim/runsim.py Source/TestPRG/gpu64_test_system.prg

Exit status is 0 only if the program reached its summary line and that line
says VERDICT PASS, so this drops straight into a build script.

What this is for: the suite asserts what docs/api_design.md specifies. Run
here, a failure means the TEST is wrong -- the model is built from the same
document. Run on hardware, a failure means the FIRMWARE is wrong. Getting
the first kind out of the way on a PC is the whole point, because bench time
is the scarce resource and a red line at the bench should mean something.

Options:
    --no-vblank     model a display whose frame clock never calibrated, so
                    every vblank feature answers UNSUPPORTED. The suite is
                    expected to pass in this mode too -- it adapts.
    --alias-drop=OFF[,OFF...]
        Corrupt a C64-space blob write: each listed byte offset comes back as
        the byte at offset $FF of its own page. Models the hardware failure
        the blit suite is chasing, so its diagnostic lines can be validated
        against a known answer here rather than at the bench.

    --firmware-rounding
                    round 8.8 products the way the firmware currently does
                    rather than the way the reference specifies, to predict
                    which suite lines a given firmware will fail.
    --ppm=PATH      write what the HDMI output would be showing when the
                    program returned -- the visible page, through the
                    current palette, inside the border -- as a PPM. This is
                    how a demo gets verified on a PC instead of at the
                    bench.
    --key=NAME:FIRST-LAST
        Hold a key down from frame FIRST to frame LAST, counting page flips
        the way --stop-after does. Repeatable. Without it every key reads as
        released, which is what an unwired $dc01 does NOT do -- it reads 0,
        i.e. the whole keyboard held at once -- so a demo that steers by the
        keyboard needs this option for its desk run to mean anything.
        Names: W A S D Q E, F1 F3 F5 F7, SPACE, RETURN, RUNSTOP, and the
        letters and digits in the table below.
    --demo          do not require a VERDICT line. The conformance suite
                    judges itself and exit status follows its verdict; a
                    demo has nothing to judge, so returning cleanly is the
                    whole of the pass condition.
    --stop-after=N  report the STOP key pressed on the Nth poll rather than
                    the 4th. A demo polls STOP once a frame, so this is how
                    many frames it runs. THE DEFAULT OF 4 IS NOT A CHECK:
                    a defect whose onset is frame 98 is invisible to it, and
                    that has already happened once on this project. Any demo
                    change wants a run of several hundred.
    --frame-log     print one line per page flip: frame number, the drawn
                    page's non-zero pixel count, and the two status rows the
                    demo prints. This is what turns "it blinks" into a frame
                    number and a delta.
    --ppm-frame=N:PATH
                    write the page being flipped away at frame N. Repeatable.
    --trace N       dump the last N instructions if something goes wrong
    --max N         instruction budget (default 200 million)
"""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from cpu6502 import Cpu6502                                   # noqa: E402
import gpu64model                                              # noqa: E402
from gpu64model import Gpu64Model                             # noqa: E402

SCREEN = 0x0400
IO2 = 0xDF00

# The C64 keyboard matrix: name -> (column driven on $dc00, row bit on
# $dc01). Only what a demo is likely to steer by; add rows as needed.
KEY_MATRIX = {
    'DEL': (0, 0x01), 'RETURN': (0, 0x02), 'CRSRRIGHT': (0, 0x04),
    'F7': (0, 0x08), 'F1': (0, 0x10), 'F3': (0, 0x20), 'F5': (0, 0x40),
    'CRSRDOWN': (0, 0x80),
    '3': (1, 0x01), 'W': (1, 0x02), 'A': (1, 0x04), '4': (1, 0x08),
    'Z': (1, 0x10), 'S': (1, 0x20), 'E': (1, 0x40),
    '5': (2, 0x01), 'R': (2, 0x02), 'D': (2, 0x04), '6': (2, 0x08),
    'C': (2, 0x10), 'F': (2, 0x20), 'T': (2, 0x40), 'X': (2, 0x80),
    '7': (3, 0x01), 'Y': (3, 0x02), 'G': (3, 0x04), '8': (3, 0x08),
    'B': (3, 0x10), 'H': (3, 0x20), 'U': (3, 0x40), 'V': (3, 0x80),
    '9': (4, 0x01), 'I': (4, 0x02), 'J': (4, 0x04), '0': (4, 0x08),
    'M': (4, 0x10), 'K': (4, 0x20), 'O': (4, 0x40), 'N': (4, 0x80),
    'P': (5, 0x02), 'L': (5, 0x04),
    'SPACE': (7, 0x10), 'Q': (7, 0x40), 'RUNSTOP': (7, 0x80),
}

# Screen code -> ASCII, enough to read a result line back.
def screen_to_ascii(c):
    if c == 0x20:
        return ' '
    if 0x01 <= c <= 0x1A:
        return chr(ord('A') + c - 1)
    if 0x30 <= c <= 0x39:
        return chr(c)
    if c == 0x2E:
        return '.'
    if c == 0x2D:
        return '-'
    if c == 0x2F:
        return '/'
    if c == 0x00:
        return '@'
    return '?'


class Machine:
    def __init__(self, calibrated=True, stop_after=4, frame_log=False,
                 ppm_frames=None, key_script=None):
        self.mem = bytearray(65536)
        self.gpu = Gpu64Model(self.raw_read, self.raw_write, calibrated=calibrated)
        self.cpu = Cpu6502(self.read, self.write)
        self.stop_polls = 0
        self.stop_after = stop_after
        self.col7_reads = 0
        self.done = False
        self.final = None
        self.frame_log = frame_log
        self.ppm_frames = ppm_frames or {}
        self.frame = 0
        self.cia_pra = 0xFF
        self.key_script = list(key_script or [])

    # Straight RAM, used by the model's DMA: a blob fetch sees memory, not
    # the IO2 window, and never re-enters the register file.
    def raw_read(self, addr):
        return self.mem[addr & 0xFFFF]

    def raw_write(self, addr, val):
        self.mem[addr & 0xFFFF] = val & 0xFF

    def read(self, addr):
        if 0xDF00 <= addr <= 0xDFFF:
            return self.gpu.read_reg(addr - IO2)
        if addr == 0xDC01:
            return self.keyboard()
        if addr == 0xDC00:
            return self.cia_pra
        if addr == 0xFFE1:
            return 0x60                     # RTS -- intercepted below
        return self.mem[addr]

    def keyboard(self):
        # Both halves of the matrix are active low: a column is selected by
        # a ZERO in $dc00 and a pressed key answers with a ZERO row bit.
        if not (self.cia_pra & 0x80):
            self.col7_reads += 1
        rows = 0xFF
        for name in self.keys_down():
            col, bit = KEY_MATRIX[name]
            if not (self.cia_pra & (1 << col)):
                rows &= ~bit
        return rows & 0xFF

    def keys_down(self):
        for name, first, last in self.key_script:
            if first <= self.frame <= last:
                yield name
        if self.stop_in_matrix():
            yield 'RUNSTOP'

    def stop_in_matrix(self):
        # A demo that scans the matrix itself rather than asking the KERNAL
        # -- quake does, because $ffe1 and its own sei-guarded scan disagree
        # on hardware -- has to be able to finish here too. Once it has read
        # column 7 more than --stop-after times, work the key the way a hand
        # would: held down long enough to be believed, released, then held
        # again for the "press RUN/STOP to leave" wait at the end. Counting
        # reads and not frames matters, because that final wait flips no
        # pages and so never advances the frame number.
        k = self.col7_reads - self.stop_after
        if k <= 0:
            return False
        return k <= 4 or k > 8

    def write(self, addr, val):
        if addr == 0xDC00:
            self.cia_pra = val & 0xFF
            return
        if 0xDF00 <= addr <= 0xDFFF:
            # A page flip is the frame boundary, and it is the only one a
            # demo tells us about. Sample on the way in, while the page that
            # was just drawn is still the draw page.
            flip = (addr == 0xDF0C and self.gpu.cmd_hi == 0
                    and (val & 0xFF) == 0x05)
            if flip:
                self.on_flip()
            self.gpu.write_reg(addr - IO2, val)
            return
        self.mem[addr] = val & 0xFF

    def on_flip(self):
        self.frame += 1
        path = self.ppm_frames.get(self.frame)
        if path is not None:
            write_ppm(path, self.gpu, page=self.gpu.draw_page)
        if not self.frame_log:
            return
        page = self.gpu.pages[self.gpu.draw_page]
        ink = sum(1 for b in page if b)
        rows = self.screen()
        status = ' | '.join(r.strip() for r in rows[20:25] if r.strip())
        print("frame %4d  ink %6d  %s" % (self.frame, ink, status))

    def load_prg(self, path):
        data = open(path, 'rb').read()
        addr = data[0] | (data[1] << 8)
        body = data[2:]
        self.mem[addr:addr + len(body)] = body
        return addr, len(body)

    def kernal(self):
        """Intercept the two KERNAL entry points the harness uses."""
        pc = self.cpu.pc
        if pc == 0xFFE1:                    # STOP key
            # Report "not pressed" a few times so a caller that expects to
            # wait actually waits, then report pressed so the run finishes.
            self.stop_polls += 1
            pressed = self.stop_polls > self.stop_after
            self.cpu.p = (self.cpu.p | 0x02) if pressed else (self.cpu.p & ~0x02)
            self.rts()
            return True
        if pc == 0xE544:                    # clear screen
            # The harness clears on the way out so BASIC's READY. does not
            # land on a result line. Snapshot first -- that final screen is
            # the entire output of the run.
            if self.final is None:
                self.final = self.screen()
            for i in range(1000):
                self.mem[SCREEN + i] = 0x20
            self.rts()
            return True
        return False

    def rts(self):
        lo = self.cpu.pop()
        hi = self.cpu.pop()
        self.cpu.pc = (((hi << 8) | lo) + 1) & 0xFFFF

    def run(self, start, max_insns=200_000_000, trace=0):
        self.cpu.pc = start
        self.cpu.sp = 0xFD
        # The harness returns with RTS; land on a sentinel we can detect.
        self.cpu.push(0xFF)
        self.cpu.push(0xFE)                 # returns to $FFFF
        ring = []
        n = 0
        while n < max_insns:
            if self.cpu.pc == 0x10000 - 1 or self.cpu.pc == 0xFFFF:
                self.done = True
                break
            if self.kernal():
                n += 1
                continue
            if trace:
                ring.append((self.cpu.pc, self.mem[self.cpu.pc]))
                if len(ring) > trace:
                    ring.pop(0)
            try:
                self.cpu.step()
            except RuntimeError as e:
                print("CPU fault: %s" % e, file=sys.stderr)
                for pc, op in ring:
                    print("  $%04X: $%02X" % (pc, op), file=sys.stderr)
                return False, n
            # A rough C64 clock, so a vblank poll loop terminates. Exactness
            # does not matter: nothing in the suite measures time, it only
            # waits for a signal that must eventually arrive.
            self.gpu.advance(4)
            n += 1
        return self.done, n

    def screen(self):
        rows = []
        for r in range(25):
            rows.append(''.join(screen_to_ascii(self.mem[SCREEN + r * 40 + c])
                                for c in range(40)).rstrip())
        return rows


def write_ppm(path, gpu, page=None):
    """The visible page as the display would show it, border included."""
    w = gpu64model.FB_W + 2 * gpu64model.BORDER_W
    h = gpu64model.FB_H + 2 * gpu64model.BORDER_H
    pal = gpu.palette
    page = gpu.pages[gpu.visible_page if page is None else page]
    border = bytes(pal[gpu.border * 3:gpu.border * 3 + 3])
    rows = [border * w] * gpu64model.BORDER_H
    for y in range(gpu64model.FB_H):
        line = bytearray(border * gpu64model.BORDER_W)
        base = y * gpu64model.FB_W
        for x in range(gpu64model.FB_W):
            c = page[base + x] * 3
            line += pal[c:c + 3]
        line += border * gpu64model.BORDER_W
        rows.append(bytes(line))
    rows += [border * w] * gpu64model.BORDER_H
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (w, h))
        f.write(b''.join(rows))


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('--')]
    opts = [a for a in argv[1:] if a.startswith('--')]
    if not args:
        print(__doc__)
        return 2

    calibrated = '--no-vblank' not in opts
    gpu64model.FIRMWARE_ROUNDING[0] = '--firmware-rounding' in opts
    for o in opts:
        if o.startswith('--alias-drop='):
            gpu64model.ALIAS_DROP[0] = [int(x, 0) for x in o.split('=')[1].split(',')]
    trace = 0
    max_insns = 200_000_000
    ppm = None
    stop_after = 4
    frame_log = '--frame-log' in opts
    ppm_frames = {}
    demo = '--demo' in opts
    key_script = []
    for o in opts:
        if o.startswith('--key='):
            name, span = o.split('=', 1)[1].split(':', 1)
            first, last = (span.split('-', 1) + [span])[:2]
            name = name.upper()
            if name not in KEY_MATRIX:
                print("unknown key: %s" % name)
                return 2
            key_script.append((name, int(first), int(last)))
    for o in opts:
        if o.startswith('--stop-after='):
            stop_after = int(o.split('=')[1])
        if o.startswith('--ppm-frame='):
            n, path = o.split('=', 1)[1].split(':', 1)
            ppm_frames[int(n)] = path
        if o.startswith('--trace'):
            trace = int(o.split('=')[1]) if '=' in o else 40
        if o.startswith('--max='):
            max_insns = int(o.split('=')[1])
        if o.startswith('--ppm='):
            ppm = o.split('=', 1)[1]

    m = Machine(calibrated=calibrated, stop_after=stop_after,
                frame_log=frame_log, ppm_frames=ppm_frames,
                key_script=key_script)
    addr, size = m.load_prg(args[0])
    # A BASIC stub sits at $0801; the code itself starts at $0810.
    start = 0x0810
    ok, n = m.run(start, max_insns=max_insns, trace=trace)

    rows = m.final if m.final is not None else m.screen()
    print("--- %s  (%s, %d bytes at $%04X, %d instructions) ---"
          % (os.path.basename(args[0]),
             "vblank available" if calibrated else "no frame clock",
             size, addr, n))
    for i, r in enumerate(rows):
        if r:
            print("%2d| %s" % (i, r))
    print("--- %d dispatches ---" % m.gpu.dispatches)

    if ppm is not None and ok:
        write_ppm(ppm, m.gpu)
        print("--- wrote %s ---" % ppm)

    if not ok:
        print("PROGRAM DID NOT RETURN (instruction budget or fault)", file=sys.stderr)
        return 1
    if demo:
        return 0
    verdict = '\n'.join(rows)
    if 'VERDICT PASS' in verdict:
        return 0
    if 'VERDICT FAIL' in verdict:
        return 1
    print("NO VERDICT ON SCREEN", file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
