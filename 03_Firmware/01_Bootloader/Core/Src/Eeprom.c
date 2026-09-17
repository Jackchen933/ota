/*********************************************************************************************************
 * Copyright (C) 2026 EternalChip, Inc. (Gmbh) or its affiliates.
 *
 * ALL Rights Reserved.
 *
 * @file Eeprom.c
 *
 * @par dependencies
 *
 * - Eeprom.h
 * - Spi.h     (只是为了借里面的 millis() 做超时, 不访问 SPI 总线)
 *
 * @brief AT24C02 驱动(标准外设库 + 硬件 I2C1 PB6/PB7)。
 *
 *        整个文件只有一件事要做对: I2C 状态机。AT24C02 本身不复杂,
 *        复杂的是 STM32F4 的 I2C 外设在出错(比如器件回 NACK)之后会
 *        卡在半个事务里, 不手动收尾的话后面每一次读写都会跟着失败。
 *        所以下面每个错误分支都调 I2c_ReleaseBus() 把总线放掉。
 *
 * Processing flow:
 *
 * call directly.
 *
 * @version V1.0 2026-09-16
 *
 * @note 1 tab == 4 spaces!
 *
 * ********************************************************************************************************/

/* Includes ---------------------------------------------------------------------------------------------*/
#include "Eeprom.h"
#include "Spi.h"
#include <string.h>

/* Private define ---------------------------------------------------------------------------------------*/

/* 一次 I2C 事务的超时。这个值只用来兜底 —— 正常情况下每个事件都是微秒级 */
#define I2C_XFER_TIMEOUT_MS     (100U)

/* 探测器件时等 START/地址的超时。器件正忙时会 NACK, 这是预期内的,
   所以要短, 不然轮询一次写周期就等掉几百毫秒 */
#define I2C_PING_TIMEOUT_MS     (2U)

/* 7 位地址左移一位后的完整字节 */
#define EEPROM_ADDR_WRITE       (uint8_t)(EEPROM_ADDR_7BIT << 1)
#define EEPROM_ADDR_READ        (uint8_t)((EEPROM_ADDR_7BIT << 1) | 0x01U)

/* Private function prototypes --------------------------------------------------------------------------*/
static uint8_t I2c_WaitEvent(uint32_t event, uint32_t timeout_ms);
static uint8_t I2c_WaitIdle(uint32_t timeout_ms);
static void    I2c_ReleaseBus(void);
static uint8_t Eeprom_Ping(void);
static uint8_t Eeprom_WaitWriteCycle(void);

/* Private functions ------------------------------------------------------------------------------------*/

/**
  * @brief  等一个 I2C 事件, 带超时
  * @param  event:      I2C_EVENT_xxx
  * @param  timeout_ms: 超时
  * @retval 0: 等到   1: 超时
  * @note   用 I2C_CheckEvent 而不是自己读 SR1/SR2, 是因为它内部会把两个
  *         状态寄存器都读一遍 —— 这恰好是清 ADDR 标志所需的动作,
  *         自己写很容易漏掉那一步。
  */
static uint8_t I2c_WaitEvent(uint32_t event, uint32_t timeout_ms)
{
    uint32_t start = millis();

    while (I2C_CheckEvent(I2C1, event) != SUCCESS)
    {
        if ((millis() - start) > timeout_ms)
        {
            return 1;
        }
    }
    return 0;
}

/**
  * @brief  等总线空闲
  * @retval 0: 空闲   1: 超时(总线被某个从机拉住了)
  */
static uint8_t I2c_WaitIdle(uint32_t timeout_ms)
{
    uint32_t start = millis();

    while (I2C_GetFlagStatus(I2C1, I2C_FLAG_BUSY) != RESET)
    {
        if ((millis() - start) > timeout_ms)
        {
            return 1;
        }
    }
    return 0;
}

/**
  * @brief  出错收尾: 清 AF 标志 + 发 STOP + 读掉 SR1/SR2
  * @note   器件回 NACK 时 AF(应答失败)标志会置起来, 且 SCL 被主机拉低不释放。
  *         不清 AF、不发 STOP, I2C1 就一直停在这次事务里, 下一次 START
  *         根本起不来 —— 表现是"第一次读失败之后次次都失败"。
  */
static void I2c_ReleaseBus(void)
{
    I2C_ClearFlag(I2C1, I2C_FLAG_AF);
    I2C_GenerateSTOP(I2C1, ENABLE);
}

/**
  * @brief  发一次 START + 器件地址, 看器件回不回 ACK
  * @retval 0: 器件应答(写周期已结束)   1: 没应答(还在忙 / 器件不在)
  * @note   只用来看"活没活着", 不读写任何数据。收尾动作放在这里做,
  *         调用方不用管。
  */
static uint8_t Eeprom_Ping(void)
{
    uint8_t ok = 1;

    I2C_GenerateSTART(I2C1, ENABLE);
    if (0 == I2c_WaitEvent(I2C_EVENT_MASTER_MODE_SELECT, I2C_PING_TIMEOUT_MS))
    {
        I2C_Send7bitAddress(I2C1, EEPROM_ADDR_WRITE, I2C_Direction_Transmitter);
        if (0 == I2c_WaitEvent(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED,
                               I2C_PING_TIMEOUT_MS))
        {
            ok = 0;
        }
    }

    I2c_ReleaseBus();
    return ok;
}

/**
  * @brief  等 AT24C02 的写周期结束
  * @retval 0: 写完   1: 超时
  * @note   做 ACK 轮询而不是死等 5ms: 器件在内部写的时候不响应地址(回 NACK),
  *         写完就回 ACK。多数情况下几百微秒就回来了, 比死等快得多,
  *         5ms 只是兜底上限。
  */
static uint8_t Eeprom_WaitWriteCycle(void)
{
    uint32_t start = millis();

    while ((millis() - start) <= EEPROM_WRITE_CYCLE_MS)
    {
        if (0 == Eeprom_Ping())
        {
            return 0;
        }
    }
    return 1;
}

/* Exported functions -----------------------------------------------------------------------------------*/

/**
  * @brief  初始化 I2C1(PB6=SCL, PB7=SDA) 并探测 AT24C02
  * @param  None
  * @retval 0: 成功   1: 失败
  */
uint8_t Eeprom_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    I2C_InitTypeDef  I2C_InitStructure;

    /* 时钟顺序: 先外设再 GPIO。和 APP 侧 CubeMX 生成的 MspInit 保持一致 */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C1, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);

    /* AT24C02 是开漏总线, 必须配成 AF + 开漏。
       模块板上一般已经有 4.7K 上拉, 这里再开内部上拉做兜底 ——
       没有外部上拉时, 只靠内部那几十 K 上拉, 100kHz 是跑不动的,
       所以接线一定要有外部上拉电阻 */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_6 | GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_OD;
    GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    GPIO_PinAFConfig(GPIOB, GPIO_PinSource6, GPIO_AF_I2C1);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource7, GPIO_AF_I2C1);

    I2C_DeInit(I2C1);

    I2C_InitStructure.I2C_Mode                = I2C_Mode_I2C;
    I2C_InitStructure.I2C_DutyCycle           = I2C_DutyCycle_2;
    I2C_InitStructure.I2C_OwnAddress1         = 0x00;   /* 只做主机, 这个值用不上 */
    I2C_InitStructure.I2C_Ack                 = I2C_Ack_Enable;
    I2C_InitStructure.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_InitStructure.I2C_ClockSpeed          = 100000; /* 100kHz, 和 APP 侧一致 */
    I2C_Init(I2C1, &I2C_InitStructure);

    I2C_Cmd(I2C1, ENABLE);

    /* 探一下器件。EEPROM 没焊 / 接线不对时这里返回 1,
       上层会当作"没有升级请求"直接跳过, 不会卡在 Boot 里 */
    if (I2c_WaitIdle(I2C_XFER_TIMEOUT_MS) != 0)
    {
        return 1;
    }
    return Eeprom_Ping();
}

/**
  * @brief  从 mem_addr 开始读 len 个字节
  * @param  mem_addr: EEPROM 内部地址(0 ~ 255)
  * @param  buf:      接收缓冲
  * @param  len:      长度
  * @retval 0: 成功   1: 失败
  */
uint8_t Eeprom_Read(uint8_t mem_addr, uint8_t *buf, uint16_t len)
{
    uint16_t i;

    if ((NULL == buf) || (0 == len))
    {
        return 1;
    }
    /* 越界直接判错, 不让地址在器件内部回绕到 0 */
    if (((uint32_t)mem_addr + len) > EEPROM_SIZE)
    {
        return 1;
    }

    if (I2c_WaitIdle(I2C_XFER_TIMEOUT_MS) != 0)
    {
        return 1;
    }

    /* ---- 第一步: 只写内存地址, 不发数据(所谓"伪写") ---- */
    I2C_GenerateSTART(I2C1, ENABLE);
    if (I2c_WaitEvent(I2C_EVENT_MASTER_MODE_SELECT, I2C_XFER_TIMEOUT_MS) != 0)
    {
        I2c_ReleaseBus();
        return 1;
    }

    I2C_Send7bitAddress(I2C1, EEPROM_ADDR_WRITE, I2C_Direction_Transmitter);
    if (I2c_WaitEvent(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED,
                      I2C_XFER_TIMEOUT_MS) != 0)
    {
        I2c_ReleaseBus();
        return 1;
    }

    /* AT24C02 是 8 位内存地址, 只发一个字节。
       AT24C32/64 要发两个字节, 换片子时这里要改 */
    I2C_SendData(I2C1, mem_addr);
    if (I2c_WaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED,
                      I2C_XFER_TIMEOUT_MS) != 0)
    {
        I2c_ReleaseBus();
        return 1;
    }

    /* ---- 第二步: 重启 START, 这次用读方向 ---- */
    I2C_GenerateSTART(I2C1, ENABLE);
    if (I2c_WaitEvent(I2C_EVENT_MASTER_MODE_SELECT, I2C_XFER_TIMEOUT_MS) != 0)
    {
        I2c_ReleaseBus();
        return 1;
    }

    I2C_Send7bitAddress(I2C1, EEPROM_ADDR_READ, I2C_Direction_Receiver);
    if (I2c_WaitEvent(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED,
                      I2C_XFER_TIMEOUT_MS) != 0)
    {
        I2c_ReleaseBus();
        return 1;
    }

    for (i = 0; i < len; i++)
    {
        if ((uint16_t)(i + 1) == len)
        {
            /* 最后一个字节: 必须【先 NACK + STOP, 再等数据】。
               反过来的话, 等数据期间主机已经把 ACK 发出去了, 器件会
               接着吐下一个字节并继续占着总线, 下一次事务就起不来。
               单字节读(len==1)走的也是这条分支。 */
            I2C_AcknowledgeConfig(I2C1, DISABLE);
            I2C_GenerateSTOP(I2C1, ENABLE);
        }

        if (I2c_WaitEvent(I2C_EVENT_MASTER_BYTE_RECEIVED,
                          I2C_XFER_TIMEOUT_MS) != 0)
        {
            I2C_AcknowledgeConfig(I2C1, ENABLE);    /* 恢复 ACK, 否则后面全读不了 */
            I2c_ReleaseBus();
            return 1;
        }
        buf[i] = I2C_ReceiveData(I2C1);
    }

    /* 收完把 ACK 恢复回来。忘了这一步的话, 下一次读只会收到第一个字节,
       而且现象很隐蔽 —— 读状态字拿到的是对的, 读大小就全错 */
    I2C_AcknowledgeConfig(I2C1, ENABLE);

    return 0;
}

/**
  * @brief  从 mem_addr 开始写 len 个字节
  * @param  mem_addr: EEPROM 内部地址(0 ~ 255)
  * @param  buf:      数据
  * @param  len:      长度
  * @retval 0: 成功   1: 失败
  * @note   AT24C02 一页 8 字节, 页写【在本页内回绕】而不是自动进位:
  *         从页内偏移 6 开始写 8 个字节, 第 3 个字节会绕回本页偏移 0,
  *         把最开始写的两个字节覆盖掉。所以必须按 8 字节页边界切分,
  *         一页一个事务。
  */
uint8_t Eeprom_Write(uint8_t mem_addr, const uint8_t *buf, uint16_t len)
{
    uint16_t done = 0;

    if ((NULL == buf) || (0 == len))
    {
        return 1;
    }
    if (((uint32_t)mem_addr + len) > EEPROM_SIZE)
    {
        return 1;
    }

    while (done < len)
    {
        uint8_t  cur       = (uint8_t)(mem_addr + done);
        uint16_t page_left = (uint16_t)(EEPROM_PAGE_SIZE - (cur & (EEPROM_PAGE_SIZE - 1U)));
        uint16_t n         = ((uint16_t)(len - done) < page_left) ? (uint16_t)(len - done)
                                                                  : page_left;
        uint16_t i;

        if (I2c_WaitIdle(I2C_XFER_TIMEOUT_MS) != 0)
        {
            return 1;
        }

        I2C_GenerateSTART(I2C1, ENABLE);
        if (I2c_WaitEvent(I2C_EVENT_MASTER_MODE_SELECT, I2C_XFER_TIMEOUT_MS) != 0)
        {
            I2c_ReleaseBus();
            return 1;
        }

        I2C_Send7bitAddress(I2C1, EEPROM_ADDR_WRITE, I2C_Direction_Transmitter);
        if (I2c_WaitEvent(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED,
                          I2C_XFER_TIMEOUT_MS) != 0)
        {
            I2c_ReleaseBus();
            return 1;
        }

        /* 先发内存地址 */
        I2C_SendData(I2C1, cur);
        if (I2c_WaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTING,
                          I2C_XFER_TIMEOUT_MS) != 0)
        {
            I2c_ReleaseBus();
            return 1;
        }

        /* 再发本页的数据 */
        for (i = 0; i < n; i++)
        {
            I2C_SendData(I2C1, buf[done + i]);
            if (I2c_WaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTING,
                              I2C_XFER_TIMEOUT_MS) != 0)
            {
                I2c_ReleaseBus();
                return 1;
            }
        }

        /* 等最后一个字节真正出去(BTF 置起)再发 STOP。
           提前发 STOP 会把末尾一两个字节截断, 现象是"偶尔少写一个字节",
           而且只在大批量写的时候偶发, 很难查 */
        if (I2c_WaitEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED,
                          I2C_XFER_TIMEOUT_MS) != 0)
        {
            I2c_ReleaseBus();
            return 1;
        }

        I2C_GenerateSTOP(I2C1, ENABLE);

        /* 等器件把这一页真正写进存储阵列。不等就发下一帧, 器件正忙会整体 NACK */
        if (Eeprom_WaitWriteCycle() != 0)
        {
            return 1;
        }

        done += n;
    }

    return 0;
}

/**
  * @brief  读 1 字节
  * @retval 0: 成功   1: 失败
  */
uint8_t Eeprom_ReadByte(uint8_t mem_addr, uint8_t *val)
{
    return Eeprom_Read(mem_addr, val, 1);
}

/**
  * @brief  写 1 字节
  * @retval 0: 成功   1: 失败
  */
uint8_t Eeprom_WriteByte(uint8_t mem_addr, uint8_t val)
{
    return Eeprom_Write(mem_addr, &val, 1);
}

/**
  * @brief  读 4 字节小端整数
  * @retval 0: 成功   1: 失败
  * @note   小端字节序必须和 APP 侧 Eeprom.c 的写法一致,
  *         不然 Boot 读出来的 AppSize 是反的(会是个天文数字, 直接被上界挡掉)
  */
uint8_t Eeprom_ReadU32(uint8_t mem_addr, uint32_t *val)
{
    uint8_t raw[4];

    if (NULL == val)
    {
        return 1;
    }
    if (Eeprom_Read(mem_addr, raw, 4) != 0)
    {
        return 1;
    }

    *val = ((uint32_t)raw[3] << 24) | ((uint32_t)raw[2] << 16) |
           ((uint32_t)raw[1] <<  8) |  (uint32_t)raw[0];
    return 0;
}

/**
  * @brief  写 4 字节小端整数
  * @retval 0: 成功   1: 失败
  */
uint8_t Eeprom_WriteU32(uint8_t mem_addr, uint32_t val)
{
    uint8_t raw[4];

    raw[0] = (uint8_t)(val      );
    raw[1] = (uint8_t)(val >>  8);
    raw[2] = (uint8_t)(val >> 16);
    raw[3] = (uint8_t)(val >> 24);

    return Eeprom_Write(mem_addr, raw, 4);
}

/*********************************** (C) COPYRIGHT 2026 EternalChip *****END OF FILE***********************/
