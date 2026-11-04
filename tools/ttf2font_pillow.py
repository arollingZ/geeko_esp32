"""Pillow 版字体生成器 —— 与 ttf2font.py 输出格式完全一致，可直接替换。

与纯几何栅格化的关键区别：Pillow 底层是 FreeType。通过
`ImageDraw.fontmode = "1"` 走 FreeType 的**单色 + hinting** 渲染路径，
笔画会被吸附到像素网格，在 8~16px 这种极小尺寸下比几何采样更均匀。

用法：
    python tools/ttf2font_pillow.py <字体> [输出.c] [像素高度]

选项：
    --art            在终端打印若干字符的 ASCII 预览
    --png <路径>     把整张字库导出成 PNG，便于肉眼比对
    --no-hint        改用灰度抗锯齿路径（fontmode="L"），便于对比
    --weight <名称>  可变字体的字重，如 Regular / Medium / DemiLight
"""
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit(
        'Pillow 未安装。请执行：\n'
        '    python -m pip install pillow -i https://pypi.tuna.tsinghua.edu.cn/simple'
    )

CELL_W, CELL_H = 8, 16
FIRST, LAST = 32, 126
BASELINE = 12          # 单元格内的基线 y 坐标（数字顶部约在 y=2）


def render_glyph(font, ch, hinted=True):
    """在 CELL_W x CELL_H 网格内渲染一个字符，返回 16 字节位图行。"""
    wide = CELL_W * 3                       # 留余量，稍后水平居中裁剪
    img = Image.new('1' if hinted else 'L', (wide, CELL_H), 0)
    draw = ImageDraw.Draw(img)
    # fontmode="1" 触发 FreeType 单色渲染（含 hinting）；"L" 为灰度抗锯齿
    draw.fontmode = '1' if hinted else 'L'
    try:
        draw.text((CELL_W, BASELINE), ch, font=font, fill=1, anchor='ls')
    except ValueError:
        # 个别字体不支持 anchor，退回到默认左上角基线
        draw.text((CELL_W, BASELINE), ch, font=font, fill=1)

    bbox = img.getbbox()
    bits = [0] * CELL_H
    if not bbox:
        return bits, 0

    x0, y0, x1, y1 = bbox
    shift = (CELL_W - (x1 - x0)) // 2 - x0     # 水平居中
    px = img.load()
    for y in range(max(0, y0), min(CELL_H, y1)):
        row = 0
        for x in range(CELL_W):
            sx = x - shift
            if 0 <= sx < wide and px[sx, y]:
                row |= (0x80 >> x)
        bits[y] = row
    return bits, x1 - x0


def emit_c(font, out_path, source='', hinted=True):
    chars = list(range(FIRST, LAST + 1))
    data = []
    for code in chars:
        bits, _ = render_glyph(font, chr(code), hinted)
        data.append(bits)

    mode = 'Pillow/FreeType, monochrome+hinted' if hinted else 'Pillow/FreeType, grayscale'
    with open(out_path, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write('/*\n')
        fp.write(' * %dx%d ASCII bitmap font, generated from %s\n' % (CELL_W, CELL_H, source))
        fp.write(' * by tools/ttf2font_pillow.py (%s) -- do not edit by hand.\n' % mode)
        fp.write(' *\n')
        fp.write(' * Coverage: ASCII %d..%d (%d glyphs), %d bytes per glyph.\n'
                 % (FIRST, LAST, len(chars), CELL_H))
        fp.write(' * Layout: one byte per row, MSB = leftmost pixel.\n')
        fp.write(' */\n\n')
        fp.write('#include "font8x16.h"\n\n')
        fp.write('const uint8_t font8x16_data[%d][%d] = {\n' % (len(chars), CELL_H))
        for code, rows in zip(chars, data):
            label = ("  /* '%s' */" % chr(code)) if 32 < code < 127 else ('  /* 0x%02x */' % code)
            fp.write('    { %s },%s\n' % (', '.join('0x%02X' % r for r in rows), label))
        fp.write('};\n')
    return data


def ascii_art(bits):
    return [''.join('#' if b & (0x80 >> i) else '.' for i in range(CELL_W)) for b in bits]


def dump_png(data, path, cols=16, scale=3):
    rows = (len(data) + cols - 1) // cols
    cw, chh = CELL_W * scale + 3, CELL_H * scale + 3
    img = Image.new('L', (cols * cw, rows * chh), 20)
    px = img.load()
    for i, bits in enumerate(data):
        cx, cy = (i % cols) * cw + 1, (i // cols) * chh + 1
        for y in range(CELL_H):
            for x in range(CELL_W):
                if bits[y] & (0x80 >> x):
                    for dy in range(scale):
                        for dx in range(scale):
                            px[cx + x * scale + dx, cy + y * scale + dy] = 255
    img.save(path)
    print('png written:', path)


if __name__ == '__main__':
    # 注意：选项值（如 --weight Regular）不带 "--" 前缀，不能混进位置参数，
    # 所以先按选项消费，再取剩下的作为位置参数。
    args = sys.argv[1:]
    flags, positional = [], []
    i = 0
    while i < len(args):
        a = args[i]
        if a in ('--weight', '--png'):
            flags.extend([a, args[i + 1] if i + 1 < len(args) else ''])
            i += 2
        elif a.startswith('--'):
            flags.append(a)
            i += 1
        else:
            positional.append(a)
            i += 1

    font_path = positional[0] if positional else r'C:\Windows\Fonts\NotoSansSC-VF.ttf'
    out = positional[1] if len(positional) > 1 else 'main/font8x16.c'
    size = int(positional[2]) if len(positional) > 2 else 14

    hinted = '--no-hint' not in flags
    font = ImageFont.truetype(font_path, size)

    # 可变字体（如 Noto Sans SC VF）支持调整字重。注意 Noto 的默认实例
    # 是 weight=100 (Thin)，不显式设置会导致小字号笔画残缺，必须指定。
    weight = None
    if '--weight' in flags:
        weight = flags[flags.index('--weight') + 1]
    if weight:
        try:
            font.set_variation_by_name(weight)
        except Exception as e:
            print('warning: cannot set weight %r (%s), using default' % (weight, e))

    print('font=%s size=%d hinted=%s weight=%s'
          % (os.path.basename(font_path), size, hinted, weight or 'default'))

    data = emit_c(font, out, source=os.path.basename(font_path), hinted=hinted)
    print('wrote %s: %d glyphs' % (out, len(data)))

    if '--art' in flags:
        for ch in '0812:9ABab':
            print('--- %r ---' % ch)
            for line in ascii_art(data[ord(ch) - FIRST])[1:14]:
                print(line)

    if '--png' in flags:
        i = flags.index('--png')
        dump_png(data, flags[i + 1] if i + 1 < len(flags) else 'font_pillow.png')
