/*********************************************************************************************************
 * Copyright (C) 2024 EternalChip, Inc. (Gmbh) or its affiliates.
 *
 * ALL Rights Reserved.
 *
 * @file W25Q_Handler.c
 *
 * @par dependencies
 *
 * - W25Q_Handler.h
 *
 * @author Jack | R&D Dept. | EternalChip 立芯嵌入式
 *
 * @brief Functions related to reading and writing in the chip's flash area.
 *
 * Processing flow:
 *
 * call directly.
 *
 * @version V1.0 2024-09-13
 *
 * @note 1 tab == 4 spaces!
 *
 * ********************************************************************************************************/
/* Includes ------------------------------------------------------------------*/
#include "W25Q_Handler.h"
/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
static st_W25Q_Handler s_st_W25Q_Handler_1;
static u8 s_u8_W25Q_Ready = 0;      /* W25Q64_Init() 成功后才置1 */
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/
/* extern variables ----------------------------------------------------------*/

/**
  * @brief  外部 Flash(W25Q64) 初始化
  * @param  None
  * @retval 0: 成功   1: 失败(芯片没焊 / 接线不对 / ID 认不出来)
  * @note   失败时会把 Ready 标志清掉, 后面的读写会直接返回错误码,
  *         不会拿"没读到容量"的参数(全 0)去算地址 —— 那样除零会 HardFault。
  */
u8 W25Q64_Init(void)
{
	/* 第一步必须是建 SPI1 总线。W25Qx_Init() 只负责复位芯片 + 读ID,
	   它不管 GPIO 和 SPI 寄存器的配置, 那些都在 SPI1_Init() 里。 */
	SPI1_Init();

	/* SPI1_Init() 里分频是 256, 96MHz/256 = 375KHz。单次读 4KB 要 87ms,
	   读满 8MB 要三分多钟 —— 自测通过以后可以打开下面这行提到 24MHz
	   (快约 16 倍), 但杜邦线太长时可能不稳, 建议先跑通再提速。 */
	//SPI1_SetSpeed(SPI_SPEED_4);

	if (W25Qx_OK != W25Qx_Init())
	{
		s_u8_W25Q_Ready = 0;
		return 1;
	}

	s_st_W25Q_Handler_1.read_index = 0;
	s_st_W25Q_Handler_1.read_sector_index = 0;
	s_st_W25Q_Handler_1.write_databuf_index = 0;
	s_st_W25Q_Handler_1.write_index = 0;
	s_st_W25Q_Handler_1.write_sector_index = 0;

	s_u8_W25Q_Ready = 1;
	return 0;
}

/**
  * @brief  整片擦除
  * @param  None
  * @retval 0: 成功   1: 失败
  * @note   8MB 芯片典型 20 秒、最长 250 秒, 全程阻塞在这个函数里。
  *         擦完把读写指针全部归零, 相当于把这块 Flash 当新片子用。
  */
u8 W25Q64_EraseChip(void)
{
	if (0 == s_u8_W25Q_Ready) return 1;

	if (0 == W25Qx_Erase_Chip())
	{
		s_st_W25Q_Handler_1.read_index = 0;
		s_st_W25Q_Handler_1.read_sector_index = 0;
		s_st_W25Q_Handler_1.write_databuf_index = 0;
		s_st_W25Q_Handler_1.write_index = 0;
		s_st_W25Q_Handler_1.write_sector_index = 0;
		return 0;
	}
	return 1;
}

/**
  * @brief  流式写入
  * @param  data:   数据缓冲
  * @param  length: 本次长度, 可以任意长
  * @retval 0: 成功   1: 失败
  * @note   数据先攒进 4KB 的 databuf, 攒满一个扇区就"擦扇区 + 写16页"落盘。
  *         所以真正的擦写发生在攒满那一刻, 最后一次不足 4KB 的尾巴要由
  *         W25Q64_WriteData_End() 补写, 不调 End 尾巴就永远留在 RAM 里。
  *
  *         失败会一路往上传(擦除或写页失败即返回 1)。失败时内部指针已经
  *         走到了半个扇区的位置, 状态不可信, 调用方应该重新 W25Q64_Init()
  *         擦掉重来, 而不是接着往里写。
  */
u8 W25Q64_WriteData(u8 *data, u32 length)
{
	u16 index = 0;
	u32 addr = 0;
	u32 i = 0;

	if (0 == s_u8_W25Q_Ready) return 1;
	if (NULL == data) return 1;

	for (i = 0; i < length; i++)
	{
		/* 1.数据写入数据缓冲区里面 */
		index = s_st_W25Q_Handler_1.write_databuf_index;
		s_st_W25Q_Handler_1.databuf[index] = *(data + i);
		s_st_W25Q_Handler_1.write_databuf_index++;

		/* 2.判断数据有没有写满4096 */
		if (s_st_W25Q_Handler_1.write_databuf_index >= W25Qx_Para.SUBSECTOR_SIZE)
		{
			s_st_W25Q_Handler_1.write_databuf_index = 0;

			/* 擦除1个sector */
			addr = W25Qx_Para.SUBSECTOR_SIZE * s_st_W25Q_Handler_1.write_sector_index;
			/* 写超容量就报错, 不要让地址进位绕回 0 号扇区把开头的数据覆盖掉 */
			if ((addr + W25Qx_Para.SUBSECTOR_SIZE) > W25Qx_Para.FLASH_SIZE) return 1;
			if (W25Qx_OK != W25Qx_Erase_Block(addr)) return 1;

			/* 写满了一个sector, 执行写入操作 4096个byte */
			for (u8 j = 0; j < (W25Qx_Para.SUBSECTOR_SIZE / W25Qx_Para.PAGE_SIZE); j++)
			{
				/* 获取当前写地址 */
				addr = (W25Qx_Para.SUBSECTOR_SIZE * s_st_W25Q_Handler_1.write_sector_index) +
				       (W25Qx_Para.PAGE_SIZE * j);
				/* 执行写入操作 */
				index = (u16)(W25Qx_Para.PAGE_SIZE * j);
				if (W25Qx_OK != W25Qx_Write(&s_st_W25Q_Handler_1.databuf[index],
				                            addr, W25Qx_Para.PAGE_SIZE))
				{
					return 1;
				}
			}

			s_st_W25Q_Handler_1.write_sector_index++;
			/* 记录总的写入数据长度 */
			s_st_W25Q_Handler_1.write_index += W25Qx_Para.SUBSECTOR_SIZE;
		}
	}
	return 0;
}

/**
  * @brief  把缓冲区里不足一个扇区的尾巴补写下去, 结束一次写入流程
  * @param  None
  * @retval 0: 成功   1: 失败
  * @note   必须和 W25Q64_WriteData() 成对使用: WriteData x N 之后紧跟一次 End。
  *         重复调用是安全的(缓冲区空的时候什么都不做)。
  */
u8 W25Q64_WriteData_End(void)
{
	u32 addr = 0;
	u16 index = 0;
	u8  page_size = 0;
	u16 tail_size = 0;

	if (0 == s_u8_W25Q_Ready) return 1;

	/* 判断还有没有剩余数据没有执行写入 */
	if (0 != s_st_W25Q_Handler_1.write_databuf_index)
	{
		/* 能凑成整页的写入次数 */
		page_size = (u8)(s_st_W25Q_Handler_1.write_databuf_index / W25Qx_Para.PAGE_SIZE);
		/* 最后不足 256 字节的部分 */
		tail_size = (u16)(s_st_W25Q_Handler_1.write_databuf_index % W25Qx_Para.PAGE_SIZE);

		/* 先执行擦除扇区 */
		addr = W25Qx_Para.SUBSECTOR_SIZE * s_st_W25Q_Handler_1.write_sector_index;
		/* 写超容量就报错, 不要让地址进位绕回 0 号扇区把开头的数据覆盖掉 */
		if ((addr + W25Qx_Para.SUBSECTOR_SIZE) > W25Qx_Para.FLASH_SIZE) return 1;
		if (W25Qx_OK != W25Qx_Erase_Block(addr)) return 1;

		for (u8 j = 0; j < page_size; j++)
		{
			/* 获取当前写入地址 */
			addr = (W25Qx_Para.SUBSECTOR_SIZE * s_st_W25Q_Handler_1.write_sector_index) +
			       (W25Qx_Para.PAGE_SIZE * j);
			/* 执行写入操作 */
			index = (u16)(W25Qx_Para.PAGE_SIZE * j);
			if (W25Qx_OK != W25Qx_Write(&s_st_W25Q_Handler_1.databuf[index],
			                            addr, W25Qx_Para.PAGE_SIZE))
			{
				return 1;
			}
		}

		/* 有没有小于256的数据了 */
		if (0 != tail_size)
		{
			/* 获取当前写入地址 */
			addr = (W25Qx_Para.SUBSECTOR_SIZE * s_st_W25Q_Handler_1.write_sector_index) +
			       (W25Qx_Para.PAGE_SIZE * page_size);
			/* 执行写入操作 */
			index = (u16)(W25Qx_Para.PAGE_SIZE * page_size);
			if (W25Qx_OK != W25Qx_Write(&s_st_W25Q_Handler_1.databuf[index],
			                            addr, tail_size))
			{
				return 1;
			}
		}

		s_st_W25Q_Handler_1.write_index += s_st_W25Q_Handler_1.write_databuf_index;

		/* ===== 下面这两行原代码漏了, 不补上就会丢数据 =====
		   写完一个"部分扇区"之后:
		   - write_databuf_index 必须归零, 否则下次 WriteData 会从旧下标
		     接着往 databuf 里填, 填满时按 4096 字节整扇区写下去, 前面那段
		     已经写好的内容和中间的空洞会一起被当成有效数据;
		   - write_sector_index 必须加一, 否则下次攒满时擦的还是这同一个扇区,
		     等于把刚写进去的数据当场擦掉。 */
		s_st_W25Q_Handler_1.write_databuf_index = 0;
		s_st_W25Q_Handler_1.write_sector_index++;
	}
	return 0;
}

/**
  * @brief  流式读取
  * @param  data:   接收缓冲, 必须 >= 4096 字节
  * @param  length: 回传本次实际读到的长度
  * @retval 0: 表示读取成功
  *         1: 读取完毕 没有数据了
  *         2: 读取失败 读取错误
  * @note   每次尽量吐满一个扇区(4096 字节), 快读完时吐剩下的零头。
  *         读走的数据不会消失, 再调 W25Q64_Init() 可以把读指针归零重读。
  */
u8 W25Q64_ReadData(u8 *data, u16 *length)
{
	u32 remain_size = 0;

	if (0 == s_u8_W25Q_Ready) return 2;
	if ((NULL == data) || (NULL == length)) return 2;

	/* 1.先判断是否读取完毕 */
	if (s_st_W25Q_Handler_1.write_index > s_st_W25Q_Handler_1.read_index)
	{
		remain_size = s_st_W25Q_Handler_1.write_index - s_st_W25Q_Handler_1.read_index;

		/* 一次最多一个扇区, 和调用方给的 4096 缓冲对齐 */
		if (remain_size > W25Qx_Para.SUBSECTOR_SIZE)
		{
			remain_size = W25Qx_Para.SUBSECTOR_SIZE;
		}
		*length = (u16)remain_size;

		/* 地址直接就是 read_index。原代码用的是
		     addr = read_sector_index * SUBSECTOR_SIZE
		   这在"读指针正好停在扇区边界"时是对的, 但短读一次之后 read_index
		   会停在扇区中间, 而 read_sector_index 没跟着进位, 算出来的地址就
		   退回扇区开头, 把已经读过的数据再吐一遍。read_index 本身就是绝对
		   偏移, 直接拿来用不会错。 */
		if (W25Qx_OK != W25Qx_Read(data, s_st_W25Q_Handler_1.read_index, *length))
		{
			return 2;
		}

		s_st_W25Q_Handler_1.read_index += *length;
		s_st_W25Q_Handler_1.read_sector_index =
			s_st_W25Q_Handler_1.read_index / W25Qx_Para.SUBSECTOR_SIZE;
		return 0;
	}
	else
	{
		return 1;
	}
}
