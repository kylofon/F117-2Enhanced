"""Mac F-117A art -> the --mac-art folder of F-117A Enhanced (PLAN.md Phase 7, S7.1).

The Mac release (2.x) keeps its pictures as 'CvPc' resources in the resource fork of "F117a Art". This tool reads that fork
from your own copy of the Mac release and writes each wanted picture as an uncompressed indexed file the engine loads
(f117port/src/enhanced/macart.c). Nothing it writes is part of this repository: put the output in a folder of your own
(never in Game/), then run the game with --mac-art <that folder>.

usage:
  python tools/mac_art.py SOURCE OUTDIR [--all]
SOURCE is one of
  - a StuffIt archive (classic SIT!, method 13, e.g. f117a_stealth_fighter.sit): searched, nested archives too,
    for the file "F117a Art" and its resource fork;
  - the resource fork itself (e.g. "F117a Art.rsrc" as written by an unstuffer, or "F117a Art/..namedfork/rsrc" on a Mac);
  - a folder: searched for one of the above.
--all writes every CvPc picture (77), not only the cockpit ones.

Output, per picture: <Name>.mpic =
  'MPIC' (4 bytes), w, h, ncolours (u16 little-endian each), ncolours * RGB (3 bytes), then w * h colour indices, rows top
  down. Pure cyan (0,255,255) is the see-through key of the cockpit pictures. Plus rects.json: per picture its size and the
  cyan regions (bounding box and pixel count, the windows and display screens), and the source of each picture.
"""
import json
import os
import struct
import sys

COCKPIT = ['CockpitOff', 'LeftCock', 'RightCock', 'RearCock', 'CockpitSmall', 'LeftSmall', 'RightSmall', 'RearSmall']
ART_NAME = 'F117a Art'

# ------------------------------------------------------------------ StuffIt (classic SIT!), methods 0 and 13, CRC-checked

META_CODES = [0x5d8, 0x058, 0x040, 0x0c0, 0x000, 0x078, 0x02b, 0x014,
              0x00c, 0x01c, 0x01b, 0x00b, 0x010, 0x020, 0x038, 0x018,
              0x0d8, 0xbd8, 0x180, 0x680, 0x380, 0xf80, 0x780, 0x480,
              0x080, 0x280, 0x3d8, 0xfd8, 0x7d8, 0x9d8, 0x1d8, 0x004,
              0x001, 0x002, 0x007, 0x003, 0x008]
META_LENS = [11, 8, 8, 8, 8, 7, 6, 5, 5, 5, 5, 6, 5, 6, 7, 7, 9, 12, 10, 11, 11, 12,
             12, 11, 11, 11, 12, 12, 12, 12, 12, 5, 2, 2, 3, 4, 5]

_CRC = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0xA001 if _c & 1 else _c >> 1
    _CRC.append(_c)


def crc16(data):
    c = 0
    for b in data:
        c = (c >> 8) ^ _CRC[(c ^ b) & 0xff]
    return c


class Bits:
    def __init__(self, d):
        self.d = bytes(d) + b'\0' * 8
        self.p = 0

    def peek(self):
        q = self.p >> 3
        return int.from_bytes(self.d[q:q + 8], 'little') >> (self.p & 7)

    def get(self, n):
        r = self.peek() & ((1 << n) - 1)
        self.p += n
        return r

    def sym(self, code):
        tab, maxl = code
        v = self.peek()
        for ln in range(1, maxl + 1):
            s = tab.get((ln, v & ((1 << ln) - 1)))
            if s is not None:
                self.p += ln
                return s
        raise ValueError('bad Huffman code')


def _rev(x, n):
    r = 0
    for _ in range(n):
        r = (r << 1) | (x & 1)
        x >>= 1
    return r


def _canon(lens):
    tab, code, maxl = {}, 0, max(lens)
    for ln in range(1, maxl + 1):
        for s, sl in enumerate(lens):
            if sl == ln:
                tab[(ln, _rev(code, ln))] = s
                code += 1
        code <<= 1
    return tab, maxl


def _parse_code(b, n, meta):
    lens = [0] * (n + 70)
    length, i = 0, 0
    while i < n:
        v = b.sym(meta)
        if v == 31: length = 0
        elif v == 32: length += 1
        elif v == 33: length -= 1
        elif v == 34:
            if b.get(1):
                lens[i] = length
                i += 1
        elif v == 35:
            for _ in range(b.get(3) + 2):
                lens[i] = length
                i += 1
        elif v == 36:
            for _ in range(b.get(6) + 10):
                lens[i] = length
                i += 1
        else:
            length = v + 1
        lens[i] = length
        i += 1
    return _canon([max(0, x) for x in lens[:n]])


def _sit13(d, ulen):
    b = Bits(d)
    val = b.get(8)
    if val >> 4:
        raise ValueError('method 13 with static tables %d is not supported' % (val >> 4))
    meta = ({(ln, c): s for s, (c, ln) in enumerate(zip(META_CODES, META_LENS))}, max(META_LENS))
    first = _parse_code(b, 321, meta)
    second = first if val & 8 else _parse_code(b, 321, meta)
    offc = _parse_code(b, (val & 7) + 10, meta)
    out = bytearray()
    cur = first
    while len(out) < ulen:
        v = b.sym(cur)
        if v < 0x100:
            out.append(v)
            cur = first
            continue
        cur = second
        if v < 0x13e: ln = v - 0x100 + 3
        elif v == 0x13e: ln = b.get(10) + 65
        elif v == 0x13f: ln = b.get(15) + 65
        else: break
        bl = b.sym(offc)
        off = 1 if bl == 0 else 2 if bl == 1 else (1 << (bl - 1)) + b.get(bl - 1) + 1
        st = len(out) - off
        for k in range(ln):
            out.append(out[st + k])
    return bytes(out[:ulen])


def _unpack(m, d, ulen):
    m &= 15
    if m == 0: return d
    if m == 13: return _sit13(d, ulen)
    raise ValueError('StuffIt method %d is not supported' % m)


def sit_entries(d):
    """Yields (path, name, rsrc_fork_or_None, data_fork_or_None) of a classic SIT! archive (forks CRC-checked)."""
    if d[:4] != b'SIT!':
        raise ValueError('not a classic StuffIt archive')
    p, path = 22, []
    while p + 112 <= len(d):
        h = d[p:p + 112]
        rm, dm, nl = h[0], h[1], h[2]
        name = h[3:3 + nl].decode('mac_roman')
        rul, dul, rcl, dcl = struct.unpack('>IIII', h[84:100])
        rcrc, dcrc = struct.unpack('>HH', h[100:104])
        p += 112
        if rm == 32 or dm == 32:
            path.append(name)
            continue
        if rm == 33 or dm == 33:
            if path: path.pop()
            continue
        rd, dd = d[p:p + rcl], d[p + rcl:p + rcl + dcl]
        p += rcl + dcl
        forks = []
        for m, cd, ul, crc in ((rm, rd, rul, rcrc), (dm, dd, dul, dcrc)):
            u = None
            if ul:
                try:
                    u = _unpack(m, cd, ul)
                    if len(u) != ul or crc16(u) != crc:
                        print('warning: CRC mismatch in', '/'.join(path + [name]))
                except ValueError as e:
                    print('skipped', '/'.join(path + [name]) + ':', e)
                    u = None
            forks.append(u)
        yield '/'.join(path + [name]), name, forks[0], forks[1]


# ------------------------------------------------------------------ resource fork

def rsrc_read(d):
    """[(type, id, name, data)] of a Mac resource fork."""
    doff, moff, dlen, mlen = struct.unpack('>IIII', d[:16])
    m = d[moff:moff + mlen]
    tlo, nlo = struct.unpack('>HH', m[24:28])
    nt = struct.unpack('>H', m[tlo:tlo + 2])[0] + 1
    res = []
    for i in range(nt):
        t = m[tlo + 2 + i * 8: tlo + 10 + i * 8]
        typ = t[:4].decode('mac_roman')
        cnt, rlo = struct.unpack('>HH', t[4:8])
        for j in range(cnt + 1):
            r = m[tlo + rlo + j * 12: tlo + rlo + j * 12 + 12]
            rid, no = struct.unpack('>hH', r[:4])
            ao = int.from_bytes(r[5:8], 'big')
            name = ''
            if no != 0xffff:
                q = nlo + no
                name = m[q + 1:q + 1 + m[q]].decode('mac_roman')
            q = doff + ao
            ln = struct.unpack('>I', d[q:q + 4])[0]
            res.append((typ, rid, name, d[q + 4:q + 4 + ln]))
    return res


def is_rsrc(d):
    if len(d) < 16: return False
    doff, moff, dlen, mlen = struct.unpack('>IIII', d[:16])
    return 16 <= doff and doff + dlen <= len(d) and moff + mlen <= len(d) and mlen >= 30


# ------------------------------------------------------------------ CvPc: w, h (BE), LZW min code size, colours - 1, RGB palette, GIF LZW sub-blocks

def lzw_gif(data, mcs, npix):
    raw = bytearray()
    p = 0
    while p < len(data):
        n = data[p]
        p += 1
        if n == 0: break
        raw += data[p:p + n]
        p += n
    clear, eoi = 1 << mcs, (1 << mcs) + 1
    out = bytearray()
    bitpos, total = 0, len(raw) * 8
    size = mcs + 1
    prefix, suffix = [0] * 4096, [0] * 4096
    for i in range(clear): suffix[i] = i
    nxt, prev = eoi + 1, -1

    def expand(c):
        s = bytearray()
        while c >= clear + 2 or c < clear:
            s.append(suffix[c])
            if c < clear: break
            c = prefix[c]
        s.reverse()
        return s

    while bitpos + size <= total and len(out) < npix:
        q = bitpos >> 3
        code = (int.from_bytes(raw[q:q + 3], 'little') >> (bitpos & 7)) & ((1 << size) - 1)
        bitpos += size
        if code == clear:
            size, nxt, prev = mcs + 1, eoi + 1, -1
            continue
        if code == eoi: break
        if prev < 0:
            out += expand(code)
            prev = code
            continue
        if code < nxt:
            s = expand(code)
            first = s[0]
        else:
            s = expand(prev)
            first = s[0]
            s.append(first)
        out += s
        if nxt < 4096:
            prefix[nxt], suffix[nxt] = prev, first
            nxt += 1
            if nxt == (1 << size) and size < 12: size += 1
        prev = code
    if len(out) < npix: out += bytes(npix - len(out))
    return bytes(out[:npix])


def cvpc_decode(d):
    w, h, mcs, n1 = struct.unpack('>HHBB', d[:6])
    n = n1 + 1
    pal = d[6:6 + 3 * n]
    return w, h, pal, lzw_gif(d[6 + 3 * n:], mcs, w * h)


def cyan_regions(w, h, pal, pix, minpx=64):
    keys = {i for i in range(len(pal) // 3) if pal[3 * i:3 * i + 3] == b'\x00\xff\xff'}
    if not keys: return []
    seen = bytearray(w * h)
    regs = []
    for s in range(w * h):
        if seen[s] or pix[s] not in keys: continue
        stack, cnt = [s], 0
        seen[s] = 1
        x0 = y0 = 1 << 30
        x1 = y1 = -1
        while stack:
            q = stack.pop()
            cnt += 1
            x, y = q % w, q // w
            x0, x1, y0, y1 = min(x0, x), max(x1, x), min(y0, y), max(y1, y)
            for nq in ((q - 1) if x > 0 else -1, (q + 1) if x < w - 1 else -1, q - w, q + w):
                if 0 <= nq < w * h and not seen[nq] and pix[nq] in keys:
                    seen[nq] = 1
                    stack.append(nq)
        if cnt >= minpx:
            regs.append({'x0': x0, 'y0': y0, 'x1': x1, 'y1': y1, 'pixels': cnt})
    regs.sort(key=lambda r: (r['y0'], r['x0']))
    return regs


# ------------------------------------------------------------------ finding the art fork

def find_art(src):
    """(resource fork bytes, description) of 'F117a Art' in src."""
    if os.path.isdir(src):
        for root, dirs, files in os.walk(src):
            for f in files:
                fp = os.path.join(root, f)
                low = f.lower()
                if low in (ART_NAME.lower() + '.rsrc', 'rsrc') or low.endswith('.sit') or low == ART_NAME.lower():
                    try:
                        return find_art(fp)
                    except (ValueError, OSError):
                        continue
        raise ValueError('no "%s" resource fork or StuffIt archive in %s' % (ART_NAME, src))
    d = open(src, 'rb').read()
    if d[:4] == b'SIT!':
        r = _find_in_sit(d, os.path.basename(src))
        if r: return r
        raise ValueError('no "%s" in %s' % (ART_NAME, src))
    if is_rsrc(d) and any(t == 'CvPc' for t, _, _, _ in rsrc_read(d)):
        return d, src
    raise ValueError('%s is neither a StuffIt archive nor a resource fork with CvPc pictures' % src)


def _find_in_sit(d, where):
    nested = []
    for path, name, rf, df in sit_entries(d):
        if name == ART_NAME and rf and is_rsrc(rf):
            return rf, where + ':' + path
        for f in (df, rf):
            if f and f[:4] == b'SIT!': nested.append((path, f))
    for path, f in nested:
        r = _find_in_sit(f, where + ':' + path)
        if r: return r
    return None


def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    if len(args) != 2:
        print(__doc__)
        return 2
    src, out = args
    want_all = '--all' in argv
    fork, where = find_art(src)
    os.makedirs(out, exist_ok=True)
    meta = {'source': where, 'key': [0, 255, 255], 'pictures': {}}
    done = 0
    for typ, rid, name, data in rsrc_read(fork):
        if typ != 'CvPc': continue
        base = os.path.splitext(name)[0].replace(' ', '_')
        if not want_all and base not in COCKPIT: continue
        w, h, pal, pix = cvpc_decode(data)
        with open(os.path.join(out, base + '.mpic'), 'wb') as f:
            f.write(b'MPIC' + struct.pack('<HHH', w, h, len(pal) // 3) + pal + pix)
        meta['pictures'][base] = {'id': rid, 'w': w, 'h': h, 'colours': len(pal) // 3, 'cyan': cyan_regions(w, h, pal, pix)}
        print('%-14s %3dx%3d %3d colours, %d see-through regions' % (base, w, h, len(pal) // 3, len(meta['pictures'][base]['cyan'])))
        done += 1
    with open(os.path.join(out, 'rects.json'), 'w') as f:
        json.dump(meta, f, indent=1)
    print('%d pictures from %s -> %s' % (done, where, out))
    return 0 if done else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
