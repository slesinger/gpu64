#!/usr/bin/env python3
"""
Builds everything gpu64_demo_stunt.a uploads and drives by: the track, the
scenery, the two cars, the palette, the textures, and the tables the 6502's
physics reads the track through.

THE TRACK

A figure of eight, 256 points round, SEG world units apart. Two
three-quarter circles joined by two diagonals that cross at the origin --
one of them on a bridge over the other, which is the most Stunt Car Racer
thing a track can do. Point i is the centre line of the road at distance
i*SEG along it, so the 6502 finds its place on the track with one byte for
the segment and one for the fraction, and the lap wraps for free.

Every point has a road height. Along the way: a climb onto the bridge, a
roller coaster of humps round the north loop, and on the south loop the big
ramp -- up, a gap with no road in it, and a landing ramp on the far side.
Fall off, or fall short, and the crane puts you back.

WHAT THE 6502 GETS

  cxLo/cxHi, czLo/czHi, cyLo/cyHi   the centre line, signed 8.8 world units
  hdTab                             segment heading, binary degrees
  ptTab                             segment pitch, binary degrees, for the
                                    car's nose (positive = nose down, which
                                    is SET_ORIENTATION's sign)
  flTab                             bit 0: this segment has no road

WHAT GPU64 GETS

  16 track chunk meshes, sixteen segments each, placed by their own nodes
  (a mesh has 256 vertices and +-128 units at most). The chunks share one
  face blob wherever their topology is the same -- which is all of them but
  the start line and the two halves of the gap -- so the face bytes are
  paid for three times, not sixteen.
  The ground (a grid the C64 keeps under the camera), a ring of hills (kept
  round it), two cars (one vertex blob, two face blobs: red and blue), and
  tree billboards.

Winding: class 1 draws a face whose (b-a)x(c-a) points at the viewer, in
its own left-handed axes -- the same test gen_quakemesh.py's room passes.
Every triangle below is built from an OUTWARD hint and flipped to match, so
nothing here depends on getting a vertex order right by hand.

Writes Source/Demos/gpu64_demo_stunt.inc. Regenerate with:
    python3 tools/gen_stunt.py
"""

import math, os, random

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "Source", "Demos", "gpu64_demo_stunt.inc")

N      = 256        # track points; the segment byte wraps at 256
SEG    = 1.5        # world units per segment
HALFW  = 1.5        # road half-width
THICK  = 0.3        # road slab thickness
CHUNK  = 16         # segments per chunk mesh
NCHUNK = N // CHUNK

TEX_ROAD, TEX_START, TEX_SIDE, TEX_GRASS, TEX_TREE = 1, 2, 3, 4, 5

# palette: 8 hues x 32 levels, level 31 the hue itself, level 0 black
HUES = [
    (86, 160, 58),      # 0 grass
    (150, 150, 162),    # 1 asphalt / concrete
    (118, 168, 240),    # 2 sky
    (232, 44, 32),      # 3 red: the player
    (40, 84, 236),      # 4 blue: the opponent
    (244, 206, 40),     # 5 yellow
    (148, 108, 70),     # 6 earth
    (244, 244, 244),    # 7 white
]
def col(h, l): return h * 32 + l

GRASS, GREY, SKY, RED, BLUE, YELLOW, EARTH, WHITE = range(8)

# ------------------------------------------------------------------ path
R = N * SEG / (4 + 3 * math.pi)     # loop radius: two diagonals of 2R, two 270-degree arcs
C = R * math.sqrt(2)                # loop centres at (0, +-C)
LD = 2 * R                          # diagonal length
LA = 1.5 * math.pi * R              # arc length
L = N * SEG

def path(s):
    """(x, z, heading radians) at arc length s; heading 0 = +z, +pi/2 = +x."""
    s %= L
    h = C / 2
    if s < LD:                                      # diagonal A, NE through the origin
        t = s / LD
        return (-h + t * C, -h + t * C, math.pi / 4)
    s -= LD
    if s < LA:                                      # north loop, anticlockwise
        th = -math.pi / 4 + s / R
        x, z = R * math.cos(th), C + R * math.sin(th)
        return (x, z, math.atan2(-math.sin(th), math.cos(th)))
    s -= LA
    if s < LD:                                      # diagonal B, SE through the origin
        t = s / LD
        return (-h + t * C, h - t * C, 3 * math.pi / 4)
    s -= LD
    th = math.pi / 4 - s / R                        # south loop, clockwise
    x, z = R * math.cos(th), -C + R * math.sin(th)
    return (x, z, math.atan2(math.sin(th), -math.cos(th)))

I_A0, I_L1 = 0, LD / SEG
I_B0 = (LD + LA) / SEG
I_L2 = (2 * LD + LA) / SEG
CROSS_A = int(round(LD / 2 / SEG))
CROSS_B = int(round((LD + LA + LD / 2) / SEG))

# ---------------------------------------------------------------- heights
def smooth(a, b, t):
    t = max(0.0, min(1.0, t))
    return a + (b - a) * (1 - math.cos(t * math.pi)) / 2

KEYS = [            # (point, height, linear?) -- cosine eased unless linear
    (0, 3.0), (6, 3.0), (16, 7.5), (24, 7.5), (36, 4.0),
    (120, 4.0), (128, 3.0), (CROSS_B - 6, 2.5), (CROSS_B + 6, 2.5),
    (168, 3.0), (172, 3.0), (177, 3.8),
    (188, 8.6, True),           # the ramp: straight, so the lip still climbs
    (191, 6.6), (193, 6.4),     # the far side of the gap, two units lower
    (214, 3.0), (256, 3.0),
]
GAP = range(188, 191)           # segments with no road: 4.5 units of air
HUMPS = (46, 118, 1.3, 12)      # first, last, amplitude, period in points

def height(i):
    for ka, kb in zip(KEYS, KEYS[1:]):
        (a, ha), (b, hb) = ka[:2], kb[:2]
        if a <= i <= b:
            t = (i - a) / (b - a)
            y = ha + (hb - ha) * t if len(kb) > 2 else smooth(ha, hb, t)
            break
    h0, h1, amp, per = HUMPS
    if h0 <= i <= h1:
        y += amp * math.sin((i - h0) * 2 * math.pi / per)
    return y

pts = []
for i in range(N):
    x, z, hd = path(i * SEG)
    pts.append((x, z, hd, height(i)))

def q88(v):
    r = int(round(v * 256))
    assert -32768 <= r <= 32767, v
    return r

cx = [q88(p[0]) for p in pts]
cz = [q88(p[1]) for p in pts]
cy = [q88(p[3]) for p in pts]

def ang8(rad):
    return int(round(rad * 128 / math.pi)) & 0xff

# heading as a 16-bit binary angle at each POINT (the path's own tangent,
# the direction the road's cross-section is built square to), and how much
# it turns by the next point -- the 6502 interpolates one by the other
hd16 = [int(round(p[2] * 32768 / math.pi)) & 0xffff for p in pts]
dh16 = [((hd16[(i + 1) % N] - hd16[i] + 32768) & 0xffff) - 32768 for i in range(N)]

hdTab, ptTab, flTab = [], [], []
for i in range(N):
    j = (i + 1) % N
    dx, dz = (cx[j] - cx[i]) / 256, (cz[j] - cz[i]) / 256
    hdTab.append(ang8(math.atan2(dx, dz)))
    dy = (cy[j] - cy[i]) / 256
    ptTab.append(ang8(math.atan2(-dy, math.hypot(dx, dz))))
    flTab.append(1 if i in GAP else 0)

# the bridge has to clear the road under it
clear = cy[CROSS_A] - cy[CROSS_B]
assert clear > 4 * 256, clear

# --------------------------------------------------------------- textures
rnd = random.Random(64)

def tex(fn, w=32, h=32):
    return bytes(fn(u, v) for v in range(h) for u in range(w))   # class 1: v*w + u

def road(u, v):
    if u < 2 or u > 29:                                     # kerbs
        return col(RED, 28) if (v // 8) % 2 else col(WHITE, 30)
    if u in (15, 16) and (v // 8) % 2 == 0:
        return col(YELLOW, 28)                              # centre dashes
    return col(GREY, 17 + rnd.randrange(4))

def start(u, v):
    if u < 2 or u > 29:
        return col(RED, 28) if (v // 8) % 2 else col(WHITE, 30)
    return col(WHITE, 30) if ((u // 4) + (v // 4)) % 2 else col(GREY, 4)

def side(u, v):                                             # hazard chevrons
    return col(YELLOW, 29) if ((u + v) // 4) % 2 else col(GREY, 5)

def grass(u, v):
    n = rnd.randrange(5)
    if (u * 7 + v * 13) % 23 == 0:
        return col(GRASS, 30)
    return col(GRASS, 18 + n)

def tree(u, v):                                             # 0 = transparent
    cxp = 16
    if v >= 24:                                             # trunk
        return col(EARTH, 16) if abs(u - cxp + 0.5) < 2.5 else 0
    # three stacked cones
    for top, bot in ((0, 11), (6, 18), (12, 25)):
        if top <= v < bot:
            half = (v - top + 1) * 13 / (bot - top)
            if abs(u - cxp + 0.5) < half:
                return col(GRASS, 10 + (rnd.randrange(4) if u < cxp else 4 + rnd.randrange(3)))
    return 0

textures = [
    ("texRoad", TEX_ROAD, tex(road)),
    ("texStart", TEX_START, tex(start)),
    ("texSide", TEX_SIDE, tex(side)),
    ("texGrass", TEX_GRASS, tex(grass)),
    ("texTree", TEX_TREE, tex(tree)),
]

# ----------------------------------------------------------------- meshes
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

class Mesh:
    def __init__(self):
        self.v = []                 # float (x, y, z), model space
        self.f = []                 # (i0,i1,i2, uv0,uv1,uv2, tex, flags)
    def vert(self, p):
        self.v.append(p)
        return len(self.v) - 1
    def tri(self, a, b, c, uva, uvb, uvc, tex, flags, out, ref=None):
        pa, pb, pc = ref if ref else (self.v[a], self.v[b], self.v[c])
        n = cross(sub(pb, pa), sub(pc, pa))
        if dot(n, out) < 0:
            b, c, uvb, uvc = c, b, uvc, uvb
        self.f.append((a, b, c, uva, uvb, uvc, tex, flags))
    def quad(self, a, b, c, d, uv, tex, flags, out, ref=None):
        """a b c d round the edge; uv = four corners, or None. ref: the four
        points to take the winding from, when the real ones are degenerate."""
        uv = uv or [(0, 0)] * 4
        r1 = (ref[0], ref[1], ref[2]) if ref else None
        r2 = (ref[0], ref[2], ref[3]) if ref else None
        self.tri(a, b, c, uv[0], uv[1], uv[2], tex, flags, out, r1)
        self.tri(a, c, d, uv[0], uv[2], uv[3], tex, flags, out, r2)
    def vblob(self):
        out = []
        for p in self.v:
            for c in p:
                r = q88(c)
                assert -128 * 256 < r < 128 * 256, p
                out += [r & 0xff, (r >> 8) & 0xff]
        assert len(self.v) <= 256, len(self.v)
        return out
    def fblob(self):
        out = []
        for (a, b, c, ua, ub, uc, t, fl) in self.f:
            for uv in (ua, ub, uc):
                assert 0 <= uv[0] <= 255 and 0 <= uv[1] <= 255, uv
            out += [a, b, c, ua[0], ua[1], ub[0], ub[1], uc[0], uc[1], t, fl, 0]
        return out

FLAT = 0x02
DOUBLE = 0x01
UNLIT = 0x04

def right(i):
    """Unit right-hand vector at point i -- the tangent there, turned 90 degrees."""
    hd = pts[i % N][2]
    return (math.cos(hd), 0.0, -math.sin(hd))

def pillar_ok(i):
    if (i - 1) % N in GAP or i % N in GAP:
        return False
    if pts[i][3] < 1.2:
        return False
    for j in range(N):
        d = min((i - j) % N, (j - i) % N)
        if d < 8:
            continue
        if math.hypot(pts[i][0] - pts[j][0], pts[i][1] - pts[j][1]) < HALFW + 1.0 \
           and pts[j][3] < pts[i][3]:
            return False                        # would come down through another road
    return True

chunks = []
for c in range(NCHUNK):
    i0 = c * CHUNK
    xs = [pts[(i0 + k) % N][0] for k in range(CHUNK + 1)]
    zs = [pts[(i0 + k) % N][1] for k in range(CHUNK + 1)]
    ox = round((min(xs) + max(xs)) / 2)
    oz = round((min(zs) + max(zs)) / 2)
    m = Mesh()

    # four vertices per section: top left/right, bottom left/right
    sec = []
    for k in range(CHUNK + 1):
        i = (i0 + k) % N
        x, z = cx[i] / 256 - ox, cz[i] / 256 - oz
        y = cy[i] / 256
        r = right(i)
        tl = m.vert((x - HALFW * r[0], y, z - HALFW * r[2]))
        tr = m.vert((x + HALFW * r[0], y, z + HALFW * r[2]))
        bl = m.vert((x - HALFW * r[0], y - THICK, z - HALFW * r[2]))
        br = m.vert((x + HALFW * r[0], y - THICK, z + HALFW * r[2]))
        sec.append((tl, tr, bl, br, r))

    for k in range(CHUNK):
        i = i0 + k
        if i in GAP:
            continue
        tl0, tr0, bl0, br0, r0 = sec[k]
        tl1, tr1, bl1, br1, r1 = sec[k + 1]
        rv = ((r0[0] + r1[0]) / 2, 0, (r0[2] + r1[2]) / 2)
        lv = (-rv[0], 0, -rv[2])
        v0, v1 = 8 * k, 8 * k + 8
        t = TEX_START if i == 0 else TEX_ROAD
        m.quad(tl0, tr0, tr1, tl1, [(0, v0), (31, v0), (31, v1), (0, v1)], t, 0, (0, 1, 0))
        m.quad(bl0, br0, br1, bl1, None, col(GREY, 8), FLAT, (0, -1, 0))
        m.quad(tl0, tl1, bl1, bl0, [(v0, 0), (v1, 0), (v1, 7), (v0, 7)], TEX_SIDE, 0, lv)
        m.quad(tr0, tr1, br1, br0, [(v0, 0), (v1, 0), (v1, 7), (v0, 7)], TEX_SIDE, 0, rv)

    # end caps where the road stops at the gap
    for k in range(CHUNK + 1):
        i = i0 + k
        before, after = (i - 1) % N in GAP, i % N in GAP
        if before == after or k == CHUNK and not after or k == 0 and not before:
            continue
        tl, tr, bl, br, r = sec[k]
        fwd = (math.sin(pts[i % N][2]), 0, math.cos(pts[i % N][2]))
        out = fwd if after else (-fwd[0], 0, -fwd[2])
        m.quad(tl, tr, br, bl, [(0, 0), (31, 0), (31, 7), (0, 7)], TEX_SIDE, 0, out)

    # pillars at sections 2, 6, 10, 14 -- or eight vertices on one point
    for k in (2, 6, 10, 14):
        i = (i0 + k) % N
        x, z = cx[i] / 256 - ox, cz[i] / 256 - oz
        top = cy[i] / 256 - THICK
        r = right(i)
        f = (math.sin(pts[i][2]), 0, math.cos(pts[i][2]))
        ok = pillar_ok(i)
        hw = 0.28
        corners = []
        for sr, sf in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            px = x + hw * (sr * r[0] + sf * f[0])
            pz = z + hw * (sr * r[2] + sf * f[2])
            corners.append((px, pz))
        vt, vb = [], []
        for (px, pz) in corners:
            if ok:
                vt.append(m.vert((px, top, pz)))
                vb.append(m.vert((px, 0.0, pz)))
            else:
                vt.append(m.vert((x, top, z)))
                vb.append(m.vert((x, top, z)))
        for a in range(4):
            b = (a + 1) % 4
            mx = (corners[a][0] + corners[b][0]) / 2 - x
            mz = (corners[a][1] + corners[b][1]) / 2 - z
            # a pillar that isn't there is eight vertices on one point; its
            # winding comes from the pillar it would have been, so its faces
            # match the standing pillars' and the chunks share a face blob
            ref = [(corners[a][0], top, corners[a][1]), (corners[b][0], top, corners[b][1]),
                   (corners[b][0], 0.0, corners[b][1]), (corners[a][0], 0.0, corners[a][1])]
            m.quad(vt[a], vt[b], vb[b], vb[a], None, col(GREY, 22), FLAT, (mx, 0, mz), ref)

    chunks.append((ox, oz, m))

# ground: a grid the C64 keeps under the camera, snapped to whole cells so
# the grass texture never slides. +-112 units, so the camera is always at
# least 80 from its edge -- which is why the hills stand at 76.
GCELL, GN = 32, 7
ground = Mesh()
gv = {}
for a in range(GN + 1):
    for b in range(GN + 1):
        gv[a, b] = ground.vert((-GCELL * GN / 2 + a * GCELL, 0.0, -GCELL * GN / 2 + b * GCELL))
for a in range(GN):
    for b in range(GN):
        ground.quad(gv[a, b], gv[a + 1, b], gv[a + 1, b + 1], gv[a, b + 1],
                    [(0, 0), (255, 0), (255, 255), (0, 255)], TEX_GRASS, 0, (0, 1, 0))

# hills: a ring round the camera, facing in
hills = Mesh()
HR, HN = 76.0, 22               # inside the ground's reach -- see GCELL
hr = random.Random(7)
for k in range(HN):
    a0 = 2 * math.pi * (k - 0.35) / HN
    a1 = 2 * math.pi * (k + 1.35) / HN
    am = (a0 + a1) / 2 + hr.uniform(-0.05, 0.05)
    hgt = hr.uniform(6, 13)
    p0 = hills.vert((HR * math.sin(a0), -1.0, HR * math.cos(a0)))
    p1 = hills.vert((HR * math.sin(a1), -1.0, HR * math.cos(a1)))
    pk = hills.vert(((HR + 3) * math.sin(am), hgt, (HR + 3) * math.cos(am)))
    inward = (-math.sin(am), 0.3, -math.cos(am))
    shade = col(GRASS, 9 + hr.randrange(5)) if k % 3 else col(EARTH, 14 + hr.randrange(4))
    hills.tri(p0, p1, pk, (0, 0), (0, 0), (0, 0), shade, FLAT, inward)

# the car: origin on the road under its centre, +z forward, 1.1 long
def box(m, x0, x1, y0, y1, z0, z1, colr, taper_front=0.0, skip=()):
    """A box (or a wedge: taper_front lowers the top of the front edge)."""
    c = {}
    for ix, x in enumerate((x0, x1)):
        for iy, y in enumerate((y0, y1)):
            for iz, z in enumerate((z0, z1)):
                yy = y - (taper_front if (iy == 1 and iz == 1) else 0)
                c[ix, iy, iz] = m.vert((x, yy, z))
    faces = {
        'top':    ([(0, 1, 0), (1, 1, 0), (1, 1, 1), (0, 1, 1)], (0, 1, 0)),
        'bottom': ([(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)], (0, -1, 0)),
        'left':   ([(0, 0, 0), (0, 1, 0), (0, 1, 1), (0, 0, 1)], (-1, 0, 0)),
        'right':  ([(1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1)], (1, 0, 0)),
        'front':  ([(0, 0, 1), (0, 1, 1), (1, 1, 1), (1, 0, 1)], (0, taper_front * 2, 1)),
        'back':   ([(0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0)], (0, 0, -1)),
    }
    for name, (q, out) in faces.items():
        if name in skip:
            continue
        colr_f = colr(name) if callable(colr) else colr
        m.quad(*[c[k] for k in q], None, colr_f, FLAT, out)

def car(body):
    m = Mesh()
    dark = col(GREY, 5)
    # chassis wedge, nose lower than the tail
    box(m, -0.34, 0.34, 0.12, 0.30, -0.55, 0.55, lambda n: col(body, 26 if n == 'top' else 22),
        taper_front=0.10, skip=('bottom',))
    # cockpit / roll cage
    box(m, -0.22, 0.22, 0.30, 0.50, -0.28, 0.08,
        lambda n: col(GREY, 6) if n in ('front', 'left', 'right') else col(body, 18), skip=('bottom',))
    # the spoiler, on its own
    box(m, -0.36, 0.36, 0.50, 0.56, -0.56, -0.40, col(body, 24), skip=())
    box(m, -0.05, 0.05, 0.30, 0.50, -0.50, -0.44, dark, skip=('top', 'bottom'))
    # wheels
    for sx in (-1, 1):
        for sz in (-1, 1):
            x0 = 0.34 if sx > 0 else -0.46
            z0 = 0.26 if sz > 0 else -0.46
            box(m, x0, x0 + 0.12, 0.0, 0.26, z0, z0 + 0.22, dark, skip=('bottom',))
    return m

carRed, carBlue = car(RED), car(BLUE)
assert carRed.vblob() == carBlue.vblob()

# trees, on the ground between the loops, clear of the road
trees = []
tr = random.Random(11)
while len(trees) < 12:
    x, z = tr.uniform(-55, 55), tr.uniform(-80, 80)
    if min(math.hypot(x - p[0], z - p[1]) for p in pts) < 6:
        continue
    if any(math.hypot(x - a, z - b) < 10 for a, b in trees):
        continue
    trees.append((x, z))

# ------------------------------------------------------------------ write
out = []
emit = out.append

def bytes_rows(label, data, per=16):
    emit(label)
    for k in range(0, len(data), per):
        emit("\t.byte " + ", ".join("$%02x" % (b & 0xff) for b in data[k:k + per]))

emit("; Generated by tools/gen_stunt.py -- do not edit by hand.")
emit("; The track, the scenery, the cars and the physics tables of gpu64_demo_stunt.a.")
emit("")
emit("TRK_N     = %d" % N)
emit("TRK_CHUNKS = %d" % NCHUNK)
emit("TEX_ROAD  = %d" % TEX_ROAD)
emit("TEX_START = %d" % TEX_START)
emit("TEX_SIDE  = %d" % TEX_SIDE)
emit("TEX_GRASS = %d" % TEX_GRASS)
emit("TEX_TREE  = %d" % TEX_TREE)
emit("SKY_COL   = %d" % col(SKY, 26))
emit("CROSS_A   = %d\t\t; the bridge" % CROSS_A)
emit("CROSS_B   = %d\t\t; and the road under it" % CROSS_B)
emit("GAP_FIRST = %d" % GAP[0])
emit("GAP_LAST  = %d" % GAP[-1])
emit("")

# the physics tables
for name, vals in (("cx", cx), ("cz", cz), ("cy", cy)):
    bytes_rows(name + "Lo", [v & 0xff for v in vals])
    bytes_rows(name + "Hi", [(v >> 8) & 0xff for v in vals])
bytes_rows("hdLo", [v & 0xff for v in hd16])
bytes_rows("hdHi", [v >> 8 for v in hd16])
bytes_rows("dhLo", [v & 0xff for v in dh16])
bytes_rows("dhHi", [(v >> 8) & 0xff for v in dh16])
bytes_rows("ptTab", ptTab)
bytes_rows("flTab", flTab)
emit("")

pal = []
for (r, g, b) in HUES:
    for l in range(32):
        pal += [round(r * l / 31), round(g * l / 31), round(b * l / 31)]
bytes_rows("palData", pal)
emit("")

for label, tid, data in textures:
    bytes_rows(label, list(data), 32)
emit("TEX_N = %d" % len(textures))
emit("texTab\t\t; id, address")
for label, tid, data in textures:
    emit("\t.word %d, %s" % (tid, label))
emit("")

# meshes, with face blobs shared where identical
fblobs = {}
meshes = []         # (id, vlabel, vlen, flabel, flen)
def add_mesh(mid, name, m, vlabel=None):
    vb = m.vblob()
    fb = tuple(m.fblob())
    if vlabel is None:
        vlabel = "mv" + name
        bytes_rows(vlabel, vb)
    if fb not in fblobs:
        fl = "mf" + name
        fblobs[fb] = fl
        bytes_rows(fl, list(fb))
    assert len(m.f) <= 255, (name, len(m.f))       # RESULT is one byte
    meshes.append((mid, vlabel, len(vb), fblobs[fb], len(fb), len(m.f)))

for c, (ox, oz, m) in enumerate(chunks):
    add_mesh(100 + c, "Trk%d" % c, m)
add_mesh(120, "Ground", ground)
add_mesh(121, "Hills", hills)
add_mesh(130, "CarRed", carRed)
add_mesh(131, "CarBlue", carBlue, vlabel="mvCarRed")
emit("")
emit("MESH_N = %d" % len(meshes))
emit("meshTab\t\t; id, vertex blob, its length, face blob, its length, faces")
for (mid, vl, vn, fl, fn, nf) in meshes:
    emit("\t.word %d, %s, %d, %s, %d, %d" % (mid, vl, vn, fl, fn, nf))
emit("")

MESH_GROUND, MESH_HILLS, MESH_CARP, MESH_CARAI = 120, 121, 130, 131
emit("MESH_GROUND = %d" % MESH_GROUND)
emit("MESH_HILLS  = %d" % MESH_HILLS)
emit("MESH_CARP   = %d" % MESH_CARP)
emit("MESH_CARAI  = %d" % MESH_CARAI)
emit("")
# the static nodes: id, mesh, x, y, z as 8.8. Nodes the frame loop moves
# (cars, ground, hills) are created from here too, at the origin.
emit("NODE_TRK0  = 10")
emit("STATIC_N = %d" % NCHUNK)
emit("staticTab\t; node id, mesh id, x, y, z (8.8)")
for c, (ox, oz, m) in enumerate(chunks):
    emit("\t.word %d, %d, $%04x, $%04x, $%04x" % (10 + c, 100 + c, q88(ox) & 0xffff, 0, q88(oz) & 0xffff))
emit("")
emit("TREE_N = %d" % len(trees))
emit("treeTab\t\t; x, z (8.8)")
for (x, z) in trees:
    emit("\t.word $%04x, $%04x" % (q88(x) & 0xffff, q88(z) & 0xffff))
emit("")

open(OUT, "w").write("\n".join(out) + "\n")

nf = sum(m[5] for m in meshes[:NCHUNK])
fb_bytes = sum(len(k) for k in fblobs)
vb_bytes = sum(m[2] for m in meshes) - meshes[-1][2]
print("R=%.2f C=%.2f  bridge clearance %.2f  %d track faces  %d face blobs (%d bytes)  %d vertex bytes"
      % (R, C, clear / 256, nf, len(fblobs), fb_bytes, vb_bytes))
print("chunk vertices: %s" % [len(m.v) for _, _, m in chunks])
print("heights: min %.2f max %.2f" % (min(cy) / 256, max(cy) / 256))
print("-> %s" % os.path.relpath(OUT))
