#!/usr/bin/env python3
"""
Converts a Quake BSP out of PAK0.PAK into a gpu64 class-1 level file.

Why this exists: step 4 of the v1 plan is to build a real game on the
retained scene and so prove the frozen API is enough to build one with. A
hand-authored room does not prove that; E1M1 does.

The two formats meet each other more than half way. Quake textures are 8bpp
indexed with power-of-two dimensions, which is exactly what UPLOAD_TEXTURE
takes; Quake's palette is 256 RGB triples, which is what SET_PALETTE takes;
and gpu64's per-corner UV bytes are texel coordinates that wrap, which is
what BSP texinfo produces. The work is therefore not format translation, it
is fitting the level into three hard limits:

  * 256 vertices per mesh          -- the 1-byte face index
  * +-128 world units per mesh     -- vertices are signed 8.8 model space
  * 255 texels of UV span per face -- UV bytes wrap at 256

so the level is cut into chunks, each chunk a mesh, each mesh placed by a
node transform. Chunking is spatial, which is also what makes per-object
culling worth anything later.

    python3 tools/gen_quakelevel.py --map e1m1 --out build/e1m1.g64lev
"""

import argparse, os, re, struct, sys
from collections import Counter, defaultdict

LUMPS = ['entities','planes','miptex','vertices','visilist','nodes','texinfo',
         'faces','lightmaps','clipnodes','leaves','marksurf','edges',
         'surfedges','models']

MAX_VERTS_PER_MESH = 256
MAX_UV_SPAN        = 255
LEVEL_VERSION      = 4      # 1 had no entities, 2 no collision,
                            # 3 no entity kind and no mover travel


def read_pak(path):
    f = open(path, 'rb')
    magic, off, size = struct.unpack('<4sii', f.read(12))
    if magic != b'PACK':
        sys.exit('%s is not a PAK file' % path)
    f.seek(off)
    ents = {}
    for _ in range(size // 64):
        name = f.read(56).split(b'\0')[0].decode()
        o, s = struct.unpack('<ii', f.read(8))
        ents[name] = (o, s)
    return f, ents


def pak_read(f, ents, name):
    if name not in ents:
        sys.exit('%s not in the PAK' % name)
    o, s = ents[name]
    f.seek(o)
    return f.read(s)


class Bsp:
    def __init__(self, data):
        self.d = data
        self.lump = {}
        for i, nm in enumerate(LUMPS):
            o, l = struct.unpack_from('<ii', data, 4 + i * 8)
            self.lump[nm] = (o, l)
        o, l = self.lump['vertices']
        self.verts = [struct.unpack_from('<3f', data, o + i * 12)
                      for i in range(l // 12)]
        o, l = self.lump['edges']
        self.edges = [struct.unpack_from('<2H', data, o + i * 4)
                      for i in range(l // 4)]
        o, l = self.lump['surfedges']
        self.surfedges = [struct.unpack_from('<i', data, o + i * 4)[0]
                          for i in range(l // 4)]
        o, l = self.lump['texinfo']
        self.texinfo = []
        for i in range(l // 40):
            v = struct.unpack_from('<8f', data, o + i * 40)
            mip, flags = struct.unpack_from('<2i', data, o + i * 40 + 32)
            self.texinfo.append((v[0:4], v[4:8], mip, flags))
        o, l = self.lump['faces']
        self.faces = []
        for i in range(l // 20):
            pl, side, fe, ne, ti = struct.unpack_from('<HhihH', data, o + i * 20)
            self.faces.append(dict(plane=pl, side=side, first=fe, n=ne, ti=ti))
        o, l = self.lump['models']
        self.models = []
        for i in range(l // 64):
            m = struct.unpack_from('<9f', data, o + i * 64)
            hn = struct.unpack_from('<4i', data, o + i * 64 + 36)
            ff, nf = struct.unpack_from('<2i', data, o + i * 64 + 56)
            self.models.append(dict(mins=m[0:3], maxs=m[3:6], origin=m[6:9],
                                    headnode=hn, firstface=ff, numfaces=nf))
        o, l = self.lump['planes']
        self.planes = []
        for i in range(l // 20):
            v = struct.unpack_from('<4f', data, o + i * 20)
            ty, = struct.unpack_from('<i', data, o + i * 20 + 16)
            self.planes.append((v[0:3], v[3], ty))
        o, l = self.lump['clipnodes']
        self.clipnodes = [struct.unpack_from('<i2h', data, o + i * 8)
                          for i in range(l // 8)]
        self.textures = self._miptex()

    def _miptex(self):
        o, _ = self.lump['miptex']
        n = struct.unpack_from('<i', self.d, o)[0]
        out = []
        for i in range(n):
            to = struct.unpack_from('<i', self.d, o + 4 + i * 4)[0]
            if to < 0:
                out.append(None)
                continue
            base = o + to
            name = self.d[base:base + 16].split(b'\0')[0].decode(errors='replace')
            w, h = struct.unpack_from('<2i', self.d, base + 16)
            off0 = struct.unpack_from('<i', self.d, base + 24)[0]
            px = self.d[base + off0: base + off0 + w * h] if off0 else b''
            out.append(dict(name=name, w=w, h=h, px=px))
        return out

    def entities(self):
        """The entity lump, as a list of dicts. Quake stores it as plain text:
        a run of { "key" "value" ... } blocks. Values stay strings -- the
        keys a game cares about differ per classname, and guessing types here
        would only lose information the level file then cannot carry."""
        o, l = self.lump['entities']
        txt = self.d[o:o + l].split(b'\0')[0].decode('latin-1')
        out, cur, key = [], None, None
        tok = re.findall(r'\{|\}|"([^"]*)"', txt)
        for i, m in enumerate(re.finditer(r'\{|\}|"([^"]*)"', txt)):
            t = m.group(0)
            if t == '{':
                cur, key = {}, None
            elif t == '}':
                if cur:
                    out.append(cur)
                cur = None
            elif cur is not None:
                if key is None:
                    key = m.group(1)
                else:
                    cur[key] = m.group(1)
                    key = None
        return out

    def face_poly(self, f):
        """The face's vertex ring, in winding order."""
        pts = []
        for k in range(f['n']):
            se = self.surfedges[f['first'] + k]
            e = self.edges[abs(se)]
            pts.append(self.verts[e[0] if se >= 0 else e[1]])
        return pts


# ---------------------------------------------------------------------------
# Coordinates.
#
# Quake is right-handed, x east, y north, z up. gpu64 class 1 is left-handed,
# x right, y UP, z away from the camera. So Quake (x,y,z) -> gpu64 (x, z, y),
# the same two-axis swap gen_quakemesh.py documents.
#
# Winding survives that swap unchanged, and this is worth stating because the
# obvious argument gets it backwards. Quake stores a face's vertex ring
# counter-clockwise seen from the front; gpu64 wants clockwise seen from
# outside (gpu64_3d_render.cpp's own header comment). Swapping two axes
# mirrors the world, which turns counter-clockwise into clockwise -- so the
# swap IS the conversion, and reversing the ring on top of it undoes the
# work. It was written the wrong way round first, and tools/hostsim/levelsim
# caught it exactly as that comment predicts: from inside a room the level
# was see-through, three of eight directions rendering nothing at all,
# instead of enclosed. --flip-winding is kept for that A/B.
# ---------------------------------------------------------------------------

QU_PER_WU = 32.0        # 128 world units of far plane == 4096 Quake units


def to_gpu64(p):
    return (p[0] / QU_PER_WU, p[2] / QU_PER_WU, p[1] / QU_PER_WU)


def tex_uv(p, ti):
    s = p[0] * ti[0][0] + p[1] * ti[0][1] + p[2] * ti[0][2] + ti[0][3]
    t = p[0] * ti[1][0] + p[1] * ti[1][1] + p[2] * ti[1][2] + ti[1][3]
    return s, t


class Chunk:
    def __init__(self):
        self.tris = []          # (pts[3], uvs[3], texid, flags)

    def verts(self):
        s = set()
        for pts, _, _, _ in self.tris:
            for p in pts:
                s.add(p)
        return s

    def bbox(self):
        vs = list(self.verts())
        return ([min(v[i] for v in vs) for i in range(3)],
                [max(v[i] for v in vs) for i in range(3)])


def split_chunks(tris, max_verts=MAX_VERTS_PER_MESH, max_extent=255.0):
    """Recursive median split on the longest axis until a chunk fits both the
    256-vertex index limit and the +-128 signed-8.8 model box."""
    out = []
    work = [tris]
    while work:
        group = work.pop()
        c = Chunk(); c.tris = group
        vs = list(c.verts())
        lo, hi = c.bbox()
        extent = max(hi[i] - lo[i] for i in range(3))
        if (len(vs) <= max_verts and extent <= max_extent) or len(group) <= 1:
            out.append(c)
            continue
        axis = max(range(3), key=lambda i: hi[i] - lo[i])
        mid = (lo[axis] + hi[axis]) / 2.0
        a, b = [], []
        for t in group:
            ctr = sum(p[axis] for p in t[0]) / 3.0
            (a if ctr < mid else b).append(t)
        if not a or not b:            # degenerate: cut by count instead
            group.sort(key=lambda t: sum(p[axis] for p in t[0]))
            a, b = group[:len(group) // 2], group[len(group) // 2:]
        work.append(a); work.append(b)
    return out


def q88(v):
    n = int(round(v * 256.0))
    return max(-32768, min(32767, n))


FLAG_DOUBLE_SIDED = 0x01
FLAG_FLAT_COLOUR  = 0x02
FLAG_UNLIT        = 0x04

MISSING_TEX_COLOUR = 0x08       # Quake palette: a mid grey


def build_mesh(chunk, texmap, stats):
    """A chunk becomes (origin, vertex blob, face blob). The origin is the
    bbox centre so the model box is used symmetrically.

    texmap turns a BSP miptex index into a gpu64 texture resource id. It has
    to exist because a face's texid is ONE byte -- gpu64_3d_render.cpp looks
    the texture up as pLookup(ctx, pF->texid) -- so resource ids must be
    dense and below 256, which raw BSP indices are not guaranteed to be. A
    texture the converter rejected has no id at all; those faces become flat
    grey rather than silently indexing whatever resource happens to sit at
    that number."""
    lo, hi = chunk.bbox()
    org = [(lo[i] + hi[i]) / 2.0 for i in range(3)]
    order, index = [], {}
    for pts, _, _, _ in chunk.tris:
        for p in pts:
            if p not in index:
                index[p] = len(order)
                order.append(p)
    vb = bytearray()
    for p in order:
        for i in range(3):
            vb += struct.pack('<h', q88(p[i] - org[i]))
    fb = bytearray()
    for pts, uvs, texid, flags in chunk.tris:
        idx = [index[p] for p in pts]
        fb += bytes(idx)
        for u, v in uvs:
            fb += bytes((u & 0xff, v & 0xff))
        rid = texmap.get(texid)
        if rid is None:
            rid, flags = MISSING_TEX_COLOUR, flags | FLAG_FLAT_COLOUR
            stats['faces_untextured'] += 1
        fb += bytes((rid, flags, 0))	# the 12th byte is Gpu64_3dFaceWire.pad
    return org, bytes(vb), bytes(fb)


def uv_span_ok(uvs):
    us = [u for u, _ in uvs]; vs = [v for _, v in uvs]
    return (max(us) - min(us)) <= MAX_UV_SPAN and (max(vs) - min(vs)) <= MAX_UV_SPAN


def subdivide(pts, uvs, depth=0):
    """Split until the triangle's texel span fits in a wrapping UV byte. Quake
    floors run hundreds of texels across one face; a byte cannot express that,
    and interpolating across the wrap would smear the texture. Splitting the
    longest UV edge is exact -- the new vertex lies on the old edge in both
    world and texel space, so nothing moves."""
    if uv_span_ok(uvs) or depth > 8:
        return [(pts, uvs)]
    e = max(range(3), key=lambda i: abs(uvs[i][0] - uvs[(i + 1) % 3][0]) +
                                    abs(uvs[i][1] - uvs[(i + 1) % 3][1]))
    a, b, c = e, (e + 1) % 3, (e + 2) % 3
    mp = tuple((pts[a][i] + pts[b][i]) / 2.0 for i in range(3))
    mu = ((uvs[a][0] + uvs[b][0]) / 2.0, (uvs[a][1] + uvs[b][1]) / 2.0)
    return (subdivide([pts[a], mp, pts[c]], [uvs[a], mu, uvs[c]], depth + 1) +
            subdivide([mp, pts[b], pts[c]], [mu, uvs[b], uvs[c]], depth + 1))


def collect(bsp, model, flip_winding, stats):
    tris = []
    for fi in range(model['firstface'], model['firstface'] + model['numfaces']):
        f = bsp.faces[fi]
        ti = bsp.texinfo[f['ti']]
        tex = bsp.textures[ti[2]]
        if tex is None or not tex['px']:
            stats['no_texture'] += 1
            continue
        nm = tex['name'].lower()
        flags = 0
        if nm == 'trigger':
            # Quake's own trigger texture. It is the word "trigger" written
            # across a 64x64 tile, and it is never drawn: the engine makes
            # every trigger volume invisible. Dropped here as well as by the
            # model rule in main(), because a brush wearing this texture is
            # by definition one nobody is meant to see, and a bench run has
            # already spent a session looking at it.
            stats['trigger_texture'] += 1
            continue
        if nm.startswith('sky'):
            stats['sky'] += 1
            continue                        # no sky layer in class 1 yet
        if nm.startswith('*'):
            stats['liquid'] += 1
            flags |= 0x01                   # double-sided: you swim through it
        if nm.startswith('{'):
            stats['masked'] += 1
        poly = bsp.face_poly(f)
        if len(poly) < 3:
            stats['degenerate'] += 1
            continue
        raw = [tex_uv(p, ti) for p in poly]
        # Bias into the texture's own tile so the bytes start small. A whole
        # number of tiles cannot change how the face lines up with neighbours.
        bu = int(min(u for u, _ in raw) // tex['w']) * tex['w']
        bv = int(min(v for _, v in raw) // tex['h']) * tex['h']
        uvs = [(u - bu, v - bv) for u, v in raw]
        pts = [to_gpu64(p) for p in poly]
        for k in range(1, len(pts) - 1):     # fan
            tp = [pts[0], pts[k], pts[k + 1]]
            tu = [uvs[0], uvs[k], uvs[k + 1]]
            if flip_winding:
                tp = tp[::-1]; tu = tu[::-1]
            parts = subdivide(tp, tu)
            if len(parts) > 1:
                stats['subdivided'] += 1
                stats['subdiv_tris'] += len(parts) - 1
            for sp, su in parts:
                tris.append((tuple(tuple(x) for x in sp),
                             tuple((int(round(u)) & 0xff, int(round(v)) & 0xff)
                                   for u, v in su),
                             ti[2], flags))
    return tris


def shift_of(n):
    s = n.bit_length() - 1
    return s if (1 << s) == n else None


# ---------------------------------------------------------------------------
# Entities.
#
# The entity lump is where the *game* lives -- spawns, monsters, items, and
# the targetname/target wiring that makes a button open a door. It is carried
# through into the level file rather than left behind in the BSP, because the
# C64 side has no BSP parser and 369 text blocks will not fit in its RAM
# anyway. Strings are deduped into one blob and referenced by offset; offset 0
# is a lone NUL so 0 reads as "no string".
#
# Two numeric slots (p0, p1) are deliberately generic. Which key lands in them
# depends on the classname -- speed/wait for a door, light/style for a light,
# health for a monster -- and hard-coding a union per class here would freeze a
# game design that does not exist yet. ENT_PARAMS says what each class puts
# there, and the game reads it the same way.
# ---------------------------------------------------------------------------

# A classname is a string, and a 6502 game that had to strcmp its way through
# 420 of them every level would spend the level loading. So each record also
# carries a `kind` byte, assigned here, and the game switches on that. The
# ranges are grouped so a game can ask a coarse question ("is this a monster?")
# with a comparison instead of a table: 1-9 world and wiring, 10-19 movers,
# 20-29 triggers, 30-49 pickups, 50-79 monsters, 80-89 scenery, 90-99 lights
# and sound. 0 is "a classname this converter has no opinion about", which a
# game is free to treat as decoration -- it is never an error.
#
# The generic value at the bottom of each range is what an unlisted member of
# a family gets (a monster this table has never heard of is still a monster),
# which is what keeps a map from another episode from arriving as 420 zeroes.

ENT_KIND_GENERIC = [          # (prefix, kind) -- first match wins
    ('monster_', 50),
    ('item_artifact', 40),
    ('item_', 30),
    ('weapon_', 35),
    ('trigger_', 20),
    ('func_', 10),
    ('light', 90),
    ('ambient_', 95),
    ('info_', 5),
    ('path_', 6),
]

ENT_KIND = {
    'worldspawn': 1,
    'info_player_start': 2,
    'info_player_deathmatch': 3,
    'info_player_coop': 4,
    'info_teleport_destination': 7,
    'info_intermission': 8,
    'path_corner': 6,

    'func_door': 11,
    'func_door_secret': 12,
    'func_plat': 13,
    'func_button': 14,
    'func_wall': 15,
    'func_train': 16,
    'func_illusionary': 17,

    'trigger_once': 21,
    'trigger_multiple': 22,
    'trigger_secret': 23,
    'trigger_teleport': 24,
    'trigger_changelevel': 25,
    'trigger_counter': 26,
    'trigger_hurt': 27,
    'trigger_push': 28,

    'item_health': 31,
    'item_armor1': 32,
    'item_armor2': 33,
    'item_armorInv': 34,
    'weapon_supershotgun': 35,
    'weapon_nailgun': 36,
    'weapon_supernailgun': 37,
    'weapon_grenadelauncher': 38,
    'weapon_rocketlauncher': 39,
    'weapon_lightning': 44,
    'item_shells': 45,
    'item_spikes': 46,
    'item_rockets': 47,
    'item_cells': 48,
    'item_weapon': 49,
    'item_artifact_super_damage': 40,
    'item_artifact_invulnerability': 41,
    'item_artifact_envirosuit': 42,
    'item_artifact_invisibility': 43,

    'monster_army': 51,
    'monster_dog': 52,
    'monster_ogre': 53,
    'monster_knight': 54,
    'monster_zombie': 55,
    'monster_wizard': 56,
    'monster_demon1': 57,
    'monster_shambler': 58,
    'monster_enforcer': 59,
    'monster_hell_knight': 60,
    'monster_shalrath': 61,
    'monster_tarbaby': 62,
    'monster_fish': 63,
    'monster_boss': 64,
    'monster_oldone': 65,

    'misc_explobox': 80,
    'misc_explobox2': 81,
    'misc_teleporttrain': 82,
    'misc_fireball': 83,

    'light': 90,
    'light_fluoro': 91,
    'light_fluorospark': 92,
    'light_globe': 93,
    'light_torch_small_walltorch': 94,
    'light_flame_large_yellow': 94,
    'light_flame_small_yellow': 94,

    'ambient_drone': 95,
    'ambient_comp_hum': 96,
}


def kind_of(cls):
    """The kind byte for a classname: the table, then the family prefixes."""
    if cls in ENT_KIND:
        return ENT_KIND[cls]
    for pre, k in ENT_KIND_GENERIC:
        if cls.startswith(pre):
            return k
    return 0


# Quake's door/plat/button travel, computed here rather than on the C64 or in
# the firmware. It is three lines of vector arithmetic over keys the game
# cannot see (angle -1/-2, lip) and a bounding box it would have to fetch, and
# getting it wrong makes a door slide into a wall -- exactly the class of
# convention that belongs next to the winding and the yaw, in the one place
# that a PC can check.
#
# The field is the displacement from where the brush sits in the file to its
# OTHER position, in gpu64 world units:
#
#   func_door, func_button   the open / pressed position
#   func_door_secret         the first of its two moves, sideways or down
#   func_plat                the LOWERED position, which is where Quake starts
#                            a plat -- the file geometry is the top of its run
#
# Everything else gets zero. Speed is not here: it is param0 for the classes
# that set it, and the game picks its own rate anyway.

SECRET_1ST_LEFT = 2
SECRET_1ST_DOWN = 4


def movedir_of(e):
    """Quake's movedir in gpu64 axes: 'angle' -1 is up, -2 is down, anything
    else is a yaw in the horizontal plane. The (x,z,y) swap sends Quake's
    (cos t, sin t, 0) to (cos t, 0, sin t)."""
    import math
    a = num(e.get('angle'), 0)
    if a == -1:
        return (0.0, 1.0, 0.0)
    if a == -2:
        return (0.0, -1.0, 0.0)
    r = math.radians(a)
    return (math.cos(r), 0.0, math.sin(r))


def open_ofs(e, mdl, bsp, stats):
    """The closed -> open displacement for a brush mover, in world units."""
    cls = e.get('classname', '')
    if not mdl or mdl >= len(bsp.models):
        return (0.0, 0.0, 0.0)
    m = bsp.models[mdl]
    lo = to_gpu64(m['mins'])
    hi = to_gpu64(m['maxs'])
    size = [abs(hi[k] - lo[k]) for k in range(3)]
    lip_qu = num(e.get('lip'), 8 if cls != 'func_button' else 4)
    lip = lip_qu / QU_PER_WU
    sf = num(e.get('spawnflags'), 0)

    if cls in ('func_door', 'func_button'):
        d = movedir_of(e)
        travel = abs(sum(d[k] * size[k] for k in range(3))) - lip
        if travel < 0.0:
            stats['mover_travel_negative'] += 1
            travel = 0.0
        return tuple(d[k] * travel for k in range(3))

    if cls == 'func_plat':
        h = num(e.get('height'), 0)
        drop = h / QU_PER_WU if h else max(0.0, size[1] - lip)
        return (0.0, -drop, 0.0)

    if cls == 'func_door_secret':
        # Two moves, and it is the second one that gets the panel out of the
        # doorway: Quake slides it sideways by its own thickness and then
        # backwards along its facing by its width. E1M1's panels are 14 units
        # thick, so a game handed only the first move would slide a secret
        # door into itself and leave the opening shut. What goes in the file
        # is therefore dest2 -- both moves added -- which is one displacement
        # the game can animate in one go.
        #
        # makevectors(angle) with pitch and roll zero gives Quake
        # v_forward = (cos t, sin t, 0) and v_right = (sin t, -cos t, 0);
        # the (x,z,y) swap sends those to (cos t, 0, sin t) and
        # (sin t, 0, -cos t).
        import math
        r = math.radians(num(e.get('angle'), 0))
        fwd   = (math.cos(r), 0.0, math.sin(r))
        right = (math.sin(r), 0.0, -math.cos(r))
        sgn = -1.0 if (sf & SECRET_1ST_LEFT) else 1.0
        if sf & SECRET_1ST_DOWN:
            first = (0.0, -size[1], 0.0)
        else:
            w = abs(sum(right[k] * size[k] for k in range(3)))
            first = tuple(right[k] * w * sgn for k in range(3))
        length = abs(sum(fwd[k] * size[k] for k in range(3)))
        return tuple(first[k] + fwd[k] * length for k in range(3))

    return (0.0, 0.0, 0.0)


ENT_PARAMS = {
    'func_door':        ('speed', 'wait'),
    'func_door_secret': ('speed', 'wait'),
    'func_plat':        ('speed', 'height'),
    'func_button':      ('speed', 'wait'),
    'light':            ('light', 'style'),
    'light_fluoro':     ('light', 'style'),
    'light_fluorospark':('light', 'style'),
    'light_globe':      ('light', 'style'),
    'light_torch_small_walltorch': ('light', 'style'),
    'trigger_multiple': ('wait', 'sounds'),
    'trigger_changelevel': ('sounds', 'sounds'),
}
ENT_DEFAULT_PARAMS = ('health', 'count')

ENT_STRIDE = 44


def num(v, dflt=0):
    """A Quake entity value as an integer. Values are free text and some are
    floats ('100.5'); truncating is what the game wants and what the 6502 can
    hold."""
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return dflt


def yaw_of(e):
    """gpu64 yaw (a full turn is 65536) from Quake's 'angle' in degrees.

    Derived rather than guessed, because an angle convention that is wrong by
    a quarter turn still renders a perfectly good picture of the wrong wall.
    Quake's forward for angle t is (cos t, sin t, 0), which the (x,z,y) swap
    takes to (cos t, 0, sin t). gpu64's forward is column 2 of
    gpu64_3dMatFromEuler() with pitch and roll zero, i.e. (sin y, 0, cos y).
    Matching the two gives y = 90 - t. Wrapping is free: the field is u16."""
    a = e.get('angle')
    if a is None and 'mangle' in e:
        a = e['mangle'].split()[1] if len(e['mangle'].split()) > 1 else '0'
    v = num(a, 0)
    if v in (-1, -2):           # "up" / "down": no meaningful yaw
        v = 0
    return int(round((90.0 - v) * 65536.0 / 360.0)) & 0xffff


def build_entities(bsp, stats):
    """Returns (records, string blob). Records are pre-packed tuples in the
    ENT_STRIDE layout; see the module docstring's file-format notes."""
    strs = bytearray(b'\0')
    pool = {'': 0}

    def sput(t):
        if t in pool:
            return pool[t]
        o = len(strs)
        if o > 0xffff:
            stats['string_blob_full'] += 1
            return 0
        strs.extend(t.encode('latin-1', 'replace') + b'\0')
        pool[t] = o
        return o

    recs = []
    for e in bsp.entities():
        cls = e.get('classname', '')
        if not cls:
            stats['ent_no_classname'] += 1
            continue
        mdl = 0
        mv = e.get('model', '')
        if mv.startswith('*'):
            mdl = num(mv[1:], 0)
        elif mv:
            stats['ent_external_model'] += 1      # a .mdl file; not a brush
        org = e.get('origin')
        if org:
            q = [num(x) for x in org.split()[:3]] + [0, 0, 0]
            x, y, z = to_gpu64(q[:3])
        elif mdl and mdl < len(bsp.models):
            # A brush entity usually has no origin; its geometry already sits
            # in world space and its node origin comes from the same chunk
            # split the world used. Record the model's centre so the game has
            # a point to measure distance to.
            m = bsp.models[mdl]
            x, y, z = to_gpu64([(m['mins'][k] + m['maxs'][k]) / 2.0 for k in range(3)])
        else:
            x = y = z = 0.0
        pk = ENT_PARAMS.get(cls, ENT_DEFAULT_PARAMS)
        p0, p1 = (num(e.get(pk[0]), 0), num(e.get(pk[1]), 0))
        ofs = open_ofs(e, mdl, bsp, stats)
        if any(ofs):
            stats['ent_mover'] += 1
        recs.append(struct.pack('<6H2h3i3iBBH',
                                sput(cls), yaw_of(e), num(e.get('spawnflags')) & 0xffff,
                                mdl, sput(e.get('target', '')),
                                sput(e.get('targetname', '')),
                                max(-32768, min(32767, p0)),
                                max(-32768, min(32767, p1)),
                                int(round(x * 65536)), int(round(y * 65536)),
                                int(round(z * 65536)),
                                int(round(ofs[0] * 65536)),
                                int(round(ofs[1] * 65536)),
                                int(round(ofs[2] * 65536)),
                                kind_of(cls), 0, 0))
        stats['ent_' + ('brush' if mdl else 'point')] += 1
    return recs, bytes(strs)


# ---------------------------------------------------------------------------
# Collision.
#
# The one piece of game state that cannot live on the C64: E1M1's clipnodes
# are 43 KB and its planes another 36 KB. The Pi already holds the level, so
# the hulls travel with it and the Pi answers "can I walk here?" -- the same
# division DRAW_WALLS established, where the 6502 sends ten bytes of camera a
# frame and gpu64 does the projection.
#
# Quake's hulls are BSP trees of clipnodes over the shared plane array, one
# tree per player size, already expanded by the size of the thing that walks
# them. Nothing here re-derives them; they are translated into gpu64 axes and
# fixed point and handed over.
# ---------------------------------------------------------------------------

PLANE_STRIDE = 12
HULL_STRIDE  = 32
CLIP_STRIDE  = 8

# Quake axis -> gpu64 axis, the same (x, z, y) swap to_gpu64() applies. A
# plane's 'type' field names the axis an axial plane is perpendicular to, and
# it has to travel through the same permutation or the trace's axial fast
# path tests the wrong component.
AXIS_MAP = (0, 2, 1)


def build_collision(bsp, stats):
    """Returns (plane records, hull records, clipnode records)."""
    planes = []
    for n, dist, ty in bsp.planes:
        # The normal is a unit vector, so 1.15 holds it exactly enough; the
        # swap is a permutation, which leaves it unit. dist is a length along
        # that normal, so it converts like any other world length.
        gn = (n[0], n[2], n[1])
        planes.append(struct.pack('<3hiBB',
                                  max(-32768, min(32767, int(round(gn[0] * 32767)))),
                                  max(-32768, min(32767, int(round(gn[1] * 32767)))),
                                  max(-32768, min(32767, int(round(gn[2] * 32767)))),
                                  int(round(dist / QU_PER_WU * 65536)),
                                  AXIS_MAP[ty] if 0 <= ty < 3 else 3, 0))

    hulls = []
    for m in bsp.models:
        lo = to_gpu64(m['mins'])
        hi = to_gpu64(m['maxs'])
        # The swap can exchange which of two components is the smaller, so
        # the box is rebuilt rather than carried across: mins that are not
        # minimal make every trace against that model miss.
        b = [(min(lo[k], hi[k]), max(lo[k], hi[k])) for k in range(3)]
        hulls.append(struct.pack('<2i6i', m['headnode'][1], m['headnode'][2],
                                 *[int(round(v * 65536))
                                   for v in [b[0][0], b[1][0], b[2][0],
                                             b[0][1], b[1][1], b[2][1]]]))

    clips = []
    for pn, c0, c1 in bsp.clipnodes:
        if pn < 0 or pn >= len(planes):
            stats['clipnode_bad_plane'] += 1
            pn = 0
        # Children are node indices when >= 0 and Quake CONTENTS_* when < 0;
        # both fit s16 and both are passed through unchanged, because the
        # trace's whole job is to tell those two apart.
        clips.append(struct.pack('<Hhh H', pn, c0, c1, 0))
    return planes, hulls, clips


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pak', default='quake/ID1/PAK0.PAK')
    ap.add_argument('--map', default='e1m1')
    ap.add_argument('--out', default=None)
    ap.add_argument('--flip-winding', action='store_true',
                    help='reverse every triangle; an A/B against the '
                         'see-through failure, not something a level needs')
    ap.add_argument('--worldspawn-only', action='store_true',
                    help='skip the 57 brush models (doors, platforms, buttons)')
    a = ap.parse_args()

    f, ents = read_pak(a.pak)
    bsp = Bsp(pak_read(f, ents, 'maps/%s.bsp' % a.map))
    pal = pak_read(f, ents, 'gfx/palette.lmp')

    stats = Counter()
    models = bsp.models if not a.worldspawn_only else bsp.models[:1]

    # Which brush models Quake actually DRAWS.
    #
    # Half of E1M1's 58 brush models are trigger volumes -- the boxes that
    # notice you walking into them. Quake never renders one: every trigger_*
    # entity runs InitTrigger(), which sets modelindex to 0 and clears the
    # model name, so the brush stays solid to the trace and invisible to the
    # renderer. gpu64's converter drew them like any other brush model, and
    # the bench saw exactly that -- the word "trigger" plastered in giant
    # letters across half the screen, because the player was standing inside
    # one. 26 models and 348 triangles, 2.2% of the level and most of the
    # picture, since they are big boxes right against the camera.
    #
    # Their geometry is dropped; their collision hull and their entity record
    # are not. A trigger the game cannot see is still a trigger the game has
    # to be able to trace against.
    invisible = set()
    if not a.worldspawn_only:
        claimed = {}
        for e in bsp.entities():
            mv = e.get('model', '')
            if mv.startswith('*'):
                claimed.setdefault(num(mv[1:], 0), []).append(
                    e.get('classname', ''))
        for mi, cls in claimed.items():
            if cls and all(c.startswith('trigger_') for c in cls):
                invisible.add(mi)
        stats['models_invisible'] = len(invisible)
    # Geometry first, but only as triangles: which textures the level needs
    # is not known until every face has been walked, and the face records
    # cannot be written until the textures have been given their dense ids.
    chunks = []
    used_tex = set()
    for mi, m in enumerate(models):
        if mi in invisible:
            continue
        tris = collect(bsp, m, a.flip_winding, stats)
        if not tris:
            continue
        for c in split_chunks(tris):
            chunks.append((c, mi))
            for _, _, t, _ in c.tris:
                used_tex.add(t)
        stats['models_with_geometry'] += 1

    # Textures, and the BSP-index -> resource-id map the face records need.
    texrecs, texblobs, texmap = [], [], {}
    for t in sorted(used_tex):
        tx = bsp.textures[t]
        if tx is None or not tx['px']:
            stats['texture_rejected'] += 1
            continue
        ws, hs = shift_of(tx['w']), shift_of(tx['h'])
        if ws is None or hs is None or not (3 <= ws <= 8 and 3 <= hs <= 8):
            stats['texture_rejected'] += 1
            continue
        if len(texrecs) >= 256:
            # A face's texid is one byte. Rejecting the overflow keeps the
            # file honest instead of wrapping two textures onto one id.
            stats['texture_over_256'] += 1
            continue
        texmap[t] = len(texrecs)
        texrecs.append((t, ws, hs, len(b''.join(texblobs)), tx['w'] * tx['h']))
        texblobs.append(tx['px'][:tx['w'] * tx['h']])

    meshes, nodes = [], []
    for c, mi in chunks:
        org, vb, fb = build_mesh(c, texmap, stats)
        if len(vb) // 6 > MAX_VERTS_PER_MESH:
            stats['oversize_mesh'] += 1
        meshes.append((vb, fb))
        nodes.append((len(meshes) - 1, org, mi))

    entrecs, entstrs = build_entities(bsp, stats)
    planes, hulls, clips = build_collision(bsp, stats)

    print('%s: %d faces, %d verts, %d textures, %d models'
          % (a.map, len(bsp.faces), len(bsp.verts), len(bsp.textures),
             len(bsp.models)))
    print('  meshes %d   nodes %d   textures used %d'
          % (len(meshes), len(nodes), len(texrecs)))
    vbytes = sum(len(v) for v, _ in meshes)
    fbytes = sum(len(x) for _, x in meshes)
    tbytes = sum(len(b) for b in texblobs)
    print('  geometry %d B (%d vert + %d face)   textures %d B   total %.1f KB'
          % (vbytes + fbytes, vbytes, fbytes, tbytes,
             (vbytes + fbytes + tbytes + 768) / 1024.0))
    vc = [len(v) // 6 for v, _ in meshes]
    print('  verts/mesh  max %d  mean %.1f       tris %d'
          % (max(vc), sum(vc) / len(vc), fbytes // 12))
    print('  entities %d records, %d B of strings'
          % (len(entrecs), len(entstrs)))
    print('  collision %d planes + %d hulls + %d clipnodes = %.1f KB'
          % (len(planes), len(hulls), len(clips),
             (len(planes) * PLANE_STRIDE + len(hulls) * HULL_STRIDE
              + len(clips) * CLIP_STRIDE) / 1024.0))
    for e in bsp.entities():
        if e.get('classname') == 'info_player_start':
            q = [num(x) for x in e.get('origin', '0 0 0').split()[:3]]
            wp = to_gpu64(q)
            print('  player start  quake %s  ->  world %.2f %.2f %.2f  yaw %04x'
                  % (' '.join('%d' % v for v in q), wp[0], wp[1], wp[2],
                     yaw_of(e)))
    for k in sorted(stats):
        print('  %-22s %d' % (k, stats[k]))

    if a.out:
        os.makedirs(os.path.dirname(a.out) or '.', exist_ok=True)
        blob = bytearray()
        def put(b):
            o = len(blob); blob.extend(b); return o
        palo = put(pal[:768])
        stro = put(entstrs)
        trec = [(t, ws, hs, put(texblobs[i]), ln)
                for i, (t, ws, hs, _, ln) in enumerate(texrecs)]
        mrec = [(put(v), len(v), put(fb), len(fb)) for v, fb in meshes]
        tbl = b''.join(struct.pack('<HBBII', *r) for r in trec)
        mtb = b''.join(struct.pack('<IIII', *r) for r in mrec)
        ntb = b''.join(struct.pack('<HiiiH', n[0],
                                   int(round(n[1][0] * 65536)),
                                   int(round(n[1][1] * 65536)),
                                   int(round(n[1][2] * 65536)), n[2])
                       for n in nodes)
        etb = b''.join(entrecs)
        ptb = b''.join(planes)
        htb = b''.join(hulls)
        ctb = b''.join(clips)

        # 36-byte header, then seven tables, then the blob area everything
        # else offsets into. `base` is absolute; every other offset in the
        # file is relative to it, so the loader adds it exactly once.
        hdr = struct.pack('<4sHH' '4H' 'HHI' '3I',
                          b'G64L', LEVEL_VERSION, int(QU_PER_WU),
                          len(trec), len(mrec), len(nodes), len(entrecs),
                          len(planes), len(hulls), len(clips),
                          0, palo, stro)
        base = (len(hdr) + len(tbl) + len(mtb) + len(ntb) + len(etb)
                + len(ptb) + len(htb) + len(ctb))
        hdr = struct.pack('<4sHH' '4H' 'HHI' '3I',
                          b'G64L', LEVEL_VERSION, int(QU_PER_WU),
                          len(trec), len(mrec), len(nodes), len(entrecs),
                          len(planes), len(hulls), len(clips),
                          base, palo, stro)
        out = bytearray(hdr) + tbl + mtb + ntb + etb + ptb + htb + ctb + blob
        open(a.out, 'wb').write(out)
        print('  wrote %s (%.1f KB)' % (a.out, len(out) / 1024.0))


if __name__ == '__main__':
    main()
