/**
  ******************************************************************************
  * @file    font_def.h
  * @brief   点阵字库描述结构。
  *
  *          这里只定义"字库长什么样"，不提供任何字库数据。
  *          你自己准备点阵数据后，按下面的规则填一个 font_def_t 实例即可：
  *
  *            点阵数据按【字符顺序】排列，每个字符占 bytes_per_char 个字节；
  *            每个字符内部按【行优先】排列，每行占 (width + 7) / 8 个字节，
  *            高位在左（MSB 对应最左边的像素），位为 1 表示点亮前景色。
  *
  *          例：8x16 的 'A'（0x41）
  *            bytes_per_char = 16，每行 1 字节，共 16 行。
  *            字符偏移 = (0x41 - first_char) * 16
  ******************************************************************************
  */
#ifndef __FONT_DEF_H__
#define __FONT_DEF_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct font_def_obj
{
  const uint8_t *p_data;      /* 点阵数据首地址 */
  uint8_t        width;       /* 字符宽度（像素） */
  uint8_t        height;      /* 字符高度（像素） */
  uint16_t       first_char;  /* 数据中第一个字符的 ASCII 码，如 0x20 */
  uint16_t       last_char;   /* 数据中最后一个字符的 ASCII 码，如 0x7E */
  uint16_t       bytes_per_char; /* 每个字符占用的字节数 = ((width + 7) / 8) * height */
} font_def_t;

#ifdef __cplusplus
}
#endif

#endif /* __FONT_DEF_H__ */
