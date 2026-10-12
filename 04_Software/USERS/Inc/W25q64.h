/*********************************************************************************************************
 * Copyright (C) 2026 EternalChip, Inc. (Gmbh) or its affiliates.
 *
 * @file W25q64.h
 *
 * @par dependencies
 *
 * - spi.h    (hspi1)
 * - main.h   (SPI1_cs_Pin / SPI1_cs_GPIO_Port)
 *
 * @brief 外部 SPI Flash W25Q64(8MB) 的底层驱动。
 *
 * @note 移植自 Bootloader 的 Core/Src/w25qxx.c —— 那份是【标准外设库】版本
 *       (SPI1_WriteByte / SPI1_ReadByte / millis)，本工程是 HAL，已全部替换。
 *       时序、命令序列、超时值与原版保持一致，只是接口换了。
 *
 *       移植时最容易踩的一个坑：原版 SPI1_WriteByte() 是"返回 0 表示失败"，
 *       而 HAL 的 HAL_SPI_Transmit() 是"返回 HAL_OK(0) 表示成功"，
 *       判断方向正好相反。这里统一按 HAL 的语义写，不要照抄原版的 == 0。
 *
 * @version V1.0
 *
 * @note 1 tab == 4 spaces!
 *
 * ********************************************************************************************************/

/* Define to prevent recursive inclusion -----------------------------------------------------------------*/
#ifndef __W25Q64_H
#define __W25Q64_H

/* Includes ---------------------------------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"
#include "spi.h"
#include "main.h"

/* Exported constants -----------------------------------------------------------------------------------*/

/* W25Q64: 8MByte = 64Mbit */
#define W25Q64_TOTAL_SIZE       (8UL * 1024UL * 1024UL)
#define W25Q64_SECTOR_SIZE      (4096UL)        /* 可擦除的最小单位: 4KB 小扇区 */
#define W25Q64_PAGE_SIZE        (256UL)         /* 可编程的最大单位: 256B 页 */
#define W25Q64_SECTOR_COUNT     (W25Q64_TOTAL_SIZE / W25Q64_SECTOR_SIZE)    /* 2048 */

/* 单次等待芯片内部操作(页编程/擦除)结束的超时(ms)。
   页编程典型 0.7ms、4KB 扇区擦除典型 45ms，给 1 秒余量足够；
   擦除另有更长超时，见 W25Q64_SECTOR_ERASE_TIMEOUT_MS */
#define W25Q64_TIMEOUT_MS               (1000UL)
#define W25Q64_SECTOR_ERASE_TIMEOUT_MS  (3000UL)    /* 4KB 扇区擦除最长 400ms */
#define W25Q64_CHIP_ERASE_TIMEOUT_MS    (250000UL)  /* 整片擦除最长 250s */

/* W25Q64 的 OTA 数据区(A区域)起始地址 —— 与 Bootloader 的 common.h 对齐。
   分区约定(App 只用 A 区域, B 区域是 Boot 的备份区, 不得碰):
     0x000000 ~ 0x07FFFF  A区域(512KB): App 后台下载的密文包落在这里,
                          收完后 Boot 子流程A 从这里解密搬运到 App 运行区
     0x080000 ~ 0x0FFFFF  B区域(512KB): Boot 的备份区(子流程E/C 用)
   收到的字节流是上位机/服务器给的【密文包】:
     [16B加密头][固件AES密文][16B MD5指纹], 头里藏着固件真实大小 */
#define W25Q64_OTA_BASE_ADDR    (0UL)

/* Exported functions -----------------------------------------------------------------------------------*/

/**
  * @brief  初始化 W25Q64: 复位芯片 -> 读 ID -> 确定容量
  * @retval 0: 成功   -1: 芯片没接 / ID 不对 / SPI 出错
  * @note   SPI1 总线本身由 MX_SPI1_Init() 初始化，这里不管。
  *         必须在 FreeRTOS 启动前后都行，但只能在一个任务里调。
  */
int8_t W25q64_Init(void);

/**
  * @brief  读数据(任意长度、任意地址，可跨页跨扇区)
  * @param  addr: 起始地址(0 ~ 8MB-1)
  * @param  buf : 目标缓冲
  * @param  len : 字节数
  * @retval 0: 成功   -1: 失败
  */
int8_t W25q64_Read(uint32_t addr, uint8_t *buf, uint32_t len);

/**
  * @brief  写数据(任意长度，内部自动按 256B 页切分)
  * @param  addr: 起始地址
  * @param  buf : 源数据
  * @param  len : 字节数
  * @retval 0: 成功   -1: 失败
  * @note   目标区域必须【已经擦过】(全 0xFF)。页只能写一次，重复写同一页
  *         会得到 0xFF 和数据的按位与，不是覆盖。整扇区写请用
  *         W25q64_WriteSector()，它会先擦。
  */
int8_t W25q64_Write(uint32_t addr, const uint8_t *buf, uint32_t len);

/**
  * @brief  擦除一个 4KB 扇区
  * @param  addr: 扇区内任意地址（芯片会自己对齐；这里额外要求按 4KB 对齐）
  * @retval 0: 成功   -1: 失败
  * @note   擦除耗时典型 45ms、最长 400ms，全程阻塞。在任务上下文调用没问题，
  *         在中断里调会把中断拖长几百毫秒，绝对不要。
  */
int8_t W25q64_EraseSector(uint32_t addr);

/**
  * @brief  擦除 + 写入一个完整扇区(4096 字节)
  * @param  sector_index: 扇区号(0 ~ 2047)
  * @param  buf         : 4096 字节数据
  * @retval 0: 成功   -1: 失败
  * @note   OTA 下载的主路径 —— 一个 4KB 缓冲对应一个扇区，一个扇区一次擦写。
  */
int8_t W25q64_WriteSector(uint32_t sector_index, const uint8_t *buf);

/**
  * @brief  读一个完整扇区
  * @param  sector_index: 扇区号(0 ~ 2047)
  * @param  buf         : 4096 字节缓冲
  * @retval 0: 成功   -1: 失败
  */
int8_t W25q64_ReadSector(uint32_t sector_index, uint8_t *buf);

/**
  * @brief  读到的芯片容量(字节)。Init 之前是 0
  */
uint32_t W25q64_GetSize(void);

/**
  * @brief  读到的芯片 ID(16 位, 高字节厂商, 低字节容量码)
  */
uint16_t W25q64_GetId(void);

#endif /* __W25Q64_H */

/*********************************** (C) COPYRIGHT 2026 EternalChip *****END OF FILE***********************/
