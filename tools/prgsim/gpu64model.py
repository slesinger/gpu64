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
        if self.cmd_hi != 0:
            self.err = ERR_BAD_CLASS
            self.status |= ST_ERROR
            return
        try:
            res = self.execute(op)
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
            info[11] = 0x01
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
