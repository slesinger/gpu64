#!/usr/bin/env python3
"""
Run a gpu64 conformance-suite .prg against the reference model and print
the C64 screen it produced.

    tools/prgsim/runsim.py Source/TestPRG/gpu64_test_system.prg

Exit status is 0 only if the program reached its summary line and that line
says VERDICT PASS, so this drops straight into a build script.

What this is for: the suite asserts what docs/README.md and the files it
indexes specify. Run
here, a failure means the TEST is wrong -- the model is built from the same
document. Run on hardware, a failure means the FIRMWARE is wrong. Getting
the first kind out of the way on a PC is the whole point, because bench time
is the scarce resource and a red line at the bench should mean something.

Options:
    --no-vblank     model a display whose frame clock never calibrated, so
                    every vblank feature answers UNSUPPORTED. The suite is
                    expected to pass in this mode too -- it adapts.
    --dump-scene        print the retained scene node table at the end
    --level=FILE        stand in for RAD/level.g64lev on the Pi's SD card, so
                        LOAD_LEVEL/LEVEL_STEP build a real level. Without it
                        LOAD_LEVEL answers BAD_ARGS, as it does on a card that
                        has no level on it
    --clip-fault=N[:KIND]  damage the Nth CLIP_MOVE answer past its checksum:
                        x (default) its x high byte, solid an ALLSOLID answer
                        that stays put; void makes answer N and every one
                        after a floorless free fall
    --bus-fault-until=F stop injecting after RENDERED frame F -- not the
                       demo's own frame counter, which runs ahead of it
                       whenever commits are missed
    --bus-fault-from=F  start injecting at RENDERED frame F, so setup runs
                       clean and the run measures what the frame loop
                       repairs rather than whether setup survived
    --bus-fault=KIND:N
        Make every Nth access to the gpu64 register window ($DF0B-$DFFF) go
        wrong, the two ways the hardware is known to be able to make one go
        wrong. KIND is:
          drop  the access never reaches the firmware at all. A read
                answers $FF off a floating bus and is not counted; a write
                simply does not happen and is not counted.
          addr  the access IS serviced, but at a mis-sampled address: bit
                6 comes back set, which is the shape of damage real
                hardware can do (A0-A3 are on dedicated GPIOs, A4-A7 on the
                multiplexed ones). A read answers $FF, a write
                is discarded, and both are counted -- including in the
                bad-address counters GET_HEALTH reports.
          both  alternate between the two, so a probe that has to tell
                them apart has to tell them apart in the same run.
          data  a WRITE is serviced at the right address but with one data
                bit flipped (the bit rotates 0..7 from fault to fault);
                reads are untouched. Bench run 38 proved this one exists:
                SCENE_COMMITs answered BAD_OPCODE, which only a corrupted
                CMD_LO byte can produce -- and a corrupted opcode that
                lands on a VALID one is a phantom command. This is the kind
                that tests a state refresh ring against phantoms rather
                than against lost ARG writes.
        There is no defect here to find: this exists so that a probe's
        diagnosis of a defect can be checked against a known answer on a PC.
        Source/TestPRG/gpu64_probe_bus.a is the one that needs it, and its
        header says which verdict each kind must produce. The injector is
        indiscriminate -- it will happily eat a CMD_LO write and turn a
        command into nothing -- so keep N well above the number of accesses
        any single command costs.

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
    --c1-stream=PATH
        Write the class-1 frame stream -- one record per accepted
        SCENE_COMMIT, plus the resource blobs beside it -- for
        tools/hostsim/scenesim to render with the firmware's own renderer.
        The model does not rasterise class 1 (see tools/prgsim/gpu64class1.py),
        so this, not --ppm, is how a class-1 demo gets a picture on a PC.
        For a class-1 demo a "frame" is an accepted SCENE_COMMIT, so
        --stop-after counts those.
    --chain=PRG     run PRG first, on the same model, before the program
        named on the command line. Repeatable, applied in order. This is how
        a state leak BETWEEN programs is reproduced on a PC: the RAD menu
        launches a .prg without resetting the C64, so gpu64 keeps whatever
        the previous program left behind -- its palette, its draw/visible
        page pairing, its framebuffer contents, its display mode. Each
        chained program gets its own --stop-after budget and its screen is
        discarded; only the last program's screen and PPM are reported.
        A demo that comes up right after a reset and wrong after another
        demo is exactly this, and $0B FULL_RESET is the cure.
    --trace N       dump the last N instructions if something goes wrong
    --max N         instruction budget (default 200 million)
"""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from cpu6502 import Cpu6502                                   # noqa: E402
import gpu64model                                              # noqa: E402
import gpu64class1                                            # noqa: E402
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
    'Z': (1, 0x10), 'S': (1, 0x20), 'E': (1, 0x40), 'LSHIFT': (1, 0x80),
    '5': (2, 0x01), 'R': (2, 0x02), 'D': (2, 0x04), '6': (2, 0x08),
    'C': (2, 0x10), 'F': (2, 0x20), 'T': (2, 0x40), 'X': (2, 0x80),
    '7': (3, 0x01), 'Y': (3, 0x02), 'G': (3, 0x04), '8': (3, 0x08),
    'B': (3, 0x10), 'H': (3, 0x20), 'U': (3, 0x40), 'V': (3, 0x80),
    '9': (4, 0x01), 'I': (4, 0x02), 'J': (4, 0x04), '0': (4, 0x08),
    'M': (4, 0x10), 'K': (4, 0x20), 'O': (4, 0x40), 'N': (4, 0x80),
    'PLUS': (5, 0x01), 'P': (5, 0x02), 'L': (5, 0x04), 'MINUS': (5, 0x08),
    'SPACE': (7, 0x10), 'Q': (7, 0x40), 'RUNSTOP': (7, 0x80),
}

# Screen code -> ASCII, enough to read a result line back.
def screen_to_ascii(c):
    if c == 0x20:
        return ' '
    if 0x01 <= c <= 0x1A:
        return chr(ord('A') + c - 1)
    if 0x21 <= c <= 0x3F:            # digits and punctuation: screen
        return chr(c)                   #   code == ASCII here
    if c == 0x00:
        return '@'
    return '?'


class Machine:
    def __init__(self, calibrated=True, stop_after=4, frame_log=False,
                 ppm_frames=None, key_script=None, c1_stream=None,
                 level=None):
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
        # A class-1 program never issues PAGE_FLIP: the autonomous loop owns
        # the framebuffer, and its frame boundary is the accepted
        # SCENE_COMMIT the model reports here instead.
        self.gpu.frame_hook = self.on_c1_frame
        # The level file the firmware reads off the SD card at start-up, which
        # is where LOAD_LEVEL gets a level from. Absent, LOAD_LEVEL answers
        # BAD_ARGS exactly as it does on a card with no RAD/level.g64lev --
        # which is a case worth being able to run.
        if level:
            with open(level, 'rb') as f:
                self.gpu.c1_level_data = f.read()
        self.c1_stream = None
        if c1_stream:
            self.c1_stream = open(c1_stream, 'w')
            self.c1_stream.write(gpu64class1.FRAME_STREAM_DOC)
            self.gpu.c1_stream = self.c1_stream
            self.gpu.c1_stream_dir = os.path.dirname(os.path.abspath(c1_stream))

    # Straight RAM, used by the model's DMA: a blob fetch sees memory, not
    # the IO2 window, and never re-enters the register file.
    def raw_read(self, addr):
        return self.mem[addr & 0xFFFF]

    def raw_write(self, addr, val):
        self.mem[addr & 0xFFFF] = val & 0xFF

    # --bus-fault: which kind, how often, and how many window accesses have
    # gone by. Class attributes so an instance that predates the option
    # still behaves.
    bus_fault_kind = None
    bus_fault_every = 0
    bus_fault_n = 0
    bus_fault_until = -1
    bus_fault_from = 0

    # True on the Nth access to the gpu64 register window. REU's own
    # $DF00-$DF0A is excluded: the firmware's decode for it is a different
    # path with different (worse) known defects, and nothing here models
    # those.
    def bus_fault(self, off):
        if not self.bus_fault_kind or off < 0x0B:
            return False
        # --bus-fault-until stops the injection partway through the run, so
        # that what the tail of the run shows is whether the program REPAIRED
        # the damage, not whether it is still being damaged. Without it a
        # faulted run and a clean run can only ever be different, and the
        # state refresh ring (docs/state-refresh.md) cannot be told from a
        # program that has no refresh at all.
        if self.bus_fault_until >= 0 and self.frame > self.bus_fault_until:
            return False
        if self.frame < self.bus_fault_from:
            return False
        self.bus_fault_n += 1
        return self.bus_fault_n % self.bus_fault_every == 0

    # Which kind this particular injected fault is. Only 'both' has to
    # decide; it alternates, so a run produces roughly equal numbers of each
    # and a probe cannot pass by guessing.
    def bus_fault_now(self):
        if self.bus_fault_kind != 'both':
            return self.bus_fault_kind
        return 'drop' if (self.bus_fault_n // self.bus_fault_every) % 2 else 'addr'

    def read(self, addr):
        if 0xDF00 <= addr <= 0xDFFF:
            off = addr - IO2
            if self.bus_fault_kind != 'data' and self.bus_fault(off):
                if self.bus_fault_now() == 'drop':
                    return 0xFF             # a floating bus, uncounted
                return self.gpu.read_reg_missampled(off)
            return self.gpu.read_reg(off)
        if addr == 0xDC01:
            return self.keyboard()
        if addr == 0xDC00:
            return self.cia_pra
        if addr in (0xDD06, 0xDD07):
            return self.cia2_timer_b(addr)
        if addr == 0xFFE1:
            return 0x60                     # RTS -- intercepted below
        return self.mem[addr]

    def cia2_timer_b(self, addr):
        # CIA2's timer pair, as gpu64_demo_level.a programs it: timer A free-
        # running on a 1ms period, timer B counting its underflows, so
        # $dd06/$dd07 is a 16-bit millisecond counter that counts DOWN. The
        # model has no cycle count -- see tools/prgsim/cpu6502.py -- so an
        # instruction is charged an average four cycles, which is the right
        # order and nothing more. It exists so the demo's clock arithmetic,
        # its clamp and its minimum actually execute here; a run that read a
        # constant would exercise none of them and the numbers it printed
        # would be zeroes either way.
        ms = (self.cpu.cycles * 4) // 985
        v = (0xFFFF - ms) & 0xFFFF
        return v & 0xFF if addr == 0xDD06 else v >> 8

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
            off = addr - IO2
            if self.bus_fault(off):
                if self.bus_fault_now() == 'data':
                    bit = (self.bus_fault_n // self.bus_fault_every) % 8
                    val = (val & 0xFF) ^ (1 << bit)
                else:
                    if self.bus_fault_now() == 'addr':
                        self.gpu.write_reg_missampled(off, val)
                    return                  # 'drop': nothing happened at all
            flip = (addr == 0xDF0C and self.gpu.cmd_hi == 0
                    and (val & 0xFF) == 0x05)
            if flip:
                self.on_flip()
            self.gpu.write_reg(off, val)
            return
        self.mem[addr] = val & 0xFF

    def on_flip(self):
        self.frame_boundary(self.gpu.draw_page)

    def on_c1_frame(self, page):
        # The page the autonomous loop just rendered into. Its ink count is
        # always zero -- the model does not rasterise class 1 -- so the
        # useful half of a --frame-log line here is the demo's own status
        # rows on the C64 screen.
        self.frame_boundary(page)

    def frame_boundary(self, page):
        self.frame += 1
        path = self.ppm_frames.get(self.frame)
        if path is not None:
            write_ppm(path, self.gpu, page=page)
        if not self.frame_log:
            return
        ink = sum(1 for b in self.gpu.pages[page] if b)
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
    """The visible surface as the display would show it, border included.

    Mode-aware: in text mode there is no page and no page geometry, so the
    picture comes from the character planes at 640x400 inside a 64x72 border
    -- exactly twice the graphics border, so the framing looks the same on
    the same display.
    """
    if gpu.mode == gpu64model.MODE_TEXT:
        fw, fh = gpu64model.TXT_W, gpu64model.TXT_H
        bw, bh = gpu64model.TXT_BORDER_W, gpu64model.TXT_BORDER_H
        surface = gpu.txt_surface()
    else:
        fw, fh = gpu64model.FB_W, gpu64model.FB_H
        bw, bh = gpu64model.BORDER_W, gpu64model.BORDER_H
        surface = gpu.pages[gpu.visible_page if page is None else page]

    w, h = fw + 2 * bw, fh + 2 * bh
    pal = gpu.palette
    border = bytes(pal[gpu.border * 3:gpu.border * 3 + 3])
    rows = [border * w] * bh
    for y in range(fh):
        line = bytearray(border * bw)
        base = y * fw
        for x in range(fw):
            c = surface[base + x] * 3
            line += pal[c:c + 3]
        line += border * bw
        rows.append(bytes(line))
    rows += [border * w] * bh
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
    bus_fault_kind = None
    bus_fault_every = 0
    for o in opts:
        if o.startswith('--bus-fault='):
            kind, every = o.split('=', 1)[1].split(':', 1)
            if kind not in ('drop', 'addr', 'both', 'data'):
                print('--bus-fault kind must be drop, addr, both or data')
                return 2
            bus_fault_kind, bus_fault_every = kind, int(every, 0)
            if bus_fault_every < 1:
                print('--bus-fault interval must be at least 1')
                return 2
    clip_fault = 0
    clip_fault_kind = 'x'
    bus_fault_until = -1
    bus_fault_from = 0
    for o in opts:
        if o.startswith('--bus-fault-until='):
            bus_fault_until = int(o.split('=', 1)[1], 0)
        if o.startswith('--bus-fault-from='):
            bus_fault_from = int(o.split('=', 1)[1], 0)
    dump_scene = '--dump-scene' in opts
    trace = 0
    max_insns = 200_000_000
    ppm = None
    stop_after = 4
    frame_log = '--frame-log' in opts
    ppm_frames = {}
    c1_stream = None
    level = None
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
        if o.startswith('--c1-stream='):
            c1_stream = o.split('=', 1)[1]
        if o.startswith('--level='):
            level = o.split('=', 1)[1]
        if o.startswith('--clip-fault='):
            v = o.split('=', 1)[1]
            n, clip_fault_kind = (v.split(':', 1) + ['x'])[:2]
            if clip_fault_kind not in ('x', 'solid', 'void'):
                print('--clip-fault kind must be x, solid or void')
                return 2
            clip_fault = int(n, 0)
    chain = [o.split('=', 1)[1] for o in opts if o.startswith('--chain=')]

    m = Machine(calibrated=calibrated, stop_after=stop_after,
                frame_log=frame_log, ppm_frames=ppm_frames,
                key_script=key_script, c1_stream=c1_stream, level=level)
    m.gpu.c1_clip_fault = clip_fault
    m.gpu.c1_clip_fault_kind = clip_fault_kind
    m.bus_fault_kind = bus_fault_kind
    m.bus_fault_every = bus_fault_every
    m.bus_fault_until = bus_fault_until
    m.bus_fault_from = bus_fault_from
    for prev in chain:
        m.load_prg(prev)
        ok, _ = m.run(0x0810, max_insns=max_insns, trace=trace)
        if not ok:
            print("chained %s did not return" % os.path.basename(prev),
                  file=sys.stderr)
            return 1
        # Reset the C64 side of the harness -- the STOP budget, the frame
        # counter, the captured screen -- and deliberately NOT m.gpu, which
        # is the state under test.
        m.stop_polls = 0
        m.col7_reads = 0
        m.frame = 0
        m.done = False
        m.final = None
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
    print("--- %d dispatches, %d register writes ---"
          % (m.gpu.dispatches, m.gpu.reg_writes))
    print("--- by class: %s ---"
          % "  ".join("c%d %d/%dw" % (c, m.gpu.class_disp[c], m.gpu.class_writes[c])
                      for c in range(4) if m.gpu.class_disp[c]))
    if m.c1_stream is not None:
        m.c1_stream.close()
        print("--- %d class-1 frames ---" % m.gpu.c1_frames)

    if dump_scene:
        # The retained scene as gpu64 actually holds it at the end of the
        # run. This is the ground truth a dropped ARG or ID write corrupts:
        # ERRCODE says OK, SEQACK matches, and one of these numbers is wrong
        # forever. Diff two runs of this to see whether a refresh scheme
        # converges.
        print("--- scene ---")
        st, sc = m.gpu.c1_state, m.gpu.c1_scene
        print("state vp %d %d %d %d fov %d near %d far %d light %d %d %d "
              "amb %d bg %d cam %d dmawin %04x+%d"
              % (st.vpX, st.vpY, st.vpW, st.vpH, st.fov, st.nearZ, st.farZ,
                 st.light_dir[0], st.light_dir[1], st.light_dir[2],
                 st.ambient, st.background,
                 sc.active_camera_id if sc.have_active_camera else -1,
                 m.gpu.dma_win_base, m.gpu.dma_win_len))
        print("palette %s" % __import__('hashlib').md5(bytes(m.gpu.palette)).hexdigest())
        live = [n for n in m.gpu.c1_scene.node if n.type != 0]
        for n in sorted(live, key=lambda n: n.id):
            print("node %3d type %d vis %d pos %11d %11d %11d "
                  "ypr %5d %5d %5d scale %5d mesh %3d tex %3d "
                  "spr %5d %5d %02x light %3d %5d"
                  % (n.id, n.type, 1 if n.visible else 0,
                     n.pos[0], n.pos[1], n.pos[2],
                     n.yaw, n.pitch, n.roll, n.scale,
                     n.mesh_id, n.tex_id,
                     n.sprite_w, n.sprite_h, n.sprite_flags,
                     n.light_strength, n.light_radius))

    if m.gpu.check_refused:
        # GPU64_KEY_CHECKED refusals: commands whose ARG14 check did not
        # match what arrived, i.e. bus faults the check stopped.
        print("checked commands refused: %d" % m.gpu.check_refused,
              file=sys.stderr)

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
