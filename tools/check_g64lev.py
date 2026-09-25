#!/usr/bin/env python3
"""Validate a .g64lev level file produced by tools/gen_quakelevel.py.

Checks the header, that every table and blob offset lands inside the file,
and that every mesh blob obeys the class-1 wire format the firmware enforces
in gpu64_3dBuildMesh(): vertex stride 6, face stride 12, at most 256 verts,
and no face index past the mesh's own vertex count. Those are exactly the
conditions that come back as BAD_ARGS from UPLOAD_MESH, so a file that
passes here cannot be rejected for its shape.

  python3 tools/check_g64lev.py build/e1m1.g64lev
"""
import struct, sys

VERT_STRIDE = 6
FACE_STRIDE = 12
MAX_VERTS = 256

HDR = '<4sHH4HHHI3I'
HDR_LEN = struct.calcsize(HDR)
TEX_REC = '<HBBII'
MESH_REC = '<IIII'
NODE_REC = '<HiiiH'
ENT_REC = '<6H2h3i3iBBH'
ENT_STRIDE = struct.calcsize(ENT_REC)
PLANE_REC = '<3hiBB'
HULL_REC = '<2i6i'
CLIP_REC = '<HhhH'


def fail(msg):
    print('FAIL  ' + msg)
    sys.exit(1)


def main(path):
    d = open(path, 'rb').read()
    if len(d) < HDR_LEN + 4:
        fail('file is shorter than its header')

    (magic, ver, scale, ntex, nmesh, nnode, nent, nplane, nhull, nclip,
     base, palo, stro) = struct.unpack_from(HDR, d, 0)
    if magic != b'G64L':
        fail('magic is %r, not G64L' % magic)
    if ver != 4:
        fail('version %d is not 4' % ver)
    print('header  ver %d  scale %d qu/wu  tex %d  mesh %d  node %d  ent %d'
          % (ver, scale, ntex, nmesh, nnode, nent))
    print('        collision  %d planes  %d hulls  %d clipnodes'
          % (nplane, nhull, nclip))

    off = HDR_LEN
    tex_off, off = off, off + ntex * struct.calcsize(TEX_REC)
    mesh_off, off = off, off + nmesh * struct.calcsize(MESH_REC)
    node_off, off = off, off + nnode * struct.calcsize(NODE_REC)
    ent_off, off = off, off + nent * ENT_STRIDE
    plane_off, off = off, off + nplane * struct.calcsize(PLANE_REC)
    hull_off, off = off, off + nhull * struct.calcsize(HULL_REC)
    clip_off, off = off, off + nclip * struct.calcsize(CLIP_REC)
    if off != base:
        fail('tables end at %d but the header says the blob area starts at %d'
             % (off, base))
    if base > len(d):
        fail('blob area starts past the end of the file')
    print('tables  end at %d == base  OK   blob area %d B' % (base, len(d) - base))

    def blob(o, n, what):
        if o + n > len(d) - base:
            fail('%s runs past the end of the blob area (%d+%d > %d)'
                 % (what, o, n, len(d) - base))
        return d[base + o: base + o + n]

    pal = blob(palo, 768, 'palette')
    if len(pal) != 768:
        fail('palette is %d bytes, not 768' % len(pal))

    texels = 0
    for i in range(ntex):
        qid, ws, hs, o, ln = struct.unpack_from(TEX_REC, d, tex_off + i * 12)
        if not (3 <= ws <= 8 and 3 <= hs <= 8):
            fail('texture %d has shifts %d,%d outside the 3..8 UPLOAD_TEXTURE range'
                 % (i, ws, hs))
        if ln != (1 << ws) * (1 << hs):
            fail('texture %d: len %d != %dx%d' % (i, ln, 1 << ws, 1 << hs))
        blob(o, ln, 'texture %d' % i)
        texels += ln
    print('tex     %d records, %d texels, all shifts 3..8 and len == w*h  OK'
          % (ntex, texels))

    vmax = fmax = 0
    tris = 0
    for i in range(nmesh):
        vo, vl, fo, fl = struct.unpack_from(MESH_REC, d, mesh_off + i * 16)
        if vl == 0 or vl % VERT_STRIDE:
            fail('mesh %d: vertex len %d is not a nonzero multiple of %d'
                 % (i, vl, VERT_STRIDE))
        if fl == 0 or fl % FACE_STRIDE:
            fail('mesh %d: face len %d is not a nonzero multiple of %d'
                 % (i, fl, FACE_STRIDE))
        nv = vl // VERT_STRIDE
        nf = fl // FACE_STRIDE
        if nv > MAX_VERTS:
            fail('mesh %d has %d verts, past the 1-byte index limit of %d'
                 % (i, nv, MAX_VERTS))
        vb = blob(vo, vl, 'mesh %d verts' % i)
        fb = blob(fo, fl, 'mesh %d faces' % i)
        for f in range(nf):
            w = fb[f * FACE_STRIDE: (f + 1) * FACE_STRIDE]
            if max(w[0], w[1], w[2]) >= nv:
                fail('mesh %d face %d indexes vertex %d of %d'
                     % (i, f, max(w[0], w[1], w[2]), nv))
            if w[11] != 0:
                fail('mesh %d face %d has pad byte %02x, not 0' % (i, f, w[11]))
            # A textured face's texid is a one-byte resource id, so it must
            # name a texture this file actually carries; the renderer's only
            # other reading of it is "palette index", which is what
            # FLAT_COLOUR selects.
            if not (w[10] & 0x02) and w[9] >= ntex:
                fail('mesh %d face %d is textured with resource %d of %d'
                     % (i, f, w[9], ntex))
        vmax = max(vmax, nv); fmax = max(fmax, nf); tris += nf
    print('mesh    %d records, max %d verts / %d faces, %d tris, indices and pad  OK'
          % (nmesh, vmax, fmax, tris))

    for i in range(nnode):
        mi, x, y, z, model = struct.unpack_from(NODE_REC, d, node_off + i * 16)
        if mi >= nmesh:
            fail('node %d points at mesh %d of %d' % (i, mi, nmesh))
        for c, n in zip((x, y, z), 'xyz'):
            if abs(c) > 128 * 65536:
                fail('node %d origin %s = %.1f wu is past the 8.8 model range'
                     % (i, n, c / 65536.0))
    print('node    %d records, mesh ids and origins in range  OK' % nnode)

    # The string blob's extent is implied: it starts at stro and every offset
    # into it must land on a byte that follows a NUL, which is what makes a
    # C-string read from it terminate inside the blob.
    if d[base + stro] != 0:
        fail('the string blob does not start with the NUL that offset 0 means')
    nul = d.index(b'\0\0', base + stro) if b'\0\0' in d[base + stro:] else None
    strend = len(d) - base
    names = {}
    unknown = set()
    for i in range(nent):
        (nameOff, yaw, sf, mdl, tgt, tnm, p0, p1, x, y, z,
         ox, oy, oz, kind, _pad, _rsv) = \
            struct.unpack_from(ENT_REC, d, ent_off + i * ENT_STRIDE)
        for o, what in ((nameOff, 'classname'), (tgt, 'target'), (tnm, 'targetname')):
            if stro + o >= strend:
                fail('entity %d %s offset %d is past the string blob' % (i, what, o))
            e = d.index(b'\0', base + stro + o)
            if e >= len(d):
                fail('entity %d %s is unterminated' % (i, what))
        if mdl and mdl >= 256:
            fail('entity %d names brush model %d, which no BSP has' % (i, mdl))
        nm = d[base + stro + nameOff: d.index(b'\0', base + stro + nameOff)].decode()
        names[nm] = names.get(nm, 0) + 1
        if not nm:
            fail('entity %d has an empty classname' % i)
        # A door whose travel is zero is a door that cannot open, and the
        # only way to find that out later is at the bench. kinds 11-14 are
        # the four brush movers; see ENT_KIND in tools/gen_quakelevel.py.
        if kind in (11, 12, 13, 14) and (ox, oy, oz) == (0, 0, 0):
            fail('entity %d (%s, kind %d) is a mover with no travel' % (i, nm, kind))
        if kind == 0:
            unknown.add(nm)
    starts = names.get('info_player_start', 0)
    if starts != 1:
        fail('%d info_player_start entities; the game needs exactly one' % starts)
    print('ent     %d records, %d classnames, strings terminated, 1 player start  OK'
          % (nent, len(names)))
    # Not a failure: a classname the kind table has no opinion about is
    # decoration as far as a game is concerned. Printed because a *lot* of
    # them means the table has fallen behind the maps being converted.
    if unknown:
        print('ent     %d classnames have kind 0: %s'
              % (len(unknown), ' '.join(sorted(unknown))))
    # --- the collision hulls ------------------------------------------
    #
    # Shape checks first, then the one check that actually says the
    # conversion is right: walk hull 1 and ask what is at the player start.
    # A sign error in a plane normal, a missed axis swap, or a dist that did
    # not get scaled all leave the tables perfectly well-formed and put the
    # player inside a wall -- which the renderer cannot tell you, because
    # the renderer never looks at a clipnode.
    planes = [struct.unpack_from(PLANE_REC, d, plane_off + i * 12)
              for i in range(nplane)]
    for i, (nx, ny, nz, dist, ty, pad) in enumerate(planes):
        if ty > 3 or pad != 0:
            fail('plane %d has type %d pad %d' % (i, ty, pad))
        # 1.15 normals of a unit vector: the square sum must land on 1.0 to
        # within rounding, or the trace's distances are wrong by that factor.
        mag = (nx * nx + ny * ny + nz * nz) / (32767.0 * 32767.0)
        if not 0.99 < mag < 1.01:
            fail('plane %d normal is not unit: |n|^2 = %.4f' % (i, mag))
    clips = [struct.unpack_from(CLIP_REC, d, clip_off + i * 8)
             for i in range(nclip)]
    for i, (pn, c0, c1, pad) in enumerate(clips):
        if pn >= nplane:
            fail('clipnode %d names plane %d of %d' % (i, pn, nplane))
        for c in (c0, c1):
            if c >= nclip:
                fail('clipnode %d has child %d of %d' % (i, c, nclip))
            if c < -16:
                fail('clipnode %d has contents %d, which Quake has no name for'
                     % (i, c))
        if pad != 0:
            fail('clipnode %d has pad %d' % (i, pad))
    hulls = [struct.unpack_from(HULL_REC, d, hull_off + i * 32)
             for i in range(nhull)]
    for i, h in enumerate(hulls):
        for k in range(3):
            if h[2 + k] > h[5 + k]:
                fail('hull %d axis %d has mins %d > maxs %d'
                     % (i, k, h[2 + k], h[5 + k]))
    print('hull    %d planes unit-length, %d clipnodes in range, %d models  OK'
          % (nplane, nclip, nhull))

    def contents(node, pt):
        """Quake's hull point test, on the converted tables. Returns the
        CONTENTS_* the point lands in: -1 empty, -2 solid, -3.. liquid."""
        while node >= 0:
            pn, c0, c1, _ = clips[node]
            nx, ny, nz, dist, _, _ = planes[pn]
            # 1.15 normal against a 16.16 position gives 16.16 after the
            # >>15, which is the unit dist is in.
            dot = ((nx * pt[0] + ny * pt[1] + nz * pt[2]) >> 15) - dist
            node = c1 if dot < 0 else c0
        return node

    start = None
    for i in range(nent):
        r = ent_off + i * ENT_STRIDE
        nm = d[base + stro + struct.unpack_from('<H', d, r)[0]:]
        if nm[:nm.index(b'\0')] == b'info_player_start':
            start = struct.unpack_from('<3i', d, r + 16)
    if start is None:
        fail('no info_player_start to test the hull with')

    root = hulls[0][0]                  # worldspawn, hull 1 (the player hull)
    eye = (start[0], start[1] + (22 << 16) // scale, start[2])
    c = contents(root, eye)
    if c != -1:
        fail('the player start is in contents %d, not empty space -- the hull'
             ' conversion has an axis or a sign wrong' % c)

    # And a second point that must NOT be empty, or "empty" proves nothing:
    # straight down from the start is floor.
    below = (start[0], start[1] - (64 << 16) // scale, start[2])
    cb = contents(root, below)
    if cb == -1:
        fail('64 Quake units below the player start is also empty -- the hull'
             ' is not describing this level')
    print('trace   player start is empty (%d), 64qu below is solid (%d)  OK'
          % (c, cb))

    print('PASS  %s (%.1f KB)' % (path, len(d) / 1024.0))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'build/e1m1.g64lev')
