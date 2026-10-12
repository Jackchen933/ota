/*********************************************************************************************************
 * @file    FlagSector.h
 *
 * @brief   标志扇区驱动(内部Flash扇区2, 0x08008000, 16KB)
 *
 *          按课程资料《标志扇区数据格式》定义, 每个字段都是"原数据+原数据反码"
 *          两个u32, 读出时互相校验, 任一字段损坏都能被发现:
 *
 *          偏移    字段                 说明
 *          0x00    App运行状态原码      0x11111111=App下载固件待更新
 *                                       0x22222222=验证新App(App侧视角即"App验证中")
 *                                       0x33333333=A区搬运至B区
 *                                       0x44444444=有效App
 *          0x04    App运行状态反码
 *          0x08    E flash A区密文大小  外部Flash A区域里密文包的总字节数
 *          0x0C    (反码)
 *          0x10    E flash B区状态      0x55555555=备份有效, 其他=无效
 *          0x14    (反码)
 *          0x18    E flash B区密文大小  外部Flash B区域里密文包的总字节数
 *          0x1C    (反码)
 *          0x20~   保留
 *
 *          写入策略: 所有字段先改RAM镜像, 再整体擦除扇区2重写, 一次写入
 *          同时落盘, 不会出现"状态改了但大小还是旧的"的中间态。
 *
 * @note    App侧写标志(置"App下载固件待更新"/"A区搬运至B区")必须按同样的
 *          布局直接操作0x08008000, 双方取值定义必须逐字节一致。
 *
 * @version V1.0 2026-09-23
 * ********************************************************************************************************/
#ifndef __FLAG_SECTOR_H
#define __FLAG_SECTOR_H

#include "stm32f4xx.h"
#include "common.h"

/* ---- App运行状态取值(原码; 反码=按位取反) ---- */
#define APP_ST_DL_PENDING       (0x11111111UL)  /* App下载固件待更新 -> 子流程A */
#define APP_ST_VERIFY_NEW       (0x22222222UL)  /* 验证新App(=App验证中) -> 子流程B */
#define APP_ST_AB_MOVE          (0x33333333UL)  /* A区搬运至B区 -> 子流程E */
#define APP_ST_VALID            (0x44444444UL)  /* 有效App -> 直接跳转 */
#define APP_ST_INVALID          (0x99999999UL)  /* 扩展: 备份也校验失败, 防止反复回滚 */

/* ---- E flash B区状态取值 ---- */
#define BAK_ST_VALID            (0x55555555UL)  /* 备份区密文有效 */

/* ---- 标志扇区RAM镜像(上电由 Flag_Load 填充) ---- */
typedef struct
{
    uint32_t AppSt;         /* 0x00: App运行状态原码 */
    uint32_t AppStInv;      /* 0x04: App运行状态反码 */
    uint32_t EaSize;        /* 0x08: E flash A区密文大小 */
    uint32_t EaSizeInv;     /* 0x0C */
    uint32_t EbSt;          /* 0x10: E flash B区状态 */
    uint32_t EbStInv;       /* 0x14 */
    uint32_t EbSize;        /* 0x18: E flash B区密文大小 */
    uint32_t EbSizeInv;     /* 0x1C */
} Flag_TypeDef;

/**
  * @brief  上电加载标志扇区到RAM镜像并做反码校验
  * @retval 0: 加载成功(字段都通过校验; 空白扇区也算成功, 状态读出来是0)
  *         1: 读取失败(Flash异常, 理论不会发生)
  * @note   校验失败的字段会被清0(视为"无效"), 不阻塞启动:
  *         AppSt无效 -> 主线走路径D; EbSt无效 -> 不回滚。
  */
uint8_t Flag_Load(void);

/**
  * @brief  读App运行状态(已经过反码校验)
  * @retval 状态原码; 空白/校验失败返回0(主线会当"其他"进路径D)
  */
uint32_t Flag_GetAppState(void);

/**
  * @brief  读外部Flash A区密文大小(字节)
  * @retval 0: 成功  1: 无效(空白/反码不符/超出A区域容量)
  */
uint8_t Flag_GetEaSize(uint32_t *size);

/**
  * @brief  读外部Flash B区状态和密文大小
  * @param  st:   [out] 状态原码(空白/校验失败返回0)
  * @param  size: [out] 密文大小(无效时返回0)
  * @retval None
  */
void Flag_GetEb(uint32_t *st, uint32_t *size);

/**
  * @brief  设置App运行状态(写标志扇区, 其余字段保持原值)
  * @retval 0: 成功  1: 写入或读回校验失败
  */
uint8_t Flag_SetAppState(uint32_t st);

/**
  * @brief  记录外部Flash A区密文大小(Ymodem下载完成后调用)
  * @retval 0: 成功  1: 失败
  */
uint8_t Flag_SetEaSize(uint32_t size);

/**
  * @brief  一次性写"App状态 + A区大小 + B区状态 + B区大小"(子流程E收尾用)
  * @retval 0: 成功  1: 失败
  */
uint8_t Flag_SetAll(uint32_t app_st, uint32_t ea_size, uint32_t eb_st, uint32_t eb_size);

#endif /* __FLAG_SECTOR_H */
