/*********************************************************************************************************
 * Copyright (C) 2024 EternalChip, Inc. (Gmbh) or its affiliates.
 *
 * @file Spi.h
 *
 * @par dependencies
 *
 * - main.h
 * - stm32f4xx.h
 *
 * @author Jack | R&D Dept. | EternalChip 立芯嵌入式
 *
 * @brief Functions related to the chip's SPI1 bus (used by the on-board SPI flash).
 *
 * Processing flow:
 *
 * SPI1_Init() -> SPI1_ReadWriteByte() / SPI1_WriteByte() / SPI1_ReadByte()
 *
 * @note 1 tab == 4 spaces!
 *
 * ********************************************************************************************************/

/* Define to prevent recursive inclusion -----------------------------------------------------------------*/
#ifndef __SPI_H
#define __SPI_H

/* Includes ---------------------------------------------------------------------------------------------*/
#include "main.h"
#include "stm32f4xx.h"

/* Exported macro ---------------------------------------------------------------------------------------*/
/* 片选脚: 软件控制, 接 SPI Flash 的 CS# */
#define F_CS_Pin                GPIO_Pin_4
#define F_CS_GPIO_Port          GPIOA

/* 常用波特率预分频值, 传给 SPI1_SetSpeed()。
   SPI1 挂在 APB2 上, 按 96MHz 算: /4=24MHz /8=12MHz /16=6MHz /32=3MHz
   /64=1.5MHz /128=750kHz /256=375kHz
   注意 PY25Q64HA / W25Q64 上电默认只支持到 100MHz 左右,
   但布线差或杜邦线飞线时建议先用 /128 或 /256 跑通, 再往上提。 */
#define SPI_SPEED_4             0x00
#define SPI_SPEED_8             0x08
#define SPI_SPEED_16            0x10
#define SPI_SPEED_32            0x18
#define SPI_SPEED_64            0x20
#define SPI_SPEED_128           0x28
#define SPI_SPEED_256           0x38

/* Exported functions -----------------------------------------------------------------------------------*/
void SPI1_Init(void);
u8   SPI1_ReadWriteByte(u8 WriteData);
void SPI1_SetSpeed(u8 SPI_BaudRate_Prescaler);
u8   SPI1_WriteByte(u8 *WriteData, u16 dataSize, u32 timeout);
u8   SPI1_ReadByte(u8 *ReadData, u16 dataSize, u32 timeout);

/* 毫秒时基: SysTick 每 1ms 累加一次(main.c 的 TimingDelay_Decrement 里加),
   SPI1_WriteByte/SPI1_ReadByte 的 timeout 参数以它为单位 */
extern volatile u32 g_msTick;
u32  millis(void);

#endif /* __SPI_H */
