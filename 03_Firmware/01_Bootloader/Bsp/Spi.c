/*********************************************************************************************************
 * Copyright (C) 2024 EternalChip, Inc. (Gmbh) or its affiliates.
 *
 * ALL Rights Reserved.
 *
 * @file Spi.c
 *
 * @par dependencies
 *
 * - Spi.h
 *
 * @author Jack | R&D Dept. | EternalChip 立芯嵌入式
 *
 * @brief Functions related to the chip's SPI1 bus (used by the on-board SPI flash).
 *
 * Processing flow:
 *
 * call directly.
 *
 * @note 1 tab == 4 spaces!
 *
 * ********************************************************************************************************/

/* Includes --------------------------------------------------------------------------------------------*/
#include "Spi.h"
/* Private typedef -------------------------------------------------------------------------------------*/
/* Private define --------------------------------------------------------------------------------------*/
/* Private macro ---------------------------------------------------------------------------------------*/
/* Private variables -----------------------------------------------------------------------------------*/

/* 毫秒时基, 由 SysTick_Handler -> TimingDelay_Decrement() 每 1ms 累加一次 */
volatile u32 g_msTick = 0;

/* Private function prototypes -------------------------------------------------------------------------*/
u8 SPI1_ReadWriteByte(u8 WriteData);
static void CS_IO_Init(void);
/* Private functions -----------------------------------------------------------------------------------*/
/* extern variables ------------------------------------------------------------------------------------*/

/**
  * @brief  返回上电至今的毫秒数, 供超时判断使用
  * @retval 毫秒计数
  * @note   计数器在中断里累加, 这里是 32 位读取, 在 Cortex-M 上是单指令,
  *         不需要额外关中断保护。约 49.7 天回绕一次。
  */
u32 millis(void)
{
    return g_msTick;
}

/**
  * @brief  配置片选脚 PA4 为推挽输出
  * @param  None
  * @retval None
  * @note   调用前 GPIOA 的时钟必须已经使能, 否则这里的配置写不进寄存器。
  *         时钟使能放在 SPI1_Init() 里, 且排在调用本函数之前。
  */
static void CS_IO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_OUT;      /* 普通输出 */
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;      /* 推挽输出 */
    GPIO_InitStructure.GPIO_Pin   = F_CS_Pin;           /* PA4 */
    GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_UP;       /* 上拉, 默认片选无效 */
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_Init(F_CS_GPIO_Port, &GPIO_InitStructure);
}

/**
  * @brief  初始化 SPI1: PA5=SCK  PA6=MISO  PA7=MOSI, PA4 作软件片选
  * @param  None
  * @retval None
  * @note   模式为 Mode 0(CPOL=0, CPHA=0), 8位, MSB 先出, 主机模式。
  */
void SPI1_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    SPI_InitTypeDef  SPI_InitStructure;

    /* 先把时钟都打开。这一步必须在 CS_IO_Init() 之前:
       GPIO 时钟没开时配置 PA4 是无效的, PA4 会一直是复位后的浮空输入,
       片选永远拉不动, 后面所有读写都会失败。 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);    /* 使能 SPI1 时钟 */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);   /* 使能 GPIOA 时钟 */

    /* 片选脚单独配成普通输出, 片选由软件控制 */
    CS_IO_Init();
    GPIO_WriteBit(F_CS_GPIO_Port, F_CS_Pin, Bit_SET);       /* 空闲时片选拉高 */

    /* SCK/MISO/MOSI 三根线配成复用推挽 */
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF;           /* 模式需要设置为复用 */
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;          /* 设置为推挽输出 */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7;
    GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource5, GPIO_AF_SPI1); //PA5复用为SPI1
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource6, GPIO_AF_SPI1); //PA6复用为SPI1
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource7, GPIO_AF_SPI1); //PA7复用为SPI1

    RCC_APB2PeriphResetCmd(RCC_APB2Periph_SPI1, ENABLE);    //复位SPI1
    RCC_APB2PeriphResetCmd(RCC_APB2Periph_SPI1, DISABLE);   //停止复位SPI1

    SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;  //SPI双向全双工
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_256;//波特率预分频值256
    SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;        //串行时钟进行奇次采样
    SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;          //串行时钟空闲状态为低电平
    SPI_InitStructure.SPI_CRCPolynomial = 7;            //CRC值计算多项式
    SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;   //SPI数据帧8位
    SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;  //数据传输高位在前
    SPI_InitStructure.SPI_Mode = SPI_Mode_Master;       //SPI主模式, 时钟时序由主机SCK提供
    SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;           //NSS信号由软件管理
    SPI_Init(SPI1, &SPI_InitStructure);

    SPI_Cmd(SPI1, ENABLE);      //使能SPI

    SPI1_ReadWriteByte(0xff);   //启动传输
}

/**
  * @brief  设置 SPI1 速度
  * @param  SPI_BaudRate_Prescaler: 分频系数, 取 SPI_BaudRatePrescaler_2 ~ _256
  *         见 Spi.h 里的 SPI_SPEED_xxx 宏
  * @retval None
  * @note   SPI 速度 = fAPB2 / 分频系数。
  *
  *         BR[2:0] 是 CR1 的第 3~5 位, 所以清位掩码是 0xFFC7
  *         (1111 1111 1100 0111), 把第 3~5 位清零后写入新分频值。
  */
void SPI1_SetSpeed(u8 SPI_BaudRate_Prescaler)
{
    assert_param(IS_SPI_BAUDRATE_PRESCALER(SPI_BaudRate_Prescaler));//判断有效性

    /* 改分频前先把 SPI 关掉: 使能状态下改 BR 位, 手册不保证时序正确,
       关掉再改、改完再开最稳妥 */
    SPI_Cmd(SPI1, DISABLE);

    //位带操作主要分两步, 第一步将所要操作的寄存器的某一位清零, 第二步将寄存器清零的相关位写入所需的值
    SPI1->CR1 &= 0xFFC7;                        //将CR1寄存器的3~5位清零
    SPI1->CR1 |= SPI_BaudRate_Prescaler;        //设置SPI速度, 这一步将所需位写入相关的值

    SPI_Cmd(SPI1, ENABLE);      //使能SPI
}

/**
  * @brief  SPI1 读写一个字节(全双工: 发一个同时收一个)
  * @param  WriteData: 要写入的字节
  * @retval 读到的字节
  * @note   本函数不带超时, 一直等到标志位就绪。调试阶段可以用,
  *         正式流程建议用下面带 timeout 的 SPI1_WriteByte/SPI1_ReadByte。
  */
u8 SPI1_ReadWriteByte(u8 WriteData)
{
    while(SPI_I2S_GetFlagStatus(SPI1,SPI_I2S_FLAG_TXE)==RESET); //等待发送缓冲区空

    SPI_I2S_SendData(SPI1,WriteData);       //通过SPI1发送一个字节的数据

    while(SPI_I2S_GetFlagStatus(SPI1,SPI_I2S_FLAG_RXNE)==RESET); //等待接收缓冲区非空

    return SPI_I2S_ReceiveData(SPI1);       //返回通过SPI1接收的数据
}

/**
  * @brief  SPI1 发送一串数据(边发边收, 收到的数据丢弃)
  * @param  WriteData: 待发送的数据
  * @param  dataSize:  数据长度, 字节
  * @param  timeout:   超时时间, 单位毫秒
  * @retval 1: 成功  0: 超时
  *
  * @note   txflow 是收发节拍标志, 防止"还没收到就发下一个"把接收缓冲区挤溢出:
  *           txflow==1 —— 允许发送(收到上一个了)
  *           txflow==0 —— 允许接收(刚发出去, 等接收)
  *         两个 if 各自判一次标志位而不是死等, 所以中途可以跳出判超时。
  */
u8 SPI1_WriteByte(u8 *WriteData, u16 dataSize, u32 timeout)
{
    u32 time = timeout;
    u32 current_time = millis();

    u16 txsize = dataSize;
    u16 rxsize = dataSize;
    u8 *pTxBuffPtr = WriteData;
    u8 txflow = 1u;

    while((txsize > 0) || (rxsize > 0))
    {
        if((SPI_I2S_GetFlagStatus(SPI1,SPI_I2S_FLAG_TXE) != RESET) && (txsize > 0) && (txflow == 1u))
        {
            SPI_I2S_SendData(SPI1,*pTxBuffPtr);
            pTxBuffPtr += sizeof(u8);
            txsize--;
            txflow = 0;
        }
        if((SPI_I2S_GetFlagStatus(SPI1,SPI_I2S_FLAG_RXNE) != RESET) && (rxsize > 0) && (txflow == 0u))
        {
            SPI_I2S_ReceiveData(SPI1);      /* 只发送, 收上来的字节丢弃 */
            rxsize--;
            txflow = 1;
        }
        if ((millis() - current_time) >= time )
        {
            SPI_I2S_ClearFlag(SPI1,SPI_I2S_FLAG_OVR);
            return 0;
        }
    }

    SPI_I2S_ClearFlag(SPI1,SPI_I2S_FLAG_OVR);
    return 1;
}

/**
  * @brief  SPI1 读取一串数据(全双工, 用读缓冲里的内容当哑字节发出去)
  * @param  ReadData: 接收缓冲, 长度至少 dataSize 字节
  * @param  dataSize: 数据长度, 字节
  * @param  timeout:  超时时间, 单位毫秒
  * @retval 1: 成功  0: 超时
  *
  * @note   收发共用同一个缓冲。主机是全双工, 要收 N 个字节就必须自己发出 N 个
  *         字节的时钟, 这里就直接把读缓冲里的内容发出去当哑数据, 回读的值
  *         通过 SPI_I2S_ReceiveData 覆盖写回同一位置。
  */
u8 SPI1_ReadByte(u8 *ReadData, u16 dataSize, u32 timeout)
{
    u32 time = timeout;
    u32 current_time = millis();

    u16 txsize = dataSize;
    u16 rxsize = dataSize;
    u8 *pTxBuffPtr = ReadData;
    u8 *pRxBuffPtr = ReadData;
    u8 txflow = 1u;

    while((txsize > 0) || (rxsize > 0))
    {
        if((SPI_I2S_GetFlagStatus(SPI1,SPI_I2S_FLAG_TXE) != RESET) && (txsize > 0) && (txflow == 1u))
        {
            SPI_I2S_SendData(SPI1,*pTxBuffPtr);
            pTxBuffPtr += sizeof(u8);
            txsize--;
            txflow = 0;
        }
        if((SPI_I2S_GetFlagStatus(SPI1,SPI_I2S_FLAG_RXNE) != RESET) && (rxsize > 0) && (txflow == 0u))
        {
            *pRxBuffPtr = SPI_I2S_ReceiveData(SPI1);
            pRxBuffPtr += sizeof(u8);
            rxsize--;
            txflow = 1;
        }
        if ((millis() - current_time) >= time )
        {
            return 0;
        }
    }

    return 1;
}
