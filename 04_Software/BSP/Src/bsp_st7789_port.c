/**
  ******************************************************************************
  * @file    bsp_st7789_port.c
  * @brief   STM32F411CEU6 + HAL 的板级适配实现。
  *
  *          本文件把 HAL 的 SPI / GPIO / SysTick 操作接到
  *          basic_op_driver_interface_t 和 timebase / yield 接口上。
  *
  *          接线（与 Core/Src/gpio.c、spi.c 的 CubeMX 配置一致）：
  *            PB13 -> SPI2_SCK   (AF5，不是默认的 PB10)
  *            PB15 -> SPI2_MOSI
  *            PA8  -> SPI_CS     (GPIO 输出)
  *            PA11 -> SPI_RESET  (GPIO 输出)
  *            PB14 -> SPI_DC     (GPIO 输出)
  *
  *          SPI2 参数：Master / 8bit / CPOL=High / CPHA=2Edge (Mode 3) / 25MHz
  *          DMA：DMA1_Stream4 CH0 -> SPI2_TX
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "bsp_st7789_port.h"

#include "main.h"
#include "spi.h"
#include "gpio.h"
#include "dma.h"

/* Private variables ---------------------------------------------------------*/

/* DMA 完成标志，由 HAL_SPI_TxCpltCallback / HAL_SPI_ErrorCallback 置位 */
static volatile uint8_t s_dma_done  = 1u;
static volatile uint8_t s_dma_error = 0u;

/* 三层接口实例，绑定见 bsp_st7789_port_init */
static basic_op_driver_interface_t   s_basic_op;
static st7789_timebase_interface_t   s_timebase;
static st7789_yield_interface_t      s_yield;

/* Private function prototypes -----------------------------------------------*/

static uint8_t port_spi_transmit(const uint8_t *pData, uint32_t datalength);
static uint8_t port_spi_transmit_dma(const uint8_t *pData, uint32_t datalength);
static uint8_t port_write_reset_pin(uint8_t pinState);
static uint8_t port_write_cs_pin(uint8_t pinState);
static uint8_t port_write_dc_pin(uint8_t pinState);
static uint32_t port_get_tick_count(void);
static void     port_delay_no_os(uint32_t delay);
static void     port_rtos_yield(const uint32_t yield_cnt);

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  polling 方式发送。用于短数据（命令、地址窗口、单像素）。
  */
static uint8_t port_spi_transmit(const uint8_t *pData, uint32_t datalength)
{
  HAL_StatusTypeDef st;

  if ((pData == NULL) || (datalength == 0u) || (datalength > 0xFFFFu))
  {
    return 1u;
  }

  /* HAL 的原型是非 const 的，这里只是去掉 const 限定，不会修改内容 */
  st = HAL_SPI_Transmit(&hspi2, (uint8_t *)pData, (uint16_t)datalength,
                        ST7789_SPI_TIMEOUT_MS);

  return (st == HAL_OK) ? 0u : 1u;
}

/**
  * @brief  DMA 方式发送，阻塞到传输结束才返回。
  *
  *         HAL_SPI_Transmit_DMA 是异步的，立刻就会返回；
  *         所以必须在这里等到完成回调置位，驱动上层才能认为"数据已发完"。
  */
static uint8_t port_spi_transmit_dma(const uint8_t *pData, uint32_t datalength)
{
  uint32_t start;

  if ((pData == NULL) || (datalength == 0u) || (datalength > 0xFFFFu))
  {
    return 1u;
  }

  s_dma_error = 0u;
  s_dma_done  = 0u;

  if (HAL_SPI_Transmit_DMA(&hspi2, (uint8_t *)pData, (uint16_t)datalength) != HAL_OK)
  {
    s_dma_done = 1u;
    return 1u;
  }

  start = HAL_GetTick();
  while (s_dma_done == 0u)
  {
    if ((HAL_GetTick() - start) > ST7789_DMA_TIMEOUT_MS)
    {
      /* 超时兜底：中止传输并把 SPI 状态复位，否则后面所有发送都会失败 */
      (void)HAL_SPI_Abort(&hspi2);
      s_dma_done = 1u;
      return 2u;
    }
  }

  return s_dma_error;
}

/**
  * @brief  写 reset 引脚
  */
static uint8_t port_write_reset_pin(uint8_t pinState)
{
  HAL_GPIO_WritePin(SPI_RESET_GPIO_Port, SPI_RESET_Pin,
                    (pinState != ST7789_PIN_RESET) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  return 0u;
}

/**
  * @brief  写 cs 引脚
  */
static uint8_t port_write_cs_pin(uint8_t pinState)
{
  HAL_GPIO_WritePin(SPI_CS_GPIO_Port, SPI_CS_Pin,
                    (pinState != ST7789_PIN_RESET) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  return 0u;
}

/**
  * @brief  写 dc 引脚
  */
static uint8_t port_write_dc_pin(uint8_t pinState)
{
  HAL_GPIO_WritePin(SPI_DC_GPIO_Port, SPI_DC_Pin,
                    (pinState != ST7789_PIN_RESET) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  return 0u;
}

/**
  * @brief  毫秒计数
  */
static uint32_t port_get_tick_count(void)
{
  return HAL_GetTick();
}

/**
  * @brief  阻塞延时
  */
static void port_delay_no_os(uint32_t delay)
{
  HAL_Delay(delay);
}

/**
  * @brief  让出 CPU。裸机工程没有调度器，空实现即可。
  *         接了 FreeRTOS 之后换成 vTaskDelay(yield_cnt) 就能让 DMA 等待期间
  *         其他任务继续跑。
  */
static void port_rtos_yield(const uint32_t yield_cnt)
{
  (void)yield_cnt;
}

/* Exported functions --------------------------------------------------------*/

/**
  * @brief  SPI 发送完成回调（覆盖 HAL 的弱定义）
  */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
  if (hspi->Instance == SPI2)
  {
    s_dma_done = 1u;
  }
}

/**
  * @brief  SPI 错误回调（覆盖 HAL 的弱定义）
  */
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
  if (hspi->Instance == SPI2)
  {
    s_dma_error = 1u;
    s_dma_done  = 1u;
  }
}

/**
  * @brief  板级初始化
  */
bsp_st7789_driver_t *bsp_st7789_port_init(void)
{
  bsp_st7789_driver_t *drv;

  s_basic_op.pf_spi_transmit     = port_spi_transmit;
  s_basic_op.pf_spi_transmit_dma = port_spi_transmit_dma;
  s_basic_op.pf_write_reset_pin  = port_write_reset_pin;
  s_basic_op.pf_write_cs_pin     = port_write_cs_pin;
  s_basic_op.pf_write_dc_pin     = port_write_dc_pin;

  s_timebase.pf_get_tick_count = port_get_tick_count;
  s_timebase.pf_delay_no_os    = port_delay_no_os;

  s_yield.pf_rtos_yield = port_rtos_yield;

  drv = bsp_st7789_create(&s_basic_op, &s_timebase, &s_yield);
  if (drv == NULL)
  {
    return NULL;
  }

  /* MX_GPIO_Init 把 CS 初始化成了低电平，这里先抬回空闲态 */
  (void)port_write_cs_pin(ST7789_PIN_SET);

  if (drv->pf_st7789_init(drv) != 0u)
  {
    return NULL;
  }

  return drv;
}
