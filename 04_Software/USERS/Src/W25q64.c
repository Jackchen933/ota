/*********************************************************************************************************
 * Copyright (C) 2026 EternalChip, Inc. (Gmbh) or its affiliates.
 *
 * @file W25q64.c
 *
 * @par dependencies
 *
 * - W25q64.h
 *
 * @brief 外部 SPI Flash W25Q64(8MB) 的底层驱动 —— HAL 版。
 *
 * @note 移植自 Bootloader 的 Core/Src/w25qxx.c。命令序列/超时值保持原样，
 *       只把 Bus 层从标准外设库换成 HAL：
 *         SPI1_WriteByte(buf,len,to)  ->  HAL_SPI_Transmit(&hspi1, buf, len, to)
 *         SPI1_ReadByte (buf,len,to)  ->  HAL_SPI_Receive (&hspi1, buf, len, to)
 *         millis()                    ->  HAL_GetTick()
 *         GPIO_WriteBit(...)          ->  HAL_GPIO_WritePin(...)
 *
 * @version V1.0
 *
 * @note 1 tab == 4 spaces!
 *
 * ********************************************************************************************************/

/* 包含头文件 *******************************************************************************************/
#include "W25q64.h"
#include "cmsis_os.h"      /* osDelay —— 等擦除时让出 CPU */

/* 私有宏 ***********************************************************************************************/

/* ---- W25Q64 命令字 ---- */
#define CMD_WRITE_ENABLE        0x06
#define CMD_READ_STATUS1        0x05
#define CMD_PAGE_PROGRAM        0x02
#define CMD_READ_DATA           0x03
#define CMD_SECTOR_ERASE        0x20    /* 擦 4KB 小扇区 */
#define CMD_CHIP_ERASE          0xC7
#define CMD_READ_ID             0x90
#define CMD_RESET_ENABLE        0x66
#define CMD_RESET_MEMORY        0x99

/* 状态寄存器1 的 BUSY 位 */
#define STATUS1_BUSY            0x01U

/* 单次 SPI 收发的超时(ms)。这里只搬几个字节到 4KB，
   100MHz/256 = 390kHz 下一个字节约 20us，4KB 约 84ms，给 500ms 余量。
   注意这【不是】等芯片内部写/擦完成的超时，那个是 W25Q64_TIMEOUT_MS */
#define SPI_XFER_TIMEOUT_MS     500U

/* HAL_SPI_Transmit/Receive 的 Size 参数是 uint16_t，
   一次传超过 65535 字节会被截断。长读必须分块，和原版一致 */
#define SPI_XFER_CHUNK          4096U

/* 片选。原版是 W25Qx_Enable()/Disable() 宏，包的是 GPIO_WriteBit */
#define W25Q_CS_LOW()           HAL_GPIO_WritePin(SPI1_cs_GPIO_Port, SPI1_cs_Pin, GPIO_PIN_RESET)
#define W25Q_CS_HIGH()          HAL_GPIO_WritePin(SPI1_cs_GPIO_Port, SPI1_cs_Pin, GPIO_PIN_SET)

/* 私有变量 *********************************************************************************************/
static uint32_t s_flash_size = 0;       /* Init 成功后填入，未初始化是 0 */
static uint16_t s_flash_id   = 0;

/* 私有函数 *********************************************************************************************/

/********************************************************************************************************
  * @函数名称：W25q64_WaitReady
  * @函数说明：轮询状态寄存器1的 BUSY 位，等芯片内部操作(页编程/擦除)结束
  * @入口参数：timeout_ms —— 最长等待时间
  * @返回参数：0: 空闲(可以下一条命令)   -1: 超时或 SPI 出错
  * @注意事项：SPI 出错也按超时返回。这样调用方不会把"读状态失败"误当成
  *           "芯片就绪"而直接放行 —— 那样会在芯片还在写的时候下一条命令。
  *******************************************************************************************************/
static int8_t W25q64_WaitReady(uint32_t timeout_ms)
{
    uint8_t  cmd = CMD_READ_STATUS1;
    uint8_t  status;
    uint32_t start = HAL_GetTick();

    for (;;)
    {
        W25Q_CS_LOW();

        if (HAL_SPI_Transmit(&hspi1, &cmd, 1U, SPI_XFER_TIMEOUT_MS) != HAL_OK)
        {
            W25Q_CS_HIGH();
            return -1;
        }
        if (HAL_SPI_Receive(&hspi1, &status, 1U, SPI_XFER_TIMEOUT_MS) != HAL_OK)
        {
            W25Q_CS_HIGH();
            return -1;
        }

        W25Q_CS_HIGH();

        if ((status & STATUS1_BUSY) == 0U)
        {
            return 0;       /* 空闲 */
        }

        if ((HAL_GetTick() - start) >= timeout_ms)
        {
            return -1;
        }

        /* 让出 CPU：擦除最长 400ms，纯忙等会把同优先级任务全饿死 */
        osDelay(1);
    }
}

/********************************************************************************************************
  * @函数名称：W25q64_WriteEnable
  * @函数说明：置位写使能锁存(WEL)，页编程/擦除前必须先发
  * @返回参数：0: 成功   -1: 失败
  *******************************************************************************************************/
static int8_t W25q64_WriteEnable(void)
{
    uint8_t cmd = CMD_WRITE_ENABLE;

    W25Q_CS_LOW();
    if (HAL_SPI_Transmit(&hspi1, &cmd, 1U, SPI_XFER_TIMEOUT_MS) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return -1;
    }
    W25Q_CS_HIGH();

    /* 原版在这里还等了一次 GetStatus，但刚发完 WEL 芯片并不是 BUSY，
       那个循环第一次判断就退出，等于白读一次状态。这里省掉。 */
    return 0;
}

/********************************************************************************************************
  * @函数名称：W25q64_Reset
  * @函数说明：软复位芯片(0x66 + 0x99)，让它从上电残留状态回到已知态
  *******************************************************************************************************/
static void W25q64_Reset(void)
{
    uint8_t cmd[2] = { CMD_RESET_ENABLE, CMD_RESET_MEMORY };

    W25Q_CS_LOW();
    (void)HAL_SPI_Transmit(&hspi1, cmd, 2U, SPI_XFER_TIMEOUT_MS);
    W25Q_CS_HIGH();

    /* 复位后芯片需要一小段时间(tRES1 约 30us)才接受新命令 */
    osDelay(1);
}

/********************************************************************************************************
  * @函数名称：W25q64_ReadId
  * @函数说明：读厂商/器件 ID(0x90)，16 位
  *******************************************************************************************************/
static uint16_t W25q64_ReadId(void)
{
    uint8_t cmd[4] = { CMD_READ_ID, 0x00, 0x00, 0x00 };
    uint8_t idt[2] = { 0, 0 };

    W25Q_CS_LOW();

    if (HAL_SPI_Transmit(&hspi1, cmd, 4U, SPI_XFER_TIMEOUT_MS) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return 0xFFFFU;
    }
    if (HAL_SPI_Receive(&hspi1, idt, 2U, SPI_XFER_TIMEOUT_MS) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return 0xFFFFU;
    }

    W25Q_CS_HIGH();

    return (uint16_t)(((uint16_t)idt[0] << 8) | (uint16_t)idt[1]);
}

/* 外部函数 *********************************************************************************************/

int8_t W25q64_Init(void)
{
    uint16_t id;
    uint8_t  density;

    W25q64_Reset();

    id = W25q64_ReadId();
    if (id == 0xFFFFU)
    {
        return -1;      /* 没接芯片，或 SPI 完全没通 */
    }

    /* 只看低字节的容量码，不看高字节厂商 ID。
       Winbond 是 0xEF，Puya(普冉) 等兼容型号是别的值，
       拿 0xEF 去比较会把兼容片全判成坏的。
       0x13=8Mbit 0x14=16M 0x15=32M 0x16=64M 0x17=128M 0x18=256M */
    density = (uint8_t)(id & 0xFFU);
    if ((density < 0x13U) || (density > 0x18U))
    {
        return -1;
    }

    /* 容量 = 2^(density-0x13) MBytes。用移位，不要引 powf() 把 libm 拖进来 */
    s_flash_size = (uint32_t)1U << (20 + (uint32_t)(density - 0x13U));
    s_flash_id   = id;

    return 0;
}

uint32_t W25q64_GetSize(void)
{
    return s_flash_size;
}

uint16_t W25q64_GetId(void)
{
    return s_flash_id;
}

int8_t W25q64_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint8_t cmd[4];

    if ((buf == NULL) || (len == 0U))
    {
        return -1;
    }
    if ((s_flash_size == 0U) || ((addr + len) > s_flash_size))
    {
        return -1;      /* 没初始化，或越过芯片末尾 */
    }

    cmd[0] = CMD_READ_DATA;
    cmd[1] = (uint8_t)(addr >> 16);
    cmd[2] = (uint8_t)(addr >> 8);
    cmd[3] = (uint8_t)(addr);

    W25Q_CS_LOW();

    if (HAL_SPI_Transmit(&hspi1, cmd, 4U, SPI_XFER_TIMEOUT_MS) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return -1;
    }

    /* 片选保持低电平，地址由芯片内部自增，一直读到完 */
    while (len > 0U)
    {
        uint16_t chunk = (len > SPI_XFER_CHUNK) ? (uint16_t)SPI_XFER_CHUNK : (uint16_t)len;

        if (HAL_SPI_Receive(&hspi1, buf, chunk, SPI_XFER_TIMEOUT_MS) != HAL_OK)
        {
            W25Q_CS_HIGH();
            return -1;
        }
        buf += chunk;
        len -= chunk;
    }

    W25Q_CS_HIGH();
    return 0;
}

int8_t W25q64_Write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint8_t  cmd[4];
    uint32_t current_addr;
    uint32_t page_left;         /* 当前页还剩多少字节可写 */
    uint32_t end_addr;

    if ((buf == NULL) || (len == 0U))
    {
        return -1;
    }
    if ((s_flash_size == 0U) || ((addr + len) > s_flash_size))
    {
        return -1;
    }

    current_addr = addr;
    end_addr     = addr + len;

    /* 第一页内还能写多少：不取模循环 256 累加，8MB 地址要循环三万多圈 */
    page_left = W25Q64_PAGE_SIZE - (current_addr % W25Q64_PAGE_SIZE);
    if (page_left > len)
    {
        page_left = len;
    }

    while (current_addr < end_addr)
    {
        cmd[0] = CMD_PAGE_PROGRAM;
        cmd[1] = (uint8_t)(current_addr >> 16);
        cmd[2] = (uint8_t)(current_addr >> 8);
        cmd[3] = (uint8_t)(current_addr);

        if (W25q64_WriteEnable() != 0)
        {
            return -1;
        }

        W25Q_CS_LOW();

        if (HAL_SPI_Transmit(&hspi1, cmd, 4U, SPI_XFER_TIMEOUT_MS) != HAL_OK)
        {
            W25Q_CS_HIGH();
            return -1;
        }
        if (HAL_SPI_Transmit(&hspi1, (uint8_t *)buf, (uint16_t)page_left,
                             SPI_XFER_TIMEOUT_MS) != HAL_OK)
        {
            W25Q_CS_HIGH();
            return -1;
        }

        W25Q_CS_HIGH();

        /* 等这一页写完再发下一页。超时从本页开始算 ——
           放在循环外的话，整段写得比 1 秒长(多页写必然如此)就会被误判超时 */
        if (W25q64_WaitReady(W25Q64_TIMEOUT_MS) != 0)
        {
            return -1;
        }

        current_addr += page_left;
        buf          += page_left;

        page_left = end_addr - current_addr;
        if (page_left > W25Q64_PAGE_SIZE)
        {
            page_left = W25Q64_PAGE_SIZE;
        }
    }

    return 0;
}

int8_t W25q64_EraseSector(uint32_t addr)
{
    uint8_t cmd[4];

    if ((s_flash_size == 0U) || (addr >= s_flash_size))
    {
        return -1;
    }

    /* 4KB 扇区擦除要求地址 4KB 对齐，不对齐芯片会忽略这条命令(数据没被擦) */
    if ((addr % W25Q64_SECTOR_SIZE) != 0U)
    {
        return -1;
    }

    cmd[0] = CMD_SECTOR_ERASE;
    cmd[1] = (uint8_t)(addr >> 16);
    cmd[2] = (uint8_t)(addr >> 8);
    cmd[3] = (uint8_t)(addr);

    if (W25q64_WriteEnable() != 0)
    {
        return -1;
    }

    W25Q_CS_LOW();
    if (HAL_SPI_Transmit(&hspi1, cmd, 4U, SPI_XFER_TIMEOUT_MS) != HAL_OK)
    {
        W25Q_CS_HIGH();
        return -1;
    }
    W25Q_CS_HIGH();

    /* 4KB 扇区擦除典型 45ms、最长 400ms */
    return W25q64_WaitReady(W25Q64_SECTOR_ERASE_TIMEOUT_MS);
}

int8_t W25q64_WriteSector(uint32_t sector_index, const uint8_t *buf)
{
    uint32_t addr;

    if (sector_index >= W25Q64_SECTOR_COUNT)
    {
        return -1;
    }

    addr = sector_index * W25Q64_SECTOR_SIZE;

    if (W25q64_EraseSector(addr) != 0)
    {
        return -1;
    }

    return W25q64_Write(addr, buf, W25Q64_SECTOR_SIZE);
}

int8_t W25q64_ReadSector(uint32_t sector_index, uint8_t *buf)
{
    if (sector_index >= W25Q64_SECTOR_COUNT)
    {
        return -1;
    }

    return W25q64_Read(sector_index * W25Q64_SECTOR_SIZE, buf, W25Q64_SECTOR_SIZE);
}

/*********************************** (C) COPYRIGHT 2026 EternalChip *****END OF FILE***********************/
