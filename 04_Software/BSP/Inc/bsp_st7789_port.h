/**
  ******************************************************************************
  * @file    bsp_st7789_port.h
  * @brief   本工程（STM32F411CEU6 + HAL）的板级适配层声明。
  *
  *          这是"移植"时唯一需要重写的文件：
  *          bsp_st7789.c 是平台无关的驱动本体，bsp_st7789_port.c 才是板级实现。
  ******************************************************************************
  */
#ifndef __BSP_ST7789_PORT_H__
#define __BSP_ST7789_PORT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "bsp_st7789.h"

/* polling 发送的超时（毫秒） */
#define ST7789_SPI_TIMEOUT_MS   100u
/* 一次 DMA 传输等待完成的上限（毫秒） */
#define ST7789_DMA_TIMEOUT_MS   1000u

/**
  * @brief  板级初始化：绑定 HAL 实现并执行 ST7789 初始化序列。
  *         调用前必须先完成 MX_GPIO_Init / MX_DMA_Init / MX_SPI2_Init。
  * @retval 驱动实例指针；失败返回 NULL
  */
bsp_st7789_driver_t *bsp_st7789_port_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_ST7789_PORT_H__ */
