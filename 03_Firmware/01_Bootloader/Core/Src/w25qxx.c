/* 原文件是基于 HAL 库写的(HAL_SPI_Transmit / hspi1 / HAL_GetTick),
   本工程用的是标准外设库, 这里已全部换成 Bsp/Spi.c 里的
   SPI1_WriteByte / SPI1_ReadByte / millis()。 */

#include "w25qxx.h"

/* 等待 Flash 内部操作结束一律用 millis() 轮询, 不要在这里写忙等死循环:
   W25Qx_WriteEnable() 可能被中断上下文调用, 轮询带超时才能安全退出。 */
#define get_tick() millis()

/* SPI1_ReadByte()/SPI1_WriteByte() 的长度参数是 u16, 一次传超过 65535 字节
   会被截断成一个小得多的值, 剩下的数据发不出来。所以长读必须分块。 */
#define W25QXXXX_XFER_CHUNK 4096

W25Qx_Parameter W25Qx_Para;

/* Private function prototypes -----------------------------------------------*/
static void    W25Qx_Reset(void);
static uint8_t W25Qx_GetStatus(void);

/**
  * @brief  Initializes the W25QXXXX interface.
  * @retval None
  * @note   本函数只做芯片复位 + 读参数, 不初始化 SPI 总线。
  *         SPI1_Init() 必须由调用方先调好(Bsp/Spi.c)。
  */
uint8_t W25Qx_Init(void)
{
	uint8_t state;
	/* Reset W25Qxxx */
	W25Qx_Reset();

	state = W25Qx_Get_Parameter(&W25Qx_Para);

	return state;
}

/**
  * @brief  This function reset the W25Qx.
  * @retval None
  */
static void	W25Qx_Reset(void)
{
	uint8_t cmd[2] = {RESET_ENABLE_CMD,RESET_MEMORY_CMD};

	W25Qx_Enable();
	/* Send the reset command */
	SPI1_WriteByte(cmd, 2, W25QXXXX_TIMEOUT_VALUE);
	W25Qx_Disable();

}

/**
  * @brief  Reads current status of the W25QXXXX.
  * @retval W25Qx_OK: 空闲(可以继续下命令)
  *         W25Qx_BUSY: 芯片内部还在写/擦
  * @note   SPI 超时也返回 W25Qx_BUSY。这样调用方的
  *         while(W25Qx_GetStatus() == W25Qx_BUSY) 循环会走到自己的超时分支,
  *         而不是把一次失败的读取当成"芯片就绪"直接放行。
  */
static uint8_t W25Qx_GetStatus(void)
{
	uint8_t cmd[] = {READ_STATUS_REG1_CMD};
	uint8_t status = 0;

	W25Qx_Enable();

	/* Send the read status command */
	if (SPI1_WriteByte(cmd, 1, W25QXXXX_TIMEOUT_VALUE) == 0)
	{
		W25Qx_Disable();
		return W25Qx_BUSY;
	}
	/* Reception of the data */
	if (SPI1_ReadByte(&status, 1, W25QXXXX_TIMEOUT_VALUE) == 0)
	{
		W25Qx_Disable();
		return W25Qx_BUSY;
	}
	W25Qx_Disable();

	/* Check the value of the register */
  if((status & W25QXXXX_FSR_BUSY) != 0)
  {
    return W25Qx_BUSY;
  }
	else
	{
		return W25Qx_OK;
	}
}

/**
  * @brief  This function send a Write Enable and wait it is effective.
  * @retval None
  */
uint8_t W25Qx_WriteEnable(void)
{
	uint8_t cmd[] = {WRITE_ENABLE_CMD};
	uint32_t tickstart = get_tick();

	/*Select the FLASH: Chip Select low */
	W25Qx_Enable();
	/* Send the write enable command */
	if (SPI1_WriteByte(cmd, 1, W25QXXXX_TIMEOUT_VALUE) == 0)
	{
		W25Qx_Disable();
		return W25Qx_TIMEOUT;
	}
	/*Deselect the FLASH: Chip Select high */
	W25Qx_Disable();

	/* Wait the end of Flash writing */
	while(W25Qx_GetStatus() == W25Qx_BUSY)
	{
		/* Check for the Timeout */
    if((get_tick() - tickstart) > W25QXXXX_TIMEOUT_VALUE)
    {
			return W25Qx_TIMEOUT;
    }
	}

	return W25Qx_OK;
}

/**
  * @brief  Read Manufacture/Device ID.
	* @param  ID: 返回值地址
	*         16 位, 高字节是厂商 ID, 低字节是容量代码:
	*           0XEF13 -> W25Q80    0XEF14 -> W25Q16
	*           0XEF15 -> W25Q32    0XEF16 -> W25Q64
	*           0XEF17 -> W25Q128
	*         注意高字节不一定是 0xEF: 兼容型号 Puya(普冉) 是 0x85,
	*         所以判断容量只看低字节。
  * @retval None
  */
void W25Qx_Read_ID(uint16_t *ID)
{
	uint8_t idt[2] = {0, 0};

	uint8_t cmd[4] = {READ_ID_CMD,0x00,0x00,0x00};

	W25Qx_Enable();
	/* Send the read ID command */
	if (SPI1_WriteByte(cmd, 4, W25QXXXX_TIMEOUT_VALUE) == 0)
	{
		W25Qx_Disable();
		*ID = 0xFFFF;
		return;
	}
	/* Reception of the data */
	if (SPI1_ReadByte(idt, 2, W25QXXXX_TIMEOUT_VALUE) == 0)
	{
		W25Qx_Disable();
		*ID = 0xFFFF;
		return;
	}

	*ID = (idt[0] << 8) + idt[1];

	W25Qx_Disable();

}

/**
  * @brief  Get W25QX Parameter.
  * @param  Para: W25Qx_Parameter
  * @retval W25Qx_OK / W25Qx_ERROR
  */
uint8_t W25Qx_Get_Parameter(W25Qx_Parameter *Para)
{
	uint16_t id = 0;
	uint8_t  device_id;
	uint32_t size;

	Para->PAGE_SIZE = 256;
	Para->SUBSECTOR_SIZE = 4096;
	Para->SECTOR_SIZE = 0x10000;

	W25Qx_Read_ID(&id);
	/* Only the low byte is the density code, and it is vendor independent:
	   0x13=8Mbit 0x14=16Mbit 0x15=32Mbit 0x16=64Mbit 0x17=128Mbit 0x18=256Mbit.
	   The high byte is the vendor ID (Winbond 0xEF, Puya 0x85, ...) and must
	   not be compared against 0xEF, or compatible parts get rejected. */
	device_id = (uint8_t)(id & 0xFF);
	if(device_id < 0x13 || device_id > 0x18) return W25Qx_ERROR;

	/* 容量 = 2^(device_id-0x13) MBytes。原来用 powf() 算, 那会把整个 libm
	   拉进工程; 2 的整数次幂用移位就够了: 0x13 -> 1<<20 = 1MB。 */
	size = (uint32_t)1 << (20 + (device_id - 0x13));

	Para->FLASH_ID = id;
	Para->FLASH_SIZE = size;
	Para->SUBSECTOR_COUNT = Para->FLASH_SIZE / Para->SUBSECTOR_SIZE;
	Para->SECTOR_COUNT = Para->FLASH_SIZE / Para->SECTOR_SIZE;

	return W25Qx_OK;
}
/**
  * @brief  Reads an amount of data from the W25QXXXX memory.
  * @param  pData: Pointer to data to be read
  * @param  ReadAddr: Read start address
  * @param  Size: Size of data to read
  * @retval W25Qx_OK / W25Qx_ERROR
  * @note   读可以跨页、跨扇区, 随便什么长度; 但受 u16 长度参数限制,
  *         这里按 W25QXXXX_XFER_CHUNK 分段传。
  */
uint8_t W25Qx_Read(uint8_t* pData, uint32_t ReadAddr, uint32_t Size)
{
	uint8_t cmd[4];

	/* Configure the command */
	cmd[0] = READ_CMD;
	cmd[1] = (uint8_t)(ReadAddr >> 16);
	cmd[2] = (uint8_t)(ReadAddr >> 8);
	cmd[3] = (uint8_t)(ReadAddr);

	W25Qx_Enable();
	/* Send the read command */
	if (SPI1_WriteByte(cmd, 4, W25QXXXX_TIMEOUT_VALUE) == 0)
	{
		W25Qx_Disable();
		return W25Qx_ERROR;
	}

	/* 剩下的数据可以慢慢读, 片选一直保持低电平, 读完再拉高 */
	while (Size > 0)
	{
		uint16_t chunk = (Size > W25QXXXX_XFER_CHUNK) ? (uint16_t)W25QXXXX_XFER_CHUNK
		                                             : (uint16_t)Size;
		if (SPI1_ReadByte(pData, chunk, W25QXXXX_TIMEOUT_VALUE) == 0)
		{
			W25Qx_Disable();
			return W25Qx_ERROR;
		}
		pData += chunk;
		Size -= chunk;   /* 地址由芯片内部自增, 不用再发 */
	}
	W25Qx_Disable();
	return W25Qx_OK;
}

/**
  * @brief  Writes an amount of data to the W25QXXXX memory.
  * @param  pData: Pointer to data to be written
  * @param  WriteAddr: Write start address
  * @param  Size: Size of data to write
  * @retval W25Qx_OK / W25Qx_ERROR / W25Qx_TIMEOUT
  * @note   长度不再限于 256 字节: 内部会按页(256B)自动切分, 一次可以写任意长度。
  *         但每页只能写一次, 跨页写=多次"写使能+页编程", 所以整段数据必须
  *         先擦过(全 0xFF), 而且中途掉电会留下写了一半的页。
  */
uint8_t W25Qx_Write(uint8_t* pData, uint32_t WriteAddr, uint32_t Size)
{
	uint8_t cmd[4];
	uint32_t end_addr, current_size, current_addr;

	if (Size == 0) return W25Qx_OK;

	/* 算出第一页还能写多少字节(页内偏移之后剩下的部分)。
	 * 原实现用 while 累加 256 到超过 WriteAddr 来求, 8MB 地址要循环 3 万多次;
	 * 取模一步就等价地算出来了。 */
	current_size = W25QXXXX_PAGE_SIZE - (WriteAddr % W25QXXXX_PAGE_SIZE);

  /* Check if the size of the data is less than the remaining place in the page */
  if (current_size > Size)
  {
    current_size = Size;
  }

  /* Initialize the adress variables */
  current_addr = WriteAddr;
  end_addr = WriteAddr + Size;

	/* Perform the write page by page */
	do
	{
		/* 超时起点必须放在循环里: 放在循环外的话, 只要整段写得比
		   1000ms 长(多页写一定会), 从第二页开始就会被误判成超时。 */
		uint32_t tickstart = get_tick();

		/* Configure the command */
		cmd[0] = PAGE_PROG_CMD;
		cmd[1] = (uint8_t)(current_addr >> 16);
		cmd[2] = (uint8_t)(current_addr >> 8);
		cmd[3] = (uint8_t)(current_addr);

		/* Enable write operations */
		if (W25Qx_WriteEnable() != W25Qx_OK)
		{
			return W25Qx_TIMEOUT;
		}

		W25Qx_Enable();
    /* Send the command */
    if (SPI1_WriteByte(cmd, 4, W25QXXXX_TIMEOUT_VALUE) == 0)
    {
      W25Qx_Disable();
      return W25Qx_ERROR;
    }

    /* Transmission of the data */
    if (SPI1_WriteByte(pData, (uint16_t)current_size, W25QXXXX_TIMEOUT_VALUE) == 0)
    {
      W25Qx_Disable();
      return W25Qx_ERROR;
    }
		W25Qx_Disable();
    	/* Wait the end of Flash writing */
		while(W25Qx_GetStatus() == W25Qx_BUSY)
		{
			/* Check for the Timeout */
			/* 单页编程最长 3ms(典型 0.7ms), 1000ms 足够 */
			if((get_tick() - tickstart) > W25QXXXX_TIMEOUT_VALUE)
			{
				return W25Qx_TIMEOUT;
			}
		}

    /* Update the address and size variables for next page programming */
    current_addr += current_size;
    pData += current_size;
    current_size = ((current_addr + W25QXXXX_PAGE_SIZE) > end_addr) ? (end_addr - current_addr) : W25QXXXX_PAGE_SIZE;
  } while (current_addr < end_addr);


	return W25Qx_OK;
}

/**
  * @brief  Erases the specified block of the QSPI memory.
  * @param  Address: Block address to erase
  * @retval QSPI memory status
  * @note   **函数名是 Block, 实际发的是 SECTOR_ERASE_CMD(0x20), 也就是 4KB 小扇区。**
  *         如果你的调用方按 64KB 大块来理解这个函数(擦完以为整块都空了),
  *         就会出错 —— 要擦 64KB 得连续调 16 次, 或者改用 BLOCK_ERASE_CMD(0xD8)。
  *         这里保持原行为不动, 只把话说清楚。
  *         地址需要按 4KB 对齐, 不对齐时芯片会忽略该命令(数据不会被擦)。
  */
uint8_t W25Qx_Erase_Block(uint32_t Address)
{
	uint8_t cmd[4];
	uint32_t tickstart = get_tick();
	cmd[0] = SECTOR_ERASE_CMD;
	cmd[1] = (uint8_t)(Address >> 16);
	cmd[2] = (uint8_t)(Address >> 8);
	cmd[3] = (uint8_t)(Address);

	/* Enable write operations */
	if (W25Qx_WriteEnable() != W25Qx_OK)
	{
		return W25Qx_TIMEOUT;
	}

	/*Select the FLASH: Chip Select low */
	W25Qx_Enable();
	/* Send the erase command */
	if (SPI1_WriteByte(cmd, 4, W25QXXXX_TIMEOUT_VALUE) == 0)
	{
		W25Qx_Disable();
		return W25Qx_ERROR;
	}
	/*Deselect the FLASH: Chip Select high */
	W25Qx_Disable();

	/* Wait the end of Flash writing */
	while(W25Qx_GetStatus() == W25Qx_BUSY)
	{
		/* Check for the Timeout */
    if((get_tick() - tickstart) > W25QXXXX_SECTOR_ERASE_MAX_TIME)
    {
			return W25Qx_TIMEOUT;
    }
	}
	return W25Qx_OK;
}

/**
  * @brief  Erases the entire QSPI memory.This function will take a very long time.
  * @retval QSPI memory status
  * @note   8MB 芯片整片擦除典型 20 秒左右, 最长可到 250 秒。
  *         这期间是**同步阻塞**的, 中断还在跑但主流程全停在这里。
  */
uint8_t W25Qx_Erase_Chip(void)
{
	uint8_t cmd[4];
	uint32_t tickstart = get_tick();
	cmd[0] = CHIP_ERASE_CMD;

	/* Enable write operations */
	if (W25Qx_WriteEnable() != W25Qx_OK)
	{
		return W25Qx_TIMEOUT;
	}

	/*Select the FLASH: Chip Select low */
	W25Qx_Enable();
	/* Send the chip erase command */
	if (SPI1_WriteByte(cmd, 1, W25QXXXX_TIMEOUT_VALUE) == 0)
	{
		W25Qx_Disable();
		return W25Qx_ERROR;
	}
	/*Deselect the FLASH: Chip Select high */
	W25Qx_Disable();

	/* Wait the end of Flash writing */
	while(W25Qx_GetStatus() == W25Qx_BUSY)
	{
		/* Check for the Timeout */
    if((get_tick() - tickstart) > W25QXXXX_BULK_ERASE_MAX_TIME)
    {
			return W25Qx_TIMEOUT;
    }
	}
	return W25Qx_OK;
}
