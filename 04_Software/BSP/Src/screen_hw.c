/**
  ******************************************************************************
  * @file    screen_hw.c
  * @brief   屏幕专用硬件上电实现（SPI2 25MHz Mode3 + DMA1_Stream4 + GPIO）。
  *
  *          参数逐项抄自源工程 jiemian1 的 MX_SPI2_Init / HAL_SPI_MspInit /
  *          MX_DMA_Init。这里不走 HAL_SPI_MspInit（CubeMX 生成的那个只认
  *          SPI1），时钟/GPIO/DMA 全部在本文件里自己配，自包含。
  ******************************************************************************
  */
#include "screen_hw.h"

#include "stm32f4xx_hal.h"
#include "spi.h"
#include "dma.h"

/* 引脚定义（与源工程 main.h 的标签一致） */
#define SCREEN_CS_Pin        GPIO_PIN_8
#define SCREEN_CS_Port       GPIOA
#define SCREEN_RESET_Pin     GPIO_PIN_11
#define SCREEN_RESET_Port    GPIOA
#define SCREEN_DC_Pin        GPIO_PIN_14
#define SCREEN_DC_Port       GPIOB
#define SCREEN_SCK_Pin       GPIO_PIN_13
#define SCREEN_MOSI_Pin      GPIO_PIN_15

SPI_HandleTypeDef hspi2;
DMA_HandleTypeDef hdma_spi2_tx;

/**
  * @brief  控制脚：CS（空闲高）、DC、RESET（空闲高）。
  *         速度 VERY_HIGH 与源工程一致（CS/DC 在 25MHz 下边沿要跟得上）。
  */
static void screen_hw_gpio_init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* 上电先给 CS/RESET 高电平，再配方向，避免复位脉冲毛刺 */
  HAL_GPIO_WritePin(SCREEN_CS_Port, SCREEN_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(SCREEN_RESET_Port, SCREEN_RESET_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(SCREEN_DC_Port, SCREEN_DC_Pin, GPIO_PIN_RESET);

  gpio.Pin   = SCREEN_CS_Pin | SCREEN_RESET_Pin;
  gpio.Mode  = GPIO_MODE_OUTPUT_PP;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOA, &gpio);

  gpio.Pin   = SCREEN_DC_Pin;
  HAL_GPIO_Init(GPIOB, &gpio);
}

/**
  * @brief  SPI2 外设 + SCK/MOSI 复用脚 + TX DMA。
  *         Mode3（CPOL=High / CPHA=2Edge）、25MB/s（APB1 50MHz / 2）。
  *         DMA1_Stream4 CH0、NORMAL、FIFO 关 —— 与源工程一致。
  */
void screen_hw_init(void)
{
  screen_hw_gpio_init();

  __HAL_RCC_DMA1_CLK_ENABLE();

  hdma_spi2_tx.Instance                 = DMA1_Stream4;
  hdma_spi2_tx.Init.Channel             = DMA_CHANNEL_0;
  hdma_spi2_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
  hdma_spi2_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_spi2_tx.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_spi2_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_spi2_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  hdma_spi2_tx.Init.Mode                = DMA_NORMAL;
  hdma_spi2_tx.Init.Priority            = DMA_PRIORITY_LOW;
  hdma_spi2_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
  (void)HAL_DMA_Init(&hdma_spi2_tx);

  /* [关键] 把 DMA 句柄挂到 SPI2 的 TX 通道(源工程由 CubeMX 的
     HAL_SPI_MspInit 完成)。漏了这步 HAL_SPI_Transmit_DMA 直接报错,
     port_init 返回 NULL, main 进 Error_Handler 死循环 */
  __HAL_LINKDMA(&hspi2, hdmatx, hdma_spi2_tx);

  /* DMA 完成中断：bsp_st7789_port.c 在 TxCpltCallback 里置完成标志 */
  HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 5u, 0u);
  HAL_NVIC_EnableIRQ(DMA1_Stream4_IRQn);

  __HAL_RCC_SPI2_CLK_ENABLE();

  hspi2.Instance               = SPI2;
  hspi2.Init.Mode              = SPI_MODE_MASTER;
  hspi2.Init.Direction         = SPI_DIRECTION_2LINES;
  hspi2.Init.DataSize          = SPI_DATASIZE_8BIT;
  hspi2.Init.CLKPolarity       = SPI_POLARITY_HIGH;
  hspi2.Init.CLKPhase          = SPI_PHASE_2EDGE;
  hspi2.Init.NSS               = SPI_NSS_SOFT;
  hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  hspi2.Init.FirstBit          = SPI_FIRSTBIT_MSB;
  hspi2.Init.TIMode            = SPI_TIMODE_DISABLE;
  hspi2.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
  hspi2.Init.CRCPolynomial     = 10u;
  if (HAL_SPI_Init(&hspi2) != HAL_OK)
  {
    Error_Handler();
  }

  /* SCK/MOSI 复用（AF5），放在 HAL_SPI_Init 之后与源工程 MspInit 等效 */
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin       = SCREEN_SCK_Pin | SCREEN_MOSI_Pin;
  gpio.Mode      = GPIO_MODE_AF_PP;
  gpio.Pull      = GPIO_NOPULL;
  gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = GPIO_AF5_SPI2;
  HAL_GPIO_Init(GPIOB, &gpio);
}

/**
  * @brief  SPI2 TX DMA(DMA1_Stream4) 传输完成中断入口。
  *         源工程里由 CubeMX 生成在 stm32f4xx_it.c；本工程没开 CubeMX 的
  *         这个流, 必须在这里补上, 否则第一次 DMA 刷屏完成时 CPU 跳进
  *         Default_Handler 死循环, 程序整体冻结。
  */
void DMA1_Stream4_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_spi2_tx);
}
