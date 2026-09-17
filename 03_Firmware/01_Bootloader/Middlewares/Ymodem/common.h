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

#define BackAppAddress     0x08020000   //B区: 备份区(Ymodem下载的固件先存这里), 扇区5, 128KB

#define ApplicationAddress 0x08008000   //A区: 运行区(Bootloader跳转执行的APP), 扇区2-4

/* 双区槽位大小: A区=扇区2-4(16K+16K+64K=96KB), B区=扇区5(128KB) */
#define APP_SLOT_SIZE      (0x18000)    /* 96 KBytes */
#define BACK_SLOT_SIZE     (0x20000)    /* 128 KBytes */
/* STM32F411: 512KB Flash, 扇区0-3各16KB、扇区4为64KB、扇区5-7各128KB.
   此处按 APP 区起始扇区(16KB)简化定义, 仅供编译通过, 正式移植需再细化 */
#define PAGE_SIZE                         (0x4000)    /* 16 Kbyte */
#define FLASH_SIZE                        (0x80000)   /* 512 KBytes */

//�����ϴ��ļ���С 
#define FLASH_IMAGE_SIZE                   (uint32_t) (FLASH_SIZE - (BackAppAddress - 0x08000000))

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
