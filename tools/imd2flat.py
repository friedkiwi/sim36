#!/usr/bin/env python3
"""imd2flat.py - convert ImageDisk (.IMD) diskette captures to the flat images
SIM/36's diskette drive attaches.

    tools/imd2flat.py [--allow-bad] [-o DIR] FILE.IMD ...

The flat layout is the byte-exact physical sector image in cylinder/head
order, which is what `attach diskette0` and `diskette insert` read (the
drive probes the VOL1 label on cylinder 0 to learn the geometry):

1. tracks in ascending (cylinder, head) order; a track the container does
   not hold is absent, a track with zero sectors contributes nothing;
2. within a track, sectors in ascending recorded sector id (never in the
   order the container stores them: some captures interleave cylinder 0);
3. every sector contributes its own track's sector size, so cylinder 0 may
   have a 128-byte-sector label track and a 256-byte-sector head 1;
4. sector ids must be exactly 1..n; a gap is a damaged capture.

An unreadable sector stops the conversion unless --allow-bad is given, in
which case it is zero-filled and reported: an invented sector looks
exactly like a real one, so the default is to refuse.

ImageDisk format (Dave Dunfield): an ASCII comment ended by 0x1A, then per
track a 5-byte header (mode, cylinder, head, sector count, sector-size
code), the sector-id map, an optional cylinder map (head bit 7) and head
map (head bit 6), and one record per sector: 0 = unavailable, 1/3/5/7 =
stored in full, 2/4/6/8 = one byte repeated for the whole sector.
"""
import argparse
import os
import sys

SECTOR_SIZES = {0: 128, 1: 256, 2: 512, 3: 1024, 4: 2048, 5: 4096, 6: 8192}


class BadCapture(Exception):
    pass


def read_imd(path):
    """Return (comment, tracks); a track is (cylinder, head, sector_size,
    {sector id: bytes or None})."""
    data = open(path, 'rb').read()
    end = data.find(b'\x1a')
    if end < 0:
        raise BadCapture('no 0x1A comment terminator: not an ImageDisk file')
    comment = data[:end].decode('latin-1')
    p = end + 1
    tracks = []
    while p < len(data):
        if p + 5 > len(data):
            raise BadCapture('truncated track header at %d' % p)
        mode, cyl, head, nsec, code = data[p:p + 5]
        p += 5
        if code not in SECTOR_SIZES:
            raise BadCapture('bad sector-size code %d at %d' % (code, p))
        size = SECTOR_SIZES[code]
        ids = list(data[p:p + nsec])
        p += nsec
        if head & 0x80:
            p += nsec
        if head & 0x40:
            p += nsec
        head &= 0x3f
        sectors = {}
        for sid in ids:
            if p >= len(data):
                raise BadCapture('truncated sector record at %d' % p)
            kind = data[p]
            p += 1
            if kind == 0:
                sectors[sid] = None
            elif kind in (1, 3, 5, 7):
                sectors[sid] = data[p:p + size]
                p += size
            elif kind in (2, 4, 6, 8):
                sectors[sid] = bytes([data[p]]) * size
                p += 1
            else:
                raise BadCapture('unknown sector record type %d at %d' % (kind, p))
        tracks.append((cyl, head, size, sectors))
    return comment, tracks


def flatten(tracks, allow_bad=False):
    out = bytearray()
    complaints = []
    for cyl, head, size, sectors in sorted(tracks, key=lambda t: (t[0], t[1])):
        if not sectors:
            continue
        ids = sorted(sectors)
        if ids != list(range(1, len(ids) + 1)):
            raise BadCapture('cylinder %d head %d has sector ids %s, not 1..%d'
                             % (cyl, head, ids, len(ids)))
        for sid in ids:
            body = sectors[sid]
            if body is None:
                if not allow_bad:
                    raise BadCapture('cylinder %d head %d sector %d is unreadable'
                                     % (cyl, head, sid))
                complaints.append('%d/%d/%d unreadable - zero filled' % (cyl, head, sid))
                body = bytes(size)
            if len(body) != size:
                raise BadCapture('cylinder %d head %d sector %d is %d bytes, not %d'
                                 % (cyl, head, sid, len(body), size))
            out += body
    return bytes(out), complaints


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('images', nargs='+', help='ImageDisk .IMD files')
    ap.add_argument('-o', '--out', metavar='DIR', default='.',
                    help='directory for the .img files (default: current)')
    ap.add_argument('--allow-bad', action='store_true',
                    help='zero-fill unreadable sectors instead of refusing')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    status = 0
    for path in args.images:
        stem = os.path.splitext(os.path.basename(path))[0]
        target = os.path.join(args.out, stem + '.img')
        try:
            comment, tracks = read_imd(path)
            image, complaints = flatten(tracks, args.allow_bad)
        except (BadCapture, OSError) as e:
            print('%s: %s' % (path, e), file=sys.stderr)
            status = 1
            continue
        with open(target, 'wb') as f:
            f.write(image)
        first = comment.replace('\r\n', ' | ').replace('\n', ' | ').strip()
        print('%s -> %s  %d bytes, %d track(s)  [%s]' % (path, target, len(image), len(tracks), first))
        for c in complaints:
            print('   ' + c)
    return status


if __name__ == '__main__':
    sys.exit(main())
