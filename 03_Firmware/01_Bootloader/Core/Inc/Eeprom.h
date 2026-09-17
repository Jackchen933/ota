/*********************************************************************************************************
 * Copyright (C) 2026 EternalChip, Inc. (Gmbh) or its affiliates.
 *
 * ALL Rights Reserved.
 *
 * @file Eeprom.h
 *
 * @par dependencies
 *
 * - stm32f4xx_i2c.h (标准外设库)
 *
 * @brief Bootloader 侧读 AT24C02 里的 OTA 状态标志。
 *
 *        APP 侧(04_Software/USERS/Src/Eeprom.c)用 HAL 写这几个字节,
 *        Bootloader 用标准外设库读 —— 库不一样, 但【地址分配和取值必须
 *        逐字节一致】, 改一边就要改另一边。
 *
 *        AT24C02: 256 字节, 7 位地址 0x50, 一页 8 字节, 写周期 5ms。
 *
 * Processing flow:
 *
 * Eeprom_Init() -> Eeprom_ReadByte()/Eeprom_ReadU32()
 *
 * @version V1.0 2026-09-16
 *
 * @note 1 tab == 4 spaces!
 *
 * ********************************************************************************************************/

/* Define to prevent recursive inclusion -----------------------------------------------------------------*/
#ifndef __BOOT_EEPROM_H
#define __BOOT_EEPROM_H

/* Includes ---------------------------------------------------------------------------------------------*/
#include "stm32f4xx.h"

/* Exported constants -----------------------------------------------------------------------------------*/

/* ---- AT24C02 器件参数 ---- */
#define EEPROM_SIZE             (256U)      /* 2Kbit = 256 字节 */
#define EEPROM_PAGE_SIZE        (8U)        /* AT24C02 是 8 字节一页 */
#define EEPROM_ADDR_7BIT        (0x50U)     /* A2A1A0 全接地 */
#define EEPROM_WRITE_CYCLE_MS   (5U)        /* 一次写周期典型 5ms */

/* ---- OTA 信息在 EEPROM 里的地址分配 ----
   务必和 APP 侧 USERS/Inc/Eeprom.h 保持一致 */
#define EE_ADDR_OTA_STATE       (0x00U)     /* 1 字节 */
#define EE_ADDR_NEW_APP_SIZE    (0x01U)     /* 4 字节, 小端 */
#define EE_ADDR_CUR_APP_SIZE    (0x05U)     /* 4 字节, 小端 */

/* ---- OTA 状态取值 ---- */
#define OTA_STATE_NONE          (0x00U)     /* 没有更新请求 */
#define OTA_STATE_DOWNLOADING   (0x11U)     /* APP 正在往 W25Q64 下载(掉电即为此态) */
#define OTA_STATE_REQ_UPDATE    (0x22U)     /* 下载完毕, 请求更新 App */

/* Exported functions -----------------------------------------------------------------------------------*/

/**
  * @brief  初始化 I2C1(PB6/PB7) 并探测 AT24C02
  * @param  None
  * @retval 0: 成功   1: 失败(器件没应答)
  * @note   本函数只做初始化, 不读也不写任何数据。
  */
uint8_t Eeprom_Init(void);

/**
  * @brief  从 mem_addr 开始读 len 个字节
  * @retval 0: 成功   1: 失败
  */
uint8_t Eeprom_Read(uint8_t mem_addr, uint8_t *buf, uint16_t len);

/**
  * @brief  从 mem_addr 开始写 len 个字节(内部按 8 字节页边界切分)
  * @retval 0: 成功   1: 失败
  */
uint8_t Eeprom_Write(uint8_t mem_addr, const uint8_t *buf, uint16_t len);

/**
  * @brief  读 1 字节
  * @retval 0: 成功   1: 失败
  */
uint8_t Eeprom_ReadByte(uint8_t mem_addr, uint8_t *val);

/**
  * @brief  写 1 字节
  * @retval 0: 成功   1: 失败
  */
uint8_t Eeprom_WriteByte(uint8_t mem_addr, uint8_t val);

/**
  * @brief  读 4 字节小端整数
  * @retval 0: 成功   1: 失败
  */
uint8_t Eeprom_ReadU32(uint8_t mem_addr, uint32_t *val);

/**
  * @brief  写 4 字节小端整数
  * @retval 0: 成功   1: 失败
  */
uint8_t Eeprom_WriteU32(uint8_t mem_addr, uint32_t val);

#endif /* __BOOT_EEPROM_H */
