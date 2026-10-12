/*********************************************************************************************************
 * @file    OtaFlag.h
 *
 * @brief   App 侧「标志扇区」驱动 —— 内部 Flash 扇区2 (0x08008000, 16KB)。
 *
 *          这是 App 和 Bootloader 之间唯一的"对话渠道": App 把升级请求写进
 *          这个扇区, 下次复位 Bootloader 读它决定走哪条升级流程。
 *
 *          布局与 Bootloader 的 Tasks/Boot_Manager/FlagSector.h 逐字节一致,
 *          两边任何一个字段改了, 另一边必须同步:
 *
 *          偏移    字段                 说明
 *          0x00    App运行状态原码      0x11111111=App下载固件待更新
 *                                       0x22222222=验证新App(Boot跳转前置的)
 *                                       0x33333333=A区搬运至B区(App自检通过后置)
 *                                       0x44444444=有效App
 *          0x04    App运行状态反码
 *          0x08    E flash A区密文大小  W25Q64 A区域里密文包的总字节数
 *          0x0C    (反码)
 *          0x10    E flash B区状态      0x55555555=备份有效 (App只读不写)
 *          0x14    (反码)
 *          0x18    E flash B区密文大小  (App只读不写)
 *          0x1C    (反码)
 *
 *          App 侧只写两个东西:
 *            1. 下载完成    -> EaSize + 状态 0x11 (OtaApp_FinishDownload)
 *            2. 自检通过    -> 状态 0x33          (main 启动自检)
 *          其余字段(AppSt 反码之外)原样保留, 不碰 Boot 的 B 区记账。
 *
 * @note    写入策略与 Boot 侧相同: 所有字段先改 RAM 镜像, 再整扇区擦除重写。
 *          F411 只有一个 Flash Bank, 擦扇区2 的约 1 秒里 CPU 从 Flash 取指
 *          会 stall —— 表现为整机卡 1 秒, FreeRTOS tick 丢几个, 无功能影响。
 *          一次升级只写两三次, 这个代价可接受。
 *
 * @version V1.0
 *
 * @note 1 tab == 4 spaces!
 *
 * ********************************************************************************************************/
#ifndef __OTA_FLAG_H
#define __OTA_FLAG_H

/* Includes ---------------------------------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Exported constants -----------------------------------------------------------------------------------*/

/* 标志扇区基址(扇区2, 16KB)。Boot 和 App 双方共同约定, 不能单独改 */
#define OTA_FLAG_ADDR           (0x08008000UL)

/* ---- App运行状态取值(原码; 反码=按位取反) ---- */
#define OTA_FLAG_ST_DL_PENDING  (0x11111111UL)  /* App下载固件待更新 -> Boot 子流程A */
#define OTA_FLAG_ST_VERIFY_NEW  (0x22222222UL)  /* 验证新App -> Boot 子流程B(开狗跳App) */
#define OTA_FLAG_ST_AB_MOVE     (0x33333333UL)  /* A区搬运至B区 -> Boot 子流程E */
#define OTA_FLAG_ST_VALID       (0x44444444UL)  /* 有效App -> Boot 直接跳转 */

/* ---- IWDG 喂狗 ----
   Boot 在子流程B(验证新App)跳转前开了独立看门狗, 超时约 8.2 秒
   (LSI 32kHz / 64 分频 / 重装 0x0FFF), 而且 IWDG 一旦开启就关不掉、
   复位后照样在跑 —— 所以 App 每次启动都必须"接管喂狗", 否则 8 秒后
   被狗咬复位。这里直接写 IWDG->KR 重装计数, 不走 HAL 句柄。
   喂狗点: main 启动先喂一次, 之后业务线程周期喂(见 freertos.c)。 */
#define OTA_IWDG_FEED()         (IWDG->KR = 0xAAAAUL)

/* Exported functions -----------------------------------------------------------------------------------*/

/**
  * @brief  上电加载标志扇区到 RAM 镜像并做反码校验
  * @retval 0: 成功(空白扇区也算成功, 状态读出来是 0)
  * @note   校验失败的字段清 0 处理, 不阻塞启动。
  *         必须最先调用, 后面的 Get/Set 都依赖镜像。
  */
uint8_t OtaFlag_Load(void);

/**
  * @brief  读 App 运行状态(已经过反码校验)
  * @retval 状态原码; 空白/校验失败返回 0
  */
uint32_t OtaFlag_GetAppState(void);

/**
  * @brief  读 W25Q64 A区域密文大小
  * @param  size: [out] 密文包总字节数
  * @retval 0: 成功   1: 无效(空白/反码不符)
  */
uint8_t OtaFlag_GetEaSize(uint32_t *size);

/**
  * @brief  写 App 运行状态(其余字段保持原值)
  * @retval 0: 成功   1: 写入或读回校验失败
  * @note   内部含整扇区擦除, 会卡约 1 秒, 只允许在任务上下文/初始化阶段调用。
  */
uint8_t OtaFlag_SetAppState(uint32_t st);

/**
  * @brief  一次性写「A区密文大小 + 状态0x11」—— 下载完成收尾专用
  * @param  size: W25Q64 A区域里密文包的总字节数
  * @retval 0: 成功   1: 失败
  * @note   两个字段一次落盘, 不会出现"大小写好了状态还是旧的"的中间态;
  *         反过来即使中途掉电, 状态没置上 Boot 就不会碰半截固件, 安全。
  */
uint8_t OtaFlag_SetDownloadDone(uint32_t size);

#endif /* __OTA_FLAG_H */

/*********************************** (C) COPYRIGHT 2026 EternalChip *****END OF FILE***********************/
