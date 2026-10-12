/*********************************************************************************************************
 * @file    FlagSector.c
 * @brief   标志扇区驱动(内部Flash扇区2)实现, 布局见 FlagSector.h
 * @version V1.0 2026-09-23
 * ********************************************************************************************************/
#include "FlagSector.h"
#include "Flash.h"
#include <string.h>

/* RAM镜像: 读改写都走它, 写入时整体落盘 */
static Flag_TypeDef s_flag;
static uint8_t s_flag_loaded = 0;

/* 按字(4字节)写一组到标志扇区并读回校验, 失败返回1 */
static uint8_t Flag_ProgramWord(uint32_t offset, uint32_t data)
{
    uint32_t addr = FlagSectorAddress + offset;

    if (FLASH_ProgramWord(addr, data) != FLASH_COMPLETE)
    {
        return 1;
    }
    return (*(__IO uint32_t *)addr != data) ? 1 : 0;
}

/**
  * @brief  把RAM镜像整体落盘: 解锁 -> 擦扇区2 -> 按字重写32字节 -> 上锁 -> 读回校验
  * @retval 0: 成功  1: 失败
  * @note   每次改任何一个字段都要整扇区擦写一次, 这是内部Flash的物理粒度决定的。
  */
static uint8_t Flag_Commit(void)
{
    uint32_t *p = (uint32_t *)&s_flag;
    uint32_t i;

    if (1 == Flash_erase(FlagSectorAddress, FLAG_SECTOR_SIZE - 1))
    {
        return 1;
    }

    Flash_Unlock();
    for (i = 0; i < (sizeof(Flag_TypeDef) / 4); i++)
    {
        if (0 != Flag_ProgramWord(i * 4, p[i]))
        {
            Flash_Lock();
            return 1;
        }
    }
    Flash_Lock();
    return 0;
}

uint8_t Flag_Load(void)
{
    const Flag_TypeDef *hw = (const Flag_TypeDef *)FlagSectorAddress;

    memcpy(&s_flag, hw, sizeof(Flag_TypeDef));
    s_flag_loaded = 1;

    /* 空白扇区(刚出厂, 全0xFF): 状态字段原码是0xFFFFFFFF, 反码也是0xFFFFFFFF,
       原码==~反码不成立, 下面的校验会把它清0 -> 主线走路径D, 正确 */
    if (s_flag.AppSt != ~s_flag.AppStInv)     { s_flag.AppSt = 0; }
    if (s_flag.EaSize != ~s_flag.EaSizeInv)   { s_flag.EaSize = 0; }
    if (s_flag.EbSt != ~s_flag.EbStInv)       { s_flag.EbSt = 0; }
    if (s_flag.EbSize != ~s_flag.EbSizeInv)   { s_flag.EbSize = 0; }

    return 0;
}

uint32_t Flag_GetAppState(void)
{
    if (!s_flag_loaded)
    {
        (void)Flag_Load();
    }
    return s_flag.AppSt;
}

uint8_t Flag_GetEaSize(uint32_t *size)
{
    if (!s_flag_loaded)
    {
        (void)Flag_Load();
    }

    *size = s_flag.EaSize;

    /* 大小必须装得进外部Flash A区域, 0也是无效值 */
    if ((*size == 0) || (*size > W25Q64_APP_SIZE))
    {
        *size = 0;
        return 1;
    }
    return 0;
}

void Flag_GetEb(uint32_t *st, uint32_t *size)
{
    if (!s_flag_loaded)
    {
        (void)Flag_Load();
    }

    *st   = s_flag.EbSt;
    *size = s_flag.EbSize;

    if (*size > W25Q64_BAK_SIZE)
    {
        *size = 0;
    }
}

uint8_t Flag_SetAppState(uint32_t st)
{
    if (!s_flag_loaded)
    {
        (void)Flag_Load();
    }

    s_flag.AppSt    = st;
    s_flag.AppStInv = ~st;

    return Flag_Commit();
}

uint8_t Flag_SetEaSize(uint32_t size)
{
    if (!s_flag_loaded)
    {
        (void)Flag_Load();
    }

    s_flag.EaSize    = size;
    s_flag.EaSizeInv = ~size;

    return Flag_Commit();
}

uint8_t Flag_SetAll(uint32_t app_st, uint32_t ea_size, uint32_t eb_st, uint32_t eb_size)
{
    if (!s_flag_loaded)
    {
        (void)Flag_Load();
    }

    s_flag.AppSt      = app_st;
    s_flag.AppStInv   = ~app_st;
    s_flag.EaSize     = ea_size;
    s_flag.EaSizeInv  = ~ea_size;
    s_flag.EbSt       = eb_st;
    s_flag.EbStInv    = ~eb_st;
    s_flag.EbSize     = eb_size;
    s_flag.EbSizeInv  = ~eb_size;

    return Flag_Commit();
}
