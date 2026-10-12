/**
  ******************************************************************************
  * @file    st7789_interface.h
  * @brief   st7789 驱动的接口与实例定义。
  *
  *          本文件对应移植文档的第 2 / 3 / 4 节：
  *            第 2 节  st7789_spi_driver_interface_t  把平台 spi 操作翻译成"命令/数据"
  *            第 3 节  timebase / yield 接口
  *            第 4 节  bsp_st7789_driver_obj          驱动实例（函数指针表）
  *
  *          设计约定：结构体里的函数指针统一用 `void * const xxx_instance`
  *          接第一个参数（与移植文档一致），驱动内部实现时再向下转型。
  *          这样赋值时不需要做函数指针类型转换，避免未定义行为。
  ******************************************************************************
  */
#ifndef __ST7789_INTERFACE_H__
#define __ST7789_INTERFACE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "basic_op_driver_interface.h"
#include "font_def.h"

/**
  * @brief 将用户的 spi 操作转化为向 lcd 发送数据和命令的操作
  */
typedef struct st7789_spi_driver_interface_obj
{
  /* 通过 spi dma 向 st7789 发送刷屏数据的内容（长数据，大块传输） */
  uint8_t (*pf_st7789_write_data)(void * const driver_instance,
                                  const uint8_t *pData, uint32_t data_length);
  /* 少量数据，通过 polling 方式发送 */
  uint8_t (*pf_st7789_write_simple_data)(void * const driver_instance,
                                         const uint8_t *pData, uint32_t data_length);
  /* 向 st7789 发送命令（dc 拉低） */
  uint8_t (*pf_st7789_write_command)(void * const driver_instance,
                                     const uint8_t *pCmd, uint32_t cmd_length);
} st7789_spi_driver_interface_t;

/**
  * @brief 时基接口：可以传入 HAL_GetTick / HAL_Delay
  */
typedef struct st7789_timebase_interface_obj
{
  /* 返回毫秒计数 */
  uint32_t (*pf_get_tick_count)(void);
  /* 阻塞延时，单位毫秒（裸机传入 HAL_Delay 即可） */
  void     (*pf_delay_no_os)(uint32_t delay);
} st7789_timebase_interface_t;

/**
  * @brief 让出接口：可以传入 vTaskDelay
  */
typedef struct st7789_yield_interface_obj
{
  /* 让出 CPU；裸机下实现为空函数即可 */
  void (*pf_rtos_yield)(const uint32_t yield_cnt);
} st7789_yield_interface_t;

/**
  * @brief 驱动实例：既持有各层接口，也持有对外的功能函数指针表。
  *
  *        注意：本轮只实现"最小可用"的 6 个功能，其余指针为 NULL。
  *        调用未实现的指针会进入 Error_Handler（见 bsp_st7789.h 的断言宏）。
  */
typedef struct bsp_st7789_driver_obj
{
  basic_op_driver_interface_t   *p_st7789_basic_operation;
  st7789_spi_driver_interface_t *p_st7789_spi_instance;
  st7789_timebase_interface_t   *p_timebase_instance;
  st7789_yield_interface_t      *p_yield_instance;

  /* ---------- Basic functions. ---------- */
  /* 将屏幕配置到默认状态 */
  uint8_t (*pf_st7789_init)(void * const st7789_instance);
  /* 配置屏幕方向 */
  uint8_t (*pf_set_direction)(void * const st7789_instance, uint8_t direction);
  /* 将屏幕所有像素刷成指定颜色 */
  uint8_t (*pf_fill_color)(void * const st7789_instance, uint16_t color);
  /* 将屏幕某像素刷成指定颜色 */
  uint8_t (*pf_draw_pixel)(void * const st7789_instance,
                           uint16_t x, uint16_t y, uint16_t color);
  /* 将屏幕某区域刷成指定颜色 */
  uint8_t (*pf_fill)(void * const st7789_instance,
                     uint16_t xSta, uint16_t ySta,
                     uint16_t xEnd, uint16_t yEnd, uint16_t color);
  /* 将屏幕某像素块刷成指定颜色 */
  uint8_t (*pf_draw_pixel_4px)(void * const st7789_instance,
                               uint16_t x, uint16_t y, uint16_t color);
  /* 指定刷屏范围 */
  uint8_t (*pf_set_addr_window)(void * const st7789_instance,
                                uint16_t x0, uint16_t y0,
                                uint16_t x1, uint16_t y1);

  /* ---------- Graphical functions. ---------- */
  /* 画线 */
  uint8_t (*pf_draw_line)(void * const st7789_instance,
                          uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                          uint16_t color);
  /* 画矩形，无填充 */
  uint8_t (*pf_draw_rectangle)(void * const st7789_instance,
                               uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                               uint16_t color);
  /* 画圆，无填充 */
  uint8_t (*pf_draw_circle)(void * const st7789_instance,
                            uint16_t x0, uint16_t y0, uint8_t r, uint16_t color);
  /* 显示预存的图片 */
  uint8_t (*pf_draw_image)(void * const st7789_instance,
                           uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           const uint16_t *data);
  uint8_t (*pf_invert_colors)(void * const st7789_instance, uint16_t invert);

  /* ---------- Text functions. ---------- */
  /* 输出字符 */
  uint8_t (*pf_write_char)(void * const st7789_instance,
                           uint16_t x, uint16_t y, uint16_t ch,
                           const font_def_t font, uint16_t color, uint16_t bgcolor);
  /* 输出字符串 */
  uint8_t (*pf_write_string)(void * const st7789_instance,
                             uint16_t x, uint16_t y, const char *str,
                             const font_def_t font, uint16_t color, uint16_t bgcolor);

  /* ---------- Extended Graphical functions. ---------- */
  /* 画矩形，并指定填充颜色 */
  uint8_t (*pf_draw_filled_rectangle)(void * const st7789_instance,
                                      uint16_t x, uint16_t y,
                                      uint16_t w, uint16_t h, uint16_t color);
  /* 画三角形，无填充 */
  uint8_t (*pf_draw_triangle)(void * const st7789_instance,
                              uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                              uint16_t x3, uint16_t y3, uint16_t color);
  /* 画三角形，并指定填充颜色 */
  uint8_t (*pf_draw_filled_triangle)(void * const st7789_instance,
                                     uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                                     uint16_t x3, uint16_t y3, uint16_t color);
  /* 画圆形，并指定填充颜色 */
  uint8_t (*pf_draw_filled_circle)(void * const st7789_instance,
                                   int16_t x0, int16_t y0, int16_t r, uint16_t color);

  /* ---------- Command functions. ---------- */
  uint8_t (*pf_tear_effect)(void * const st7789_instance, uint8_t tear);

  /* ---------- test function. ---------- */
  /* 测试用例，包含以上所有函数的测试集合 */
  uint8_t (*pf_test_your_device)(void * const st7789_instance);
} bsp_st7789_driver_t;

#ifdef __cplusplus
}
#endif

#endif /* __ST7789_INTERFACE_H__ */
