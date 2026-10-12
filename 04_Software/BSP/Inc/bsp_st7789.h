/**
  ******************************************************************************
  * @file    bsp_st7789.h
  * @brief   st7789 驱动对外接口。
  *
  *          典型用法（见 Core/Src/main.c 的自测代码）：
  *
  *            bsp_st7789_driver_t *lcd = bsp_st7789_port_init();
  *            lcd->pf_fill_color(lcd, ST7789_RED);
  ******************************************************************************
  */
#ifndef __BSP_ST7789_H__
#define __BSP_ST7789_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "st7789_interface.h"

/* ---------------------------- 屏幕几何参数 ---------------------------- */
/* 这里填的是**可见区**的像素数，不是 ST7789 芯片 GRAM 的尺寸。
   ST7789 的 GRAM 固定是 240x320，屏厂把它切出中间一段当可视区，
   剩下的靠 ST7789_Y_SHIFT 偏移掉。

   本板实测：1.69 寸 240x280 屏，可视区对应 GRAM 的 y=20~299。
   量化方法见 Core/Src/main.c 的 LCD_PROBE_SECONDS。 */
#define ST7789_WIDTH      240u   /* 屏幕横向可见像素数 */
#define ST7789_HEIGHT     280u   /* 屏幕纵向可见像素数 */

/* 可见区在 GRAM 中的起始偏移。
   240x320 的屏填 0；240x280 的屏通常填 20（本板就是）。
   刷出来的图像整体偏移、上下被切掉一截、边缘有条纹时，优先调这两个值。 */
#define ST7789_X_SHIFT    0u
#define ST7789_Y_SHIFT    20u

/* 条带缓冲的高度（行数）。只被 pf_fill / pf_fill_color 这两个"纯色填充"用。
   占用 RAM = ST7789_WIDTH * ST7789_HOR_LEN * 2 字节。

   接了 LVGL 之后这个值被调小了：刷屏改由 LVGL 的显示缓冲负责
   （见 UI/Src/lv_port_disp.c），BSP 这里的 60KB 缓冲纯属白占内存，
   所以在保留 pf_fill 可用的前提下压到最小。
   当前 240*8*2 = 3840 字节。

   如果以后要脱离 LVGL、直接用 BSP 刷图片，再把它调回 128
   （240*128*2 = 61440 字节，仍在 DMA 单次 65535 字节上限内）。 */
#define ST7789_HOR_LEN    8u

/* DMA 单次传输上限（DMA 的 NDTR 是 16 位） */
#define ST7789_DMA_MAX_CHUNK  65535u
/* 低于这个字节数就降级为 polling —— 开 DMA 的开销比直接发的开销还大 */
#define ST7789_DMA_MIN_SIZE   16u

/* ------------------------------- 颜色 -------------------------------- */
#define ST7789_BLACK    0x0000u
#define ST7789_WHITE    0xFFFFu
#define ST7789_RED      0xF800u
#define ST7789_GREEN    0x07E0u
#define ST7789_BLUE     0x001Fu
#define ST7789_YELLOW   0xFFE0u
#define ST7789_CYAN     0x07FFu
#define ST7789_MAGENTA  0xF81Fu
#define ST7789_GRAY     0x8410u
#define ST7789_ORANGE   0xFD20u

/* --------------------------- 未实现指针的断言 -------------------------- */
/**
  * @brief 调用未实现的功能时进入这里，方便在调试器里一眼定位。
  *        实现放在 bsp_st7789.c，用 __LINE__ 记录出错位置。
  */
void bsp_st7789_unimplemented_handler(uint32_t line);

/* 出错行号。死循环之后在调试器的 Watch 窗口里看这个变量即可定位。 */
extern volatile uint32_t g_st7789_unimplemented_line;

#define ST7789_ASSERT_PTR(p)                                                  \
  do {                                                                        \
    if ((p) == NULL) { bsp_st7789_unimplemented_handler(__LINE__); }          \
  } while (0)

/* ------------------------------ 构造接口 ------------------------------ */
/**
  * @brief  构造驱动实例（只做绑定，不碰硬件）。
  * @param  basic_op 平台基础操作，不能为空
  * @param  timebase 时基接口，不能为空
  * @param  yield    让出接口，不能为空（裸机可传空实现）
  * @retval 驱动实例指针；失败返回 NULL
  */
bsp_st7789_driver_t *bsp_st7789_create(basic_op_driver_interface_t *basic_op,
                                       st7789_timebase_interface_t *timebase,
                                       st7789_yield_interface_t *yield);

/**
  * @brief  取当前驱动实例（bsp_st7789_create 之后有效）。
  */
bsp_st7789_driver_t *bsp_st7789_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_ST7789_H__ */
