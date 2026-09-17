/*********************************************************************************************************
 * Copyright (C) 2024 EternalChip, Inc. (Gmbh) or its affiliates.
 *
 * @file Flash.h
 *
 * @par dependencies
 *
 * - Flash.c
 *
 * @author Jack | R&D Dept. | EternalChip 立芯嵌入式
 *
 * @brief Functions related to reading and writing in the chip's flash area.
 *
 * Processing flow:
 *
 * call directly.
 *
 * @version V1.0 2024-09-13
 *
 * @note 1 tab == 4 spaces!
 *
 * ********************************************************************************************************/

/* Define to prevent recursive inclusion -----------------------------------------------------------------*/
#ifndef __FLASH_H
#define __FLASH_H

/* Includes ---------------------------------------------------------------------------------------------*/
#include "stm32f4xx.h"
#include "main.h"
/* Exported types ----------------------------------------------------------------------------------------*/
/* Exported constants ------------------------------------------------------------------------------------*/
/* Exported macro ----------------------------------------------------------------------------------------*/
//FLASH 扇区的起始地址
#define ADDR_FLASH_SECTOR_0     ((u32)0x08000000)  //扇区0起始地址, 16 Kbytes
#define ADDR_FLASH_SECTOR_1     ((u32)0x08004000)  //扇区1起始地址, 16 Kbytes
#define ADDR_FLASH_SECTOR_2     ((u32)0x08008000)  //扇区2起始地址, 16 Kbytes
#define ADDR_FLASH_SECTOR_3     ((u32)0x0800C000)  //扇区3起始地址, 16 Kbytes
#define ADDR_FLASH_SECTOR_4     ((u32)0x08010000)  //扇区4起始地址, 64 Kbytes
#define ADDR_FLASH_SECTOR_5     ((u32)0x08020000)  //扇区5起始地址, 128 Kbytes
#define ADDR_FLASH_SECTOR_6     ((u32)0x08040000)  //扇区6起始地址, 128 Kbytes
#define ADDR_FLASH_SECTOR_7     ((u32)0x08060000)  //扇区7起始地址, 128 Kbytes
#define ADDR_FLASH_SECTOR_8     ((u32)0x08080000)  //扇区8起始地址, 128 Kbytes
#define ADDR_FLASH_SECTOR_9     ((u32)0x080A0000)  //扇区9起始地址, 128 Kbytes
#define ADDR_FLASH_SECTOR_10    ((u32)0x080C0000)  //扇区10起始地址, 128 Kbytes
#define ADDR_FLASH_SECTOR_11    ((u32)0x080E0000)  //扇区11起始地址, 128 Kbytes

/* Exported functions ------------------------------------------------------------------------------------*/
uint8_t     Flash_erase   (u32 addr, u32 size);
FLASH_Status EraseAppSector(uint32_t FLASH_Sector);
void         Flash_Write   (uint32_t address, uint32_t data);
/* 下面两个是 Flash.c 里的解锁/上锁封装（内部在 FLASH_Unlock 后等了一次 BUSY），
   之前漏了声明，Boot_Manager.c 的 Backup_Invalidate() 调不到，AC6 下直接报错 */
void         Flash_Unlock  (void);
void         Flash_Lock    (void);

#endif /* __FLASH_H */
