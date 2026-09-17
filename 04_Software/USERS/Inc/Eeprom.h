/*********************************************************************************************************
 * Copyright (C) 2026 EternalChip, Inc. (Gmbh) or its affiliates.
 *
 * @file Eeprom.h
 *
 * @par dependencies
 *
 * - i2c.h   (hi2c1)
 *
 * @brief AT24C02(2Kbit = 256 字节 I2C EEPROM) 驱动 + OTA 状态存取。
 *
 * @note 硬件连接: I2C1, PB6=SCL / PB7=SDA, A2/A1/A0 全接地 -> 器件地址 0x50。
 *
 * @version V1.0
 *
 * @note 1 tab == 4 spaces!
 *
 * ********************************************************************************************************/

/* Define to prevent recursive inclusion -----------------------------------------------------------------*/
#ifndef __EEPROM_H
#define __EEPROM_H

/* Includes ---------------------------------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"
#include "i2c.h"

/* Exported constants -----------------------------------------------------------------------------------*/

/* AT24C02 容量 256 字节 */
#define EEPROM_SIZE             (256U)

/* 页写边界。AT24C02 是 8 字节一页 —— 这一点和 AT24C32/64(32字节)、
   AT24C256(64字节) 都不一样，写代码时最容易记错。跨页写不会自动进位，
   而是回绕到本页开头覆盖，所以超过 8 字节必须切开 */
#define EEPROM_PAGE_SIZE        (8U)

/* 一次写周期(WRC)最长 5ms，写完必须等，不然下一条读写会读回旧值 */
#define EEPROM_WRITE_CYCLE_MS   (5U)

/* ---- OTA 在 EEPROM 里的地址表 (与上位机/服务器约定，不能随便改) ----
   | 数据类型        | 长度 | 起始地址 | 取值                                  |
   |----------------|-----|---------|---------------------------------------|
   | OTA 状态        | 1   | 0x00    | 0x00 没有App更新 / 0x11 下载中 / 0x22 请求更新 |
   | OTA 新App字节大小| 4   | 0x01    | 本次下载的固件大小(小端)                  |
   | 当前App字节大小  | 4   | 0x05    | 现在运行的 App 大小(小端)                 | */
#define EE_ADDR_OTA_STATE       (0x00U)     /* 1 字节 */
#define EE_ADDR_NEW_APP_SIZE    (0x01U)     /* 4 字节 */
#define EE_ADDR_CUR_APP_SIZE    (0x05U)     /* 4 字节 */

/* OTA 状态取值 */
#define OTA_STATE_NONE          (0x00U)     /* 没有 App 更新 */
#define OTA_STATE_DOWNLOADING   (0x11U)     /* App 数据下载过程中 */
#define OTA_STATE_REQ_UPDATE    (0x22U)     /* App 请求更新 */

/* Exported functions -----------------------------------------------------------------------------------*/

/**
  * @brief  探测 EEPROM 是否在线
  * @retval 0: 在线   -1: 无应答
  * @note   I2C1 总线由 MX_I2C1_Init() 初始化，这里只做一次器件应答探测。
  */
int8_t Eeprom_Init(void);

/**
  * @brief  读任意长度(可跨页)
  * @param  mem_addr: EEPROM 内地址(0 ~ 255)
  * @param  buf     : 目标缓冲
  * @param  len     : 字节数
  * @retval 0: 成功   -1: 失败
  */
int8_t Eeprom_Read(uint8_t mem_addr, uint8_t *buf, uint16_t len);

/**
  * @brief  写任意长度(内部自动按 8 字节页切分，每页之间等写周期)
  * @param  mem_addr: EEPROM 内地址(0 ~ 255)
  * @param  buf     : 源数据
  * @param  len     : 字节数
  * @retval 0: 成功   -1: 失败
  * @note   本函数是阻塞的：每写一页要等 5ms 写周期。
  *         最多 32 页，最坏 160ms。不要在中断里调用。
  */
int8_t Eeprom_Write(uint8_t mem_addr, const uint8_t *buf, uint16_t len);

/**
  * @brief  读一个字节
  * @retval 读到的值；读失败返回 0xFF
  */
uint8_t Eeprom_ReadByte(uint8_t mem_addr);

/**
  * @brief  写一个字节
  * @retval 0: 成功   -1: 失败
  */
int8_t Eeprom_WriteByte(uint8_t mem_addr, uint8_t val);

/* ---- OTA 状态存取(直接对应上面那张地址表) ---- */

/** @brief 读 OTA 状态(0x00 / 0x11 / 0x22)，读失败返回 0xFF */
uint8_t  OtaEe_GetState(void);

/** @brief 写 OTA 状态   @retval 0: 成功 -1: 失败 */
int8_t   OtaEe_SetState(uint8_t state);

/** @brief 读"本次下载的固件大小"(小端 4 字节)，读失败返回 0xFFFFFFFF */
uint32_t OtaEe_GetNewAppSize(void);

/** @brief 写"本次下载的固件大小"   @retval 0: 成功 -1: 失败 */
int8_t   OtaEe_SetNewAppSize(uint32_t size);

/** @brief 读"当前运行的 App 大小"，读失败返回 0xFFFFFFFF */
uint32_t OtaEe_GetCurAppSize(void);

/** @brief 写"当前运行的 App 大小"   @retval 0: 成功 -1: 失败 */
int8_t   OtaEe_SetCurAppSize(uint32_t size);

#endif /* __EEPROM_H */

/*********************************** (C) COPYRIGHT 2026 EternalChip *****END OF FILE***********************/
