#!/usr/bin/env python3
"""
Need for Speed II SE car model (GEO) and archive (VIV/BIGF) tool.

  nfs2geo.py viv-list  <file.viv>                      list archive entries
  nfs2geo.py viv-extract <file.viv> <out_dir>          extract all entries
  nfs2geo.py geo-info  <file.geo>                      parts, vertex/polygon counts, textures
  nfs2geo.py geo-obj   <file.geo> <out.obj> [parts]    export parts (default: 0-8, high detail body) as OBJ
  nfs2geo.py viv-replace <file.viv> <name> <new_file>  replace one archive entry (writes the .viv in place)

Formats (see docs/ai/game-formats.md):
  VIV: "BIGF", u32 BE total size, u32 BE count, u32 BE header size, entries (u32 BE offset, u32 BE size, name\\0), data.
  GEO: 140-byte header (u32 version, 32 runtime part pointers, 8 bytes), then 32 parts:
       52-byte part header (u32 vertex count, u32 polygon count, i32 position[3] 16.16 metres, runtime pointers),
       vertices i16 x, y, z (256 units per metre, count padded to even),
       polygons 12 bytes: u32 type/flags, u8 index[4], char texture[4] (no texture coordinates).
"""
import struct
import sys

GEO_HEADER = 140
PART_HEADER = 52


def geo_parse(data):
    """Returns a list of 32 parts: dict(nv, np, pos, verts, polys, off)."""
    p = GEO_HEADER
    parts = []
    while p + PART_HEADER <= len(data) and len(parts) < 32:
        nv, npoly = struct.unpack_from('<II', data, p)
        pos = struct.unpack_from('<iii', data, p + 8)
        q = p + PART_HEADER
        verts = [struct.unpack_from('<hhh', data, q + 6 * i) for i in range(nv)]
        q += 6 * (nv + nv % 2)
        polys = []
        for i in range(npoly):
            t = struct.unpack_from('<I', data, q)[0]
            polys.append((t, tuple(data[q + 4:q + 8]), data[q + 8:q + 12].decode('latin1')))
            q += 12
        parts.append(dict(nv=nv, np=npoly, pos=pos, verts=verts, polys=polys, off=p))
        p = q
    return parts


def geo_write(data, parts):
    """Rebuilds a GEO file from parsed (possibly modified) parts, keeping the original headers."""
    out = bytearray(data[:GEO_HEADER])
    for pt in parts:
        hdr = bytearray(data[pt['off']:pt['off'] + PART_HEADER])
        struct.pack_into('<II', hdr, 0, len(pt['verts']), len(pt['polys']))
        out += hdr
        vs = list(pt['verts'])
        if len(vs) % 2:
            vs.append((0, 0, 0))
        for v in vs:
            out += struct.pack('<hhh', *v)
        for t, idx, tex in pt['polys']:
            out += struct.pack('<I', t) + bytes(idx) + tex.encode('latin1')
    return bytes(out)


def viv_read(path):
    data = open(path, 'rb').read()
    if data[:4] != b'BIGF':
        raise ValueError('not a BIGF archive')
    count = struct.unpack('>I', data[8:12])[0]
    p = 16
    entries = []
    for _ in range(count):
        off, size = struct.unpack('>II', data[p:p + 8])
        p += 8
        end = data.index(b'\0', p)
        entries.append([data[p:end], data[off:off + size]])
        p = end + 1
    return data, entries


def viv_write(path, original, entries):
    """Writes the archive keeping the original header size (entry data starts at the same offset)."""
    header_size = struct.unpack('>I', original[12:16])[0]
    table_size = 16 + sum(8 + len(name) + 1 for name, _ in entries)
    if table_size > header_size:
        raise ValueError('entry table does not fit the original header')
    off = header_size
    table = b''
    blob = b''
    for name, payload in entries:
        table += struct.pack('>II', off, len(payload)) + name + b'\0'
        blob += payload
        off += len(payload)
    out = b'BIGF' + struct.pack('>III', header_size + len(blob), len(entries), header_size) + table
    out += original[len(out):header_size] + blob
    open(path, 'wb').write(out)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    cmd = argv[1]
    if cmd == 'viv-list':
        _, entries = viv_read(argv[2])
        for name, payload in entries:
            print(f'{len(payload):8d}  {name.decode("latin1")}')
    elif cmd == 'viv-extract':
        import os
        _, entries = viv_read(argv[2])
        os.makedirs(argv[3], exist_ok=True)
        for name, payload in entries:
            open(os.path.join(argv[3], name.decode('latin1').lower()), 'wb').write(payload)
    elif cmd == 'viv-replace':
        original, entries = viv_read(argv[2])
        target = argv[3].lower().encode()
        hits = [e for e in entries if e[0].lower() == target]
        if not hits:
            print('entry not found')
            return 1
        new = open(argv[4], 'rb').read()
        for e in hits:
            e[1] = new
        viv_write(argv[2], original, entries)
    elif cmd == 'geo-info':
        parts = geo_parse(open(argv[2], 'rb').read())
        print('polygons', sum(p['np'] for p in parts), 'vertices', sum(p['nv'] for p in parts))
        for i, p in enumerate(parts):
            if p['nv']:
                tex = sorted(set(t for _, _, t in p['polys']))
                print(f'{i:2d} verts {p["nv"]:3d} polys {p["np"]:3d} pos {[round(c / 65536, 3) for c in p["pos"]]} tex {tex[:6]}')
    elif cmd == 'geo-obj':
        parts = geo_parse(open(argv[2], 'rb').read())
        sel = [int(x) for x in argv[4].split(',')] if len(argv) > 4 else list(range(9))
        with open(argv[3], 'w') as f:
            base = 1
            for i in sel:
                p = parts[i]
                f.write(f'o part{i}\n')
                for v in p['verts']:
                    f.write('v %f %f %f\n' % tuple(p['pos'][c] / 65536 + v[c] / 256 for c in range(3)))
                for _, idx, _ in p['polys']:
                    face = list(dict.fromkeys(j for j in idx if j < p['nv']))
                    if len(face) >= 3:
                        f.write('f ' + ' '.join(str(base + j) for j in face) + '\n')
                base += p['nv']
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
