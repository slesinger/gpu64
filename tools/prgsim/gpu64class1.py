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
    OP_GET_TRANSFORM,
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

    return [
        s16(m15(cr, cy) - m15_3(sr, sp, sy)),
        s16(-m15(sr, cp)),
        s16(m15(cr, sy) + m15_3(sr, sp, cy)),
        s16(m15(sr, cy) + m15_3(cr, sp, sy)),
        s16(m15(cr, cp)),
        s16(m15(sr, sy) - m15_3(cr, sp, cy)),
        s16(-m15(cp, sy)),
        s16(sp),
        s16(m15(cp, cy)),
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

    # --- execute ----------------------------------------------------------
    def execute_r1(self, op):
        from gpu64model import (ERR_OK, ERR_BAD_OPCODE, ERR_OUT_OF_RANGE,
                                ERR_BAD_ARGS, ERR_UNSUPPORTED, ERR_BUSY,
                                ERR_OUT_OF_MEMORY, ERR_BAD_ID)
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

        if op == OP_SCENE_RESET:
            self.c1_state = State()
            self.c1_scene = Scene()
            self.c1_loop_running = False
            self.status &= ~ST_FRAME_READY
            self.c1_shadow_state = State()
            self.c1_shadow_scene = Scene()
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
            if sc.have_active_camera and sc.active_camera_id == self.c1_id():
                sc.have_active_camera = False
            n.type = NODE_NONE
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
            if not self.c1_scene.have_active_camera:
                return NO_CAMERA
            self.c1_shadow_scene = copy.deepcopy(self.c1_scene)
            self.c1_shadow_state = copy.deepcopy(self.c1_state)
            self.status &= ~ST_FRAME_READY
            self.c1_push_frame()
            self.c1_loop_running = True
            return ERR_OK

        if op == OP_LOOP_STOP:
            # Always succeeds, even against a loop that was not running.
            self.c1_loop_running = False
            return ERR_OK

        if op == OP_SCENE_COMMIT:
            if not self.c1_loop_running:
                return ERR_UNSUPPORTED
            if not (self.status & ST_FRAME_READY):
                return ERR_BUSY
            if not self.calibrated:
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
