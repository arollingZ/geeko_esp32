"""并排对比两版字库（几何栅格化 vs Pillow/FreeType）。

解析两个 .c 字库文件（格式相同），逐字符打印 ASCII art 并排结果，
并统计有多少字符的像素不同。不需要 Pillow，纯文本输出。

用法：
    python tools/compare_fonts.py main/font8x16.c font_pillow.c [字符...]
"""
import re
import sys

CELL_W, CELL_H = 8, 16


def load_font(path):
    src = open(path, encoding='utf-8').read()
    body = src.split('= {', 1)[1]
    glyphs = []
    for m in re.finditer(r'\{\s*((?:0x[0-9A-Fa-f]{2}\s*,\s*){15}0x[0-9A-Fa-f]{2})\s*\}', body):
        vals = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{2}', m.group(1))]
        glyphs.append(vals)
    return glyphs


def art(bits):
    return [''.join('#' if b & (0x80 >> i) else '.' for i in range(CELL_W)) for b in bits]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    a = load_font(sys.argv[1])
    b = load_font(sys.argv[2])
    chars = sys.argv[3] if len(sys.argv) > 3 else '0123456789:'
    print('A = %s (%d glyphs)' % (sys.argv[1], len(a)))
    print('B = %s (%d glyphs)' % (sys.argv[2], len(b)))

    diff_glyphs = 0
    diff_pixels = 0
    for i in range(min(len(a), len(b))):
        bits_a, bits_b = a[i], b[i]
        d = sum(bin(x ^ y).count('1') for x, y in zip(bits_a, bits_b))
        if d:
            diff_glyphs += 1
            diff_pixels += d
    n = min(len(a), len(b))
    print('\n差异统计：%d/%d 个字符有差异，共 %d 个像素不同（平均 %.1f 像素/字符）'
          % (diff_glyphs, n, diff_pixels, diff_pixels / float(n) if n else 0))

    for ch in chars:
        idx = ord(ch) - 32
        if not (0 <= idx < n):
            continue
        la, lb = art(a[idx]), art(b[idx])
        d = sum(bin(x ^ y).count('1') for x, y in zip(a[idx], b[idx]))
        print('\n===== %r  (%d px 不同) =====' % (ch, d))
        for i in range(2, 13):
            mark = '   ' if la[i] == lb[i] else '  <'
            print('%s   %s%s' % (la[i], lb[i], mark))


if __name__ == '__main__':
    main()
