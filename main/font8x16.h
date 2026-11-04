/*
 * 8x16 ASCII 点阵字库（由 tools/ttf2font.py 从 Arial 栅格化生成，勿手工修改）
 *
 * 覆盖 ASCII 32..126，每个字符 16 字节，每字节一行，MSB 为最左像素。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT8X16_W      8
#define FONT8X16_H      16
#define FONT8X16_FIRST  32
#define FONT8X16_LAST   126

extern const uint8_t font8x16_data[FONT8X16_LAST - FONT8X16_FIRST + 1][FONT8X16_H];

#ifdef __cplusplus
}
#endif
