"""A third reader of the .g64lev format, for tools/prgsim.

There are now three: Source/Firmware/gpu64_level.cpp (what the Pi runs),
tools/check_g64lev.py (the converter's own gate), and this one. That is
deliberate, and it has already paid for itself once -- the firmware's magic
constant was 0x4c343947 ('G94L') rather than 0x4c343647 and built perfectly
clean; nothing but a second reader disagreeing would have said so before the
bench. tools/hostsim/levelsim.cpp holds the comparison that caught it.

This reader exists so that runsim.py can model LOAD_LEVEL/LEVEL_STEP, which
is what lets gpu64_demo_level.a run end to end on a PC. It reads only what
the loader reads: the palette, the textures, the meshes, the node placements
and info_player_start. The collision tables are skipped -- nothing in class 1
consumes them yet.

Copyright (c) 2026 Honza Slesinger

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.
"""
import struct

MAGIC = b'G64L'
VERSION = 4

HDR = '<4sHH4HHHI3I'
HDR_LEN = struct.calcsize(HDR)          # 36
TEX_REC = '<HBBII'                      # 12
MESH_REC = '<IIII'                      # 16
NODE_REC = '<HiiiH'                     # 16
ENT_REC = '<6H2h3i3iBBH'                # 44
ENT_STRIDE = struct.calcsize(ENT_REC)
PLANE_REC = '<3hiBB'                    # 12
HULL_REC = '<2i6i'                      # 32
CLIP_REC = '<HhhH'                      #  8

# Eye height above an entity origin, in Quake units. GPU64_LEVEL_EYE_QU in
# Source/Firmware/gpu64_3d_class1.cpp -- the two must agree or the model puts
# the camera somewhere the Pi does not.
EYE_QU = 22


class LevelError(Exception):
    pass


class Level:
    """A parsed .g64lev. Every accessor raises LevelError rather than
    returning a short blob: the firmware answers BAD_ARGS for exactly these
    cases, and the caller turns the exception into that."""

    def __init__(self, data):
        self.d = data
        if len(data) < HDR_LEN + 4:
            raise LevelError('shorter than its header')
        (magic, ver, self.scale, self.ntex, self.nmesh, self.nnode, self.nent,
         self.nplane, self.nhull, self.nclip, self.base, self.palo,
         self.stro) = struct.unpack_from(HDR, data, 0)
        if magic != MAGIC:
            raise LevelError('magic %r is not %r' % (magic, MAGIC))
        if ver != VERSION:
            raise LevelError('version %d is not %d' % (ver, VERSION))
        if self.scale == 0:
            raise LevelError('scale is zero')

        off = HDR_LEN
        self.tex_off, off = off, off + self.ntex * 12
        self.mesh_off, off = off, off + self.nmesh * 16
        self.node_off, off = off, off + self.nnode * 16
        self.ent_off, off = off, off + self.nent * ENT_STRIDE
        off += self.nplane * 12
        self.hull_off, off = off, off + self.nhull * 32
        off += self.nclip * 8
        # Equality, not "fits": a converter and a loader that disagree on a
        # stride still both land inside the file, and this is the only test
        # that notices.
        if off != self.base:
            raise LevelError('tables end at %d, header says base %d'
                             % (off, self.base))
        if self.base > len(data):
            raise LevelError('blob area starts past the end of the file')
        self.palette = self.blob(self.palo, 768)

    # --- the blob area ---------------------------------------------------
    def blob(self, off, length):
        if off + length > len(self.d) - self.base:
            raise LevelError('blob %d+%d runs past the %d-byte blob area'
                             % (off, length, len(self.d) - self.base))
        return self.d[self.base + off: self.base + off + length]

    def string(self, off):
        p = self.base + self.stro + off
        end = self.d.find(b'\0', p)
        if end < 0:
            raise LevelError('string at %d is not terminated' % off)
        return self.d[p:end].decode('latin-1')

    # --- the tables ------------------------------------------------------
    def tex(self, i):
        """(wshift, hshift, texels). The resource id is i and is not stored:
        a face's texid is one byte the renderer looks up directly, so the
        table index IS the id. The record's first field is the source BSP
        miptex number, provenance only."""
        _, ws, hs, o, ln = struct.unpack_from(TEX_REC, self.d,
                                             self.tex_off + i * 12)
        if not (3 <= ws <= 8 and 3 <= hs <= 8):
            raise LevelError('texture %d shifts %d,%d outside 3..8' % (i, ws, hs))
        if ln != (1 << ws) * (1 << hs):
            raise LevelError('texture %d len %d != %dx%d'
                             % (i, ln, 1 << ws, 1 << hs))
        return ws, hs, self.blob(o, ln)

    def mesh(self, i):
        """(vertbytes, facebytes) in the class-1 wire format -- stride 6 and
        stride 12, straight through to UPLOAD_MESH's own validation."""
        vo, vl, fo, fl = struct.unpack_from(MESH_REC, self.d,
                                            self.mesh_off + i * 16)
        return self.blob(vo, vl), self.blob(fo, fl)

    def node(self, i):
        """(meshidx, x, y, z) with the position already 16.16. The fifth
        field is the Quake brush model the chunk came from, 0 for the world
        itself -- how a game finds the nodes belonging to its doors."""
        mi, x, y, z, model = struct.unpack_from(NODE_REC, self.d,
                                               self.node_off + i * 16)
        if mi >= self.nmesh:
            raise LevelError('node %d names mesh %d of %d' % (i, mi, self.nmesh))
        return mi, x, y, z

    def hull(self, model):
        """One brush model's collision box and its two head clipnodes --
        gpu64_levelHull(). Model 0 is the world."""
        if model >= self.nhull:
            raise LevelError('model %d of %d hulls' % (model, self.nhull))
        f = struct.unpack_from(HULL_REC, self.d, self.hull_off + model * 32)
        return {'head': (f[0], f[1]),
                'mins': (f[2], f[3], f[4]), 'maxs': (f[5], f[6], f[7])}

    def node_model(self, i):
        return struct.unpack_from('<H', self.d, self.node_off + i * 16 + 14)[0]

    def ent(self, i):
        (nameo, yaw, spawn, modeli, targeto, targetnameo,
         p0, p1, x, y, z, ox, oy, oz, kind, _pad, _rsv) = \
            struct.unpack_from(ENT_REC, self.d, self.ent_off + i * ENT_STRIDE)
        return {
            'classname': self.string(nameo),
            'target': self.string(targeto),
            'targetname': self.string(targetnameo),
            'target_id': targeto, 'targetname_id': targetnameo,
            'yaw': yaw, 'spawnflags': spawn, 'model': modeli,
            'p0': p0, 'p1': p1, 'x': x, 'y': y, 'z': z,
            'ofs': (ox, oy, oz), 'kind': kind,
        }

    def player_start(self):
        """(x, y, z, yaw) at eye height, 16.16 -- levelPlayerStart() in the
        firmware. None if the level has no info_player_start."""
        for i in range(self.nent):
            e = self.ent(i)
            if e['classname'] != 'info_player_start':
                continue
            return (e['x'], e['y'] + EYE_QU * 65536 // self.scale, e['z'],
                    e['yaw'])
        return None
