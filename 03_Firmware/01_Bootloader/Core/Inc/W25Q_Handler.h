/*********************************************************************************************************
 * Copyright (C) 2024 EternalChip, Inc. (Gmbh) or its affiliates.
 *
 * ALL Rights Reserved.
 *
 * @file W25Q_Handler.h
 *
 * @par dependencies
 *
 * - W25Q_Handler.h
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __W25Q_HANDLER_H
#define __W25Q_HANDLER_H

/* Includes ------------------------------------------------------------------*/
/* 原来这里只有 main.h / stm32f4xx.h, 但下面用到了 W25Qx_Init / W25Qx_Para /
   W25Qx_Read 这些 w25qxx 的符号, 而 .c 只 include 了本头文件 —— 少这一个
   include, AC6 下就是一串 "undeclared identifier" 硬错误。 */
#include "main.h"
#include "stm32f4xx.h"
#include "w25qxx.h"

/* Exported types ------------------------------------------------------------*/
/*
最小读写单元 Pag = 256 byte
一个扇区        = 16个Pag = 4096 byte
一个块          = 16个扇区 = 64KB
*/
typedef struct
{
	u8  databuf[4096];          /* 按4096个数据进行读写设置 */

	u16 write_databuf_index;    /* databuf 里已经填了多少字节 */

	u32 write_index;            /* 已写入的总字节数(跨扇区累加), 也是"读指针的上界" */

	/* 注意: 这两个索引原本是 u8, 只能数到 255 —— 也就是 255 * 4096 = 1020KB。
	   W25Q64 有 8MB / 2048 个扇区, 写到 1MB 之后索引回绕到 0, 接着就从
	   第 0 个扇区重新擦写, 把最开头的数据覆盖掉。已改成 u32。 */
	u32 write_sector_index;     /* 当前正在写的扇区号 = write_index / 4096 */

	u32 read_index;             /* 已读出的总字节数 */
	u32 read_sector_index;      /* 当前读到的扇区号(由 read_index 推导, 供调试观察) */
}st_W25Q_Handler;

/* Exported constants --------------------------------------------------------*/
/* Exported macro ------------------------------------------------------------*/

/* Exported functions --------------------------------------------------------*/

/* 上电初始化: 建 SPI1 总线 + 复位芯片 + 读容量参数。
   返回 0 成功, 1 失败(芯片没接 / ID 不对)。失败后下面的读写都会直接返回错误,
   不会再拿没初始化好的参数去算地址。 */
u8 W25Q64_Init(void);

/* 整片擦除, 8MB 芯片约 20 秒(最长 250 秒), 全程阻塞 */
u8 W25Q64_EraseChip(void);

/* 流式写入: data 可以任意长, 内部攒够 4096 字节(bss databuf 大小)就落盘一个扇区。
   注意它只写整扇区, 尾巴不足 4KB 的部分要再调一次 W25Q64_WriteData_End() 冲出去。 */
u8 W25Q64_WriteData(u8 *data, u32 length);

/* 把 databuf 里不足 4096 的尾巴补写下去, 结束一次写入流程。
   必须成对使用: WriteData(...) * N 之后紧跟 WriteData_End()。 */
u8 W25Q64_WriteData_End(void);

/* 流式读取: 每次尽量吐 4096 字节, data 必须是 >= 4096 字节的缓冲。
   *length 回传本次实际读到的长度。
   返回: 0 读取成功   1 已经读完, 没数据了   2 读取失败(SPI 出错) */
u8 W25Q64_ReadData(u8 *data, u16 *length);

#endif /* __W25Q_HANDLER_H */
