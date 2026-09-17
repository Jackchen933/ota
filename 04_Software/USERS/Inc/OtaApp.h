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
 *        服务器从串口把固件推下来 -> APP 转存到外部 W25Q64 -> 全程不打断
 *        正在跑的业务。下完之后在 EEPROM 里记下状态和大小，等用户按按键
 *        确认才软复位，由 Bootloader 从 W25Q64 搬到内部 Flash 的 A 区。
 *
 * Processing flow:
 *   WaitReqDownload --收到 0x11 0x22 0x33--> OtaDownload
 *   OtaDownload     --数据传完-------------> WaitReqUpdate
 *   WaitReqUpdate   --收到 0x77 0x88 0x99--> OtaEnd
 *   OtaEnd          --20 秒内按下按键------> 软复位进 Boot
 *
 * @note 本模块【替换】了原先基于 Ymodem 的方案（密文写内部 B 区 0x08020000）。
 *       两者都靠按键触发会互相打架，旧的 Ota.c/Ota.h 已移除。
 *
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

/* ---- 双缓冲 ----
   每块正好一个 W25Q 扇区(4096B)：收满一块就交给下载线程擦+写一个扇区，
   期间 OTA 线程接着收另一块，两边不打架。
   RAM 开销: 2 x 4096 = 8KB */
#define OTA_BUF_SIZE            4096U
#define OTA_BUF_COUNT           2U

/* DMA 一次搬进来的块大小。比缓冲小，收到就转进流缓冲 */
#define OTA_RX_CHUNK            1024U

/* 流缓冲容量。服务器是连续推流的(不像 Ymodem 那样停等)，
   所以要能吸收"下载线程正在写 Flash 时上游继续来的数据"。
   W25Q64 写一个扇区约 130ms，115200 下这期间最多来 1.5KB，4KB 够用 */
#define OTA_STREAM_SIZE         4096U

/* ---- 超时(ms) ----
   FRAME_IDLE_MS: 一帧的判据。串口空闲这么久 = 服务器发完了一帧 */
#define OTA_FRAME_IDLE_MS       100U
/* END_IDLE_MS: 整个下载结束的判据。空闲这么久 = 服务器发完了全部固件。
   比 FRAME_IDLE_MS 大得多，避免把帧间的小停顿误判成结束。
   如果你的服务器有显式的结束命令，把这个超时机制换掉即可。 */
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
