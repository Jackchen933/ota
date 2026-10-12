/*********************************************************************************************************
 * Copyright (C) 2026 EternalChip, Inc. (Gmbh) or its affiliates.
 *
 * @file OtaApp.c
 *
 * @par dependencies
 *
 * - OtaApp.h / Eeprom.h / W25q64.h / usart.h / gpio.h / main.h
 *
 * @brief APP 侧「后台无感下载」OTA 实现。
 *
 * @version V2.1  传输层由自定义推流改为标准 Ymodem 接收端
 *
 * @note 1 tab == 4 spaces!
 *
 * ********************************************************************************************************/

/* 包含头文件 *******************************************************************************************/
#include "OtaApp.h"
#include "OtaFlag.h"
#include "Eeprom.h"
#include "W25q64.h"
#include "usart.h"
#include "gpio.h"
#include "main.h"

#include <string.h>
#include <stddef.h>

/* 私有宏 ***********************************************************************************************/

/* 从流缓冲一次最多取多少字节。取大点减少系统调用次数，
   但不能太大 —— 本函数的栈上要放得下 */
#define OTA_DRAIN_CHUNK         128U

/* 流缓冲取数据时的等待时间。它同时决定了"空闲超时"的判定粒度：
   取不到数据就返回，正好是检查 idle 的时机 */
#define OTA_RX_POLL_MS          20U

/* 单字节串口发送超时(ms)。115200 下一个字节约 87us */
#define OTA_TX_TIMEOUT_MS       10U

/* 按键连续按下的判定次数(消抖)。10ms 轮询 x 5 = 50ms */
#define OTA_KEY_STABLE_CNT      5U

/* App 映像大小。
   用 Keil 链接器符号算：ER_IROM1 的结束地址减去 A 区基址。
   armcc 允许标识符里出现 '$'，所以 Image$$ER_IROM1$$Limit 能直接声明。
   这个值写进 EEPROM 的"当前App字节大小"，服务器据此知道板子上是哪一版。
   A 区基址 = 0x0800C000：Boot 32KB + 标志扇区 16KB 之后才是 App，
   与 04_Software.sct / system_stm32f4xx.c 的 VECT_TAB_OFFSET 三处同步 */
#define OTA_APP_BASE            0x0800C000UL

/* 私有类型 *********************************************************************************************/

/* 下载线程的队列消息：一块填满的缓冲 */
typedef struct
{
    uint8_t  *buf;
    uint16_t  len;
    uint8_t   id;       /* 0 = buffer1, 1 = buffer2 */
    uint32_t  session;  /* 第几轮下载。变了就说明是新的一轮，写指针要回到起点 */
} OtaFrame_t;

/* Ymodem 接收子状态机：逐字节推进一帧的接收 */
typedef enum
{
    YM_RX_HEAD = 0,     /* 等帧头(SOH/STX/EOT/CAN) */
    YM_RX_SEQ,          /* 序号 */
    YM_RX_SEQC,         /* 序号反码 */
    YM_RX_DATA,         /* 数据区(边收边算CRC) */
    YM_RX_CRC_HI,       /* CRC 高字节 */
    YM_RX_CRC_LO        /* CRC 低字节 -> 整帧校验 */
} YmRxState_t;

/* 私有变量 *********************************************************************************************/

/* 链接器符号: App 映像的结束地址(sct 里执行区名 ER_IROM1)。
   armclang(AC6) 同样支持 '$' 标识符, 但必须显式 extern 声明, 不声明就是
   "identifier undefined" —— 之前缺的就是这一行 */
extern unsigned char Image$$ER_IROM1$$Limit;

/* ---- 串口接收通道 ---- */
__align(4) static uint8_t s_stream_storage[OTA_STREAM_SIZE];
static StaticStreamBuffer_t s_stream_struct;
static StreamBufferHandle_t s_stream = NULL;

__align(4) static uint8_t s_rx_chunk[OTA_RX_CHUNK];      /* DMA 的目标 */

/* ---- 双缓冲 ---- */
__align(4) static uint8_t s_buf[OTA_BUF_COUNT][OTA_BUF_SIZE];

/* 每块缓冲的"空闲"信号量：OTA 线程填充前获取，下载线程写完释放。
   开局两块都是空的，所以初始化时就各给一次 */
static osSemaphoreId_t s_sem_free[OTA_BUF_COUNT];
static const osSemaphoreAttr_t s_sem_free_attr[OTA_BUF_COUNT] = {
    { .name = "otaBuf0Free" },
    { .name = "otaBuf1Free" },
};

/* 下载线程的满块队列 */
static osMessageQueueId_t s_frame_q = NULL;
static const osMessageQueueAttr_t s_frame_q_attr = { .name = "otaFrameQ" };

/* ---- 状态机 ---- */
static volatile OtaState_t s_state = OTA_ST_WAIT_REQ_DOWNLOAD;

static uint8_t  s_cmd_win[OTA_CMD_LEN];     /* 3 字节滑动窗口，用于匹配命令 */
static uint8_t  s_cmd_cnt;

static uint8_t  s_cur_buf;                  /* 当前在填哪块缓冲 */
static uint16_t s_cur_len;                  /* 当前缓冲已填字节数 */
static uint32_t s_total;                    /* 本次已收固件总字节数 */
static uint32_t s_last_rx_tick;             /* 最后一次收到字节的时间 */
static uint8_t  s_uart_error;               /* 串口出错标志(中断里置位) */
static uint32_t s_session;                  /* 下载轮次，每进入一次下载 +1 */
static volatile uint8_t s_wr_error;         /* 下载线程写 W25Q64 失败，OTA 线程据此中止本轮 */

/* ---- Ymodem 接收会话 ----
   帧数据先进这里攒着，CRC 校验通过才 memcpy 进 4KB 缓冲：
   坏帧 NAK 重传时缓冲还没被污染，不需要回滚 */
__align(4) static uint8_t s_ym_frame[YM_BLK_1024];

static YmRxState_t s_ym_state;              /* 接收子状态机 */
static uint16_t s_ym_frame_len;             /* 本帧数据区长(128/1024) */
static uint16_t s_ym_data_cnt;              /* 本帧已收数据字节数 */
static uint16_t s_ym_crc_calc;              /* 数据区增量 CRC */
static uint8_t  s_ym_crc_hi;                /* 收到的 CRC 高字节 */
static uint8_t  s_ym_seq;                   /* 本帧序号 */
static uint8_t  s_ym_expected;              /* 期望的下一个数据帧序号 */
static uint8_t  s_ym_got_name;              /* 帧0(文件名)已确认 */
static uint8_t  s_ym_eot_phase;             /* EOT 握手 0=传数据 1=回过NAK 2=等结束帧 */
static uint8_t  s_ym_can_cnt;               /* CAN 连击计数 */
static uint8_t  s_ym_c_cnt;                 /* 'C' 已发次数 */
static uint32_t s_ym_file_size;             /* 帧0解析出的文件大小(写标志扇区用) */

/* 私有函数 *********************************************************************************************/

/* Ymodem 组引用了文件后部定义的缓冲/收尾函数，先给原型保证先声明后使用 */
static void Ota_BufTake(uint8_t id);
static void Ota_BufCommit(uint8_t id, uint16_t len);
static void Ota_WaitAllWritten(void);
static void Ota_FinishDownload(void);

/********************************************************************************************************
  * @函数名称：Ota_SendCmd
  * @函数说明：向上位机/服务器发 3 字节命令
  * @注意事项：阻塞发送，只在 OTA 任务上下文调用，不要在中断里调。
  *******************************************************************************************************/
static void Ota_SendCmd(uint8_t b0, uint8_t b1, uint8_t b2)
{
    uint8_t buf[OTA_CMD_LEN];

    buf[0] = b0;
    buf[1] = b1;
    buf[2] = b2;

    (void)HAL_UART_Transmit(&huart1, buf, OTA_CMD_LEN, OTA_TX_TIMEOUT_MS);
}

/********************************************************************************************************
  * @函数名称：Ota_CmdFeed
  * @函数说明：把一个字节推进 3 字节滑动窗口，并判断是否等于给定命令
  * @返回参数：1: 匹配   0: 不匹配
  * @注意事项：用滑动窗口而不是"攒够 3 个再比"，是因为攒够就清零的话，
   形如 [0x11][0x11][0x22][0x33] 的流会被误判 —— 第二个 0x11 才是真正的
   命令开头，但前三个字节比较失败后一起被丢掉了。滑动窗口能覆盖这种重叠。
  *******************************************************************************************************/
static uint8_t Ota_CmdFeed(uint8_t byte, uint8_t b0, uint8_t b1, uint8_t b2)
{
    uint8_t i;

    for (i = 0U; i < (OTA_CMD_LEN - 1U); i++)
    {
        s_cmd_win[i] = s_cmd_win[i + 1U];
    }
    s_cmd_win[OTA_CMD_LEN - 1U] = byte;

    if (s_cmd_cnt < OTA_CMD_LEN)
    {
        s_cmd_cnt++;
    }

    if (s_cmd_cnt < OTA_CMD_LEN)
    {
        return 0U;
    }

    return ((s_cmd_win[0] == b0) && (s_cmd_win[1] == b1) && (s_cmd_win[2] == b2))
           ? 1U : 0U;
}

/********************************************************************************************************
  * @函数名称：Ota_CmdReset
  * @函数说明：清空命令滑动窗口。进入新状态前调用，避免上一状态的残留
  *           字节和这一状态的开头凑成一个假命令
  *******************************************************************************************************/
static void Ota_CmdReset(void)
{
    s_cmd_cnt = 0U;
    (void)memset(s_cmd_win, 0, sizeof(s_cmd_win));
}

/********************************************************************************************************
  * @函数名称：Ota_YmTx1
  * @函数说明：向对端发 1 个 Ymodem 控制字节(ACK/NAK/'C'/CAN)
  * @注意事项：阻塞发送，只在 OTA 任务上下文调用。
  *******************************************************************************************************/
static void Ota_YmTx1(uint8_t b)
{
    (void)HAL_UART_Transmit(&huart1, &b, 1U, OTA_TX_TIMEOUT_MS);
}

/********************************************************************************************************
  * @函数名称：Ota_Crc16Upd
  * @函数说明：CRC16-XMODEM 增量更新(多项式0x1021, 初值0x0000, 不反射不异或)
  * @入口参数：crc —— 上一轮累计值(首字节前传0x0000)；byte —— 新字节
  * @返回参数：更新后的 CRC
  * @注意事项：逐位实现，1024字节约0.2ms@100MHz，不值得为查表费512B ROM。
  *           只算数据区(帧头/序号/CRC本身不参与)，与标准 Ymodem 一致。
  *******************************************************************************************************/
static uint16_t Ota_Crc16Upd(uint16_t crc, uint8_t byte)
{
    uint8_t i;

    crc ^= (uint16_t)((uint16_t)byte << 8U);

    for (i = 0U; i < 8U; i++)
    {
        if ((crc & 0x8000U) != 0U)
        {
            crc = (uint16_t)((uint16_t)(crc << 1U) ^ 0x1021U);
        }
        else
        {
            crc = (uint16_t)(crc << 1U);
        }
    }

    return crc;
}

/********************************************************************************************************
  * @函数名称：Ota_YmSessionReset
  * @函数说明：复位 Ymodem 会话变量。进入下载态(或中止后重启)时调用
  *******************************************************************************************************/
static void Ota_YmSessionReset(void)
{
    s_ym_state     = YM_RX_HEAD;
    s_ym_frame_len = 0U;
    s_ym_data_cnt  = 0U;
    s_ym_crc_calc  = 0U;
    s_ym_seq       = 0U;
    s_ym_expected  = 1U;        /* 数据帧序号从 0x01 开始，帧0 是 0x00 */
    s_ym_got_name  = 0U;
    s_ym_eot_phase = 0U;
    s_ym_can_cnt   = 0U;
    s_ym_c_cnt     = 0U;
    s_ym_file_size = 0U;
}

/********************************************************************************************************
  * @函数名称：Ota_YmParseSize
  * @函数说明：从帧0数据区解析文件大小：文件名'\0'后跟十进制ASCII大小串
  * @返回参数：文件字节数；解析失败返回 0(调用方回退用实际收到的字节数)
  *******************************************************************************************************/
static uint32_t Ota_YmParseSize(const uint8_t *data, uint16_t len)
{
    uint16_t i     = 0U;
    uint32_t val   = 0U;
    uint8_t  digit = 0U;

    while ((i < len) && (data[i] != 0U))
    {
        i++;                                /* 跳过文件名 */
    }
    i++;                                    /* 跳过 '\0' */

    while ((i < len) && (data[i] >= '0') && (data[i] <= '9'))
    {
        val = (val * 10U) + (uint32_t)(data[i] - '0');
        digit++;
        i++;
    }

    return (digit != 0U) ? val : 0U;
}

/********************************************************************************************************
  * @函数名称：Ota_YmToBuf
  * @函数说明：把校验通过的帧数据填进当前 4KB 缓冲，满块则交下载线程
  * @注意事项：BufTake 会阻塞等下载线程写完 Flash(典型130ms)。停等协议下
  *           对端在等我们的 ACK 不会推数据，这里阻塞是安全的；
  *           ACK 由调用方在【本函数返回后】才发 —— ACK 门槛 = 数据落 RAM 缓冲。
  *******************************************************************************************************/
static void Ota_YmToBuf(const uint8_t *data, uint16_t len)
{
    if (s_cur_len == 0U)
    {
        Ota_BufTake(s_cur_buf);
    }

    (void)memcpy(&s_buf[s_cur_buf][s_cur_len], data, len);
    s_cur_len = (uint16_t)(s_cur_len + len);
    s_total  += len;

    if (s_cur_len >= OTA_BUF_SIZE)
    {
        Ota_BufCommit(s_cur_buf, s_cur_len);

        s_cur_buf = (uint8_t)((s_cur_buf + 1U) % OTA_BUF_COUNT);
        s_cur_len = 0U;
    }
}

/********************************************************************************************************
  * @函数名称：Ota_YmAbort
  * @函数说明：中止本轮 Ymodem 下载：等落盘、清 EEPROM 标志、回等待态
  * @入口参数：send_can —— 1: 先发 CAN CAN 通知对端挂断；0: 静默退出
  *******************************************************************************************************/
static void Ota_YmAbort(uint8_t send_can)
{
    if (send_can != 0U)
    {
        Ota_YmTx1(YM_CAN);
        Ota_YmTx1(YM_CAN);
    }

    Ota_WaitAllWritten();
    (void)OtaEe_SetState(OTA_STATE_NONE);
    Ota_CmdReset();
    s_state = OTA_ST_WAIT_REQ_DOWNLOAD;
}

/********************************************************************************************************
  * @函数名称：Ota_YmFrameDone
  * @函数说明：一帧收完(CRC两字节已到)：整帧校验并分发处理
  * @注意事项：ACK 门槛 = 数据落 RAM 缓冲(Ota_YmToBuf 返回后)；重复序号
  *           只重发 ACK 不重复填数据 —— 那是"上个ACK丢了"的对端重传。
  *******************************************************************************************************/
static void Ota_YmFrameDone(uint16_t crc_rx)
{
    s_ym_state = YM_RX_HEAD;                /* 无论成败，本帧到此结束 */

    if (crc_rx != s_ym_crc_calc)
    {
        Ota_YmTx1(YM_NAK);                  /* 坏帧重传；缓冲未被污染 */
        return;
    }

    if (s_ym_seq == 0U)
    {
        /* ---- 帧0：文件名帧，或收尾的空文件名结束帧 ---- */
        if (s_ym_frame[0] == 0U)
        {
            if (s_ym_eot_phase == 2U)
            {
                Ota_YmTx1(YM_ACK);          /* 会话正式结束 */
                Ota_FinishDownload();       /* 等落盘->写标志->回0x44.. */
            }
            return;
        }

        if (s_ym_got_name != 0U)
        {
            Ota_YmTx1(YM_ACK);              /* 帧0重发(上个ACK丢了)，重确认 */
            Ota_YmTx1(YM_CRC_CH);
        }
        else
        {
            s_ym_file_size = Ota_YmParseSize(s_ym_frame, s_ym_frame_len);
            s_ym_got_name  = 1U;
            s_ym_expected  = 1U;
            s_ym_eot_phase = 0U;
            Ota_YmTx1(YM_ACK);
            Ota_YmTx1(YM_CRC_CH);           /* 请求数据帧 */
        }
        return;
    }

    /* ---- 数据帧 ---- */
    if (s_ym_eot_phase == 2U)
    {
        Ota_YmAbort(1U);                    /* 结束阶段还来数据帧 = 流已乱 */
        return;
    }

    if (s_ym_seq == s_ym_expected)
    {
        Ota_YmToBuf(s_ym_frame, s_ym_frame_len);
        s_ym_expected++;
        Ota_YmTx1(YM_ACK);
    }
    else if (s_ym_seq == (uint8_t)(s_ym_expected - 1U))
    {
        Ota_YmTx1(YM_ACK);                  /* 重复帧：重ACK，不重复填 */
    }
    else
    {
        Ota_YmAbort(1U);                    /* 跳号，序号链断了，中止 */
    }
}

/********************************************************************************************************
  * @函数名称：Ota_YmFeed
  * @函数说明：Ymodem 接收状态机，按当前子状态处理一个收到的字节
  *******************************************************************************************************/
static void Ota_YmFeed(uint8_t b)
{
    switch (s_ym_state)
    {
        case YM_RX_HEAD:
        {
            if ((b == YM_SOH) || (b == YM_STX))
            {
                s_ym_frame_len = (b == YM_SOH) ? YM_BLK_128 : YM_BLK_1024;
                s_ym_data_cnt  = 0U;
                s_ym_crc_calc  = 0U;
                s_ym_state     = YM_RX_SEQ;
            }
            else if (b == YM_EOT)
            {
                /* EOT 双次握手(标准流程)：第一次 NAK 让发送方确认文件
                   真发完了，第二次 ACK+'C' 进入结束帧阶段 */
                if (s_ym_eot_phase == 0U)
                {
                    s_ym_eot_phase = 1U;
                    Ota_YmTx1(YM_NAK);
                }
                else
                {
                    s_ym_eot_phase = 2U;
                    Ota_YmTx1(YM_ACK);
                    Ota_YmTx1(YM_CRC_CH);   /* 要结束帧(空文件名帧) */
                }
            }
            else if (b == YM_CAN)
            {
                s_ym_can_cnt++;
                if (s_ym_can_cnt >= 2U)
                {
                    Ota_YmAbort(0U);        /* 对端取消，静默退出 */
                }
            }
            else
            {
                s_ym_can_cnt = 0U;          /* 杂散字节忽略 */
            }
            break;
        }

        case YM_RX_SEQ:
        {
            s_ym_seq   = b;
            s_ym_state = YM_RX_SEQC;
            break;
        }

        case YM_RX_SEQC:
        {
            if ((uint8_t)(b ^ 0xFFU) != s_ym_seq)
            {
                Ota_YmTx1(YM_NAK);          /* 序号对不上反码，整帧报废 */
                s_ym_state = YM_RX_HEAD;
            }
            else
            {
                s_ym_state = YM_RX_DATA;
            }
            break;
        }

        case YM_RX_DATA:
        {
            s_ym_frame[s_ym_data_cnt] = b;
            s_ym_data_cnt++;
            s_ym_crc_calc = Ota_Crc16Upd(s_ym_crc_calc, b);

            if (s_ym_data_cnt >= s_ym_frame_len)
            {
                s_ym_state = YM_RX_CRC_HI;
            }
            break;
        }

        case YM_RX_CRC_HI:
        {
            s_ym_crc_hi = b;
            s_ym_state  = YM_RX_CRC_LO;
            break;
        }

        case YM_RX_CRC_LO:
        {
            uint16_t crc_rx = (uint16_t)(((uint16_t)s_ym_crc_hi << 8U) | (uint16_t)b);

            Ota_YmFrameDone(crc_rx);
            break;
        }

        default:
        {
            s_ym_state = YM_RX_HEAD;
            break;
        }
    }
}

/********************************************************************************************************
  * @函数名称：Ota_BufTake
  * @函数说明：取一块空闲缓冲准备填充
  * @入口参数：id —— 缓冲号
  * @返回参数：0: 成功
  * @注意事项：会阻塞。等的是"下载线程把这块缓冲写完"，典型 130ms
  *           (擦 4KB 扇区 ~45ms + 写 4KB @390kHz ~84ms)。
  *           这期间新到的串口数据由流缓冲兜住，见 OTA_STREAM_SIZE 的说明。
  *******************************************************************************************************/
static void Ota_BufTake(uint8_t id)
{
    (void)osSemaphoreAcquire(s_sem_free[id], portMAX_DELAY);
}

/********************************************************************************************************
  * @函数名称：Ota_BufCommit
  * @函数说明：把当前缓冲交给下载线程
  *******************************************************************************************************/
static void Ota_BufCommit(uint8_t id, uint16_t len)
{
    OtaFrame_t frame;

    frame.buf     = s_buf[id];
    frame.len     = len;
    frame.id      = id;
    frame.session = s_session;

    /* 队列深度就是缓冲块数，不会满 —— 每块交出前都先 take 过 */
    (void)osMessageQueuePut(s_frame_q, &frame, 0U, portMAX_DELAY);
}

/********************************************************************************************************
  * @函数名称：Ota_FlushPartial
  * @函数说明：把当前缓冲里不足一块的尾巴也交出去
  *******************************************************************************************************/
static void Ota_FlushPartial(void)
{
    if (s_cur_len > 0U)
    {
        Ota_BufCommit(s_cur_buf, s_cur_len);

        s_cur_buf = (uint8_t)((s_cur_buf + 1U) % OTA_BUF_COUNT);
        s_cur_len = 0U;
    }
}

/********************************************************************************************************
  * @函数名称：Ota_WaitAllWritten
  * @函数说明：等两块缓冲都回到空闲态 = 已经提交的数据全部落盘
  * @注意事项：取到后立刻还回去，不影响下一轮下载。
  *           不这么做的话，下面写完 EEPROM 就发 0x44 0x55 0x66 报"下载完毕"，
  *           但下载线程可能还在写最后一块 —— 服务器以为好了、实际没落盘。
  *******************************************************************************************************/
static void Ota_WaitAllWritten(void)
{
    uint8_t i;

    for (i = 0U; i < OTA_BUF_COUNT; i++)
    {
        (void)osSemaphoreAcquire(s_sem_free[i], portMAX_DELAY);
    }
    for (i = 0U; i < OTA_BUF_COUNT; i++)
    {
        (void)osSemaphoreRelease(s_sem_free[i]);
    }
}

/********************************************************************************************************
  * @函数名称：Ota_FinishDownload
  * @函数说明：数据传输结束的收尾：冲尾巴 -> 等落盘 -> 写标志 -> 回正响应
  * @注意事项：写标志扇区是升级的"判决书"，要最后写：
  *           1. 先等 W25Q64 全部落盘 —— 数据没落盘就置标志，掉电后 Boot
  *              会拿半截密文去升级；
  *           2. 再写标志扇区(EaSize + 状态0x11) —— 这是给 Bootloader 看的，
  *              下次复位它读到 0x11 就会把 W25Q64 A区域的密文搬到 App 运行区；
  *              EEPROM 那份只是 App/服务器侧的记录，Boot 不读它。
  *           3. 全部成功后才回 0x44 0x55 0x66 正响应。
  *           写标志含整扇区擦除，会卡约 1 秒(见 OtaFlag.c 说明)，此时下载
  *           已经结束，不影响收包。
  *******************************************************************************************************/
static void Ota_FinishDownload(void)
{
    /* EaSize 优先用帧0里声明的文件大小：Ymodem 会把文件用 0x1A 填充到
       1024 整数倍，s_total 含填充字节，直接拿去写标志会让 Boot 的包长
       互验(cipher_len == 16 + body_len + 16)失败。解析不出来才回退 s_total */
    uint32_t ea_size = (s_ym_file_size != 0U) ? s_ym_file_size : s_total;

    Ota_FlushPartial();
    Ota_WaitAllWritten();

    /* 给 Bootloader 的判决书：A区密文大小 + 状态"App下载固件待更新"。
       这步失败(极少见)就不回正响应，让服务器重传 —— Boot 不知道这次
       下载，A区数据当不存在 */
    if (OtaFlag_SetDownloadDone(ea_size) != 0U)
    {
        (void)OtaEe_SetState(OTA_STATE_NONE);
        Ota_CmdReset();
        s_state = OTA_ST_WAIT_REQ_DOWNLOAD;
        return;
    }

    /* App/服务器侧的记录(EEPROM): 状态从"下载中"改成"请求更新"。
       和标志扇区是两本账: 这本给服务器/调试看, 标志扇区给 Boot 看 */
    (void)OtaEe_SetNewAppSize(ea_size);
    (void)OtaEe_SetState(OTA_STATE_REQ_UPDATE);

    /* 后台下载完毕，返回正响应 */
    Ota_SendCmd(OTA_CMD_DONE_0, OTA_CMD_DONE_1, OTA_CMD_DONE_2);

    Ota_CmdReset();
    s_state = OTA_ST_WAIT_REQ_UPDATE;
}

/********************************************************************************************************
  * @函数名称：Ota_EnterDownload
  * @函数说明：进入下载模式：清计数、在 EEPROM 打"下载中"标志、等服务器开始推数据
  *******************************************************************************************************/
static void Ota_EnterDownload(void)
{
    /* 先把上一轮排空：等两块缓冲都回到空闲，说明队列里没有待写的数据，
       也没有正在写的。否则下面翻轮次时，下载线程可能还在用旧写指针。
       同时这也就把两块缓冲置成了空闲态，不用再手工 release。 */
    Ota_WaitAllWritten();

    s_session++;                /* 新的一轮，下载线程据此把写指针归零 */

    s_cur_buf      = 0U;
    s_cur_len      = 0U;
    s_total        = 0U;
    s_last_rx_tick = (uint32_t)osKernelGetTickCount();

    /* 先打标志再收数据。中途掉电时 Boot 看到 0x11 就知道上次没下完，
       最多是白等，不会拿半截固件去刷 A 区 */
    (void)OtaEe_SetState(OTA_STATE_DOWNLOADING);

    Ota_YmSessionReset();
    Ota_CmdReset();
    s_state = OTA_ST_DOWNLOAD;

    /* Ymodem 是接收方主导的协议：主动发 'C' 请求 CRC 模式，
       终端工具收到后才开始发帧0。空闲超时由 Ota_CheckIdle 负责重发 */
    Ota_YmTx1(YM_CRC_CH);
}

/********************************************************************************************************
  * @函数名称：Ota_ConsumeByte
  * @函数说明：状态机主体，按当前状态处理一个收到的字节
  *******************************************************************************************************/
static void Ota_ConsumeByte(uint8_t b)
{
    s_last_rx_tick = (uint32_t)osKernelGetTickCount();

    switch (s_state)
    {
        /* ---------------- 等待服务器请求下载 ---------------- */
        case OTA_ST_WAIT_REQ_DOWNLOAD:
        {
            if (Ota_CmdFeed(b, OTA_CMD_DOWNLOAD_0, OTA_CMD_DOWNLOAD_1, OTA_CMD_DOWNLOAD_2) != 0U)
            {
                Ota_EnterDownload();
            }
            break;
        }

        /* ---------------- 下载中(Ymodem 接收) ---------------- */
        case OTA_ST_DOWNLOAD:
        {
            Ota_YmFeed(b);
            break;
        }

        /* ---------------- 等待服务器请求更新 ---------------- */
        case OTA_ST_WAIT_REQ_UPDATE:
        {
            if (Ota_CmdFeed(b, OTA_CMD_UPDATE_0, OTA_CMD_UPDATE_1, OTA_CMD_UPDATE_2) != 0U)
            {
                Ota_SendCmd(OTA_CMD_UPDATE_0, OTA_CMD_UPDATE_1, OTA_CMD_UPDATE_2);
                Ota_CmdReset();
                s_state = OTA_ST_END;
            }
            break;
        }

        default:
        {
            break;
        }
    }
}

/********************************************************************************************************
  * @函数名称：Ota_CheckIdle
  * @函数说明：Ymodem 会话超时监督。正常收发由 ACK/NAK 驱动，帧间隔毫秒级，
  *           空闲到超时只可能是异常
  * @注意事项：握手段(还没收到帧0)空闲 -> 重发'C'，超次数放弃；
  *           收到帧0后空闲 -> 对端死了/线断了，中止本轮。
  *******************************************************************************************************/
static void Ota_CheckIdle(void)
{
    uint32_t idle;

    if (s_state != OTA_ST_DOWNLOAD)
    {
        return;
    }

    idle = (uint32_t)osKernelGetTickCount() - s_last_rx_tick;

    if (idle < OTA_END_IDLE_MS)
    {
        return;
    }

    if (s_ym_got_name == 0U)
    {
        /* 还在等对端响应 'C'：重发，超过次数就放弃本轮 */
        if (s_ym_c_cnt < YM_C_MAX_RETRY)
        {
            s_ym_c_cnt++;
            Ota_YmTx1(YM_CRC_CH);
            s_last_rx_tick = (uint32_t)osKernelGetTickCount();
        }
        else
        {
            Ota_YmAbort(0U);        /* 30 秒没人来，悄悄退出 */
        }
    }
    else
    {
        /* 传到一半对端没了(互等死锁/拔线)：发 CAN 挂断，回等待态 */
        Ota_YmAbort(1U);
    }
}

/********************************************************************************************************
  * @函数名称：Ota_HandleEndState
  * @函数说明：OtaEnd 状态：20 秒内按下按键就软复位进 Boot，超时就回正常业务
  * @返回参数：无(按下按键时不返回 —— 直接软复位)
  * @注意事项：这里不返回的原因是软复位是"下一次上电"的入口，
  *           Bootloader 会在那时读 EEPROM 的 0x22 标志把 W25Q64 里的固件
  *           搬进 A 区。超时则什么都不做：标志留在 EEPROM 里，
  *           用户下次自己关机重启时 Boot 照样会完成升级。
  *******************************************************************************************************/
static void Ota_HandleEndState(void)
{
    uint32_t start = (uint32_t)osKernelGetTickCount();
    uint8_t  stable = 0U;

    while (((uint32_t)osKernelGetTickCount() - start) < OTA_KEY_WINDOW_MS)
    {
        if (HAL_GPIO_ReadPin(Key_GPIO_Port, Key_Pin) == GPIO_PIN_RESET)
        {
            stable++;
            if (stable >= OTA_KEY_STABLE_CNT)
            {
                /* 用户确认更新 -> 软复位。不复位外设直接跳 Boot：
                   软复位能把时钟/中断/外设全恢复干净，省掉一堆收尾的坑 */
                HAL_NVIC_SystemReset();
                for (;;) { }
            }
        }
        else
        {
            stable = 0U;
        }

        osDelay(OTA_KEY_POLL_MS);
    }

    /* 20 秒没按 -> 放弃这次立即更新，回到等待下载。
       EEPROM 里的 0x22 不动，留给用户下次关机重启时由 Boot 处理 */
    Ota_CmdReset();
    s_state = OTA_ST_WAIT_REQ_DOWNLOAD;
}

/* 外部函数 *********************************************************************************************/

int8_t OtaApp_Init(void)
{
    uint8_t i;
    uint32_t cur_size;
    uint32_t app_size;

    if (s_stream != NULL)
    {
        return 0;       /* 重复调用直接返回 */
    }

    /* 触发阈值 1：收到 1 个字节就唤醒任务，命令只有 3 字节，阈值大了会饿死 */
    s_stream = xStreamBufferCreateStatic(sizeof(s_stream_storage), 1U,
                                         s_stream_storage, &s_stream_struct);
    if (s_stream == NULL)
    {
        return -1;
    }

    s_frame_q = osMessageQueueNew(OTA_BUF_COUNT, sizeof(OtaFrame_t), &s_frame_q_attr);
    if (s_frame_q == NULL)
    {
        return -1;
    }

    for (i = 0U; i < OTA_BUF_COUNT; i++)
    {
        s_sem_free[i] = osSemaphoreNew(1U, 1U, &s_sem_free_attr[i]);
        if (s_sem_free[i] == NULL)
        {
            return -1;
        }
    }

    /* 外设初始化。两者都不参与中断，放内核启动前没问题 */
    if (Eeprom_Init() != 0)
    {
        return -1;
    }
    if (W25q64_Init() != 0)
    {
        return -1;
    }

    /* 记录当前运行的 App 大小，供服务器判断板子上是哪一版。
       用链接器符号算，不用手工维护一个数字 —— 那样每次改代码都会忘 */
    app_size = (uint32_t)((uintptr_t)&Image$$ER_IROM1$$Limit) - OTA_APP_BASE;
    cur_size = OtaEe_GetCurAppSize();
    if (cur_size != app_size)
    {
        (void)OtaEe_SetCurAppSize(app_size);
    }

    /* 上电时如果 EEPROM 停在"下载中"(0x11)，说明上次下载中途掉电了。
       清成 0x00，让服务器重新开始，不要拿着半截数据往下走 */
    if (OtaEe_GetState() == OTA_STATE_DOWNLOADING)
    {
        (void)OtaEe_SetState(OTA_STATE_NONE);
    }

    s_state = OTA_ST_WAIT_REQ_DOWNLOAD;
    Ota_CmdReset();

    return 0;
}

void OtaApp_RxArm(void)
{
    (void)HAL_UARTEx_ReceiveToIdle_DMA(&huart1, s_rx_chunk, OTA_RX_CHUNK);
}

void OtaApp_OnRxEvent(uint16_t len)
{
    BaseType_t woken = pdFALSE;

    if (s_stream == NULL)
    {
        return;
    }

    if (len > 0U)
    {
        /* 超时传 0 = 非阻塞。中断里不能等。
           缓冲满就丢 —— 说明任务没跟上，这不是能在这里补救的事 */
        (void)xStreamBufferSendFromISR(s_stream, s_rx_chunk, (size_t)len, &woken);
    }

    /* 重挂接收。HAL 进回调前已把 RxState 置回 READY、关掉了 DMAR 和 IDLEIE，
       所以这里重挂是安全的 */
    OtaApp_RxArm();

    portYIELD_FROM_ISR(woken);
}

void OtaApp_RxError(void)
{
    /* 只置标志，重挂交给任务。
       原因：HAL_DMA_Abort 是"关流 + 轮询等待"的阻塞实现，在中断里做会拖长中断 */
    s_uart_error = 1U;
}

OtaState_t OtaApp_GetState(void)
{
    return s_state;
}

void OtaApp_Task(void *argument)
{
    uint8_t chunk[OTA_DRAIN_CHUNK];
    size_t  n;
    size_t  i;

    (void)argument;

    /* 必须在 osKernelStart() 之后挂：中断回调里要调 xStreamBufferSendFromISR */
    OtaApp_RxArm();

    for (;;)
    {
        /* ---- 串口出过错 -> 重新挂起接收 ---- */
        if (s_uart_error != 0U)
        {
            s_uart_error = 0U;
            OtaApp_RxArm();
        }

        /* ---- 下载线程写 W25Q64 失败 -> 中止本轮 ----
           不等它的话会一直空转：OTA 线程继续收、下载线程继续失败，
           服务器那边还以为传输正常 */
        if (s_wr_error != 0U)
        {
            s_wr_error = 0U;

            Ota_WaitAllWritten();       /* 等还在写的那块收尾，别留下半写状态 */
            (void)OtaEe_SetState(OTA_STATE_NONE);

            Ota_CmdReset();
            s_state = OTA_ST_WAIT_REQ_DOWNLOAD;
        }

        /* ---- OtaEnd 是独立的一段：要等 20 秒按键，不能走下面的收包逻辑 ---- */
        if (s_state == OTA_ST_END)
        {
            Ota_HandleEndState();
            continue;
        }

        /* ---- 取数据并逐字节喂给状态机 ----
           用带超时的取法：取不到会返回 0，那时正好去检查空闲超时 */
        n = xStreamBufferReceive(s_stream, chunk, sizeof(chunk),
                                 pdMS_TO_TICKS(OTA_RX_POLL_MS));

        for (i = 0U; i < n; i++)
        {
            Ota_ConsumeByte(chunk[i]);
        }

        Ota_CheckIdle();
    }
}

void OtaApp_DownloadTask(void *argument)
{
    OtaFrame_t frame;
    uint32_t   wr_addr    = 0;              /* 下一个字节写到 W25Q64 的哪个地址 */
    uint32_t   sector     = 0xFFFFFFFFU;    /* 当前已擦好的扇区号，0xFFFFFFFF = 还没擦 */
    uint32_t   my_session = 0xFFFFFFFFU;    /* 正在处理第几轮；和 frame 里的不一样就重置 */

    (void)argument;

    for (;;)
    {
        if (osMessageQueueGet(s_frame_q, &frame, NULL, portMAX_DELAY) != osOK)
        {
            continue;
        }

        /* 轮次变了 = 新的一轮下载，写指针和擦除记录都回到起点。
           不能用"第一次写"这种一次性标志 —— 那样第二轮会接着上一轮的
           地址往下写，固件前半段永远不会被覆盖 */
        if (frame.session != my_session)
        {
            my_session = frame.session;
            wr_addr    = W25Q64_OTA_BASE_ADDR;
            sector     = 0xFFFFFFFFU;
        }

        {
            uint16_t done = 0U;

            while (done < frame.len)
            {
                uint32_t addr      = wr_addr;
                uint32_t sec_index = addr / W25Q64_SECTOR_SIZE;
                uint32_t sec_off   = addr % W25Q64_SECTOR_SIZE;
                uint32_t space     = W25Q64_SECTOR_SIZE - sec_off;
                uint32_t remain    = (uint32_t)(frame.len - done);
                uint16_t n         = (uint16_t)((space < remain) ? space : remain);

                /* 进入一个新扇区就擦一次。wr_addr 从 0 单调递增，
                   每次跨扇区必然正好落在扇区开头，不会擦掉刚写的数据 */
                if (sec_index != sector)
                {
                    if (W25q64_EraseSector(sec_index * W25Q64_SECTOR_SIZE) != 0)
                    {
                        (void)OtaEe_SetState(OTA_STATE_NONE);
                        s_wr_error = 1U;    /* 让 OTA 线程中止这一轮 */
                        break;
                    }
                    sector = sec_index;
                }

                if (W25q64_Write(addr, &frame.buf[done], n) != 0)
                {
                    (void)OtaEe_SetState(OTA_STATE_NONE);
                    s_wr_error = 1U;
                    break;
                }

                wr_addr += n;
                done    = (uint16_t)(done + n);
            }
        }

        /* 无论写成功还是中途失败，都要把缓冲还回去，
           否则 OTA 线程会永远卡在 Ota_BufTake。上面所有 break 都只跳出
           内层 while，一定会走到这里 */
        (void)osSemaphoreRelease(s_sem_free[frame.id]);
    }
}

/*********************************** (C) COPYRIGHT 2026 EternalChip *****END OF FILE***********************/
