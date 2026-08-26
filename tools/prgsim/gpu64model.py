"""
A reference model of the gpu64 class 0 command API, written from
docs/api_design.md rather than from the firmware.

That direction matters. The point of this file is to be a second opinion:
the conformance suite is checked against what the reference SAYS, so a test
that encodes a misreading of the spec fails here, on a PC, instead of
turning into an argument at the bench. Where the model and the firmware
disagree, one of them is wrong and the disagreement is the finding.

Not modelled, because the suite does not assert on them: the actual display,
the on-screen log overlay, cache behaviour, and anything about timing beyond
"a vblank happens every frame period".
"""

import math
import struct

FB_W, FB_H, FB_PAGES = 320, 200, 2
BORDER_W, BORDER_H = 32, 36

# The palette a reset gpu64 comes up with (Source/Firmware/gpu64_fb.cpp):
# entries 0-15 are the C64's own colours in the C64's own numbering, 16-254
# black, 255 white for the on-screen log. Only the PPM dump reads this --
# nothing in the API can read the palette back -- but a demo rendered with
# the wrong reset palette would look wrong for a reason that is not the
# demo's fault.
C64_PALETTE = (
    (  0,   0,   0), (255, 255, 255), (104,  55,  43), (112, 164, 178),
    (111,  61, 134), ( 88, 141,  67), ( 53,  40, 121), (184, 199, 111),
    (111,  79,  37), ( 67,  57,   0), (154, 103,  89), ( 68,  68,  68),
    (108, 108, 108), (154, 210, 132), (108,  94, 181), (149, 149, 149),
)

ERR_OK = 0x00
ERR_BAD_OPCODE = 0x01
ERR_BAD_CLASS = 0x02
ERR_OUT_OF_RANGE = 0x03
ERR_BAD_ARGS = 0x04
ERR_SINGULAR = 0x05
ERR_UNSUPPORTED = 0x06
ERR_BUSY = 0x07
ERR_OUT_OF_MEMORY = 0x08
ERR_QUEUE_FULL = 0x09
ERR_BAD_ID = 0x0A

# Class 2 -- the raster layer (docs/api_design.md, "Class 2 opcodes").
RASTER_REC_BYTES = 16
RASTER_WALL2_BYTES = 32
RASTER_SEC_BYTES = 8
RASTER_MAX_SECTORS = 128
RASTER_MAX_DIM = 1024
RASTER_MAX_LEVELS = 64
RASTER_MAX_TEXTURES = 255
RASTER_ARENA_BYTES = 8 * 1024 * 1024
RASTER_POLY_BYTES = 16
RASTER_VERT_BYTES = 8
RASTER_TEXINFO_BYTES = 16
RASTER_MAX_VERTS = 4096
RASTER_MAX_TEXINFO = 255
RASTER_MAX_POLY_VERTS = 16
RASTER_MAX_LIGHTS = 8
RASTER_LIGHT_SHIFT = 40
POLY_MASKED = 0x01
POLY_FLATLIT = 0x02
POLY_TWOSIDED = 0x04
COL_MASKED = 0x01
SPR_MASKED = 0x01
SPR_FLIPX = 0x02
THING_MASKED = 0x01
THING_FLIPX = 0x02
THING_NODEPTH = 0x04
THING_FLATLIT = 0x08
THING_DIRECTIONAL = 0x10
BATCH_CHECKSUM = 0x01
BATCH_CAM3D = 0x02

ST_BUSY = 0x01
ST_ERROR = 0x02
ST_VB_PEND = 0x04
ST_VB_ARMED = 0x08

REG_CMD_HI, REG_CMD_LO = 0x0B, 0x0C
REG_STATUS, REG_ERRCODE = 0x0D, 0x0E
REG_ID_LO, REG_ID_HI = 0x0F, 0x10
REG_ARG0, REG_ARG15 = 0x11, 0x20
REG_RESULT = 0x21


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s8(v):
    v &= 0xFF
    return v - 0x100 if v & 0x80 else v


def idiv(n, d):
    """Integer division truncating toward zero, which is what C's / does.

    Python's // floors, and the two disagree for exactly the negative
    operands the wall projection is full of. Every division in r_walls()
    goes through here so the model and gpu64_raster_core.cpp cannot drift
    apart on a sign.
    """
    q = abs(n) // abs(d)
    return -q if (n < 0) != (d < 0) else q


# sin(2*pi*i/256) in 8.8, the same table gpu64_raster_core.cpp holds.
SINTAB = [int(round(256 * math.sin(2 * math.pi * i / 256))) for i in range(256)]


def fsin(a):
    return SINTAB[a & 0xFF]


def fcos(a):
    return SINTAB[(a + 64) & 0xFF]


# Which of eight 45-degree sectors a vector points into; see octant8() in
# gpu64_raster_core.cpp, which this must agree with bit for bit.
def octant8(x, y):
    if y >= 0:
        if x >= 0:
            return 0 if y <= x else 1
        return 2 if y >= -x else 3
    if x < 0:
        return 4 if -y < -x else 5
    return 6 if -y > x else 7


def fix_read(buf, i):
    return s16(buf[2 * i] | (buf[2 * i + 1] << 8))


def fix_sat(v):
    """Saturate to the 8.8 range rather than wrapping. A saturated highlight
    looks like clipping; a wrapped one looks like a hardware fault."""
    return max(-32768, min(32767, v))


# When true, round the way Source/Firmware/gpu64_api.cpp's fixWrite()
# actually does rather than the way docs/api_design.md specifies, so a run
# can predict exactly which suite lines a given firmware will fail. See
# runsim.py --firmware-rounding.
FIRMWARE_ROUNDING = [False]


# Fault injection for the bulk-transfer instrument. A list of byte offsets
# within a C64-space blob write; each named byte comes back as the byte at
# offset $FF of its own 256-byte page instead of itself. That is the exact
# shape of the two hardware mismatches seen so far -- right page, wrong low
# address -- so switching this on lets the suite's own diagnostic lines be
# checked against a known answer on a PC, before a bench run is spent
# reading them. See runsim.py --alias-drop.
ALIAS_DROP = [[]]


def fix_write_product(buf, i, acc):
    """Bring a 16.16 accumulator back to 8.8, rounded half away from zero and
    saturated -- the contract docs/api_design.md states for the $8x set."""
    if FIRMWARE_ROUNDING[0]:
        # acc += +-128 then an arithmetic shift right. The shift floors, so
        # for a negative accumulator the bias is applied twice and every
        # negative product comes back one LSB too far from zero.
        a = acc + (128 if acc >= 0 else -128)
        v = a >> 8
    elif acc >= 0:
        v = (acc + 128) // 256
    else:
        v = -((-acc + 128) // 256)
    fix_store(buf, i, v)


def fix_store(buf, i, v):
    v = fix_sat(v)
    buf[2 * i] = v & 0xFF
    buf[2 * i + 1] = (v >> 8) & 0xFF


def flt_read(buf, i):
    return struct.unpack_from('<f', buf, 4 * i)[0]


def flt_write(buf, i, v):
    struct.pack_into('<f', buf, 4 * i, v)


class Gpu64Model:
    def __init__(self, mem_read, mem_write, reu_size=16 * 1024 * 1024,
                 frame_period_us=20000, calibrated=True):
        self.mem_read = mem_read
        self.mem_write = mem_write
        self.reu = bytearray(reu_size)
        self.frame_period_us = frame_period_us if calibrated else 0
        self.calibrated = calibrated

        self.cmd_hi = 0
        self.status = 0
        self.err = ERR_OK
        self.result = 0
        self.ident = [0, 0]
        self.arg = [0] * 16

        self.pages = [bytearray(FB_W * FB_H) for _ in range(FB_PAGES)]
        self.draw_page = 0
        self.visible_page = 0
        self.border = 0
        self.palette = bytearray(256 * 3)
        for i, rgb in enumerate(C64_PALETTE):
            self.palette[i * 3:i * 3 + 3] = bytes(rgb)
        self.palette[255 * 3:256 * 3] = b'\xff\xff\xff'
        self.log_enabled = True
        self.api_active = False

        # --- class 2 (raster) -----------------------------------------
        # ids are 1..255; each entry is (w, h, texels) with texels stored
        # column-major, texel(u, v) at texels[u * h + v].
        self.rtex = {}
        self.rarena_used = 0
        self.rview = (0, 0, FB_W, FB_H)
        self.rcolormap = None
        self.rlevels = 0
        self.rbatch = [0, 0, 0]          # accepted, rejected, pixels
        self.rcam = dict(x=0, y=0, ang=0, flags=0, eye=0x0080, ceil=0x0200,
                         proj=0xA000, floorc=0, ceilc=0, horizon=0)
        self.rsectors = []
        # Milestone 9's polygon layer: a 3D camera that has never been set
        # (proj 0), and the two level-lifetime tables a face record indexes.
        self.rcam3 = dict(x=0, y=0, z=0, yaw=0, pitch=0, proj=0, flags=0)
        self.rverts = []
        self.rtexinfo = []
        # Milestone 12's dynamic point lights: eight slots, each either None
        # (off) or (x, y, z, r2, fall).
        self.rlights = [None] * RASTER_MAX_LIGHTS
        self.rrequested = 0
        # Persistent, exactly like the firmware's static s_ZBuf: DRAW_SECTORS
        # clears it and DRAW_THINGS reads what DRAW_SECTORS left.
        self.zbuf = [0xFFFF] * (FB_W * FB_H)

        self.vb_armed = False
        self.flip_pending = False
        self.dispatches = 0
        self.reu_cmd = 0
        self.reu_c64 = 0
        self.reu_addr = 0
        self.reu_len = 0
        self.reu_ctrl = 0
        # Advanced by the runner; a vblank is "every frame_period_us".
        self.now_us = 0
        self._next_vb = frame_period_us

    # --- time ----------------------------------------------------------
    def advance(self, us):
        """Let wall-clock time pass. Called by the runner between
        instructions so a poll loop waiting on a vblank can finish."""
        if not self.calibrated:
            return
        self.now_us += us
        while self.now_us >= self._next_vb:
            self._next_vb += self.frame_period_us
            self.status |= ST_VB_PEND
            if self.flip_pending:
                self.flip_pending = False
                self.draw_page, self.visible_page = self.visible_page, self.draw_page
                self.status &= ~ST_BUSY

    # --- the REU controller ------------------------------------------
    # Only enough of a 1764 to move a block: the suite uses it to get bytes
    # into REU space so that the "space = 1" side of a blob descriptor is
    # exercised at all. Everything else about the REU is somebody else's
    # test.
    def reu_write(self, off, val):
        if off == 0x01:                                 # command
            self.reu_cmd = val
            if val & 0x80:                              # execute
                self.reu_execute()
        elif 0x02 <= off <= 0x03:
            i = off - 0x02
            self.reu_c64 = (self.reu_c64 & ~(0xFF << (8 * i))) | (val << (8 * i))
        elif 0x04 <= off <= 0x06:
            i = off - 0x04
            self.reu_addr = (self.reu_addr & ~(0xFF << (8 * i))) | (val << (8 * i))
        elif 0x07 <= off <= 0x08:
            i = off - 0x07
            self.reu_len = (self.reu_len & ~(0xFF << (8 * i))) | (val << (8 * i))
        elif off == 0x0A:
            self.reu_ctrl = val

    def reu_read(self, off):
        if off == 0x00:
            return 0x10                                 # status: nothing pending
        return 0xFF

    def reu_execute(self):
        n = self.reu_len if self.reu_len else 0x10000
        kind = self.reu_cmd & 0x03
        c64, ext = self.reu_c64, self.reu_addr
        for i in range(n):
            if kind == 0:                               # C64 -> REU
                self.reu[(ext + i) % len(self.reu)] = self.mem_read((c64 + i) & 0xFFFF)
            elif kind == 1:                             # REU -> C64
                self.mem_write((c64 + i) & 0xFFFF, self.reu[(ext + i) % len(self.reu)])
        # Address autoincrement is the default; the suite does not depend on
        # where the pointers end up, so they are simply advanced.
        self.reu_c64 = (c64 + n) & 0xFFFF
        self.reu_addr = (ext + n) % len(self.reu)
        self.reu_len = 1

    # --- register window ------------------------------------------------
    def read_reg(self, off):
        if off <= 0x0A:
            return self.reu_read(off)
        if off == REG_STATUS:
            return self.status
        if off == REG_ERRCODE:
            return self.err
        if off == REG_RESULT:
            return self.result
        return 0xFF

    def write_reg(self, off, val):
        val &= 0xFF
        if off <= 0x0A:
            self.reu_write(off, val)
            return
        if off == REG_CMD_HI:
            self.cmd_hi = val
        elif off == REG_CMD_LO:
            self.dispatch(val)
        elif off == REG_ID_LO:
            self.ident[0] = val
        elif off == REG_ID_HI:
            self.ident[1] = val
        elif REG_ARG0 <= off <= REG_ARG15:
            self.arg[off - REG_ARG0] = val

    # --- argument decoding ----------------------------------------------
    def a_u16(self, i):
        return self.arg[i] | (self.arg[i + 1] << 8)

    def a_s16(self, i):
        return s16(self.a_u16(i))

    def a_blob(self, i):
        space = self.arg[i]
        addr = self.arg[i + 1] | (self.arg[i + 2] << 8) | (self.arg[i + 3] << 16)
        length = self.arg[i + 4] | (self.arg[i + 5] << 8)
        return space, addr, length

    def a_compact(self, i):
        space = self.arg[i]
        addr = self.arg[i + 1] | (self.arg[i + 2] << 8) | (self.arg[i + 3] << 16)
        return space, addr

    # --- blobs ------------------------------------------------------------
    def blob_read(self, space, addr, length):
        if length == 0:
            return ERR_OK, bytearray()
        if space == 1:
            if addr + length > len(self.reu):
                return ERR_OUT_OF_RANGE, None
            return ERR_OK, bytearray(self.reu[addr:addr + length])
        if space != 0:
            return ERR_OUT_OF_RANGE, None
        if addr + length > 65536:
            return ERR_OUT_OF_RANGE, None
        return ERR_OK, bytearray(self.mem_read(addr + i) for i in range(length))

    def blob_write(self, space, addr, data):
        if len(data) == 0:
            return ERR_OK
        if space == 1:
            if addr + len(data) > len(self.reu):
                return ERR_OUT_OF_RANGE
            self.reu[addr:addr + len(data)] = data
            return ERR_OK
        if space != 0:
            return ERR_OUT_OF_RANGE
        if addr + len(data) > 65536:
            return ERR_OUT_OF_RANGE
        data = bytearray(data)
        for k in ALIAS_DROP[0]:
            if 0 <= k < len(data):
                end = (k | 0xFF)
                if end < len(data):
                    data[k] = data[end]
        for i, b in enumerate(data):
            self.mem_write(addr + i, b)
        return ERR_OK

    # --- drawing ----------------------------------------------------------
    def page(self):
        return self.pages[self.draw_page]

    def put(self, x, y, c):
        if 0 <= x < FB_W and 0 <= y < FB_H:
            self.page()[y * FB_W + x] = c

    def rect_fill(self, x, y, w, h, c):
        if w <= 0 or h <= 0:
            return
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(FB_W, x + w), min(FB_H, y + h)
        if x0 >= x1 or y0 >= y1:
            return
        p = self.page()
        row = bytes([c]) * (x1 - x0)
        for yy in range(y0, y1):
            p[yy * FB_W + x0:yy * FB_W + x1] = row

    def line(self, x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        while True:
            self.put(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = err * 2
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy

    # --- dispatch ---------------------------------------------------------
    def dispatch(self, op):
        self.dispatches += 1
        if self.cmd_hi not in (0, 2):
            self.err = ERR_BAD_CLASS
            self.status |= ST_ERROR
            return
        try:
            res = self.execute_r2(op) if self.cmd_hi == 2 else self.execute(op)
        except IndexError:
            res = ERR_OUT_OF_RANGE
        self.err = res
        if res == ERR_OK:
            self.status &= ~ST_ERROR
            if not self.api_active and op != 0x07:
                self.log_enabled = False
            self.api_active = True
        else:
            self.status |= ST_ERROR

    def execute(self, op):
        a = self.arg
        if op == 0x00:
            return ERR_OK
        if op == 0x01:                                  # RESET_STATE
            self.status = 0
            self.vb_armed = False
            self.flip_pending = False
            self.draw_page = self.visible_page = 0
            return ERR_OK
        if op == 0x02:                                  # VBLANK_ARM
            if a[0] > 1:
                return ERR_BAD_ARGS
            if a[0] == 1:
                if not self.calibrated:
                    return ERR_UNSUPPORTED
                self.vb_armed = True
                self.status |= ST_VB_ARMED
            else:
                self.vb_armed = False
                self.status &= ~ST_VB_ARMED
            return ERR_OK
        if op == 0x03:                                  # VBLANK_ACK
            self.status &= ~ST_VB_PEND
            return ERR_OK
        if op == 0x04:                                  # SET_DRAW_PAGE
            if a[0] > 1:
                return ERR_BAD_ARGS
            self.draw_page = a[0]
            return ERR_OK
        if op == 0x05:                                  # PAGE_FLIP
            if a[0] > 1:
                return ERR_BAD_ARGS
            if a[0] == 1:
                if not self.calibrated:
                    return ERR_UNSUPPORTED
                if self.flip_pending:
                    return ERR_BUSY
                self.flip_pending = True
                self.status |= ST_BUSY
                return ERR_OK
            self.draw_page, self.visible_page = self.visible_page, self.draw_page
            return ERR_OK
        if op == 0x06:                                  # GET_INFO
            space, addr, length = self.a_blob(0)
            if length < 16:
                return ERR_BAD_ARGS
            info = bytearray(16)
            info[0:3] = b'G64'
            info[3], info[4] = 1, 0
            info[5] = FB_W & 0xFF
            info[6] = FB_W >> 8
            info[7] = FB_H & 0xFF
            info[8] = FB_H >> 8
            info[9] = 8
            info[10] = FB_PAGES
            info[11] = 0x01 | 0x04       # class 0 and class 2; class 1 is not modelled
            info[12] = BORDER_W
            info[13] = BORDER_H
            info[14] = self.frame_period_us & 0xFF
            info[15] = (self.frame_period_us >> 8) & 0xFF
            return self.blob_write(space, addr, info)
        if op == 0x07:                                  # LOG_ENABLE
            if a[0] > 1:
                return ERR_BAD_ARGS
            self.log_enabled = bool(a[0])
            return ERR_OK
        if op == 0x08:                                  # SET_BORDER
            self.border = a[0]
            return ERR_OK
        if op == 0x09:                                  # VBLANK_SYNC
            if not self.calibrated:
                return ERR_UNSUPPORTED
            if self.flip_pending:
                return ERR_BUSY
            return ERR_OK
        if op == 0x10:                                  # CLEAR
            self.rect_fill(0, 0, FB_W, FB_H, a[0])
            return ERR_OK
        if op == 0x20:                                  # SET_PIXEL
            self.put(self.a_s16(0), self.a_s16(2), a[4])
            return ERR_OK
        if op == 0x21:                                  # LINE
            self.line(self.a_s16(0), self.a_s16(2), self.a_s16(4), self.a_s16(6), a[8])
            return ERR_OK
        if op in (0x22, 0x23):                          # RECT / RECT_FILL
            x, y = self.a_s16(0), self.a_s16(2)
            w, h = self.a_u16(4), self.a_u16(6)
            c = a[8]
            if w == 0 or h == 0:
                return ERR_OK
            if op == 0x23:
                self.rect_fill(x, y, w, h, c)
            else:
                self.rect_fill(x, y, w, 1, c)
                self.rect_fill(x, y + h - 1, w, 1, c)
                if h > 2:
                    self.rect_fill(x, y + 1, 1, h - 2, c)
                    self.rect_fill(x + w - 1, y + 1, 1, h - 2, c)
            return ERR_OK
        if op == 0x30:                                  # PAL_SET
            i = a[0]
            self.palette[i * 3:i * 3 + 3] = bytes(a[1:4])
            return ERR_OK
        if op == 0x31:                                  # PAL_LOAD
            space, addr, length = self.a_blob(0)
            first, count = a[6], a[7]
            if count == 0:
                return ERR_OK
            if length != count * 3:
                return ERR_BAD_ARGS
            if first + count > 256:
                return ERR_BAD_ARGS
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            self.palette[first * 3:(first + count) * 3] = data
            return ERR_OK
        if op in (0x40, 0x41):                          # BLIT / BLIT_KEYED
            space, addr, length = self.a_blob(0)
            dx, dy = self.a_s16(6), self.a_s16(8)
            w, h = self.a_u16(10), self.a_u16(12)
            if w == 0 or h == 0:
                return ERR_OK
            if length != w * h:
                return ERR_BAD_ARGS
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            key = a[14] if op == 0x41 else -1
            for sy in range(h):
                yy = dy + sy
                if not (0 <= yy < FB_H):
                    continue
                for sx in range(w):
                    xx = dx + sx
                    if not (0 <= xx < FB_W):
                        continue
                    v = data[sy * w + sx]
                    if key >= 0 and v == key:
                        continue
                    self.page()[yy * FB_W + xx] = v
            return ERR_OK
        if op == 0x42:                                  # READ_RECT
            space, addr, length = self.a_blob(0)
            sx, sy = self.a_s16(6), self.a_s16(8)
            w, h = self.a_u16(10), self.a_u16(12)
            if w == 0 or h == 0:
                return ERR_OK
            if length != w * h:
                return ERR_BAD_ARGS
            if sx < 0 or sy < 0 or sx + w > FB_W or sy + h > FB_H:
                return ERR_BAD_ARGS
            out = bytearray(w * h)
            p = self.page()
            for yy in range(h):
                out[yy * w:(yy + 1) * w] = p[(sy + yy) * FB_W + sx:(sy + yy) * FB_W + sx + w]
            return self.blob_write(space, addr, out)
        if 0x80 <= op < 0xA0:
            return self.math(op)
        return ERR_BAD_OPCODE

    # --- matrix and vector -------------------------------------------------
    def math(self, op):
        a = self.arg
        elem = 2 if (op & 0xF0) == 0x80 else 4
        sub = op & 0x0F
        fixed = elem == 2

        def nbytes(rows, cols):
            n = rows * cols * elem
            return 0 if (n == 0 or n > 65535) else n

        def rd(buf, i):
            return fix_read(buf, i) if fixed else flt_read(buf, i)

        if sub == 0x00:                                 # MAT_MUL
            m, k, n = a[0], a[1], a[2]
            if m == 0 or k == 0 or n == 0:
                return ERR_BAD_ARGS
            ba, bb, bc = nbytes(m, k), nbytes(k, n), nbytes(m, n)
            if not (ba and bb and bc):
                return ERR_OUT_OF_RANGE
            spA, adA = self.a_compact(3)
            spB, adB = self.a_compact(7)
            spC, adC = self.a_compact(11)
            res, A = self.blob_read(spA, adA, ba)
            if res != ERR_OK:
                return res
            res, B = self.blob_read(spB, adB, bb)
            if res != ERR_OK:
                return res
            C = bytearray(bc)
            for i in range(m):
                for j in range(n):
                    if fixed:
                        acc = sum(rd(A, i * k + x) * rd(B, x * n + j) for x in range(k))
                        fix_write_product(C, i * n + j, acc)
                    else:
                        acc = 0.0
                        for x in range(k):
                            acc += rd(A, i * k + x) * rd(B, x * n + j)
                        flt_write(C, i * n + j, acc)
            return self.blob_write(spC, adC, C)

        if sub in (0x01, 0x02):                         # MAT_ADD / MAT_SUB
            m, n = a[0], a[1]
            if m == 0 or n == 0:
                return ERR_BAD_ARGS
            nb = nbytes(m, n)
            if not nb:
                return ERR_OUT_OF_RANGE
            spA, adA = self.a_compact(2)
            spB, adB = self.a_compact(6)
            spC, adC = self.a_compact(10)
            res, A = self.blob_read(spA, adA, nb)
            if res != ERR_OK:
                return res
            res, B = self.blob_read(spB, adB, nb)
            if res != ERR_OK:
                return res
            C = bytearray(nb)
            for i in range(m * n):
                if fixed:
                    v = rd(A, i) + rd(B, i) if sub == 1 else rd(A, i) - rd(B, i)
                    fix_store(C, i, v)
                else:
                    v = rd(A, i) + rd(B, i) if sub == 1 else rd(A, i) - rd(B, i)
                    flt_write(C, i, v)
            return self.blob_write(spC, adC, C)

        if sub == 0x03:                                 # MAT_SCALE
            m, n = a[0], a[1]
            if m == 0 or n == 0:
                return ERR_BAD_ARGS
            nb = nbytes(m, n)
            if not nb:
                return ERR_OUT_OF_RANGE
            spA, adA = self.a_compact(2)
            spC, adC = self.a_compact(6)
            res, A = self.blob_read(spA, adA, nb)
            if res != ERR_OK:
                return res
            C = bytearray(nb)
            if fixed:
                s = s16(a[10] | (a[11] << 8))
                for i in range(m * n):
                    fix_write_product(C, i, rd(A, i) * s)
            else:
                s = struct.unpack('<f', bytes(a[10:14]))[0]
                for i in range(m * n):
                    flt_write(C, i, rd(A, i) * s)
            return self.blob_write(spC, adC, C)

        if sub == 0x04:                                 # MAT_TRANSPOSE
            m, n = a[0], a[1]
            if m == 0 or n == 0:
                return ERR_BAD_ARGS
            nb = nbytes(m, n)
            if not nb:
                return ERR_OUT_OF_RANGE
            spA, adA = self.a_compact(2)
            spC, adC = self.a_compact(6)
            res, A = self.blob_read(spA, adA, nb)
            if res != ERR_OK:
                return res
            C = bytearray(nb)
            for i in range(m):
                for j in range(n):
                    if fixed:
                        fix_store(C, j * m + i, rd(A, i * n + j))
                    else:
                        flt_write(C, j * m + i, rd(A, i * n + j))
            return self.blob_write(spC, adC, C)

        if sub == 0x05:                                 # MAT_IDENTITY
            n = a[0]
            if n == 0:
                return ERR_BAD_ARGS
            nb = nbytes(n, n)
            if not nb:
                return ERR_OUT_OF_RANGE
            spC, adC = self.a_compact(1)
            C = bytearray(nb)
            for i in range(n):
                if fixed:
                    fix_store(C, i * n + i, 256)
                else:
                    flt_write(C, i * n + i, 1.0)
            return self.blob_write(spC, adC, C)

        if sub == 0x06:                                 # MAT_INVERSE
            n = a[0]
            if n == 0 or n > 64:
                return ERR_BAD_ARGS
            nb = nbytes(n, n)
            if not nb:
                return ERR_OUT_OF_RANGE
            spA, adA = self.a_compact(1)
            spC, adC = self.a_compact(5)
            res, A = self.blob_read(spA, adA, nb)
            if res != ERR_OK:
                return res
            # Gauss-Jordan in double precision whatever the element format,
            # so an 8.8 operand keeps its precision through the inversion.
            M = [[0.0] * (2 * n) for _ in range(n)]
            for i in range(n):
                for j in range(n):
                    v = rd(A, i * n + j)
                    M[i][j] = v / 256.0 if fixed else v
                M[i][n + i] = 1.0
            for col in range(n):
                piv, best = -1, 0.0
                for r in range(col, n):
                    if abs(M[r][col]) > best:
                        best, piv = abs(M[r][col]), r
                if piv < 0 or best < 1e-12:
                    return ERR_SINGULAR
                M[col], M[piv] = M[piv], M[col]
                d = M[col][col]
                for j in range(2 * n):
                    M[col][j] /= d
                for r in range(n):
                    if r == col:
                        continue
                    f = M[r][col]
                    if f == 0.0:
                        continue
                    for j in range(2 * n):
                        M[r][j] -= f * M[col][j]
            C = bytearray(nb)
            for i in range(n):
                for j in range(n):
                    v = M[i][n + j]
                    if fixed:
                        fix_write_product(C, i * n + j, int(round(v * 65536.0)))
                    else:
                        flt_write(C, i * n + j, v)
            return self.blob_write(spC, adC, C)

        return ERR_BAD_OPCODE

    # ==================================================================
    # Class 2 -- the raster layer
    #
    # Written from docs/api_design.md's class 2 section, not from
    # Source/Firmware/gpu64_raster_core.cpp. Where this and the firmware
    # disagree, one of them is wrong and the disagreement is the finding --
    # tools/rastercheck exists to surface exactly that, on a PC.
    # ==================================================================

    def r_lookup(self, texid):
        return self.rtex.get(texid)

    def r_light_row(self, light):
        if self.rcolormap is None or self.rlevels == 0:
            return None
        lvl = min(light, self.rlevels - 1)
        return memoryview(self.rcolormap)[lvl * 256:(lvl + 1) * 256]

    def r_set_light(self, slot, x, y, z, radius, strength):
        if radius == 0 or strength == 0:
            self.rlights[slot] = None
            return
        r2 = radius * radius
        self.rlights[slot] = (x, y, z, r2,
                              (strength << RASTER_LIGHT_SHIFT) // r2)

    def r_lights_live(self):
        return any(l is not None for l in self.rlights)

    def r_light_adjust(self, level, wx, wy, wz):
        # Mirrors lightAdjust() in gpu64_raster_core.cpp: falloff linear in
        # d squared, subtracted from the colormap index because higher index
        # is darker.
        sub = 0
        for l in self.rlights:
            if l is None:
                continue
            lx, ly, lz, r2, fall = l
            dx, dy, dz = wx - lx, wy - ly, wz - lz
            d2 = dx * dx + dy * dy + dz * dz
            if d2 >= r2:
                continue
            sub += (fall * (r2 - d2)) >> RASTER_LIGHT_SHIFT
        if sub <= 0:
            return level
        return 0 if sub >= level else level - sub

    def r_put(self, x, y, c):
        self.page()[y * FB_W + x] = c

    def execute_r2(self, op):
        a = self.arg
        if op == 0x00:                                  # RASTER_RESET
            self.rtex = {}
            self.rarena_used = 0
            self.rview = (0, 0, FB_W, FB_H)
            self.rcolormap = None
            self.rlevels = 0
            self.rbatch = [0, 0, 0]
            self.rrequested = 0
            self.rcam = dict(x=0, y=0, ang=0, flags=0, eye=0x0080,
                             ceil=0x0200, proj=0xA000, floorc=0, ceilc=0,
                             horizon=0)
            self.rsectors = []
            self.rcam3 = dict(x=0, y=0, z=0, yaw=0, pitch=0, proj=0, flags=0)
            self.rverts = []
            self.rtexinfo = []
            self.rlights = [None] * RASTER_MAX_LIGHTS
            # The depth buffer's lifecycle matches the firmware's: RESET
            # makes it empty again, so a program that sends things but no
            # sectors does not inherit the previous one's depth.
            self.zbuf = [0xFFFF] * (FB_W * FB_H)
            return ERR_OK

        if op == 0x01:                                  # SET_VIEW
            x, y = self.a_u16(0), self.a_u16(2)
            w, h = self.a_u16(4), self.a_u16(6)
            if w == 0 or h == 0:
                return ERR_BAD_ARGS
            if x + w > FB_W or y + h > FB_H:
                return ERR_BAD_ARGS
            self.rview = (x, y, w, h)
            return ERR_OK

        if op == 0x02:                                  # SET_COLORMAP
            space, addr, length = self.a_blob(0)
            levels = a[6]
            if levels == 0:
                self.rcolormap, self.rlevels = None, 0
                return ERR_OK
            if levels > RASTER_MAX_LEVELS:
                return ERR_BAD_ARGS
            if length != levels * 256:
                return ERR_BAD_ARGS
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            self.rcolormap, self.rlevels = data, levels
            return ERR_OK

        if op == 0x03:                                  # RASTER_STATS
            space, addr, length = self.a_blob(0)
            if length < 16:
                return ERR_BAD_ARGS
            acc, rej, pix = self.rbatch
            free_kb = (RASTER_ARENA_BYTES - self.rarena_used) >> 10
            info = bytearray(16)
            info[0:2] = b'R2'
            info[2] = acc & 0xFF
            info[3] = (acc >> 8) & 0xFF
            info[4] = rej & 0xFF
            info[5] = (rej >> 8) & 0xFF
            info[6] = self.rrequested & 0xFF
            info[7] = (self.rrequested >> 8) & 0xFF
            for i in range(4):
                info[8 + i] = (pix >> (8 * i)) & 0xFF
            live = len(self.rtex)
            info[12] = live & 0xFF
            info[13] = (live >> 8) & 0xFF
            info[14] = free_kb & 0xFF
            info[15] = (free_kb >> 8) & 0xFF
            return self.blob_write(space, addr, info)

        if op == 0x04:                                  # FILL_VIEW
            vx, vy, vw, vh = self.rview
            self.rect_fill(vx, vy, vw, vh, a[0])
            # The depth of the pixels this erases goes with them: a pixel
            # just painted background has nothing in it. Without this a
            # thing batch would be occluded by geometry no longer drawn.
            for y in range(vy, vy + vh):
                for x in range(vx, vx + vw):
                    self.zbuf[y * FB_W + x] = 0xFFFF
            return ERR_OK

        if op == 0x10:                                  # UPLOAD_TEXTURE
            space, addr, length = self.a_blob(0)
            texid = self.ident[0] | (self.ident[1] << 8)
            w, h = self.a_u16(6), self.a_u16(8)
            flags = a[10]
            if texid == 0 or texid > RASTER_MAX_TEXTURES:
                return ERR_BAD_ID
            if w == 0 or h == 0 or w > RASTER_MAX_DIM or h > RASTER_MAX_DIM:
                return ERR_BAD_ARGS
            if h & (h - 1):
                return ERR_BAD_ARGS
            if length != w * h:
                return ERR_BAD_ARGS
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            need = (w * h + 63) & ~63
            if need > RASTER_ARENA_BYTES - self.rarena_used:
                return ERR_OUT_OF_MEMORY
            self.rarena_used += need
            if flags & 0x01:                            # source is row-major
                col = bytearray(w * h)
                for u in range(w):
                    for v in range(h):
                        col[u * h + v] = data[v * w + u]
                data = col
            self.rtex[texid] = (w, h, data)
            return ERR_OK

        if op == 0x11:                                  # FREE_TEXTURE
            texid = self.ident[0] | (self.ident[1] << 8)
            if texid == 0 or texid > RASTER_MAX_TEXTURES:
                return ERR_BAD_ID
            if texid not in self.rtex:
                return ERR_BAD_ID
            del self.rtex[texid]
            return ERR_OK

        if op == 0x05:                                  # SET_CAMERA
            proj = self.a_u16(10)
            eye, ceil = self.a_s16(6), self.a_s16(8)
            if proj == 0 or eye <= 0 or ceil <= eye:
                return ERR_BAD_ARGS
            self.rcam = dict(x=self.a_s16(0), y=self.a_s16(2), ang=a[4],
                             flags=a[5], eye=eye, ceil=ceil, proj=proj,
                             floorc=a[12], ceilc=a[13],
                             horizon=s8(a[14]))
            return ERR_OK

        if op in (0x20, 0x21):                          # DRAW_COLUMNS / SPANS
            res, recs, count = self.r_pull_batch()
            if res != ERR_OK or count == 0:
                return res
            if op == 0x20:
                self.r_columns(recs, count, a[9])
            else:
                self.r_spans(recs, count)
            return ERR_OK

        if op == 0x22:                                  # DRAW_SPRITE
            return self.r_sprite()

        if op == 0x23:                                  # DRAW_WALLS
            res, recs, count = self.r_pull_batch()
            if res != ERR_OK or count == 0:
                return res
            self.r_walls(recs, count, a[9])
            return ERR_OK

        if op == 0x06:                                  # SET_SECTORS
            space, addr, length = self.a_blob(0)
            count = self.a_u16(6)
            if count == 0:
                self.rsectors = []
                return ERR_OK
            if count > RASTER_MAX_SECTORS:
                return ERR_BAD_ARGS
            if length != count * RASTER_SEC_BYTES:
                return ERR_BAD_ARGS
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            built = []
            for i in range(count):
                r = data[i * RASTER_SEC_BYTES:(i + 1) * RASTER_SEC_BYTES]
                f = s16(r[0] | (r[1] << 8))
                c = s16(r[2] | (r[3] << 8))
                if c <= f:
                    # Rejected whole. The table that was working stays.
                    return ERR_BAD_ARGS
                built.append(dict(floor=f, ceil=c, floorc=r[4], ceilc=r[5],
                                  light=r[6], flags=r[7]))
            self.rsectors = built
            return ERR_OK

        if op == 0x07:                                  # SET_CAMERA3D
            proj = self.a_u16(8)
            if proj == 0:
                return ERR_BAD_ARGS
            self.rcam3 = dict(x=self.a_s16(0), y=self.a_s16(2),
                              z=self.a_s16(4), yaw=a[6], pitch=s8(a[7]),
                              proj=proj, flags=a[10])
            return ERR_OK

        if op == 0x08:                                  # SET_LIGHT
            slot = a[0]
            if slot >= RASTER_MAX_LIGHTS:
                return ERR_BAD_ARGS
            self.r_set_light(slot, self.a_s16(1), self.a_s16(3),
                             self.a_s16(5), self.a_u16(7), a[9])
            return ERR_OK

        if op == 0x12:                                  # UPLOAD_VERTS
            space, addr, length = self.a_blob(0)
            count = self.a_u16(6)
            if count == 0:
                self.rverts = []
                return ERR_OK
            if count > RASTER_MAX_VERTS:
                return ERR_BAD_ARGS
            if length != count * RASTER_VERT_BYTES:
                return ERR_BAD_ARGS
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            self.rverts = [
                (s16(data[i * 8] | (data[i * 8 + 1] << 8)),
                 s16(data[i * 8 + 2] | (data[i * 8 + 3] << 8)),
                 s16(data[i * 8 + 4] | (data[i * 8 + 5] << 8)))
                for i in range(count)]
            return ERR_OK

        if op == 0x13:                                  # UPLOAD_TEXINFO
            space, addr, length = self.a_blob(0)
            count = self.a_u16(6)
            if count == 0:
                self.rtexinfo = []
                return ERR_OK
            if count > RASTER_MAX_TEXINFO:
                return ERR_BAD_ARGS
            if length != count * RASTER_TEXINFO_BYTES:
                return ERR_BAD_ARGS
            res, data = self.blob_read(space, addr, length)
            if res != ERR_OK:
                return res
            self.rtexinfo = [
                tuple(s16(data[i * 16 + 2 * k] | (data[i * 16 + 2 * k + 1] << 8))
                      for k in range(8))
                for i in range(count)]
            return ERR_OK

        if op == 0x26:                                  # DRAW_POLYS
            res, recs, count = self.r_pull_batch(RASTER_POLY_BYTES)
            if res != ERR_OK or count == 0:
                return res
            if self.rcam3['proj'] == 0 or not self.rverts:
                return ERR_BAD_ARGS
            self.r_polys(recs, count, a[9])
            return ERR_OK

        if op == 0x24:                                  # DRAW_SECTORS
            res, recs, count = self.r_pull_batch(RASTER_WALL2_BYTES)
            if res != ERR_OK or count == 0:
                return res
            if not self.rsectors:
                return ERR_BAD_ARGS
            self.r_sectors(recs, count, a[9])
            return ERR_OK

        if op == 0x25:                                  # DRAW_THINGS
            res, recs, count = self.r_pull_batch()
            if res != ERR_OK or count == 0:
                return res
            if (a[8] & BATCH_CAM3D) and self.rcam3['proj'] == 0:
                return ERR_BAD_ARGS
            self.r_things(recs, count, a[9], a[8])
            return ERR_OK

        return ERR_BAD_OPCODE

    def r_pull_batch(self, stride=None):
        stride = RASTER_REC_BYTES if stride is None else stride
        space, addr, length = self.a_blob(0)
        count = self.a_u16(6)
        flags = self.arg[8]

        self.rrequested = count
        self.rbatch = [0, 0, 0]
        if count == 0:
            return ERR_OK, None, 0

        want = count * stride + (2 if flags & BATCH_CHECKSUM else 0)
        if length != want or want > 65536:
            return ERR_BAD_ARGS, None, 0

        res, data = self.blob_read(space, addr, length)
        if res != ERR_OK:
            return res, None, 0

        if flags & BATCH_CHECKSUM:
            body = count * stride
            want16 = data[body] | (data[body + 1] << 8)
            if (sum(data[:body]) & 0xFFFF) != want16:
                # "A failed dispatch does nothing" -- nothing is drawn.
                return ERR_BAD_ARGS, None, 0

        return ERR_OK, data, count

    def r_columns(self, recs, count, key):
        vx, vy, vw, vh = self.rview
        for i in range(count):
            r = recs[i * RASTER_REC_BYTES:(i + 1) * RASTER_REC_BYTES]
            x = r[0] | (r[1] << 8)
            y0 = s16(r[2] | (r[3] << 8))
            y1 = s16(r[4] | (r[5] << 8))
            texid, light = r[6], r[7]
            u = r[8] | (r[9] << 8)
            v = s16(r[10] | (r[11] << 8))
            dv = s16(r[12] | (r[13] << 8))
            flags = r[14]

            if x < vx or x >= vx + vw or y1 < y0:
                self.rbatch[1] += 1
                continue
            tex = None
            if texid != 0:
                tex = self.r_lookup(texid)
                if tex is None:
                    self.rbatch[1] += 1
                    continue
            self.rbatch[0] += 1

            if y0 < vy:
                v += dv * (vy - y0)
                y0 = vy
            if y1 >= vy + vh:
                y1 = vy + vh - 1
            if y0 > y1:
                continue

            cmap = self.r_light_row(light)
            if tex is None:
                c = u & 0xFF
                if cmap is not None:
                    c = cmap[c]
                for y in range(y0, y1 + 1):
                    self.r_put(x, y, c)
                self.rbatch[2] += y1 - y0 + 1
                continue

            tw, th, texels = tex
            uu = u % tw
            base = uu * th
            hmask = th - 1
            masked = bool(flags & COL_MASKED)
            for y in range(y0, y1 + 1):
                t = texels[base + ((v >> 8) & hmask)]
                v += dv
                if masked and t == key:
                    continue
                self.r_put(x, y, cmap[t] if cmap is not None else t)
                self.rbatch[2] += 1

    def r_spans(self, recs, count):
        vx, vy, vw, vh = self.rview
        for i in range(count):
            r = recs[i * RASTER_REC_BYTES:(i + 1) * RASTER_REC_BYTES]
            y = s16(r[0] | (r[1] << 8))
            x0 = s16(r[2] | (r[3] << 8))
            x1 = s16(r[4] | (r[5] << 8))
            texid, light = r[6], r[7]
            u = s16(r[8] | (r[9] << 8))
            v = s16(r[10] | (r[11] << 8))
            du = s16(r[12] | (r[13] << 8))
            dv = s16(r[14] | (r[15] << 8))

            if y < vy or y >= vy + vh or x1 < x0:
                self.rbatch[1] += 1
                continue
            tex = None
            if texid != 0:
                tex = self.r_lookup(texid)
                # A span wraps u every pixel, so w has to be a mask too.
                if tex is None or (tex[0] & (tex[0] - 1)):
                    self.rbatch[1] += 1
                    continue
            self.rbatch[0] += 1

            if x0 < vx:
                n = vx - x0
                u += du * n
                v += dv * n
                x0 = vx
            if x1 >= vx + vw:
                x1 = vx + vw - 1
            if x0 > x1:
                continue

            cmap = self.r_light_row(light)
            if tex is None:
                c = u & 0xFF
                if cmap is not None:
                    c = cmap[c]
                for x in range(x0, x1 + 1):
                    self.r_put(x, y, c)
                self.rbatch[2] += x1 - x0 + 1
                continue

            tw, th, texels = tex
            wmask, hmask = tw - 1, th - 1
            for x in range(x0, x1 + 1):
                t = texels[((u >> 8) & wmask) * th + ((v >> 8) & hmask)]
                self.r_put(x, y, cmap[t] if cmap is not None else t)
                u += du
                v += dv
            self.rbatch[2] += x1 - x0 + 1


    # ------------------------------------------------------------------
    # DRAW_WALLS.
    #
    # A line-for-line mirror of gpu64_rasterWalls() in
    # Source/Firmware/gpu64_raster_core.cpp. Written from the same
    # description rather than translated from the C, and then diffed
    # against it by tools/rastercheck -- which is the only reason either
    # one can be trusted about a projection this fiddly.
    # ------------------------------------------------------------------

    NEAR = 0x0040                                       # 0.25 world units

    def r_to_view(self, x, y):
        c = fcos(self.rcam['ang'])
        sn = fsin(self.rcam['ang'])
        dx = x - self.rcam['x']
        dy = y - self.rcam['y']
        return ((dx * sn - dy * c) >> 8, (dx * c + dy * sn) >> 8)

    def r_walls(self, recs, count, key):
        vx, vy, vw, vh = self.rview
        vx0, vx1 = vx, vx + vw
        vy0, vy1 = vy, vy + vh
        cam = self.rcam

        if cam['proj'] == 0 or cam['eye'] <= 0 or cam['ceil'] <= cam['eye']:
            self.rbatch[1] += count
            return

        depth = [0] * FB_W
        proj = cam['proj']
        centre_x = vx0 + vw // 2
        horizon = vy0 + vh // 2 + cam['horizon']
        eye_h = cam['eye']
        top_h = cam['ceil'] - cam['eye']
        page = self.page()

        for i in range(count):
            r = recs[i * RASTER_REC_BYTES:(i + 1) * RASTER_REC_BYTES]
            avx, avz = self.r_to_view(s16(r[0] | (r[1] << 8)),
                                      s16(r[2] | (r[3] << 8)))
            bvx, bvz = self.r_to_view(s16(r[4] | (r[5] << 8)),
                                      s16(r[6] | (r[7] << 8)))
            au = s16(r[10] | (r[11] << 8))
            bu = s16(r[12] | (r[13] << 8))
            texid, light, flags = r[8], r[9], r[14]

            if avz < self.NEAR and bvz < self.NEAR:
                self.rbatch[1] += 1
                continue

            tex = self.rtex.get(texid) if texid != 0 else None
            if texid != 0 and tex is None:
                self.rbatch[1] += 1
                continue

            if avz < self.NEAR:
                t = idiv((self.NEAR - avz) << 16, bvz - avz)
                avx += (bvx - avx) * t >> 16
                au += (bu - au) * t >> 16
                avz = self.NEAR
            elif bvz < self.NEAR:
                t = idiv((self.NEAR - bvz) << 16, avz - bvz)
                bvx += (avx - bvx) * t >> 16
                bu += (au - bu) * t >> 16
                bvz = self.NEAR

            sxa = centre_x + (idiv(avx * proj, avz) >> 8)
            sxb = centre_x + (idiv(bvx * proj, bvz) >> 8)
            if sxb <= sxa:
                self.rbatch[1] += 1
                continue

            self.rbatch[0] += 1

            iza = idiv(1 << 22, avz)
            izb = idiv(1 << 22, bvz)
            uza = (au * iza) >> 8
            uzb = (bu * izb) >> 8

            x0, x1 = max(sxa, vx0), min(sxb - 1, vx1 - 1)
            if x0 > x1:
                continue
            span = sxb - sxa

            if tex is not None:
                tw, th, tdata = tex
                hmask = th - 1

            for x in range(x0, x1 + 1):
                t = x - sxa
                iz = iza + idiv((izb - iza) * t, span)
                if iz <= 0 or iz <= depth[x]:
                    continue
                depth[x] = iz
                z = idiv(1 << 22, iz)
                if z <= 0:
                    continue

                yb = horizon + (idiv(eye_h * proj, z) >> 8)
                yt = horizon - (idiv(top_h * proj, z) >> 8)
                if yb <= yt:
                    continue

                lvl = light
                if not (flags & 0x02):
                    lvl = min(light + (z >> 9), 255)
                cmap = self.r_light_row(lvl)

                if cam['flags'] & 0x01:
                    for y in range(vy0, min(yt - 1, vy1 - 1) + 1):
                        page[y * FB_W + x] = cam['ceilc']
                        self.rbatch[2] += 1
                    for y in range(max(yb, vy0), vy1):
                        page[y * FB_W + x] = cam['floorc']
                        self.rbatch[2] += 1

                ya, yz = max(yt, vy0), min(yb - 1, vy1 - 1)
                if ya > yz:
                    continue

                if tex is None:
                    c = (r[10] | (r[11] << 8)) & 0xFF
                    if cmap is not None:
                        c = cmap[c]
                    for y in range(ya, yz + 1):
                        page[y * FB_W + x] = c
                    self.rbatch[2] += yz - ya + 1
                    continue

                uz = uza + idiv((uzb - uza) * t, span)
                uu = (idiv(uz << 8, iz) >> 8) % tw
                col = uu * th
                dv = idiv(th << 8, yb - yt)
                v = (ya - yt) * dv
                for y in range(ya, yz + 1):
                    tx = tdata[col + ((v >> 8) & hmask)]
                    v += dv
                    if (flags & 0x01) and tx == key:
                        continue
                    page[y * FB_W + x] = cmap[tx] if cmap is not None else tx
                    self.rbatch[2] += 1

    # ------------------------------------------------------------------
    # DRAW_SECTORS. Same projection as r_walls, with the two things a
    # level with steps in it needs: heights that come from a sector table
    # rather than from the camera, and a two-sided wall that draws a band
    # above the far ceiling and a band below the far floor and leaves the
    # middle see-through.
    #
    # Depth is per pixel here, as z in 8.8 with 0xFFFF for empty and
    # nearer meaning smaller -- a portal column has no single depth, so
    # r_walls's one-1/z-per-column buffer cannot express it.
    # ------------------------------------------------------------------

    Z_EMPTY = 0xFFFF

    @staticmethod
    def r_zstore(z):
        if z < 0:
            return 0
        return min(z, Gpu64Model.Z_EMPTY - 1)

    @staticmethod
    def r_lit(base, z, flatlit):
        return base if flatlit else min(base + (z >> 9), 255)

    def r_sectors(self, recs, count, key):
        vx, vy, vw, vh = self.rview
        vx0, vx1 = vx, vx + vw
        vy0, vy1 = vy, vy + vh
        cam = self.rcam

        if cam['proj'] == 0 or not self.rsectors:
            self.rbatch[1] += count
            return

        zbuf = self.zbuf
        for y in range(vy0, vy1):
            for x in range(vx0, vx1):
                zbuf[y * FB_W + x] = self.Z_EMPTY
        proj = cam['proj']
        centre_x = vx0 + vw // 2
        horizon = vy0 + vh // 2 + cam['horizon']
        eye = cam['eye']
        page = self.page()

        def band(x, z, y_top, y_bot, yt, yb, tex, colour, cmap, flags, uu):
            yt, yb = max(yt, vy0), min(yb, vy1)
            if yt >= yb or y_bot <= y_top:
                return
            zs = self.r_zstore(z)
            if tex is None:
                c = cmap[colour] if cmap is not None else colour
                for y in range(yt, yb):
                    if zs >= zbuf[y * FB_W + x]:
                        continue
                    zbuf[y * FB_W + x] = zs
                    page[y * FB_W + x] = c
                    self.rbatch[2] += 1
                return
            tw, th, tdata = tex
            col = (uu % tw) * th
            hmask = th - 1
            dv = idiv(th << 8, y_bot - y_top)
            v = (yt - y_top) * dv
            for y in range(yt, yb):
                if zs >= zbuf[y * FB_W + x]:
                    v += dv
                    continue
                tx = tdata[col + ((v >> 8) & hmask)]
                v += dv
                if (flags & 0x01) and tx == key:
                    continue
                zbuf[y * FB_W + x] = zs
                page[y * FB_W + x] = cmap[tx] if cmap is not None else tx
                self.rbatch[2] += 1

        # The depth a flat writes is never nearer than the wall whose
        # column painted it: a plane is infinite and a sector's floor is
        # not, so without the clamp a low ceiling two rooms away wins the
        # rows above the wall that hides it. Lighting still uses the row's
        # true distance.
        def flat(x, z_wall, yt, yb, drop, colour, base, flatlit):
            yt, yb = max(yt, vy0), min(yb, vy1)
            for y in range(yt, yb):
                dy = y - horizon
                if dy == 0 or (dy > 0) != (drop > 0):
                    continue
                z = idiv(drop * proj, dy << 8)
                if z <= 0:
                    continue
                zs = self.r_zstore(max(z, z_wall))
                if zs >= zbuf[y * FB_W + x]:
                    continue
                cmap = self.r_light_row(self.r_lit(base, z, flatlit))
                zbuf[y * FB_W + x] = zs
                page[y * FB_W + x] = cmap[colour] if cmap is not None else colour
                self.rbatch[2] += 1

        nsec = len(self.rsectors)
        for i in range(count):
            r = recs[i * RASTER_WALL2_BYTES:(i + 1) * RASTER_WALL2_BYTES]
            avx, avz = self.r_to_view(s16(r[0] | (r[1] << 8)),
                                      s16(r[2] | (r[3] << 8)))
            bvx, bvz = self.r_to_view(s16(r[4] | (r[5] << 8)),
                                      s16(r[6] | (r[7] << 8)))
            au = s16(r[8] | (r[9] << 8))
            bu = s16(r[10] | (r[11] << 8))
            front, back, light, flags = r[12], r[13], r[14], r[15]

            if avz < self.NEAR and bvz < self.NEAR:
                self.rbatch[1] += 1
                continue
            if front >= nsec:
                self.rbatch[1] += 1
                continue
            two_sided = back != 0xFF
            if two_sided and back >= nsec:
                self.rbatch[1] += 1
                continue

            fs = self.rsectors[front]
            bs = self.rsectors[back] if two_sided else None

            tex = [None, None, None]
            bad = False
            for k in range(3):
                tid = r[16 + k]
                if tid:
                    tex[k] = self.rtex.get(tid)
                    if tex[k] is None:
                        bad = True
            if bad:
                self.rbatch[1] += 1
                continue
            tex_m, tex_u, tex_l = tex

            if avz < self.NEAR:
                t = idiv((self.NEAR - avz) << 16, bvz - avz)
                avx += (bvx - avx) * t >> 16
                au += (bu - au) * t >> 16
                avz = self.NEAR
            elif bvz < self.NEAR:
                t = idiv((self.NEAR - bvz) << 16, avz - bvz)
                bvx += (avx - bvx) * t >> 16
                bu += (au - bu) * t >> 16
                bvz = self.NEAR

            sxa = centre_x + (idiv(avx * proj, avz) >> 8)
            sxb = centre_x + (idiv(bvx * proj, bvz) >> 8)
            if sxb <= sxa:
                self.rbatch[1] += 1
                continue

            self.rbatch[0] += 1

            iza = idiv(1 << 22, avz)
            izb = idiv(1 << 22, bvz)
            uza = (au * iza) >> 8
            uzb = (bu * izb) >> 8

            x0, x1 = max(sxa, vx0), min(sxb - 1, vx1 - 1)
            if x0 > x1:
                continue
            span = sxb - sxa
            flatlit = bool(flags & 0x02)
            flats = bool(cam['flags'] & 0x01) and not (flags & 0x04)

            for x in range(x0, x1 + 1):
                t = x - sxa
                iz = iza + idiv((izb - iza) * t, span)
                if iz <= 0:
                    continue
                z = idiv(1 << 22, iz)
                if z <= 0:
                    continue

                yf = horizon + (idiv((eye - fs['floor']) * proj, z) >> 8)
                yc = horizon + (idiv((eye - fs['ceil']) * proj, z) >> 8)

                if flats:
                    if not (fs['flags'] & 0x01):
                        flat(x, z, vy0, yc, eye - fs['ceil'], fs['ceilc'],
                             fs['light'], flatlit)
                    flat(x, z, yf, vy1, eye - fs['floor'], fs['floorc'],
                         fs['light'], flatlit)

                cmap = self.r_light_row(self.r_lit(light, z, flatlit))

                uu = 0
                if tex_m is not None or tex_u is not None or tex_l is not None:
                    uz = uza + idiv((uzb - uza) * t, span)
                    uu = idiv(uz << 8, iz) >> 8

                if not two_sided:
                    band(x, z, yc, yf, yc, yf, tex_m, r[19], cmap, flags, uu)
                    continue

                ybf = horizon + (idiv((eye - bs['floor']) * proj, z) >> 8)
                ybc = horizon + (idiv((eye - bs['ceil']) * proj, z) >> 8)

                if bs['floor'] > fs['floor']:
                    band(x, z, ybf, yf, ybf, yf, tex_l, r[21], cmap, flags, uu)
                if bs['ceil'] < fs['ceil'] and not (fs['flags'] & 0x01):
                    band(x, z, yc, ybc, yc, ybc, tex_u, r[20], cmap, flags, uu)

                # The far sector's flats, seen through the window. Nothing
                # else paints them: flats are painted by the columns of the
                # walls standing on them, and a corridor's own side walls
                # seen end-on cover almost no columns.
                if flats:
                    w_top, w_bot = max(yc, ybc), min(yf, ybf)
                    if not (bs['flags'] & 0x01):
                        flat(x, z, w_top, w_bot, eye - bs['ceil'],
                             bs['ceilc'], bs['light'], flatlit)
                    flat(x, z, w_top, w_bot, eye - bs['floor'],
                         bs['floorc'], bs['light'], flatlit)

    # ------------------------------------------------------------------
    # DRAW_THINGS: billboards in world space, depth-tested per pixel
    # against what DRAW_SECTORS left in the buffer. Depth is one value for
    # the whole card, and it is written as well as tested at drawn pixels,
    # so a batch is order-independent.
    # ------------------------------------------------------------------

    def r_things(self, recs, count, key, batch_flags=0):
        vx, vy, vw, vh = self.rview
        vx0, vx1 = vx, vx + vw
        vy0, vy1 = vy, vy + vh
        cam3 = bool(batch_flags & BATCH_CAM3D)
        cam = self.rcam3 if cam3 else self.rcam

        if cam['proj'] == 0:
            self.rbatch[1] += count
            return

        zbuf = self.zbuf
        page = self.page()
        proj = cam['proj']
        centre_x = vx0 + vw // 2
        horizon = vy0 + vh // 2 + (0 if cam3 else cam['horizon'])
        eye = self.rcam['eye']
        c3 = self.rcam3
        cyaw, syaw = fcos(c3['yaw']), fsin(c3['yaw'])
        cpit, spit = fcos(c3['pitch'] & 0xFF), fsin(c3['pitch'] & 0xFF)

        for i in range(count):
            r = recs[i * RASTER_REC_BYTES:(i + 1) * RASTER_REC_BYTES]
            wx, wy = s16(r[0] | (r[1] << 8)), s16(r[2] | (r[3] << 8))
            base = s16(r[4] | (r[5] << 8))
            world_h = r[6] | (r[7] << 8)
            world_w = r[8] | (r[9] << 8)
            texid, light, flags = r[10], r[11], r[12]

            # Eight views: byte 13 is where the thing faces, and the id is
            # the first of eight consecutive ones.
            usetex = texid
            if flags & THING_DIRECTIONAL:
                cx = c3['x'] if cam3 else self.rcam['x']
                cy = c3['y'] if cam3 else self.rcam['y']
                dx, dy = cx - wx, cy - wy
                g = (r[13] - 16) & 0xFF
                cc, ss = fcos(g), fsin(g)
                fx = (dx * cc + dy * ss) >> 8
                fy = (dy * cc - dx * ss) >> 8
                usetex = (texid + octant8(fx, fy)) & 0xFF

            # hbase/htop are heights measured downward from the eye, so
            # the row arithmetic below is one expression for both cameras.
            if cam3:
                dx, dy = wx - c3['x'], wy - c3['y']
                dzb = base - c3['z']
                dzt = dzb + world_h
                fwd = (dx * cyaw + dy * syaw) >> 8
                vxx = (dx * syaw - dy * cyaw) >> 8
                vzz = (fwd * cpit + dzb * spit) >> 8
                vz_top = (fwd * cpit + dzt * spit) >> 8
                hbase = (fwd * spit - dzb * cpit) >> 8
                htop = (fwd * spit - dzt * cpit) >> 8
            else:
                vxx, vzz = self.r_to_view(wx, wy)
                vz_top = vzz
                hbase = eye - base
                htop = hbase - world_h

            if vzz < self.NEAR:
                self.rbatch[1] += 1
                continue
            if vz_top < self.NEAR:
                vz_top = self.NEAR
            if world_w == 0 or world_h == 0:
                self.rbatch[1] += 1
                continue
            tex = self.r_lookup(usetex) if (texid and usetex) else None
            if tex is None:
                self.rbatch[1] += 1
                continue

            self.rbatch[0] += 1

            wpx = idiv(world_w * proj, vzz) >> 8
            sxc = centre_x + (idiv(vxx * proj, vzz) >> 8)
            y_bot = horizon + (idiv(hbase * proj, vzz) >> 8)
            y_top = horizon + (idiv(htop * proj, vz_top) >> 8)
            hpx = y_bot - y_top
            if wpx <= 0 or hpx <= 0:
                continue

            x_left = sxc - wpx // 2
            x0, x1 = max(x_left, vx0), min(x_left + wpx - 1, vx1 - 1)
            y0, y1 = max(y_top, vy0), min(y_top + hpx - 1, vy1 - 1)
            if x0 > x1 or y0 > y1:
                continue

            tw, th, texels = tex
            ustep = (tw << 16) // wpx
            vstep = (th << 16) // hpx

            masked = bool(flags & THING_MASKED)
            flip = bool(flags & THING_FLIPX)
            nodepth = bool(flags & THING_NODEPTH)
            lvl = self.r_lit(light, vzz, bool(flags & THING_FLATLIT))
            if self.r_lights_live():
                # Once per thing, at the billboard's centre.
                lvl = self.r_light_adjust(lvl, wx, wy, base + (world_h >> 1))
            cmap = self.r_light_row(lvl)
            zs = self.r_zstore(vzz)

            ustart = (x0 - x_left) * ustep
            vrow = (y0 - y_top) * vstep

            for sy in range(y0, y1 + 1):
                sv = min(vrow >> 16, th - 1)
                vrow += vstep
                ucol = ustart
                for sx in range(x0, x1 + 1):
                    su = min(ucol >> 16, tw - 1)
                    ucol += ustep
                    if not nodepth and zs >= zbuf[sy * FB_W + sx]:
                        continue
                    if flip:
                        su = tw - 1 - su
                    t = texels[su * th + sv]
                    if masked and t == key:
                        continue
                    if not nodepth:
                        zbuf[sy * FB_W + sx] = zs
                    page[sy * FB_W + sx] = cmap[t] if cmap is not None else t
                    self.rbatch[2] += 1

    # ------------------------------------------------------------------
    # DRAW_POLYS -- milestone 9's polygon layer.
    #
    # Written from docs/api_design.md and docs/milestone9_poly_design.md,
    # not from gpu64_raster_core.cpp. Every division goes through idiv() and
    # every shift is arithmetic, because the two implementations have to
    # agree on a truncation, not merely on a picture.
    # ------------------------------------------------------------------

    def r_poly_plane(self, v, plane, proj, kl, kr, kt, kb):
        vx, vy, vz = v[0], v[1], v[2]
        if plane == 0:
            return vz - self.NEAR
        if plane == 1:
            return (vx * proj - kl * vz) >> 8
        if plane == 2:
            return (kr * vz - vx * proj) >> 8
        if plane == 3:
            return (kt * vz - vy * proj) >> 8
        return (vy * proj - kb * vz) >> 8

    def r_poly_clip(self, verts, plane, proj, kl, kr, kt, kb):
        out = []
        n = len(verts)
        for i in range(n):
            a = verts[i]
            b = verts[(i + 1) % n]
            da = self.r_poly_plane(a, plane, proj, kl, kr, kt, kb)
            db = self.r_poly_plane(b, plane, proj, kl, kr, kt, kb)
            if da >= 0:
                out.append(a)
            if (da >= 0) != (db >= 0):
                num, den = -da, db - da
                # 8 components: view x/y/z, s, t, and the world position the
                # dynamic lights need (milestone 12).
                out.append(tuple(a[k] + idiv((b[k] - a[k]) * num, den)
                                 for k in range(8)))
        return out

    def r_polys(self, recs, count, key):
        vx, vy, vw, vh = self.rview
        vx0, vx1 = vx, vx + vw
        vy0, vy1 = vy, vy + vh
        cam = self.rcam3

        if cam['proj'] == 0 or not self.rverts:
            self.rbatch[1] += count
            return

        proj = cam['proj']
        centre_x = vx0 + vw // 2
        centre_y = vy0 + vh // 2
        kl = (vx0 - centre_x) * 256
        kr = (vx1 - centre_x) * 256
        kt = (centre_y - vy0) * 256
        kb = (centre_y - vy1) * 256

        cyaw, syaw = fcos(cam['yaw']), fsin(cam['yaw'])
        cpit = fcos(cam['pitch'] & 0xFF)
        spit = fsin(cam['pitch'] & 0xFF)

        zbuf = self.zbuf
        page = self.page()

        for i in range(count):
            r = recs[i * RASTER_POLY_BYTES:(i + 1) * RASTER_POLY_BYTES]
            first = r[0] | (r[1] << 8)
            nverts, tinfo, texid = r[2], r[3], r[4]
            colour, light, flags = r[5], r[6], r[7]

            if nverts < 3 or nverts > RASTER_MAX_POLY_VERTS:
                self.rbatch[1] += 1
                continue
            if first + nverts > len(self.rverts):
                self.rbatch[1] += 1
                continue

            tex = ti = None
            if texid:
                tex = self.r_lookup(texid)
                if tex is None or (tex[0] & (tex[0] - 1)):
                    # Both coordinates are masked per pixel, so a width that
                    # is not a power of two is a rejected record here even
                    # though the same texture is legal for DRAW_COLUMNS.
                    self.rbatch[1] += 1
                    continue
                if tinfo == 0 or tinfo > len(self.rtexinfo):
                    self.rbatch[1] += 1
                    continue
                ti = self.rtexinfo[tinfo - 1]

            poly = []
            for k in range(nverts):
                wx, wy, wz = self.rverts[first + k]
                dx, dy, dz = wx - cam['x'], wy - cam['y'], wz - cam['z']
                fwd = (dx * cyaw + dy * syaw) >> 8
                rgt = (dx * syaw - dy * cyaw) >> 8
                pvz = (fwd * cpit + dz * spit) >> 8
                pvy = (dz * cpit - fwd * spit) >> 8
                if ti is not None:
                    sc = ((wx * ti[0] + wy * ti[1] + wz * ti[2]) >> 8) + ti[3]
                    tc = ((wx * ti[4] + wy * ti[5] + wz * ti[6]) >> 8) + ti[7]
                else:
                    sc = tc = 0
                poly.append((rgt, pvy, pvz, sc, tc, wx, wy, wz))

            poly = self.r_poly_clip(poly, 0, proj, kl, kr, kt, kb)
            if len(poly) < 3:
                self.rbatch[1] += 1          # entirely behind the near plane
                continue
            for plane in (1, 2, 3, 4):
                if len(poly) < 3:
                    break
                poly = self.r_poly_clip(poly, plane, proj, kl, kr, kt, kb)
            if len(poly) < 3:
                self.rbatch[0] += 1          # off the side: drew nothing
                continue

            scr = []
            for v in poly:
                vz = max(v[2], self.NEAR)
                sx = (centre_x << 8) + idiv(v[0] * proj, vz)
                sy = (centre_y << 8) - idiv(v[1] * proj, vz)
                w = idiv(1 << 30, vz)
                scr.append([sx, sy, w, v[3] * w, v[4] * w,
                            v[5] * w, v[6] * w, v[7] * w])

            n = len(scr)
            area = 0
            for k in range(n):
                a, b = scr[k], scr[(k + 1) % n]
                area += a[0] * b[1] - b[0] * a[1]
            if area == 0:
                self.rbatch[0] += 1
                continue
            if area < 0 and not (flags & POLY_TWOSIDED):
                self.rbatch[1] += 1
                continue

            self.rbatch[0] += 1

            min_y = min(v[1] for v in scr)
            max_y = max(v[1] for v in scr)
            y_top = max((min_y + 255) >> 8, vy0)
            y_bot = min((max_y + 255) >> 8, vy1)

            tw = th = 0
            texels = None
            if tex is not None:
                tw, th, texels = tex
            flatlit = bool(flags & POLY_FLATLIT)
            masked = bool(flags & POLY_MASKED)
            lit = self.r_lights_live()

            for y in range(y_top, y_bot):
                Y = y << 8
                hit_l = hit_r = None
                for k in range(n):
                    a, b = scr[k], scr[(k + 1) % n]
                    if not ((a[1] <= Y < b[1]) or (b[1] <= Y < a[1])):
                        continue
                    num, den = Y - a[1], b[1] - a[1]
                    h = [a[j] + idiv((b[j] - a[j]) * num, den)
                         for j in (0, 2, 3, 4, 5, 6, 7)]
                    if hit_l is None:
                        hit_l = hit_r = h
                    elif h[0] < hit_l[0]:
                        hit_l = h
                    elif h[0] > hit_r[0]:
                        hit_r = h

                if hit_l is None or hit_r[0] <= hit_l[0]:
                    continue

                xs = max((hit_l[0] + 255) >> 8, vx0)
                xe = min((hit_r[0] + 255) >> 8, vx1)
                if xs >= xe:
                    continue

                dx88 = hit_r[0] - hit_l[0]
                off = (xs << 8) - hit_l[0]
                w = hit_l[1] + idiv((hit_r[1] - hit_l[1]) * off, dx88)
                sq = hit_l[2] + idiv((hit_r[2] - hit_l[2]) * off, dx88)
                tq = hit_l[3] + idiv((hit_r[3] - hit_l[3]) * off, dx88)
                dw = idiv((hit_r[1] - hit_l[1]) << 8, dx88)
                ds = idiv((hit_r[2] - hit_l[2]) << 8, dx88)
                dt = idiv((hit_r[3] - hit_l[3]) << 8, dx88)

                lq = [0, 0, 0]
                dl = [0, 0, 0]
                if lit:
                    for j in range(3):
                        d = hit_r[4 + j] - hit_l[4 + j]
                        lq[j] = hit_l[4 + j] + idiv(d * off, dx88)
                        dl[j] = idiv(d << 8, dx88)

                for x in range(xs, xe):
                    ww, ss, tt = w, sq, tq
                    lw = (lq[0], lq[1], lq[2])
                    w += dw
                    sq += ds
                    tq += dt
                    for j in range(3):
                        lq[j] += dl[j]
                    if ww <= 0:
                        continue
                    z = idiv(1 << 30, ww)
                    zs = self.r_zstore(z)
                    if zs >= zbuf[y * FB_W + x]:
                        continue
                    if texels is not None:
                        ui = (idiv(ss, ww) >> 8) & (tw - 1)
                        vi = (idiv(tt, ww) >> 8) & (th - 1)
                        c = texels[ui * th + vi]
                        if masked and c == key:
                            continue
                    else:
                        c = colour
                    lvl = self.r_lit(light, z, flatlit)
                    if lit:
                        lvl = self.r_light_adjust(lvl, idiv(lw[0], ww),
                                                  idiv(lw[1], ww),
                                                  idiv(lw[2], ww))
                    cmap = self.r_light_row(lvl)
                    zbuf[y * FB_W + x] = zs
                    page[y * FB_W + x] = cmap[c] if cmap is not None else c
                    self.rbatch[2] += 1

    def r_sprite(self):
        a = self.arg
        texid = self.ident[0] | (self.ident[1] << 8)
        if texid == 0 or texid > RASTER_MAX_TEXTURES:
            return ERR_BAD_ID
        tex = self.r_lookup(texid)
        if tex is None:
            return ERR_BAD_ID

        x, y = self.a_s16(0), self.a_s16(2)
        w, h = self.a_u16(4), self.a_u16(6)
        light, key = a[8], a[9]
        clip_y0, clip_y1 = self.a_s16(10), self.a_s16(12)
        flags = a[14]

        self.rbatch = [0, 0, 0]
        self.rrequested = 1
        if w == 0 or h == 0:
            self.rbatch[1] = 1
            return ERR_OK
        self.rbatch[0] = 1

        tw, th, texels = tex
        ustep = (tw << 16) // w
        vstep = (th << 16) // h

        vx, vy, vw, vh = self.rview
        x0, x1 = x, x + w - 1
        y0, y1 = y, y + h - 1
        y0 = max(y0, clip_y0)
        y1 = min(y1, clip_y1)
        x0 = max(x0, vx)
        x1 = min(x1, vx + vw - 1)
        y0 = max(y0, vy)
        y1 = min(y1, vy + vh - 1)
        if x0 > x1 or y0 > y1:
            return ERR_OK

        cmap = self.r_light_row(light)
        masked = bool(flags & SPR_MASKED)
        flip = bool(flags & SPR_FLIPX)
        ustart = (x0 - x) * ustep
        vrow = (y0 - y) * vstep

        for sy in range(y0, y1 + 1):
            sv = min(vrow >> 16, th - 1)
            ucol = ustart
            for sx in range(x0, x1 + 1):
                su = min(ucol >> 16, tw - 1)
                ucol += ustep
                if flip:
                    su = tw - 1 - su
                t = texels[su * th + sv]
                if masked and t == key:
                    continue
                self.r_put(sx, sy, cmap[t] if cmap is not None else t)
                self.rbatch[2] += 1
            vrow += vstep
        return ERR_OK
