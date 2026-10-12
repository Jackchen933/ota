/**
  ******************************************************************************
  * @file    basic_op_driver_interface.h
  * @brief   平台抽象层（BSP 的最底层）。
  *
  *          这一层是驱动与具体硬件平台之间的唯一契约：换 MCU / 换板子时，
  *          只需要重新实现这里的 5 个函数指针，st7789 驱动本体一行都不用改。
  ******************************************************************************
  */
#ifndef __BASIC_OP_DRIVER_INTERFACE_H__
#define __BASIC_OP_DRIVER_INTERFACE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 引脚电平定义，避免在驱动里出现魔法数字 */
#define ST7789_PIN_RESET   0u
#define ST7789_PIN_SET     1u

/**
  * @brief SPI + GPIO 基础操作接口
  */
typedef struct spi_gpio_driver_interface_obj
{
  /*---- need to be realized by user based on the hardware platform ----*/
  /* spi 发送，polling 方式（短数据用，无中断开销） */
  uint8_t (*pf_spi_transmit)(const uint8_t *pData, uint32_t datalength);
  /* spi 发送，dma 方式（长数据用，需要阻塞到传输完成才返回） */
  uint8_t (*pf_spi_transmit_dma)(const uint8_t *pData, uint32_t datalength);
  /* 写 reset gpio */
  uint8_t (*pf_write_reset_pin)(uint8_t pinState);
  /* 写 cs gpio */
  uint8_t (*pf_write_cs_pin)(uint8_t pinState);
  /* 写 dc gpio */
  uint8_t (*pf_write_dc_pin)(uint8_t pinState);
  /*---- need to be realized by user based on the hardware platform ----*/
} basic_op_driver_interface_t;

#ifdef __cplusplus
}
#endif

#endif /* __BASIC_OP_DRIVER_INTERFACE_H__ */
