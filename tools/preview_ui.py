"""Preview what the LCD UI will look like, by reusing the generated font.

Parses main/font8x16.c and renders the two UI lines into ASCII art so the
layout/characters can be checked without flashing the board.
"""
import re
import sys

FB_W, FB_H = 160, 80
CHAR_W, CHAR_H = 8, 16
TIME_Y, SSID_Y = 18, 46


def load_font(path):
    src = open(path, encoding='utf-8').read()
    body = src.split('= {', 1)[1]
    glyphs = []
    for m in re.finditer(r'\{\s*((?:0x[0-9A-Fa-f]{2}\s*,\s*){15}0x[0-9A-Fa-f]{2})\s*\}', body):
        vals = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{2}', m.group(1))]
        glyphs.append(vals)
    return glyphs


def draw_text(fb, x, y, s, glyphs, on='#'):
    for i, ch in enumerate(s):
        idx = ord(ch) - 32
        if not (0 <= idx < len(glyphs)):
            idx = ord('?') - 32
        g = glyphs[idx]
        for r in range(CHAR_H):
            for c in range(CHAR_W):
                if g[r] & (0x80 >> c):
                    px, py = x + i * CHAR_W + c, y + r
                    if 0 <= px < FB_W and 0 <= py < FB_H:
                        fb[py][px] = on


def center_x(s):
    return (FB_W - len(s) * CHAR_W) // 2


def show(fb, y0, y1, x0=0, x1=FB_W):
    for y in range(y0, y1):
        print(''.join(fb[y][x0:x1]))


if __name__ == '__main__':
    font = sys.argv[1] if len(sys.argv) > 1 else 'main/font8x16.c'
    glyphs = load_font(font)
    print('glyphs loaded:', len(glyphs))
    assert len(glyphs) == 95, 'expected 95 glyphs'

    for line1, line2 in [
        ('2026-11-04 16:13:26', 'MyHomeWiFi'),
        ('2026-12-31 23:59:59', 'ChinaNet-5G-8A3F'),
        ('-- waiting for NTP --', 'no wifi config'),
    ]:
        fb = [['.'] * FB_W for _ in range(FB_H)]
        draw_text(fb, center_x(line1), TIME_Y, line1, glyphs)
        draw_text(fb, center_x(line2), SSID_Y, line2, glyphs)
        print('=' * FB_W)
        print('line1: %-24s line2: %s' % (line1, line2))
        show(fb, TIME_Y - 1, TIME_Y + CHAR_H + 1)
        print('-' * FB_W)
        show(fb, SSID_Y - 1, SSID_Y + CHAR_H + 1)
