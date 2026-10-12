/*********************************************************************************************************
 * @file    OtaFlag.c
 *
 * @brief   App 侧「标志扇区」驱动实现(HAL 版)。
 *
 *          与 Bootloader 的 Tasks/Boot_Manager/FlagSector.c 是"同一份逻辑的
 *          两个工程副本": 字段布局、状态取值、镜像写入策略完全一致。
 *          那边是标准外设库(FLASH_EraseSector/ProgramWord), 这边全换成 HAL
 *          (HAL_FLASHEx_Erase / HAL_FLASH_Program), 判断方向注意别照抄。
 *
 * @version V1.0
 *
 * @note 1 tab == 4 spaces!
 *
 * ********************************************************************************************************/

/* 包含头文件 *******************************************************************************************/
#include "OtaFlag.h"

#include <string.h>

/* 私有宏 ***********************************************************************************************/

/* 扇区2 的 8 个字段(4 个原码 + 4 个反码), 一个字一个字写回 */
#define OTA_FLAG_WORDS          (8U)

/* 私有变量 *********************************************************************************************/

/* 标志扇区 RAM 镜像。所有写操作先改这里, 再整体落盘 ——
   和 Boot 侧相同的策略, 保证 8 个字永远同一次擦写产生, 无中间态 */
static uint32_t s_flag[OTA_FLAG_WORDS];

static uint8_t s_loaded = 0U;       /* 防呆: 没 Load 就写, 直接拒绝 */

/* 私有函数 *********************************************************************************************/

/********************************************************************************************************
  * @函数名称：OtaFlag_Commit
  * @函数说明：把 RAM 镜像整体落盘: 擦扇区2 -> 逐字写回 -> 读回校验
  * @返回参数：0: 成功   1: 失败
  * @注意事项：擦 16KB 扇区最长约 1 秒, 期间 F411(单 Bank) 从 Flash 取指全部
  *           stall, 整机表现为卡 1 秒(FreeRTOS 的 tick 也会丢几个, 无功能
  *           影响)。只能在任务上下文/内核启动前调用, 不要在中断里调。
  *******************************************************************************************************/
static uint8_t OtaFlag_Commit(void)
{
    FLASH_EraseInitTypeDef erase;
    uint32_t sector_error = 0U;
    uint32_t i;

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return 1U;
    }

    /* 整扇区擦除。Flash 只能 1 -> 0, 不擦直接写会得到旧值与新值的按位与 */
    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Sector       = FLASH_SECTOR_2;            /* 0x08008000, 16KB */
    erase.NbSectors    = 1U;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;     /* 本板 3.3V 供电 */

    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK)
    {
        (void)HAL_FLASH_Lock();
        return 1U;
    }

    /* 逐字(32bit)写回镜像。F4 内部 Flash 的最小可编程单位就是字 */
    for (i = 0U; i < OTA_FLAG_WORDS; i++)
    {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                              OTA_FLAG_ADDR + (i * 4U),
                              (uint64_t)s_flag[i]) != HAL_OK)
        {
            (void)HAL_FLASH_Lock();
            return 1U;
        }
    }

    (void)HAL_FLASH_Lock();

    /* 读回比对。写 Flash 理论上不会错, 但这里是"要不要升级"的判决书,
       值得多花几十微秒验一遍 */
    if (memcmp((const void *)OTA_FLAG_ADDR, s_flag, sizeof(s_flag)) != 0)
    {
        return 1U;
    }

    return 0U;
}

/* 外部函数 *********************************************************************************************/

uint8_t OtaFlag_Load(void)
{
    /* Flash 在程序视角是内存映射的, 直接拷 */
    memcpy(s_flag, (const void *)OTA_FLAG_ADDR, sizeof(s_flag));

    /* 反码校验: 原码 ^ 反码 必须等于 0xFFFFFFFF。
       空白扇区(全 0xFF)会校验失败, 按约定清 0 —— 状态 0 会被 Boot 当
       "其他"走路径D, 正是全新芯片该有的行为 */
    {
        static const uint32_t idx[4] = { 0U, 2U, 4U, 6U };  /* 原码字段下标 */
        uint8_t k;

        for (k = 0U; k < 4U; k++)
        {
            uint32_t raw = s_flag[idx[k]];
            uint32_t inv = s_flag[idx[k] + 1U];

            if ((raw ^ inv) != 0xFFFFFFFFUL)
            {
                s_flag[idx[k]]     = 0U;
                s_flag[idx[k] + 1U] = 0xFFFFFFFFUL;
            }
        }
    }

    s_loaded = 1U;
    return 0U;
}

uint32_t OtaFlag_GetAppState(void)
{
    return s_flag[0];
}

uint8_t OtaFlag_GetEaSize(uint32_t *size)
{
    if (s_flag[2] == 0U)
    {
        return 1U;      /* 空白或校验失败 */
    }

    if (size != NULL)
    {
        *size = s_flag[2];
    }
    return 0U;
}

uint8_t OtaFlag_SetAppState(uint32_t st)
{
    if ((s_loaded == 0U) || (st == 0U))
    {
        return 1U;
    }

    s_flag[0] = st;
    s_flag[1] = ~st;        /* 反码跟着走, Load 那边的校验靠它 */

    return OtaFlag_Commit();
}

uint8_t OtaFlag_SetDownloadDone(uint32_t size)
{
    if ((s_loaded == 0U) || (size == 0U))
    {
        return 1U;
    }

    /* 顺序: 先记大小, 再置状态。一个 Commit 同时落盘 */
    s_flag[2] = size;
    s_flag[3] = ~size;
    s_flag[0] = OTA_FLAG_ST_DL_PENDING;
    s_flag[1] = (~OTA_FLAG_ST_DL_PENDING);

    return OtaFlag_Commit();
}

/*********************************** (C) COPYRIGHT 2026 EternalChip *****END OF FILE***********************/
