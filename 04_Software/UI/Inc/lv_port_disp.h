/**
  ******************************************************************************
  * @file    lv_port_disp.h
  * @brief   LVGL 与 ST7789 之间的显示桥。
  *
  *          职责只有一件事：把 LVGL 渲染出来的像素，通过 BSP 送到屏幕上。
  *          不掺任何业务逻辑。
  ******************************************************************************
  */
#ifndef __LV_PORT_DISP_H__
#define __LV_PORT_DISP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* 屏幕分辨率，必须和 BSP/Inc/bsp_st7789.h 里的 ST7789_WIDTH/HEIGHT 一致。
   本板是 1.69 寸 240x280（不是 240x320）。
   两个值对不上的话，LVGL 会按错误的高度排版，屏幕底部被切掉一截。 */
#define LV_PORT_DISP_HOR_RES   240
#define LV_PORT_DISP_VER_RES   280

/**
  * @brief  把 LVGL 接到 ST7789 上。
  *
  *          【调用前提】必须先执行过 bsp_st7789_port_init()，
  *          本函数内部通过 bsp_st7789_get_instance() 取驱动实例。
  *
  *          【做了什么】
  *            1. lv_display_create + 设置分辨率
  *            2. 指定颜色格式 RGB565_SWAPPED（见 .c 里的说明，很关键）
  *            3. 挂上 flush 回调
  *            4. lv_display_set_default，让 lv_screen_active() 能拿到这块屏
  */
void lv_port_disp_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __LV_PORT_DISP_H__ */
