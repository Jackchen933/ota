/*******************************************************************************
** �ļ���: 		common.h
** �汾��  		1.0
** ��������: 	RealView MDK-ARM 5.20
** ����: 		liupeng
** ��������: 	2016-03-20
** ����:		common�ļ���ͷ�ļ�����
** ����ļ�:	string.h��stdio.h��stm32f10x.h��ymodem.h
** �޸���־��	2016-03-20   �����ĵ�
*******************************************************************************/

/* ��ֹ�ض��� ----------------------------------------------------------------*/
#ifndef _COMMON_H
#define _COMMON_H

/* ����ͷ�ļ� *****************************************************************/
#include "stdio.h"
#include "string.h"
#include "stm32f4xx.h"
#include "ymodem.h"


/* �������� -----------------------------------------------------------------*/
typedef  void (*pFunction)(void);

//* �� ------------------------------------------------------------------------*/
#define CMD_STRING_SIZE       128

/*==============================================================================
 * 内部Flash分区(STM32F411, 512KB, 扇区0-3各16KB, 扇区4为64KB, 扇区5-7各128KB):
 *   扇区0-1  0x08000000~0x08007FFF  Bootloader 本体(32KB)
 *   扇区2    0x08008000~0x0800BFFF  标志扇区(16KB, 见 FlagSector.h 的字段布局)
 *   扇区3-7  0x0800C000~0x0807FFFF  App运行区(464KB, App可按512KB准备, 实际用464KB)
 * 外部Flash(W25Q64, 8MB)分区:
 *   A区域    0x000000~0x07FFFF      新固件密文下载区(512KB, Ymodem和App后台下载都写这里)
 *   B区域    0x080000~0x0FFFFF      备份密文区(512KB, "A区搬运至B区"备份用)
 * 注意: App运行区起始地址从旧版的0x08008000挪到了0x0800C000, App侧工程的
 *       链接起始地址(ROM起始)和VECT_TAB_OFFSET必须同步改成0x0800C000!
 *============================================================================*/
#define FlagSectorAddress  0x08008000   //标志扇区: 扇区2(16KB), 存App状态/密文大小等
#define FLAG_SECTOR_SIZE   (0x4000)     /* 16 KBytes */

#define ApplicationAddress 0x0800C000   //A区: 运行区(Bootloader跳转执行的APP), 扇区3-7

#define APP_SLOT_SIZE      (0x74000)    /* App运行区 464 KBytes */

/* 外部Flash W25Q64 的 A/B 区域基地址和容量(必须与App侧W25q64.h的定义一致) */
#define W25Q64_APP_BASE    0x000000UL   /* A区域: 新固件密文 */
#define W25Q64_APP_SIZE    (0x80000UL)  /* A区域 512KB */
#define W25Q64_BAK_BASE    0x080000UL   /* B区域: 备份密文 */
#define W25Q64_BAK_SIZE    (0x80000UL)  /* B区域 512KB */
/* 注意: 不要在这里定义 PAGE_SIZE / FLASH_SIZE 这类通用宏 ——
   w25qxx.h 的 W25Qx_Parameter 结构体成员就叫这两个名字,
   一旦同时包含两个头文件, 宏展开会把结构体定义炸掉 */

//允许上传/下载的固件密文包最大尺寸(外部Flash A区域容量)
#define FLASH_IMAGE_SIZE                   (uint32_t) (W25Q64_APP_SIZE)

#define IS_AF(c)  ((c >= 'A') && (c <= 'F'))
#define IS_af(c)  ((c >= 'a') && (c <= 'f'))
#define IS_09(c)  ((c >= '0') && (c <= '9'))
#define ISVALIDHEX(c)  IS_AF(c) || IS_af(c) || IS_09(c)
#define ISVALIDDEC(c)  IS_09(c)
#define CONVERTDEC(c)  (c - '0')

#define CONVERTHEX_alpha(c)  (IS_AF(c) ? (c - 'A'+10) : (c - 'a'+10))
#define CONVERTHEX(c)   (IS_09(c) ? (c - '0') : CONVERTHEX_alpha(c))

#define SerialPutString(x) Serial_PutString((uint8_t*)(x))


/* �������� ------------------------------------------------------------------*/
void Int2Str(uint8_t* str,int32_t intnum);
uint32_t Str2Int(uint8_t *inputstr,int32_t *intnum);
uint32_t GetIntegerInput(int32_t * num);
uint32_t SerialKeyPressed(uint8_t *key);
uint8_t GetKey(void);
void SerialPutChar(uint8_t c);
void Serial_PutString(uint8_t *s);
void GetInputString(uint8_t * buffP);
uint32_t FLASH_PagesMask(__IO uint32_t Size);
void FLASH_DisableWriteProtectionPages(void);
void Main_Menu(void);
void SerialDownload(void);
void SerialUpload(void);

#endif  /* _COMMON_H */

/*******************************�ļ�����***************************************/
