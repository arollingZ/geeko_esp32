"""TTF -> 8x16 bitmap font generator (pure python, no external deps).

Parses the TrueType tables needed for simple glyph outlines, rasterizes each
ASCII char into an 8x16 cell with 4x4 supersampling, and emits a C array.
"""
import struct
import sys


class TTF:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        num_tables = struct.unpack('>H', self.d[4:6])[0]
        self.t = {}
        for i in range(num_tables):
            off = 12 + i * 16
            tag = self.d[off:off + 4].decode('latin1')
            toff, tlen = struct.unpack('>II', self.d[off + 8:off + 16])
            self.t[tag] = (toff, tlen)

        head = self.t['head'][0]
        self.units_per_em = struct.unpack('>H', self.d[head + 18:head + 20])[0]
        self.loc_format = struct.unpack('>h', self.d[head + 50:head + 52])[0]

        maxp = self.t['maxp'][0]
        self.num_glyphs = struct.unpack('>H', self.d[maxp + 4:maxp + 6])[0]

        hhea = self.t['hhea'][0]
        self.ascent = struct.unpack('>h', self.d[hhea + 4:hhea + 6])[0]
        self.descent = struct.unpack('>h', self.d[hhea + 6:hhea + 8])[0]
        self.num_hmetrics = struct.unpack('>H', self.d[hhea + 34:hhea + 36])[0]

        self.cmap = self._parse_cmap()
        self.loca = self._parse_loca()
        self.hmtx = self._parse_hmtx()

    def _parse_cmap(self):
        base = self.t['cmap'][0]
        n = struct.unpack('>H', self.d[base + 2:base + 4])[0]
        best = None
        for i in range(n):
            off = base + 4 + i * 8
            pid, eid, sub = struct.unpack('>HHI', self.d[off:off + 8])
            if (pid, eid) in ((3, 1), (3, 10), (0, 3), (0, 4)):
                best = base + sub
                break
        if best is None:
            raise RuntimeError('no unicode cmap')
        fmt = struct.unpack('>H', self.d[best:best + 2])[0]
        if fmt != 4:
            raise RuntimeError('only cmap format 4 supported, got %d' % fmt)
        seg_x2 = struct.unpack('>H', self.d[best + 6:best + 8])[0]
        segs = seg_x2 // 2
        p = best + 14
        ends = struct.unpack('>%dH' % segs, self.d[p:p + seg_x2]); p += seg_x2
        p += 2  # reservedPad
        starts = struct.unpack('>%dH' % segs, self.d[p:p + seg_x2]); p += seg_x2
        deltas = struct.unpack('>%dh' % segs, self.d[p:p + seg_x2]); p += seg_x2
        ro_base = p
        ranges = struct.unpack('>%dH' % segs, self.d[p:p + seg_x2])

        def lookup(c):
            for i in range(segs):
                if starts[i] <= c <= ends[i]:
                    if ranges[i] == 0:
                        return (c + deltas[i]) & 0xFFFF
                    addr = ro_base + i * 2 + ranges[i] + (c - starts[i]) * 2
                    g = struct.unpack('>H', self.d[addr:addr + 2])[0]
                    return (g + deltas[i]) & 0xFFFF if g else 0
            return 0
        return lookup

    def _parse_loca(self):
        base = self.t['loca'][0]
        n = self.num_glyphs + 1
        if self.loc_format == 0:
            v = struct.unpack('>%dH' % n, self.d[base:base + n * 2])
            return [x * 2 for x in v]
        return list(struct.unpack('>%dI' % n, self.d[base:base + n * 4]))

    def _parse_hmtx(self):
        base = self.t['hmtx'][0]
        out = []
        for i in range(self.num_hmetrics):
            adv = struct.unpack('>H', self.d[base + i * 4:base + i * 4 + 2])[0]
            out.append(adv)
        return out

    def advance(self, gid):
        if gid < len(self.hmtx):
            return self.hmtx[gid]
        return self.hmtx[-1] if self.hmtx else self.units_per_em

    def contours(self, gid):
        """Return list of contours, each a list of (x, y, on_curve)."""
        if gid + 1 >= len(self.loca):
            return []
        start, end = self.loca[gid], self.loca[gid + 1]
        if start == end:
            return []
        base = self.t['glyf'][0] + start
        ncont = struct.unpack('>h', self.d[base:base + 2])[0]
        if ncont <= 0:
            return []  # composite / empty: skip
        p = base + 10  # skip numberOfContours(2) + bbox xMin/yMin/xMax/yMax (8)
        end_pts = struct.unpack('>%dH' % ncont, self.d[p:p + ncont * 2]); p += ncont * 2
        ilen = struct.unpack('>H', self.d[p:p + 2])[0]; p += 2 + ilen

        npts = end_pts[-1] + 1
        flags = []
        while len(flags) < npts:
            f = self.d[p]; p += 1
            flags.append(f)
            if f & 0x08:
                rep = self.d[p]; p += 1
                flags.extend([f] * rep)

        xs = []
        x = 0
        for f in flags:
            if f & 0x02:
                dx = self.d[p]; p += 1
                x += dx if (f & 0x10) else -dx
            elif not (f & 0x10):
                x += struct.unpack('>h', self.d[p:p + 2])[0]; p += 2
            xs.append(x)
        ys = []
        y = 0
        for f in flags:
            if f & 0x04:
                dy = self.d[p]; p += 1
                y += dy if (f & 0x20) else -dy
            elif not (f & 0x20):
                y += struct.unpack('>h', self.d[p:p + 2])[0]; p += 2
            ys.append(y)

        out = []
        s = 0
        for e in end_pts:
            pts = [(xs[i], ys[i], bool(flags[i] & 1)) for i in range(s, e + 1)]
            out.append(pts)
            s = e + 1
        return out


def flatten(contours, steps=10):
    """Convert quadratic contours into closed polygons (line segments).

    Each contour yields exactly one closed loop: extra/duplicated closing
    edges would corrupt the non-zero winding count and fill counters.
    """
    segs = []
    for pts in contours:
        if not pts:
            continue
        n = len(pts)
        if all(not p[2] for p in pts):
            # all off-curve: implied on-curve points at pairwise midpoints
            pts = [((pts[i][0] + pts[(i + 1) % n][0]) / 2.0,
                    (pts[i][1] + pts[(i + 1) % n][1]) / 2.0, True) for i in range(n)]
        else:
            si = next(i for i, p in enumerate(pts) if p[2])
            pts = pts[si:] + pts[:si]

        poly = [(pts[0][0], pts[0][1])]
        cur = poly[0]
        i = 1
        while i < n:
            p = pts[i]
            if p[2]:
                cur = (p[0], p[1])
                poly.append(cur)
                i += 1
            else:
                ctrl = (p[0], p[1])
                nxt = pts[(i + 1) % n]
                if nxt[2]:
                    end = (nxt[0], nxt[1])
                    i += 2
                else:
                    end = ((p[0] + nxt[0]) / 2.0, (p[1] + nxt[1]) / 2.0)
                    i += 1
                for s in range(1, steps + 1):
                    t = s / float(steps)
                    x = (1 - t) ** 2 * cur[0] + 2 * (1 - t) * t * ctrl[0] + t * t * end[0]
                    y = (1 - t) ** 2 * cur[1] + 2 * (1 - t) * t * ctrl[1] + t * t * end[1]
                    poly.append((x, y))
                cur = end

        m = len(poly)
        for k in range(m):
            segs.append((poly[k], poly[(k + 1) % m]))
    return segs


def inside(segs, px, py):
    """Non-zero winding test."""
    w = 0
    for (x0, y0), (x1, y1) in segs:
        if y0 == y1:
            continue
        if (y0 <= py < y1) or (y1 <= py < y0):
            t = (py - y0) / (y1 - y0)
            x = x0 + t * (x1 - x0)
            if x > px:
                w += 1 if y1 > y0 else -1
    return w != 0


def render(ttf, ch, cell_w=8, cell_h=16, px_per_em=16.0, baseline=13.0, ss=4):
    gid = ttf.cmap(ord(ch))
    contours = ttf.contours(gid) if gid else []
    if not contours:
        return None, 0
    segs = flatten(contours)
    scale = px_per_em / float(ttf.units_per_em)
    segs = [((x0 * scale, baseline - y0 * scale), (x1 * scale, baseline - y1 * scale))
            for (x0, y0), (x1, y1) in segs]
    # horizontal centering based on advance width
    adv = ttf.advance(gid) * scale
    xoff = (cell_w - adv) / 2.0

    rows = []
    for y in range(cell_h):
        bits = 0
        for x in range(cell_w):
            hit = 0
            for sy in range(ss):
                for sx in range(ss):
                    px = x + (sx + 0.5) / ss - xoff
                    py = y + (sy + 0.5) / ss
                    if inside(segs, px, py):
                        hit += 1
            if hit * 2 >= ss * ss:  # >= 50% coverage
                bits |= (0x80 >> x)
        rows.append(bits)
    return rows, adv


def ascii_art(rows, cell_w=8):
    out = []
    for r in rows:
        out.append(''.join('#' if r & (0x80 >> i) else '.' for i in range(cell_w)))
    return '\n'.join(out)


def emit_c(ttf, out_path, cell_w=8, cell_h=16, px_per_em=14.0, baseline=12.0,
           first=32, last=126, source=''):
    chars = list(range(first, last + 1))
    data = []
    for code in chars:
        rows, _ = render(ttf, chr(code), cell_w, cell_h, px_per_em, baseline)
        data.append(rows if rows else [0] * cell_h)

    with open(out_path, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write('/*\n')
        fp.write(' * %dx%d ASCII bitmap font, generated from %s\n' % (cell_w, cell_h, source))
        fp.write(' * by tools/ttf2font.py -- do not edit by hand.\n')
        fp.write(' *\n')
        fp.write(' * Coverage: ASCII %d..%d (%d glyphs), %d bytes per glyph.\n'
                 % (first, last, len(chars), cell_h))
        fp.write(' * Layout: one byte per row, MSB = leftmost pixel.\n')
        fp.write(' */\n\n')
        fp.write('#include "font8x16.h"\n\n')
        fp.write('const uint8_t font8x16_data[%d][%d] = {\n' % (len(chars), cell_h))
        for code, rows in zip(chars, data):
            if 32 < code < 127:
                label = "  /* '%s' */" % chr(code)
            else:
                label = '  /* 0x%02x */' % code
            fp.write('    { %s },%s\n' % (', '.join('0x%02X' % r for r in rows), label))
        fp.write('};\n')

    nonempty = sum(1 for r in data if any(r))
    return len(chars), nonempty


if __name__ == '__main__':
    import os
    font_path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Windows\Fonts\arial.ttf'
    out = sys.argv[2] if len(sys.argv) > 2 else 'main/font8x16.c'
    px = float(sys.argv[3]) if len(sys.argv) > 3 else 14.0
    baseline = float(sys.argv[4]) if len(sys.argv) > 4 else 12.0

    f = TTF(font_path)
    print('font: %s unitsPerEm=%d ascent=%d descent=%d'
          % (os.path.basename(font_path), f.units_per_em, f.ascent, f.descent))
    n, nonempty = emit_c(f, out, px_per_em=px, baseline=baseline,
                         source=os.path.basename(font_path))
    print('wrote %s: %d glyphs (%d non-empty)' % (out, n, nonempty))
