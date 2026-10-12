/*********************************************************************************************************
 * Copyright (C) 2026 EternalChip, Inc. (Gmbh) or its affiliates.
 *
 * @file OtaApp.h
 *
 * @par dependencies
 *
 * - FreeRTOS.h / stream_buffer.h / cmsis_os.h
 * - Eeprom.h  (AT24C02 状态标志)
 * - W25q64.h  (外部 Flash 存放固件)
 *
 * @brief APP 侧「后台无感下载」OTA。
 *
 *        服务器从串口把固件传下来 -> APP 转存到外部 W25Q64 -> 全程不打断
 *        正在跑的业务。下完之后在 EEPROM 里记下状态和大小，等用户按按键
 *        确认才软复位，由 Bootloader 从 W25Q64 搬到内部 Flash 的 A 区。
 *
 * Processing flow:
 *   WaitReqDownload --收到 0x11 0x22 0x33--> OtaDownload(发'C'握手)
 *   OtaDownload     --Ymodem 收完结束帧----> WaitReqUpdate
 *   WaitReqUpdate   --收到 0x77 0x88 0x99--> OtaEnd
 *   OtaEnd          --20 秒内按下按键------> 软复位进 Boot
 *
 * @note 传输层为标准 Ymodem(接收端)：任意终端工具(Tera Term/SecureCRT)
 *       以 Ymodem 方式发送密文包文件即可，不需要专用推流上位机。
 *       4 个 1024 字节数据帧凑满一块 4KB 才交给下载线程写 W25Q64。
 *
 * @version V2.1  传输层由自定义推流改为标准 Ymodem
 * @version V2.0  改为 W25Q64 + EEPROM 状态机方案
 *
 * @note 1 tab == 4 spaces!
 *
 * ********************************************************************************************************/

/* Define to prevent recursive inclusion -----------------------------------------------------------------*/
#ifndef __OTA_APP_H
#define __OTA_APP_H

/* Includes ---------------------------------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "stream_buffer.h"
#include "cmsis_os.h"

/* Exported types ---------------------------------------------------------------------------------------*/

/* OTA 状态机。取值和 EEPROM 里的"OTA状态"字段不是一回事：
   前者是内存里的运行态，后者是掉电后要给 Bootloader 看的持久化标志 */
typedef enum
{
    OTA_ST_WAIT_REQ_DOWNLOAD = 0,   /* 等待服务器请求下载 */
    OTA_ST_DOWNLOAD,                /* OTA 数据下载中 */
    OTA_ST_WAIT_REQ_UPDATE,         /* 等待服务器请求更新 App */
    OTA_ST_END                      /* 等待用户确认 -> 软复位 */
} OtaState_t;

/* Exported constants -----------------------------------------------------------------------------------*/

/* ---- 串口协议(3 字节定长命令) ----
   方向里的 MCU 是"从 MCU 看"：Rx = 服务器发给 MCU */
#define OTA_CMD_DOWNLOAD_0      0x11    /* Rx: 服务器请求进入下载模式 */
#define OTA_CMD_DOWNLOAD_1      0x22
#define OTA_CMD_DOWNLOAD_2      0x33

#define OTA_CMD_DONE_0          0x44    /* Tx: 后台下载完毕，返回正响应 */
#define OTA_CMD_DONE_1          0x55
#define OTA_CMD_DONE_2          0x66

#define OTA_CMD_UPDATE_0        0x77    /* Rx: 服务器请求更新 App */
#define OTA_CMD_UPDATE_1        0x88
#define OTA_CMD_UPDATE_2        0x99

#define OTA_CMD_LEN             3U      /* 命令一律 3 字节 */

/* ---- Ymodem 协议(接收端) ----
   帧格式: SOH/STX + seq + ~seq + 128/1024字节数据 + CRC16-XMODEM(2B)
   握手:   MCU 发 'C' -> 对端发帧0(文件名+大小) -> MCU 回 ACK+'C' -> 数据帧...
   结束:   EOT -> MCU 回 NAK -> 再收 EOT -> MCU 回 ACK+'C' -> 空文件名帧 -> ACK */
#define YM_SOH                  0x01    /* 128 字节数据帧 */
#define YM_STX                  0x02    /* 1024 字节数据帧 */
#define YM_EOT                  0x04    /* 文件传输结束 */
#define YM_ACK                  0x06
#define YM_NAK                  0x15    /* 坏帧，请求重传 */
#define YM_CAN                  0x18    /* 取消(连收两个生效) */
#define YM_CRC_CH               0x43    /* 'C'：CRC 模式请求 */

#define YM_BLK_128              128U
#define YM_BLK_1024             1024U
#define YM_C_MAX_RETRY          10U     /* 'C' 最大重发次数(每次间隔 END_IDLE_MS) */

/* ---- 双缓冲 ----
   每块正好一个 W25Q 扇区(4096B)：收满一块就交给下载线程擦+写一个扇区，
   期间 OTA 线程接着收另一块，两边不打架。
   RAM 开销: 2 x 4096 = 8KB */
#define OTA_BUF_SIZE            4096U
#define OTA_BUF_COUNT           2U

/* DMA 一次搬进来的块大小。比缓冲小，收到就转进流缓冲 */
#define OTA_RX_CHUNK            1024U

/* 流缓冲容量。吸收 DMA 波次、以及"BufTake 等下载线程写完 Flash"期间
   到达的字节。Ymodem 停等协议下发送方在等 ACK 不会推数据，压力很小，
   4KB 是余量设计 */
#define OTA_STREAM_SIZE         4096U

/* ---- 超时(ms) ----
   END_IDLE_MS: Ymodem 会话超时。握手段(还没收到帧0)空闲这么久就重发'C'；
   收到帧0后空闲这么久 = 对端死了/线断了，中止本轮下载 */
#define OTA_END_IDLE_MS         3000U

/* 收到 0x77 0x88 0x99 后，留给用户按按键确认的时间 */
#define OTA_KEY_WINDOW_MS       20000U

/* 按键轮询周期 */
#define OTA_KEY_POLL_MS         10U
/* 按键消抖 */
#define OTA_KEY_DEBOUNCE_MS     50U

/* Exported functions -----------------------------------------------------------------------------------*/

/**
  * @brief  初始化 OTA 模块：建流缓冲、双缓冲、队列、信号量
  * @retval 0: 成功   -1: 创建失败
  * @note   必须在 osKernelStart() 之前调用(创建内核对象)。
  */
int8_t OtaApp_Init(void);

/**
  * @brief  挂起一次「DMA + 空闲中断」接收
  * @note   必须在 osKernelStart() 之后调用 —— 中断回调里会调
  *         xStreamBufferSendFromISR，内核没起来会断言。
  */
void OtaApp_RxArm(void);

/**
  * @brief  串口空闲/半满事件：把收到的一块数据转进流缓冲并重挂接收
  * @param  len: 本次收到的字节数
  * @note   在【中断上下文】执行，只能调带 FromISR 的 FreeRTOS API。
  *         USART1 中断优先级必须低于 configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY，
  *         否则会命中 configASSERT。
  */
void OtaApp_OnRxEvent(uint16_t len);

/**
  * @brief  串口出错后由任务重新挂起接收
  */
void OtaApp_RxError(void);

/**
  * @brief  OTA 状态管理线程(oTA_taskHandle)
  * @note   收串口数据、跑状态机、管双缓冲的分配
  */
void OtaApp_Task(void *argument);

/**
  * @brief  下载线程(DownloadAppData_taskHandle)
  * @note   从队列取满块，擦+写 W25Q64 的一个扇区，写完放信号量
  */
void OtaApp_DownloadTask(void *argument);

/**
  * @brief  当前状态(供调试/业务查询)
  */
OtaState_t OtaApp_GetState(void);

#endif /* __OTA_APP_H */

/*********************************** (C) COPYRIGHT 2026 EternalChip *****END OF FILE***********************/
