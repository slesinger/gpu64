"""
A reference model of the gpu64 class 1 command API -- the retained scene
graph, its resources, and Stage 16's handshake commit loop.

Deliberately NOT a renderer. Classes 0 and 2 are rasterised in Python in
gpu64model.py because their output is what the conformance suite asserts on;
class 1's output is a perspective-projected, texture-mapped, point-lit
picture, and a second implementation of that in Python would be a second
thing to be wrong rather than an oracle. So this half models the *protocol*
-- what a command does to the scene, when the loop accepts a commit, which
edits land in the shadow copy -- and hands each accepted frame to
tools/hostsim/scenesim, which draws it with the firmware's own
gpu64_3dSceneRender(). The frame stream between the two is the whole
interface; see FRAME_STREAM_DOC below for its format.

Written from docs/class1-3d-mesh-reference.md and
project/milestone6_3d_design.md, same second-opinion rule the rest of the
model follows.
"""

import copy
import os
import re

# --- opcodes (docs/class1-3d-mesh-reference.md) --------------------------
OP_SCENE_RESET = 0x00
OP_SET_VIEWPORT = 0x01
OP_SET_PERSPECTIVE = 0x02
OP_SET_LIGHT = 0x03
OP_BUILD_COLORMAP = 0x04
OP_SET_BACKGROUND = 0x05
OP_LOOP_START = 0x06
OP_LOOP_STOP = 0x07
OP_SCENE_COMMIT = 0x08
OP_ARENA_STATUS = 0x09
OP_UPLOAD_MESH = 0x10
OP_UPLOAD_TEXTURE = 0x11
OP_FREE_RESOURCE = 0x12
OP_LOAD_LEVEL = 0x13
OP_LEVEL_STEP = 0x14
OP_CLIP_MOVE = 0x15
OP_LEVEL_ENT = 0x16
OP_LEVEL_PALETTE = 0x17
OP_LEVEL_NODE = 0x1C
LEVEL_DONE = 0xFF
# Bytes of source blob one LEVEL_STEP consumes -- GPU64_LEVEL_SLICE_BYTES.
LEVEL_SLICE_BYTES = 16384
OP_CREATE_OBJECT = 0x20
OP_CREATE_CAMERA = 0x21
OP_DESTROY_NODE = 0x22
OP_SET_ACTIVE_CAMERA = 0x23
OP_SET_VISIBLE = 0x24
OP_CREATE_SPRITE = 0x25
OP_CREATE_LIGHT = 0x26
OP_SET_SPRITE = 0x27
OP_SET_POINT_LIGHT = 0x28
OP_SET_POSITION = 0x30
OP_SET_ORIENTATION = 0x31
OP_MOVE_LOCAL = 0x32
OP_MOVE_WORLD = 0x33
OP_ROTATE_LOCAL = 0x34
OP_SET_SCALE = 0x35
OP_GET_TRANSFORM = 0x36
OP_CLEAR_VIEWPORT = 0x40
OP_DRAW_MESH = 0x41
OP_DRAW_NODE = 0x42

# Opcodes whose edits are redirected into the shadow copy while the loop is
# running -- gpu64_3d_class1.cpp's isShadowRedirectable(). SCENE_COMMIT and
# SCENE_RESET are absent on purpose: both act on the live copy themselves.
SHADOW_REDIRECTABLE = frozenset((
    OP_SET_VIEWPORT, OP_SET_PERSPECTIVE, OP_SET_LIGHT, OP_BUILD_COLORMAP,
    OP_SET_BACKGROUND, OP_CREATE_OBJECT, OP_CREATE_CAMERA, OP_DESTROY_NODE,
    OP_SET_ACTIVE_CAMERA, OP_SET_VISIBLE, OP_CREATE_SPRITE, OP_CREATE_LIGHT,
    OP_SET_SPRITE, OP_SET_POINT_LIGHT, OP_SET_POSITION, OP_SET_ORIENTATION,
    OP_MOVE_LOCAL, OP_MOVE_WORLD, OP_ROTATE_LOCAL, OP_SET_SCALE,
    OP_GET_TRANSFORM, OP_LEVEL_PALETTE, OP_LEVEL_NODE,
))

NODE_NONE, NODE_OBJECT, NODE_CAMERA, NODE_SPRITE, NODE_LIGHT = 0, 1, 2, 3, 4

MAX_NODES = 256
MAX_RESOURCES = 512
SURFACE_W, SURFACE_H = 320, 200
BUDGET = 196608
ARENA_BYTES = 32 * 1024 * 1024
STAGE_BYTES = 65536

FX8_ONE = 1 << 8
FX15_ONE = 1 << 15
FX16_ONE = 1 << 16

# STATUS bit 4: "the autonomous loop has a finished frame waiting".
ST_FRAME_READY = 0x10


# --- fixed-point maths, ported from Source/Firmware/gpu64_3d_math.cpp ----
#
# Only what the *protocol* needs, which is MOVE_LOCAL's rotate-then-add and
# the focal length SET_VIEWPORT/SET_PERSPECTIVE derive. Everything else the
# renderer needs is recomputed on the C++ side from the raw angles this
# model carries, so there is nothing here to drift out of step with it.

def _load_sintab():
    """The firmware's own table, parsed rather than regenerated.

    Regenerating it would mean agreeing with whatever rounding produced it,
    which is exactly the kind of one-LSB disagreement this model exists to
    catch rather than to reproduce by luck.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, '..', '..', 'Source', 'Firmware',
                        'gpu64_3d_sintab.h')
    text = open(path).read()
    body = text[text.index('gpu64_3dSinTab['):]
    body = body[body.index('{') + 1:body.index('}')]
    tab = [int(v) for v in re.findall(r'-?\d+', body)]
    assert len(tab) == 1024, len(tab)
    return tab


SINTAB = _load_sintab()


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s32(v):
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v


def fsin(a):
    return SINTAB[(a & 0xFFFF) >> 6]


def fcos(a):
    return SINTAB[((a + 16384) & 0xFFFF) >> 6]


def focal_from_fov(fov, width):
    if fov == 0 or fov >= 32768:
        return 0
    s = fsin(fov // 2)
    c = fcos(fov // 2)
    if s <= 0:
        return 0
    # Truncating C division, and the numerator is always positive here.
    return ((width // 2) * c * FX16_ONE) // s


def mat_from_euler(yaw, pitch, roll):
    sy, cy = fsin(yaw), fcos(yaw)
    sp, cp = fsin(pitch), fcos(pitch)
    sr, cr = fsin(roll), fcos(roll)

    def m15(a, b):
        return s16((a * b) >> 15)

    def m15_3(a, b, c):
        return s16((((a * b) >> 15) * c) >> 15)

    # R = Ry(yaw) * Rx(pitch) * Rz(roll) -- must stay term-for-term identical
    # to gpu64_3dMatFromEuler() in Source/Firmware/gpu64_3d_math.cpp, down to
    # where each product truncates to 1.15, or the model and the firmware
    # disagree silently.
    return [
        s16(m15(cy, cr) + m15_3(sy, sp, sr)),
        s16(-m15(cy, sr) + m15_3(sy, sp, cr)),
        s16(m15(sy, cp)),
        s16(m15(cp, sr)),
        s16(m15(cp, cr)),
        s16(-sp),
        s16(-m15(sy, cr) + m15_3(cy, sp, sr)),
        s16(m15(sy, sr) + m15_3(cy, sp, cr)),
        s16(m15(cy, cp)),
    ]


def vec_rotate(m, v):
    x, y, z = v
    return (
        s32((x * m[0] + y * m[1] + z * m[2]) >> 15),
        s32((x * m[3] + y * m[4] + z * m[5]) >> 15),
        s32((x * m[6] + y * m[7] + z * m[8]) >> 15),
    )


# --- the scene ------------------------------------------------------------

class Node:
    __slots__ = ('id', 'type', 'visible', 'pos', 'yaw', 'pitch', 'roll',
                 'scale', 'mesh_id', 'tex_id', 'sprite_w', 'sprite_h',
                 'sprite_flags', 'light_strength', 'light_radius')

    def __init__(self):
        self.type = NODE_NONE
        self.reset(0, NODE_NONE)

    def reset(self, node_id, node_type):
        self.id = node_id
        self.type = node_type
        self.visible = True
        self.pos = [0, 0, 0]
        self.yaw = self.pitch = self.roll = 0
        self.scale = FX8_ONE
        self.mesh_id = 0
        self.tex_id = 0
        self.sprite_w = self.sprite_h = 0
        self.sprite_flags = 0
        self.light_strength = 0
        self.light_radius = 0


class Scene:
    def __init__(self):
        self.node = [Node() for _ in range(MAX_NODES)]
        self.active_camera_id = 0
        self.have_active_camera = False

    def find(self, node_id, node_type=NODE_NONE):
        for n in self.node:
            if n.type != NODE_NONE and n.id == node_id:
                if node_type == NODE_NONE or n.type == node_type:
                    return n
                return None
        return None

    def slot(self, node_id):
        free = None
        for n in self.node:
            if n.type != NODE_NONE and n.id == node_id:
                return n
            if n.type == NODE_NONE and free is None:
                free = n
        return free


class State:
    """Only the fields an opcode can set. viewRot/viewPos/lights[] are
    derived per frame by the renderer, so they are the C++ side's business
    and are deliberately absent here."""

    def __init__(self):
        self.vpX, self.vpY = 0, 0
        self.vpW, self.vpH = SURFACE_W, SURFACE_H
        self.fov = 10923
        self.focal = focal_from_fov(self.fov, self.vpW)
        self.nearZ = FX16_ONE
        self.farZ = 256 * FX16_ONE
        self.light_dir = [0, 0, -(FX15_ONE - 1)]
        self.ambient = 4
        self.background = 0
        self.colormap_valid = False


FRAME_STREAM_DOC = """\
# gpu64 class-1 frame stream, v1. Line-based on purpose: Python writes it and
# tools/hostsim/scenesim.cpp reads it with sscanf, and a stream a human can
# diff is worth more during bring-up than a compact one.
#
#   PAL <file>                        256 x RGB, the palette in force
#   COLORMAP                          BUILD_COLORMAP ran against that palette
#   MESH <id> <vertfile> <facefile>   an UPLOAD_MESH that succeeded
#   TEX <id> <file> <wshift> <hshift> an UPLOAD_TEXTURE that succeeded
#   FREE <id>                         FREE_RESOURCE
#   RESET                             SCENE_RESET
#   FRAME <n> <page>                  one accepted autonomous frame
#     ST vp <x> <y> <w> <h>
#     ST persp <fov> <near16.16> <far16.16>
#     ST light <dx> <dy> <dz> <ambient>      raw SET_LIGHT s16 triple
#     ST bg <index> <colormapvalid>
#     CAM <id>                        or -1 for none
#     N <slot> <id> <type> <visible> <x> <y> <z> <yaw> <pitch> <roll> <scale>
#       <meshid> <texid> <spritew> <spriteh> <spriteflags> <lstrength>
#       <lradius>
#   ENDFRAME
#
# Node lines are emitted in scene-slot order, which is the order
# gpu64_3dSceneRender() walks -- a frame that renders differently because two
# nodes swapped slots is a real difference and must survive the round trip.
"""


class Class1Mixin:
    """Mixed into Gpu64Model. Everything it owns is prefixed c1_ except the
    execute entry point, to keep it obvious which half of the model a name
    belongs to."""

    def c1_init(self):
        self.c1_state = State()
        self.c1_scene = Scene()
        self.c1_shadow_state = State()
        self.c1_shadow_scene = Scene()
        self.c1_loop_running = False
        self.c1_res = {}                # id -> ('mesh'|'tex', payload)
        self.c1_arena_used = 0
        self.c1_frames = 0
        self.c1_ready_deferred = False
        # The page the frame now in flight is rendering into. pollLoopFrame()
        # publishes it as RESULT at the moment it raises FRAME_READY, which is
        # what the C64 reads back after a SCENE_COMMIT -- without this the
        # model leaves RESULT at whatever the previous opcode put there and
        # any program that checks the page sees a stale value.
        self.c1_frame_page = 0
        # Set by the runner to open the frame stream; None means "model the
        # protocol but write nothing", which is what every non-class-1 run
        # does.
        self.c1_stream = None
        self.c1_stream_dir = None
        self.c1_blobs = 0
        self.frame_hook = None
        # The level file the Pi would have read off its SD card at start-up,
        # set by the runner from --level=. None models a card with no
        # RAD/level.g64lev on it, which is what LOAD_LEVEL's BAD_ARGS means
        # and is worth being able to simulate.
        self.c1_level_data = None
        self.c1_load = None
        self.c1_clip_lib = None

    # --- refusal bookkeeping (Source/Firmware/gpu64_apidiag.h) ----------
    def c1_loop_stop(self, cause):
        if self.c1_loop_running:
            self.loop_stops += 1
        self.loop_stop_cause = cause
        self.loop_stop_seq = self.disp_seq
        self.loop_stop_seq_prev = self.disp_seq_prev
        self.loop_stop_prev_op = self.disp_op_prev
        self.c1_loop_running = False

    def c1_refuse(self, op):
        self.last_refuse_state = self.diag_state_byte()
        self.last_refuse_op = op
        self.last_refuse_class = 1

    def diag_state_byte(self):
        st = 0
        if self.c1_loop_running:
            st |= 0x01
        if self.calibrated:
            st |= 0x02
        from gpu64model import MODE_GRAPHICS, ST_BUSY
        if self.mode == MODE_GRAPHICS:
            st |= 0x04
        if self.flip_pending:
            st |= 0x08
        if self.status & ST_FRAME_READY:
            st |= 0x10
        if self.status & ST_BUSY:
            st |= 0x20
        st |= 0x40                                      # framebuffer is always up here
        return st

    # --- redirection ----------------------------------------------------
    def c1_scene_target(self):
        return self.c1_shadow_scene if self.c1_loop_running else self.c1_scene

    def c1_state_target(self):
        return self.c1_shadow_state if self.c1_loop_running else self.c1_state

    def c1_id(self):
        return self.ident[0] | (self.ident[1] << 8)

    # --- the frame stream ------------------------------------------------
    def c1_emit(self, line):
        if self.c1_stream is not None:
            self.c1_stream.write(line + "\n")

    def c1_write_blob(self, tag, data):
        """Blobs go beside the stream as raw files: scenesim feeds them to
        the firmware's own gpu64_3dBuildMesh()/gpu64_3dBuildTexture(), so
        what it parses is byte-for-byte what the C64 uploaded."""
        if self.c1_stream is None:
            return '-'
        self.c1_blobs += 1
        name = "blob%04d_%s.bin" % (self.c1_blobs, tag)
        with open(os.path.join(self.c1_stream_dir, name), 'wb') as f:
            f.write(bytes(data))
        return name

    def c1_emit_palette(self):
        name = self.c1_write_blob('pal', self.palette)
        self.c1_emit("PAL %s" % name)

    def c1_emit_frame(self, page):
        self.c1_frames += 1
        if self.frame_hook is not None:
            self.frame_hook(page)
        if self.c1_stream is None:
            return
        st, sc = self.c1_state, self.c1_scene
        self.c1_emit("FRAME %d %d" % (self.c1_frames, page))
        self.c1_emit("ST vp %d %d %d %d" % (st.vpX, st.vpY, st.vpW, st.vpH))
        self.c1_emit("ST persp %d %d %d" % (st.fov, st.nearZ, st.farZ))
        self.c1_emit("ST light %d %d %d %d"
                     % (st.light_dir[0], st.light_dir[1], st.light_dir[2],
                        st.ambient))
        self.c1_emit("ST bg %d %d" % (st.background, 1 if st.colormap_valid else 0))
        self.c1_emit("CAM %d" % (sc.active_camera_id if sc.have_active_camera else -1))
        for i, n in enumerate(sc.node):
            if n.type == NODE_NONE:
                continue
            # The id, not just the slot: CAM names a node by id, and so do
            # the scene's own lookups. Emitting only the slot left every node
            # with id 0 on the scenesim side and no camera ever resolved.
            self.c1_emit("N %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d"
                         % (i, n.id, n.type, 1 if n.visible else 0,
                            n.pos[0], n.pos[1], n.pos[2],
                            n.yaw, n.pitch, n.roll, n.scale,
                            n.mesh_id, n.tex_id, n.sprite_w, n.sprite_h,
                            n.sprite_flags, n.light_strength, n.light_radius))
        self.c1_emit("ENDFRAME")

    # --- LOAD_LEVEL / LEVEL_STEP -----------------------------------------
    #
    # The firmware reads RAD/level.g64lev off the SD card once, at REU
    # start-up, and LOAD_LEVEL then builds a whole level out of memory in
    # bounded slices. The model does the same from --level=, through the very
    # same c1_res / c1_scene / frame-stream paths UPLOAD_MESH and
    # CREATE_OBJECT use -- so a level built this way renders in scenesim
    # exactly as an uploaded one does, and gpu64_demo_level.a gets a picture
    # on a PC without a bench trip.
    #
    # What is NOT modelled: the arena. levelBuildTexture/levelBuildMesh can
    # answer OUT_OF_MEMORY on the Pi at a size this side has no notion of.
    # That is a capacity question for tools/check_g64lev.py, not for a
    # protocol model.
    def c1_load_level(self, mesh_base, node_base, camera_id):
        from gpu64model import ERR_OK, ERR_BAD_ARGS, ERR_BUSY
        import gpu64level

        if self.c1_loop_running:
            return ERR_BUSY             # a whole level will not fit the shadow
        if self.c1_level_data is None:
            return ERR_BAD_ARGS         # no level file on the card

        try:
            lev = gpu64level.Level(self.c1_level_data)
        except gpu64level.LevelError:
            return ERR_BAD_ARGS

        # Texture resource ids are forced to 0..ntex-1 by the one-byte texid
        # in a face record, so the mesh ids must start above them.
        if mesh_base < lev.ntex:
            return ERR_BAD_ARGS
        if mesh_base + lev.nmesh > 0x10000 or node_base + lev.nnode > 0x10000:
            return ERR_BAD_ARGS
        if camera_id != 0 and node_base <= camera_id < node_base + lev.nnode:
            return ERR_BAD_ARGS

        self.c1_res = {}
        self.c1_arena_used = 0
        self.c1_scene = Scene()
        self.c1_shadow_scene = Scene()
        # c1_state is deliberately left alone: the viewport and the clip
        # planes belong to the caller. opLoadLevel() says why.
        self.c1_emit("RESET")

        self.c1_load = {
            'lev': lev, 'phase': 'palette', 'index': 0,
            'mesh_base': mesh_base, 'node_base': node_base,
            'camera_id': camera_id, 'done': 0,
            'total': 1 + lev.ntex + lev.nmesh + lev.nnode + 1,
        }
        self.result = 0
        return ERR_OK

    # --- CLIP_MOVE ($15) -------------------------------------------------
    #
    # The block layout is in Source/Firmware/gpu64_3d.h. The trace itself is
    # NOT modelled here: it is called in the firmware's own C, through
    # tools/hostsim/libclipmove.so, for the reason at the top of this file --
    # a Python hull trace that could disagree with the Pi's would be a second
    # thing to be wrong, not an oracle. What this method models is the
    # protocol around it: the magics, the checksums, the blob window, and
    # which failures write nothing back.
    CLIPMOVE_BYTES = 56
    CLIPMOVE_IN_BYTES = 32
    CLIPMOVE_OUT_OFF = 32
    CLIPMOVE_OUT_BYTES = 24
    CLIPMOVE_MAGIC_IN = 0xC5
    CLIPMOVE_MAGIC_OUT = 0x5C
    CLIPMOVE_MOVER_OFF = 56
    CLIPMOVE_MOVER_BYTES = 16
    CLIPMOVE_MAX_MOVERS = 16

    def c1_clipmove_lib(self):
        """The shared library, loaded once and fed the level file the model is
        already holding. None if it was never built -- the caller turns that
        into a hard error rather than a wrong answer."""
        import ctypes
        import os

        if getattr(self, 'c1_clip_lib', None) is not None:
            return self.c1_clip_lib
        if self.c1_level_data is None:
            return None

        so = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          '..', 'hostsim', 'libclipmove.so')
        if not os.path.exists(so):
            raise RuntimeError(
                "CLIP_MOVE needs %s -- run 'make -C tools/hostsim'" % so)
        lib = ctypes.CDLL(so)
        lib.gpu64shim_open.restype = ctypes.c_int
        lib.gpu64shim_open.argtypes = [ctypes.c_char_p, ctypes.c_uint]
        lib.gpu64shim_move_ents.restype = ctypes.c_int
        lib.gpu64shim_move_ents.argtypes = [ctypes.POINTER(ctypes.c_int),
                                            ctypes.c_int, ctypes.c_int,
                                            ctypes.c_int,
                                            ctypes.POINTER(ctypes.c_int),
                                            ctypes.c_int,
                                            ctypes.POINTER(ctypes.c_int)]
        blob = bytes(self.c1_level_data)
        if not lib.gpu64shim_open(blob, len(blob)):
            raise RuntimeError("libclipmove.so refused the level file")
        self.c1_clip_lib = lib
        return lib

    # --clip-fault=N corrupts the Nth answer in a way its own checksum
    # cannot see; see the comment at the injection point below.
    c1_clip_fault = 0
    c1_clip_fault_kind = 'x'
    c1_clip_n = 0

    def c1_clip_move(self, space, addr, length):
        import ctypes
        import struct
        from gpu64model import ERR_OK, ERR_BAD_ARGS

        if length < self.CLIPMOVE_BYTES:
            return ERR_BAD_ARGS
        if self.c1_load is None or self.c1_load['phase'] != 'done':
            return ERR_BAD_ARGS

        err, blk = self.blob_read(space, addr, self.CLIPMOVE_IN_BYTES)
        if err != ERR_OK:
            return err

        if blk[27] != self.CLIPMOVE_MAGIC_IN:
            return ERR_BAD_ARGS

        nmv = blk[29]
        if nmv > self.CLIPMOVE_MAX_MOVERS:
            return ERR_BAD_ARGS
        mvbytes = nmv * self.CLIPMOVE_MOVER_BYTES
        if length < self.CLIPMOVE_MOVER_OFF + mvbytes:
            return ERR_BAD_ARGS
        mvb = bytearray()
        if mvbytes:
            err, mvb = self.blob_read(space, addr + self.CLIPMOVE_MOVER_OFF,
                                      mvbytes)
            if err != ERR_OK:
                return err

        x = blk[29]
        for b in blk[:28]:
            x ^= b
        for b in mvb:
            x ^= b
        if x != blk[28]:
            return ERR_BAD_ARGS

        lib = self.c1_clipmove_lib()
        if lib is None:
            return ERR_BAD_ARGS

        sixin = (ctypes.c_int * 6)(*struct.unpack('<6i', bytes(blk[0:24])))
        movers = (ctypes.c_int * (4 * max(nmv, 1)))()
        for i in range(nmv):
            r = mvb[i * self.CLIPMOVE_MOVER_BYTES:]
            movers[i * 4] = r[0]
            ox, oy, oz = struct.unpack('<3i', bytes(r[4:16]))
            movers[i * 4 + 1] = ox
            movers[i * 4 + 2] = oy
            movers[i * 4 + 3] = oz
        out = (ctypes.c_int * 7)()
        if not lib.gpu64shim_move_ents(sixin, blk[26], blk[24], blk[25],
                                       movers, nmv, out):
            return ERR_BAD_ARGS

        o = bytearray(self.CLIPMOVE_OUT_BYTES)
        o[0:12] = struct.pack('<3i', out[0], out[1], out[2])
        o[12] = out[3] & 0xFF
        o[13] = out[4] & 0xFF
        o[14] = out[5] & 0xFF
        o[15] = out[6] & 0xFF
        # Fault injection, --clip-fault=N. Reproduce bench run 36: one byte
        # of the answer's x coordinate arrives wrong, and because the damage
        # happened on the way to the C64 the trailer is computed over the
        # corrupted block, so the magic and the checksum both pass. A client
        # that only checks those accepts it, writes it into a position it is
        # itself authoritative about, and is 222 world units outside the map
        # for the rest of the session. Catching it needs a bound on the
        # answer, not a better hash -- and an assertion that has never fired
        # is not an assertion, which is what this switch is for.
        #
        # --clip-fault=N:solid and N:void reach the recovery behind that
        # bound instead, which a coordinate fault never can: one answer
        # that says ALLSOLID and stays put (a bad start the client must
        # leave), and -- from answer N on -- a world with no floor, every
        # answer the asked-for move with no flags (bench run 39: a player
        # through the floor, falling at terminal speed for good).
        self.c1_clip_n += 1
        kind = self.c1_clip_fault_kind
        if self.c1_clip_fault and kind == 'x' and self.c1_clip_n == self.c1_clip_fault:
            o[3] = 0xFF
        if self.c1_clip_fault and kind == 'solid' and self.c1_clip_n == self.c1_clip_fault:
            o[0:12] = bytes(blk[0:12])
            o[12] = 0x20
        if self.c1_clip_fault and kind == 'void' and self.c1_clip_n >= self.c1_clip_fault:
            sx, sy, sz, dx, dy, dz = struct.unpack('<6i', bytes(blk[0:24]))
            o[0:12] = struct.pack('<3i', sx + dx, sy + dy, sz + dz)
            o[12] = 0

        o[16] = self.CLIPMOVE_MAGIC_OUT
        x = 0
        for b in o[:17]:
            x ^= b
        o[17] = x

        err = self.blob_write(space, addr + self.CLIPMOVE_OUT_OFF, o)
        if err != ERR_OK:
            return err
        self.result = o[12]
        return ERR_OK

    # --- LEVEL_ENT ($16) -------------------------------------------------
    #
    # The block layout is in Source/Firmware/gpu64_3d.h. Unlike CLIP_MOVE
    # there is no algorithm here to get wrong -- the answer is a record out of
    # the level file, a box out of its hull table and a run of node ids -- so
    # this one is modelled in Python rather than called in the firmware.

    LEVELENT_BYTES = 96
    LEVELENT_IN_BYTES = 4
    LEVELENT_OUT_OFF = 16
    LEVELENT_OUT_BYTES = 80
    LEVELENT_MAGIC_IN = 0xC6
    LEVELENT_MAGIC_OUT = 0x6C

    def c1_level_ent(self, space, addr, length):
        import struct
        from gpu64model import ERR_OK, ERR_BAD_ARGS, ERR_OUT_OF_RANGE

        if length < self.LEVELENT_BYTES:
            return ERR_BAD_ARGS
        if self.c1_load is None or self.c1_load['phase'] != 'done':
            return ERR_BAD_ARGS

        err, blk = self.blob_read(space, addr, self.LEVELENT_IN_BYTES)
        if err != ERR_OK:
            return err
        if blk[2] != self.LEVELENT_MAGIC_IN:
            return ERR_BAD_ARGS
        x = 0
        for b in blk[:3]:
            x ^= b
        if x != blk[3]:
            return ERR_BAD_ARGS

        lev = self.c1_load['lev']
        index = blk[0] | (blk[1] << 8)
        if index >= lev.nent:
            return ERR_OUT_OF_RANGE
        e = lev.ent(index)

        mins = maxs = (0, 0, 0)
        first = count = 0
        if e['model']:
            h = lev.hull(e['model'])
            mins, maxs = h['mins'], h['maxs']
            for i in range(lev.nnode):
                if lev.node_model(i) != e['model']:
                    continue
                if count == 0:
                    first = self.c1_load['node_base'] + i
                if count < 255:
                    count += 1

        o = bytearray(self.LEVELENT_OUT_BYTES)
        o[0:12] = struct.pack('<3i', e['x'], e['y'], e['z'])
        o[12:24] = struct.pack('<3i', *mins)
        o[24:36] = struct.pack('<3i', *maxs)
        o[36:48] = struct.pack('<3i', *e['ofs'])
        o[48:64] = struct.pack('<8H', e['yaw'] & 0xFFFF, e['spawnflags'] & 0xFFFF,
                               e['p0'] & 0xFFFF, e['p1'] & 0xFFFF,
                               e['target_id'], e['targetname_id'],
                               first & 0xFFFF, lev.nent)
        o[64] = count
        o[65] = e['kind']
        o[66] = e['model'] & 0xFF if e['model'] < 256 else 0
        o[67] = 0
        o[68] = self.LEVELENT_MAGIC_OUT
        x = 0
        for b in o[:69]:
            x ^= b
        o[69] = x

        err = self.blob_write(space, addr + self.LEVELENT_OUT_OFF, o)
        if err != ERR_OK:
            return err
        self.result = e['kind']
        return ERR_OK

    def c1_level_step(self):
        from gpu64model import (ERR_OK, ERR_BAD_ARGS, ERR_BUSY,
                                ERR_OUT_OF_MEMORY)
        import gpu64level

        ld = self.c1_load
        if ld is None:
            return ERR_BAD_ARGS         # LEVEL_STEP without a LOAD_LEVEL
        if self.c1_loop_running:
            return ERR_BUSY             # a step creates nodes in the LIVE scene
        if ld['phase'] == 'done':
            self.result = LEVEL_DONE    # idempotent: a lost RESULT read retries
            return ERR_OK

        lev = ld['lev']
        budget = LEVEL_SLICE_BYTES

        # At least one item per call even if it blows the budget on its own,
        # or a single 128x128 texture would stall the load forever.
        while ld['phase'] != 'done':
            nbytes = 64                 # bookkeeping phases: not data
            try:
                if ld['phase'] == 'palette':
                    self.palette = bytearray(lev.palette)
                    self.c1_state.colormap_valid = True
                    self.c1_emit_palette()
                    self.c1_emit("COLORMAP")
                    nbytes = 768
                elif ld['phase'] == 'textures':
                    ws, hs, pix = lev.tex(ld['index'])
                    rid = ld['index']
                    self.c1_res[rid] = ('tex', (pix, ws, hs))
                    self.c1_arena_used += len(pix)
                    self.c1_emit("TEX %d %s %d %d"
                                 % (rid, self.c1_write_blob('tex', pix), ws, hs))
                    nbytes = len(pix)
                elif ld['phase'] == 'meshes':
                    vb, fb = lev.mesh(ld['index'])
                    if len(vb) == 0 or len(vb) % 6 or len(fb) == 0 or len(fb) % 12:
                        return ERR_BAD_ARGS
                    rid = ld['mesh_base'] + ld['index']
                    self.c1_res[rid] = ('mesh', (vb, fb))
                    self.c1_arena_used += len(vb) * 4 + len(fb) * 4
                    self.c1_emit("MESH %d %s %s"
                                 % (rid, self.c1_write_blob('mv', vb),
                                    self.c1_write_blob('mf', fb)))
                    nbytes = len(vb) + len(fb)
                elif ld['phase'] == 'nodes':
                    mi, x, y, z = lev.node(ld['index'])
                    n = self.c1_scene.slot(ld['node_base'] + ld['index'])
                    if n is None:
                        return ERR_OUT_OF_MEMORY
                    n.reset(ld['node_base'] + ld['index'], NODE_OBJECT)
                    n.mesh_id = ld['mesh_base'] + mi
                    n.pos = [x, y, z]
                elif ld['phase'] == 'camera':
                    if ld['camera_id'] != 0:
                        st = lev.player_start()
                        if st is None:
                            return ERR_BAD_ARGS
                        n = self.c1_scene.slot(ld['camera_id'])
                        if n is None:
                            return ERR_OUT_OF_MEMORY
                        n.reset(ld['camera_id'], NODE_CAMERA)
                        n.pos = [st[0], st[1], st[2]]
                        n.yaw = st[3]
                        self.c1_scene.active_camera_id = ld['camera_id']
                        self.c1_scene.have_active_camera = True
            except gpu64level.LevelError:
                # The failing item names itself in RESULT, and the phase stays
                # put so a retry re-attempts the same item.
                self.result = ld['index'] & 0xFF
                self.c1_load = None
                return ERR_BAD_ARGS

            ld['done'] += 1
            ld['index'] += 1

            nxt = {'palette': 'textures', 'camera': 'done'}
            if ld['phase'] in nxt:
                ld['phase'] = nxt[ld['phase']]
                ld['index'] = 0
            elif ld['phase'] == 'textures' and ld['index'] >= lev.ntex:
                ld['phase'], ld['index'] = 'meshes', 0
            elif ld['phase'] == 'meshes' and ld['index'] >= lev.nmesh:
                ld['phase'], ld['index'] = 'nodes', 0
            elif ld['phase'] == 'nodes' and ld['index'] >= lev.nnode:
                ld['phase'], ld['index'] = 'camera', 0

            if nbytes >= budget:
                break
            budget -= nbytes

        if ld['phase'] == 'done':
            self.result = LEVEL_DONE
            return ERR_OK
        pct = ld['done'] * 100 // ld['total'] if ld['total'] else 0
        self.result = min(pct, 99)
        return ERR_OK

    # --- execute ----------------------------------------------------------
    def execute_r1(self, op):
        from gpu64model import (ERR_OK, ERR_BAD_OPCODE, ERR_OUT_OF_RANGE,
                                ERR_BAD_ARGS, ERR_UNSUPPORTED, ERR_BUSY,
                                ERR_OUT_OF_MEMORY, ERR_BAD_ID,
                                KEY_DESTRUCTIVE)
        NO_CAMERA = 0x0B
        a = self.arg

        def u16(i):
            return a[i] | (a[i + 1] << 8)

        def i16(i):
            return s16(u16(i))

        def i32(i):
            return s32(a[i] | (a[i + 1] << 8) | (a[i + 2] << 16) | (a[i + 3] << 24))

        def node_err(n):
            return ERR_OK if n is not None else ERR_BAD_ID

        # The one-shot key, mirroring the gate at the top of execute() in
        # Source/Firmware/gpu64_3d_class1.cpp. Unconditional: a destructive
        # opcode is destructive whether or not the loop happens to be
        # running, and ARG15 (spent by every dispatch, read by no opcode)
        # is what says the C64 meant it. See GPU64_KEY_DESTRUCTIVE in
        # Source/Firmware/gpu64_api.h for the bench runs behind it.
        if op in (OP_SCENE_RESET, OP_LOOP_STOP,
                  OP_DESTROY_NODE, OP_FREE_RESOURCE, OP_LOAD_LEVEL):
            if self.api_key != KEY_DESTRUCTIVE:
                self.key_refused += 1
                self.key_refused_op = op
                return ERR_BAD_ARGS

        if op == OP_SCENE_RESET:
            if self.c1_scene.have_active_camera:
                self.scene_wipes += 1
            self.c1_state = State()
            self.c1_scene = Scene()
            self.c1_loop_stop(2)                        # GPU64_LOOPSTOP_SCENE_RESET
            self.status &= ~ST_FRAME_READY
            self.c1_shadow_state = State()
            self.c1_shadow_scene = Scene()
            self.c1_load = None         # levelLoadAbort(): the scene it was
                                        # filling no longer exists
            self.c1_emit("RESET")
            return ERR_OK

        if op == OP_SET_VIEWPORT:
            x, y, w, h = u16(0), u16(2), u16(4), u16(6)
            if w == 0 or h == 0:
                return ERR_BAD_ARGS
            if x + w > SURFACE_W or y + h > SURFACE_H:
                return ERR_BAD_ARGS
            if w * h * 3 > BUDGET:
                return ERR_OUT_OF_RANGE
            st = self.c1_state_target()
            st.vpX, st.vpY, st.vpW, st.vpH = x, y, w, h
            # The focal length is derived from the fov and the viewport
            # width, so a viewport change re-derives it.
            st.focal = focal_from_fov(st.fov, w)
            return ERR_OK

        if op == OP_SET_PERSPECTIVE:
            fov = u16(0)
            near = i16(2) << 8
            far = i16(4) << 8
            if near <= 0 or far <= near:
                return ERR_BAD_ARGS
            st = self.c1_state_target()
            focal = focal_from_fov(fov, st.vpW)
            if focal <= 0:
                return ERR_BAD_ARGS
            st.fov, st.focal, st.nearZ, st.farZ = fov, focal, near, far
            return ERR_OK

        if op == OP_SET_LIGHT:
            if a[6] > 15:
                return ERR_BAD_ARGS
            if i16(0) == 0 and i16(2) == 0 and i16(4) == 0:
                return ERR_BAD_ARGS         # a zero-length direction
            st = self.c1_state_target()
            # Stored raw: gpu64_3dNormalise() is the renderer's, and running
            # it here would be a second normaliser to disagree with.
            st.light_dir = [i16(0), i16(2), i16(4)]
            st.ambient = a[6]
            return ERR_OK

        if op == OP_BUILD_COLORMAP:
            self.c1_state_target().colormap_valid = True
            self.c1_emit_palette()
            self.c1_emit("COLORMAP")
            return ERR_OK

        if op == OP_SET_BACKGROUND:
            self.c1_state_target().background = a[0]
            return ERR_OK

        if op == OP_ARENA_STATUS:
            units = (ARENA_BYTES - self.c1_arena_used) >> 17
            self.result = 255 if units > 255 else units
            return ERR_OK

        if op == OP_UPLOAD_MESH:
            sv, av, lv = self.a_blob(0)
            sf, af, lf = self.a_blob(6)
            if lv == 0 or lf == 0 or lv > STAGE_BYTES or lf > STAGE_BYTES:
                return ERR_BAD_ARGS
            err, vb = self.blob_read(sv, av, lv)
            if err != ERR_OK:
                return err
            err, fb = self.blob_read(sf, af, lf)
            if err != ERR_OK:
                return err
            if len(self.c1_res) >= MAX_RESOURCES and self.c1_id() not in self.c1_res:
                return ERR_OUT_OF_MEMORY
            if lv % 6 or lf % 12:
                return ERR_BAD_ARGS
            self.c1_res[self.c1_id()] = ('mesh', (vb, fb))
            self.c1_arena_used += lv * 4 + lf * 4
            self.result = min(255, lf // 12)
            self.c1_emit("MESH %d %s %s"
                         % (self.c1_id(),
                            self.c1_write_blob('mv', vb),
                            self.c1_write_blob('mf', fb)))
            return ERR_OK

        if op == OP_UPLOAD_TEXTURE:
            sp, ad, ln = self.a_blob(0)
            if ln == 0 or ln > STAGE_BYTES:
                return ERR_BAD_ARGS
            err, blob = self.blob_read(sp, ad, ln)
            if err != ERR_OK:
                return err
            if (1 << a[6]) * (1 << a[7]) != ln:
                return ERR_BAD_ARGS
            if len(self.c1_res) >= MAX_RESOURCES and self.c1_id() not in self.c1_res:
                return ERR_OUT_OF_MEMORY
            self.c1_res[self.c1_id()] = ('tex', (blob, a[6], a[7]))
            self.c1_arena_used += ln
            self.c1_emit("TEX %d %s %d %d"
                         % (self.c1_id(), self.c1_write_blob('tex', blob),
                            a[6], a[7]))
            return ERR_OK

        if op == OP_FREE_RESOURCE:
            if self.c1_id() not in self.c1_res:
                return ERR_BAD_ID
            del self.c1_res[self.c1_id()]
            self.c1_emit("FREE %d" % self.c1_id())
            return ERR_OK

        if op == OP_LOAD_LEVEL:
            return self.c1_load_level(u16(0), u16(2), u16(4))

        if op == OP_LEVEL_STEP:
            return self.c1_level_step()

        if op == OP_CLIP_MOVE:
            return self.c1_clip_move(*self.a_blob(0))
        if op == OP_LEVEL_ENT:
            return self.c1_level_ent(*self.a_blob(0))
        if op == OP_LEVEL_PALETTE:
            # gpu64_3d.h has the contract: rewrite only what differs from
            # the level's own palette, and rebuild the colormap only if
            # anything did.
            ld = self.c1_load
            if ld is None or ld['phase'] != 'done':
                return ERR_BAD_ARGS
            want = bytes(ld['lev'].palette)
            fixed = sum(1 for i in range(256)
                        if self.palette[i * 3:i * 3 + 3] != want[i * 3:i * 3 + 3])
            if fixed:
                self.palette = bytearray(want)
                self.c1_state_target().colormap_valid = True
                self.c1_emit_palette()
                self.c1_emit("COLORMAP")
            self.result = min(fixed, 255)
            return ERR_OK

        if op == OP_LEVEL_NODE:
            # gpu64_3d.h: put one level node back the way LEVEL_STEP built
            # it; ARG0 bit 0 also restores the file's position.
            ld = self.c1_load
            if ld is None or ld['phase'] != 'done':
                return ERR_BAD_ARGS
            if self.arg[0] & ~1:
                return ERR_BAD_ARGS
            nid = self.c1_id()
            i = (nid - ld['node_base']) & 0xFFFF
            if nid < ld['node_base'] or i >= ld['lev'].nnode:
                return ERR_BAD_ID
            mi, x, y, z = ld['lev'].node(i)
            if mi >= ld['lev'].nmesh:
                return ERR_BAD_ARGS
            mesh_id = (ld['mesh_base'] + mi) & 0xFFFF
            sc = self.c1_scene_target()
            n = sc.find(nid)
            fixed = 0
            if (n is None or n.type != NODE_OBJECT or n.mesh_id != mesh_id
                    or not n.visible or n.scale != FX8_ONE
                    or n.yaw or n.pitch or n.roll):
                keep = list(n.pos) if n is not None else [x, y, z]
                if sc.have_active_camera and sc.active_camera_id == nid:
                    sc.have_active_camera = False
                n = sc.slot(nid)
                if n is None:
                    return ERR_OUT_OF_MEMORY
                n.reset(nid, NODE_OBJECT)
                n.mesh_id = mesh_id
                n.pos = keep
                fixed |= 1
            if (self.arg[0] & 1) and list(n.pos) != [x, y, z]:
                n.pos = [x, y, z]
                fixed |= 2
            self.result = fixed
            return ERR_OK

        # --- the scene graph ---------------------------------------------
        if op in (OP_CREATE_OBJECT, OP_CREATE_CAMERA, OP_CREATE_SPRITE,
                  OP_CREATE_LIGHT):
            sc = self.c1_scene_target()
            n = sc.slot(self.c1_id())
            if n is None:
                return ERR_OUT_OF_MEMORY
            if op == OP_CREATE_OBJECT:
                n.reset(self.c1_id(), NODE_OBJECT)
                n.mesh_id = u16(0)
            elif op == OP_CREATE_CAMERA:
                n.reset(self.c1_id(), NODE_CAMERA)
            elif op == OP_CREATE_SPRITE:
                n.reset(self.c1_id(), NODE_SPRITE)
                n.tex_id = u16(0)
                # One world unit square, masked off, screen-upright.
                n.sprite_w = n.sprite_h = FX8_ONE
            else:
                # Created off: strength and radius stay zero until
                # SET_POINT_LIGHT gives it both.
                n.reset(self.c1_id(), NODE_LIGHT)
            return ERR_OK

        if op == OP_DESTROY_NODE:
            sc = self.c1_scene_target()
            n = sc.find(self.c1_id())
            if n is None:
                return ERR_BAD_ID
            had_camera = self.c1_scene.have_active_camera
            if sc.have_active_camera and sc.active_camera_id == self.c1_id():
                sc.have_active_camera = False
            n.type = NODE_NONE
            # Counted against the LIVE scene, as the firmware does: losing
            # the shadow's camera is repaired by the next commit, losing the
            # live one is what run 14 never recovered from.
            if had_camera and not self.c1_scene.have_active_camera:
                self.cam_lost += 1
            return ERR_OK

        if op == OP_SET_ACTIVE_CAMERA:
            sc = self.c1_scene_target()
            if sc.find(self.c1_id(), NODE_CAMERA) is None:
                return ERR_BAD_ID
            sc.active_camera_id = self.c1_id()
            sc.have_active_camera = True
            return ERR_OK

        if op == OP_SET_VISIBLE:
            if a[0] > 1:
                return ERR_BAD_ARGS
            n = self.c1_scene_target().find(self.c1_id())
            if n is None:
                return ERR_BAD_ID
            n.visible = a[0] != 0
            return ERR_OK

        if op == OP_SET_SPRITE:
            n = self.c1_scene_target().find(self.c1_id(), NODE_SPRITE)
            if n is None:
                return ERR_BAD_ID
            n.sprite_w, n.sprite_h, n.sprite_flags = u16(0), u16(2), a[4]
            return ERR_OK

        if op == OP_SET_POINT_LIGHT:
            n = self.c1_scene_target().find(self.c1_id(), NODE_LIGHT)
            if n is None:
                return ERR_BAD_ID
            n.light_strength, n.light_radius = a[0], u16(1)
            return ERR_OK

        if op == OP_SET_POSITION:
            n = self.c1_scene_target().find(self.c1_id())
            if n is None:
                return ERR_BAD_ID
            n.pos = [i32(0), i32(4), i32(8)]
            return ERR_OK

        if op == OP_SET_ORIENTATION:
            n = self.c1_scene_target().find(self.c1_id())
            if n is None:
                return ERR_BAD_ID
            n.yaw, n.pitch, n.roll = u16(0), u16(2), u16(4)
            return ERR_OK

        if op == OP_MOVE_LOCAL:
            n = self.c1_scene_target().find(self.c1_id())
            if n is None:
                return ERR_BAD_ID
            d = vec_rotate(mat_from_euler(n.yaw, n.pitch, n.roll),
                           (i16(0) << 8, i16(2) << 8, i16(4) << 8))
            for k in range(3):
                n.pos[k] = s32(n.pos[k] + d[k])
            return ERR_OK

        if op == OP_MOVE_WORLD:
            n = self.c1_scene_target().find(self.c1_id())
            if n is None:
                return ERR_BAD_ID
            for k in range(3):
                n.pos[k] = s32(n.pos[k] + (i16(2 * k) << 8))
            return ERR_OK

        if op == OP_ROTATE_LOCAL:
            n = self.c1_scene_target().find(self.c1_id())
            if n is None:
                return ERR_BAD_ID
            n.yaw = (n.yaw + u16(0)) & 0xFFFF
            n.pitch = (n.pitch + u16(2)) & 0xFFFF
            n.roll = (n.roll + u16(4)) & 0xFFFF
            return ERR_OK

        if op == OP_SET_SCALE:
            if u16(0) == 0:
                return ERR_BAD_ARGS
            n = self.c1_scene_target().find(self.c1_id())
            if n is None:
                return ERR_BAD_ID
            n.scale = u16(0)
            return ERR_OK

        if op == OP_GET_TRANSFORM:
            sp, ad, ln = self.a_blob(0)
            if ln < 18:
                return ERR_BAD_ARGS
            n = self.c1_scene_target().find(self.c1_id())
            if n is None:
                return ERR_BAD_ID
            buf = bytearray()
            for v in n.pos:
                buf += (v & 0xFFFFFFFF).to_bytes(4, 'little')
            for v in (n.yaw, n.pitch, n.roll):
                buf += v.to_bytes(2, 'little')
            return self.blob_write(sp, ad, buf)

        # --- immediate mode ------------------------------------------------
        # CLEAR_VIEWPORT/DRAW_MESH/DRAW_NODE draw one thing, right now, into
        # the draw page. The model does not rasterise class 1 (see the module
        # docstring), so it validates them and reports what they would have
        # answered -- which is what the protocol half is for -- and draws
        # nothing.
        if op in (OP_CLEAR_VIEWPORT, OP_DRAW_MESH, OP_DRAW_NODE):
            if self.c1_loop_running:
                return ERR_BUSY
            if op == OP_DRAW_MESH:
                r = self.c1_res.get(self.c1_id())
                if r is None or r[0] != 'mesh':
                    return ERR_BAD_ID
                if u16(12) == 0:
                    return ERR_BAD_ARGS
                self.result = 0
                return ERR_OK
            if op == OP_DRAW_NODE:
                n = self.c1_scene.find(self.c1_id())
                if n is None or n.type not in (NODE_OBJECT, NODE_SPRITE):
                    return ERR_BAD_ID
                if not n.visible:
                    self.result = 0
                    return ERR_OK
                if n.type == NODE_OBJECT:
                    r = self.c1_res.get(n.mesh_id)
                    if r is None or r[0] != 'mesh':
                        return ERR_BAD_ID
                self.result = 0
            return ERR_OK

        # --- Stage 16: the autonomous loop ---------------------------------
        if op == OP_LOOP_START:
            if self.c1_loop_running:
                return ERR_BUSY
            # Mode 1 (free-running) is Stage 18; answering UNSUPPORTED rather
            # than quietly running mode 0 is what keeps a program written for
            # a later firmware from running the wrong protocol here.
            if a[0] != 0:
                return ERR_UNSUPPORTED
            return self.c1_loop_start_armed()

        if op == OP_LOOP_STOP:
            # Always succeeds, even against a loop that was not running.
            self.c1_loop_stop(1)                        # GPU64_LOOPSTOP_OPCODE
            return ERR_OK

        if op == OP_SCENE_COMMIT:
            if not self.c1_loop_running:
                # Re-arm rather than refuse: a stopped loop is no longer a
                # permanent state. The counter still climbs, so it stays
                # the measure of how often this had to be repaired.
                self.commit_no_loop += 1
                self.c1_refuse(op)
                return self.c1_loop_start_armed()
            if not (self.status & ST_FRAME_READY):
                return ERR_BUSY
            if not self.calibrated:
                self.commit_no_clock += 1
                self.c1_refuse(op)
                return ERR_UNSUPPORTED
            if self.flip_pending:
                return ERR_BUSY
            # Publish: every shadow-redirected edit since LOOP_START (or the
            # last commit) becomes what the NEXT frame renders from.
            self.c1_scene = copy.deepcopy(self.c1_shadow_scene)
            self.c1_state = copy.deepcopy(self.c1_shadow_state)
            self._prepare_flip()
            self.flip_pending = True
            from gpu64model import ST_BUSY
            self.status |= ST_BUSY
            self.status &= ~ST_FRAME_READY
            self.c1_push_frame()
            return ERR_OK

        return ERR_BAD_OPCODE

    def c1_loop_start_armed(self):
        """Everything LOOP_START does once its mode argument is accepted.
        SCENE_COMMIT calls it too, to re-arm a loop that stopped without
        the C64 asking -- gpu64_3d_class1.cpp's loopStartArmed()."""
        from gpu64model import ERR_OK
        if not self.c1_scene.have_active_camera:
            return 0x0B                             # NO_CAMERA
        self.c1_shadow_scene = copy.deepcopy(self.c1_scene)
        self.c1_shadow_state = copy.deepcopy(self.c1_state)
        self.status &= ~ST_FRAME_READY
        self.c1_push_frame()
        self.c1_loop_running = True
        return ERR_OK

    def c1_push_frame(self):
        """What pushLoopFrame() queues and core 1 then renders.

        The model has no second core, so the render itself finishes
        immediately. FRAME_READY is still not raised until the flip this
        commit queued has retired, because the very next SCENE_COMMIT would
        answer BUSY on flip_pending anyway: raising it earlier would make the
        bit mean "rendered" here and "rendered and committable" on the Pi,
        and a demo that paces on it correctly would still show four commits
        in five refused on the desk. The frame clock -- not the C64's speed
        -- is what limits both.
        """
        self.c1_emit_frame(self.draw_page)
        self.c1_frame_page = self.draw_page
        if self.flip_pending:
            self.c1_ready_deferred = True
        else:
            self.status |= ST_FRAME_READY
            self.result = self.c1_frame_page
