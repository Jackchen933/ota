/**
  ******************************************************************************
  * @file    screen_hw.h
  * @brief   屏幕（SPI2/ST7789）专用硬件上电：时钟、GPIO、DMA、SPI2 外设。
  *
  *          022 界面移植。引脚与源工程（jiemian1）的 CubeMX 配置一致：
  *            PB13 -> SPI2_SCK   (AF5)
  *            PB15 -> SPI2_MOSI  (AF5)
  *            PA8  -> SPI_CS     (输出，空闲高)
  *            PA11 -> SPI_RESET  (输出，复位用)
  *            PB14 -> SPI_DC     (输出)
  *          与本工程的 SPI1(W25Q64)、I2C1(AT24C02) 互不占用。
  *          CubeMX 重新生成代码不受影响（全部写在 USER CODE 段之外的新文件里）。
  ******************************************************************************
  */
#ifndef __SCREEN_HW_H__
#define __SCREEN_HW_H__

#ifdef __cplusplus
extern "C" {
#endif

void screen_hw_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __SCREEN_HW_H__ */
