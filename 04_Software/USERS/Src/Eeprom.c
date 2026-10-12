/*********************************************************************************************************
 * Copyright (C) 2026 EternalChip, Inc. (Gmbh) or its affiliates.
 *
 * @file Eeprom.c
 *
 * @par dependencies
 *
 * - Eeprom.h
 *
 * @brief AT24C02(256 字节 I2C EEPROM) 驱动 + OTA 状态存取。
 *
 * @version V1.0
 *
 * @note 1 tab == 4 spaces!
 *
 * ********************************************************************************************************/

/* 包含头文件 *******************************************************************************************/
#include "Eeprom.h"
#include "cmsis_os.h"      /* osDelay —— 等写周期时让出 CPU */
#include "bsp_i2c1_bus.h"  /* I2C1 总线互斥锁：022 移植后触摸芯片(CST816S)与
                              EEPROM 共享 I2C1，两个任务的事务必须互斥 */

/* 私有宏 ***********************************************************************************************/

/* AT24C02 的 7 位器件地址。A2/A1/A0 三根地址脚全接地 -> 0b1010000 = 0x50。
   HAL 用的是"左移一位后的 8 位地址"，所以要 <<1 */
#define AT24C02_ADDR_7BIT       (0x50U)
#define AT24C02_ADDR_HAL        (AT24C02_ADDR_7BIT << 1)

/* 单次 I2C 传输超时(ms)。100kHz 下传 2 字节约 200us，给 100ms 绰绰有余 */
#define I2C_XFER_TIMEOUT_MS     (100U)

/* 私有函数 *********************************************************************************************/

/********************************************************************************************************
  * @函数名称：Eeprom_WaitWriteDone
  * @函数说明：用"ACK 轮询"等一次写周期结束
  * @返回参数：0: 芯片已就绪   -1: 超时
  * @注意事项：比死等 5ms 更快 —— 芯片写完后会立刻应答地址，所以一旦 ACK 回来
  *           就可以走，典型只要 1~2ms。超时上限仍按最坏 5ms 给。
  *******************************************************************************************************/
static int8_t Eeprom_WaitWriteDone(void)
{
    uint32_t start = HAL_GetTick();

    for (;;)
    {
        /* 只发地址不发数据：芯片应答 = 内部写周期已结束。
           HAL_I2C_IsDeviceReady 就是干这个的，省得自己拼 */
        bsp_i2c1_lock();
        if (HAL_I2C_IsDeviceReady(&hi2c1, AT24C02_ADDR_HAL, 1U,
                                  I2C_XFER_TIMEOUT_MS) == HAL_OK)
        {
            bsp_i2c1_unlock();
            return 0;
        }
        bsp_i2c1_unlock();

        if ((HAL_GetTick() - start) >= EEPROM_WRITE_CYCLE_MS)
        {
            return -1;
        }

        osDelay(1);
    }
}

/* 外部函数 *********************************************************************************************/

int8_t Eeprom_Init(void)
{
    /* 上电后 EEPROM 可能还在做上一次的写周期，多给几次机会 */
    bsp_i2c1_lock();
    if (HAL_I2C_IsDeviceReady(&hi2c1, AT24C02_ADDR_HAL, 3U,
                              I2C_XFER_TIMEOUT_MS) != HAL_OK)
    {
        bsp_i2c1_unlock();
        return -1;
    }
    bsp_i2c1_unlock();
    return 0;
}

int8_t Eeprom_Read(uint8_t mem_addr, uint8_t *buf, uint16_t len)
{
    if ((buf == NULL) || (len == 0U))
    {
        return -1;
    }

    /* AT24C02 的内存地址是 8 位，读写都要先发一个字节的地址 */
    bsp_i2c1_lock();
    if (HAL_I2C_Mem_Read(&hi2c1,
                         AT24C02_ADDR_HAL,
                         (uint16_t)mem_addr,
                         I2C_MEMADD_SIZE_8BIT,
                         buf,
                         len,
                         I2C_XFER_TIMEOUT_MS) != HAL_OK)
    {
        bsp_i2c1_unlock();
        return -1;
    }
    bsp_i2c1_unlock();
    return 0;
}

int8_t Eeprom_Write(uint8_t mem_addr, const uint8_t *buf, uint16_t len)
{
    uint16_t written = 0;

    if ((buf == NULL) || (len == 0U))
    {
        return -1;
    }

    if (((uint32_t)mem_addr + len) > EEPROM_SIZE)
    {
        return -1;      /* 越界，会绕回 0 地址把前面的数据覆盖掉 */
    }

    while (written < len)
    {
        uint8_t  addr  = (uint8_t)(mem_addr + written);
        uint16_t chunk;

        /* 本页还剩多少字节可写。AT24C02 页写【不会】自动进位到下一页，
           而是回绕到本页开头 —— 所以超过页边界的部分必须切开单独发，
           否则页尾的几个字节会被页头的数据覆盖。 */
        chunk = (uint16_t)(EEPROM_PAGE_SIZE - (addr % EEPROM_PAGE_SIZE));
        if (chunk > (uint16_t)(len - written))
        {
            chunk = (uint16_t)(len - written);
        }

        bsp_i2c1_lock();
        if (HAL_I2C_Mem_Write(&hi2c1,
                              AT24C02_ADDR_HAL,
                              (uint16_t)addr,
                              I2C_MEMADD_SIZE_8BIT,
                              (uint8_t *)&buf[written],
                              chunk,
                              I2C_XFER_TIMEOUT_MS) != HAL_OK)
        {
            bsp_i2c1_unlock();
            return -1;
        }
        bsp_i2c1_unlock();

        /* 每页写完都要等写周期，不然下一次传输会被芯片 NAK */
        if (Eeprom_WaitWriteDone() != 0)
        {
            return -1;
        }

        written = (uint16_t)(written + chunk);
    }

    return 0;
}

uint8_t Eeprom_ReadByte(uint8_t mem_addr)
{
    uint8_t val = 0xFF;

    if (Eeprom_Read(mem_addr, &val, 1U) != 0)
    {
        return 0xFF;        /* 读失败统一返回 0xFF，调用方按 0xFF 判无效 */
    }
    return val;
}

int8_t Eeprom_WriteByte(uint8_t mem_addr, uint8_t val)
{
    return Eeprom_Write(mem_addr, &val, 1U);
}

/********************************************************************************************************
  * OTA 状态存取 —— 直接对应 Eeprom.h 里的地址表。
  * 大小字段统一按【小端】存(低字节在低地址)，和 ARM 的内存布局一致，
  * 上位机/服务器那边也按小端解析。
  *******************************************************************************************************/

uint8_t OtaEe_GetState(void)
{
    return Eeprom_ReadByte(EE_ADDR_OTA_STATE);
}

int8_t OtaEe_SetState(uint8_t state)
{
    return Eeprom_WriteByte(EE_ADDR_OTA_STATE, state);
}

uint32_t OtaEe_GetNewAppSize(void)
{
    uint8_t  b[4];
    uint32_t size;

    if (Eeprom_Read(EE_ADDR_NEW_APP_SIZE, b, 4U) != 0)
    {
        return 0xFFFFFFFFU;
    }

    size = ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[1] <<  8) |  (uint32_t)b[0];
    return size;
}

int8_t OtaEe_SetNewAppSize(uint32_t size)
{
    uint8_t b[4];

    b[0] = (uint8_t)(size);
    b[1] = (uint8_t)(size >> 8);
    b[2] = (uint8_t)(size >> 16);
    b[3] = (uint8_t)(size >> 24);

    return Eeprom_Write(EE_ADDR_NEW_APP_SIZE, b, 4U);
}

uint32_t OtaEe_GetCurAppSize(void)
{
    uint8_t  b[4];
    uint32_t size;

    if (Eeprom_Read(EE_ADDR_CUR_APP_SIZE, b, 4U) != 0)
    {
        return 0xFFFFFFFFU;
    }

    size = ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[1] <<  8) |  (uint32_t)b[0];
    return size;
}

int8_t OtaEe_SetCurAppSize(uint32_t size)
{
    uint8_t b[4];

    b[0] = (uint8_t)(size);
    b[1] = (uint8_t)(size >> 8);
    b[2] = (uint8_t)(size >> 16);
    b[3] = (uint8_t)(size >> 24);

    return Eeprom_Write(EE_ADDR_CUR_APP_SIZE, b, 4U);
}

/*********************************** (C) COPYRIGHT 2026 EternalChip *****END OF FILE***********************/
