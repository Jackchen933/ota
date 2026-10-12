/*********************************************************************************************************
 * @file    Boot_Manager.c
 * @brief   Bootloader 主状态机实现(子流程A/B/C/E + 主线路径D), 流程见 Boot_Manager.h
 * @version V1.0 2026-09-23
 * ********************************************************************************************************/
#include "Boot_Manager.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_flash.h"
#include "stm32f4xx_iwdg.h"
#include "tim.h"
#include "Flash.h"
#include "Eeprom.h"
#include "W25Q_Handler.h"
#include "w25qxx.h"
#include "aes.h"
#include "md5.h"
#include "gpio.h"
#include "usart.h"
#include "ymodem.h"
#include "elog.h"
#include <string.h>

int32_t app_size = 0;   /* Ymodem收到的密文包总长度(文件大小), Ymodem_Receive赋值 */

/* ===== AES-128 解密参数("读取加密芯片获取AES密钥"的软件模拟) =====
   目前没有独立加密芯片, 密钥固件内置; 以后换真加密芯片时只需把
   Boot_Get_AES_Key/IV 改成从芯片读取。IV 会被 Aes_IV_128bit_Decode
   改写(每解一组前移一次), 所以只存模板, 每次搬运前重装工作副本。
   密钥16字节=128位(轮数10), 必须与上位机打包工具用的密钥/位宽一致。 */
static const uint8_t Key[16] =
{
    0x31,0x32,0x31,0x32,0x31,0x32,0x31,0x32,
    0x31,0x32,0x31,0x32,0x31,0x32,0x31,0x32
};
static const uint8_t IV_Init[16] =
{
    0x31,0x32,0x31,0x32,0x31,0x32,0x31,0x32,
    0x31,0x32,0x31,0x32,0x31,0x32,0x31,0x32
};
static uint8_t AES_IV[16];      /* IV工作副本 */

/* 搬运用中转缓冲, 必须放静态区: 启动文件栈只有1KB, 局部大数组会踩穿栈 */
#define BOOT_COPY_BUF_SIZE   (1024U)
static uint8_t s_Boot_CopyBuf[BOOT_COPY_BUF_SIZE];

/*==============================================================================
 * 看门狗
 *============================================================================*/

/**
  * @brief  启动独立看门狗("SS看门狗"), 超时约8.2秒
  *         LSI约32kHz / 64分频 = 500Hz, 重装值4095 -> 8.19s
  * @note   IWDG开启后无法停止, App侧必须在超时内接管喂狗
  */
void IWDG_Start(void)
{
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_64);
    IWDG_SetReload(0x0FFF);
    IWDG_ReloadCounter();
    IWDG_Enable();
}

void IWDG_Feed(void)
{
    IWDG_ReloadCounter();
}

/*==============================================================================
 * 解密搬运核心(子流程A/C/D2共用)
 *============================================================================*/

/**
  * @brief  从外部Flash读密文包 -> 逐组AES解密 -> 写App运行区 -> 全程MD5 -> 尾部比对
  * @param  src_base:   外部Flash里密文包起始地址(W25Q64_APP_BASE 或 W25Q64_BAK_BASE)
  * @param  cipher_len: 密文包总长(含16字节头+固件密文+16字节MD5尾)
  * @param  fw_size_out:[out] 解出的固件真实大小(成功时有效)
  * @retval 0: 成功(MD5比对通过)
  *         -1: 参数非法/读密文失败
  *         -2: 写App运行区失败
  *         -3: MD5比对失败
  * @note   调用前保证 W25Q64_Init() 已成功; 搬运中每1K喂一次狗。
  */
static int8_t Boot_Decrypt_Copy(uint32_t src_base, uint32_t cipher_len, uint32_t *fw_size_out)
{
    uint32_t fw_size  = 0;
    uint32_t body_len = 0;          /* 固件密文体长度 */
    uint32_t done     = 0;          /* 已处理的密文体字节数 */
    uint32_t written  = 0;          /* 已写入运行区的明文字节数 */
    MD5_CTX   md5;
    uint8_t   digest[16];
    uint8_t   digest_ref[16];

    /* 密文包最小 = 16头 + 16(至少一组密文) + 16(MD5尾); 上限按App运行区推 */
    if (cipher_len < (16 + 16 + 16))
    {
        return -1;
    }
    if (cipher_len > (16 + APP_SLOT_SIZE + 16))
    {
        return -1;
    }

    /* 1. 解加密头: 16字节密文 -> 明文, 第12~15字节是固件真实大小(小端) */
    if (W25Qx_Read(s_Boot_CopyBuf, src_base, 16) != W25Qx_OK)
    {
        return -1;
    }
    memcpy(AES_IV, IV_Init, sizeof(AES_IV));        /* CBC链值复位 */
    Aes_IV_128bit_Decode(AES_IV, s_Boot_CopyBuf, Key);

    fw_size = ((uint32_t)s_Boot_CopyBuf[15] << 24) | ((uint32_t)s_Boot_CopyBuf[14] << 16) |
              ((uint32_t)s_Boot_CopyBuf[13] <<  8) |  (uint32_t)s_Boot_CopyBuf[12];

    /* 解出来的大小不可信, 先卡上界再拿去算擦除范围 */
    if ((fw_size == 0) || (fw_size > APP_SLOT_SIZE))
    {
        return -1;
    }

    /* 头部大小与密文包总长互验: 对不上说明包损坏/标志记录错误 */
    body_len = ((fw_size % 16) != 0) ? ((fw_size / 16) + 1) * 16 : (fw_size / 16) * 16;
    if (cipher_len != (16 + body_len + 16))
    {
        return -1;
    }

    /* 2. 擦App运行区(覆盖固件大小的所有扇区)。
       传 fw_size-1 绕开 Flash_erase 边界问题: addr+size 正好落在下一扇区起始
       时会把那个扇区也擦掉(fw_size=0x14000时0x0800C000+0x14000=0x08020000,
       会把扇区5擦了, 扇区5目前未用, 但仍按规矩减1) */
    if (1 == Flash_erase(ApplicationAddress, fw_size - 1))
    {
        return -2;
    }

    /* 3. 整段只解锁一次(Boot_Ota_Update时代的教训: 每字解锁两万多次又慢又无意义) */
    Flash_Unlock();
    MD5Init(&md5);

    /* 4. 按1K分块: 读密文 -> 解密 -> MD5 -> 写运行区 -> 喂狗 */
    while (done < body_len)
    {
        uint32_t chunk = body_len - done;
        uint32_t i;

        if (chunk > BOOT_COPY_BUF_SIZE)
        {
            chunk = BOOT_COPY_BUF_SIZE;
        }

        if (W25Qx_Read(s_Boot_CopyBuf, src_base + 16 + done, (uint16_t)chunk) != W25Qx_OK)
        {
            Flash_Lock();
            return -1;
        }

        for (i = 0; i < chunk; i += 16)
        {
            uint8_t *p = &s_Boot_CopyBuf[i];
            uint32_t valid = fw_size - written;     /* 本组里真正属于固件的字节数 */
            uint32_t w, words;

            Aes_IV_128bit_Decode(AES_IV, p, Key);   /* 原地解密16字节 */

            if (valid > 16)
            {
                valid = 16;
            }

            if (valid > 0)
            {
                MD5Update(&md5, p, valid);

                /* 按4字节字写入, 末尾不足4字节的整字补写(落在擦除区内, 无害) */
                words = (valid + 3) / 4;
                for (w = 0; w < words; w++)
                {
                    uint32_t word;
                    uint32_t addr = ApplicationAddress + written + w * 4;

                    memcpy(&word, p + w * 4, 4);    /* 明文缓冲不保证对齐, memcpy取字 */

                    if (FLASH_ProgramWord(addr, word) != FLASH_COMPLETE)
                    {
                        Flash_Lock();
                        return -2;
                    }
                    if (*(__IO uint32_t *)addr != word)   /* 写完读回校验 */
                    {
                        Flash_Lock();
                        return -2;
                    }
                }
                written += valid;
            }
        }

        done += chunk;
        IWDG_Feed();
    }
    Flash_Lock();

    /* 5. MD5比对: 与密文包末尾16字节(上位机打包时算好的固件明文MD5)比较 */
    MD5Final(digest, &md5);
    if ((W25Qx_Read(digest_ref, src_base + cipher_len - 16, 16) != W25Qx_OK) ||
        (memcmp(digest, digest_ref, 16) != 0))
    {
        return -3;
    }

    *fw_size_out = fw_size;
    return 0;
}

/* 把解出的App真实大小记进EEPROM(App侧监控用), 失败不阻塞主流程 */
static void Boot_RecordAppSize(uint32_t fw_size)
{
    (void)Eeprom_WriteU32(EE_ADDR_CUR_APP_SIZE, fw_size);
}

/*==============================================================================
 * 子流程A: App下载固件待更新
 *============================================================================*/
void Boot_SubflowA(void)
{
    uint32_t cipher_len = 0;
    uint32_t fw_size    = 0;

    log_i("Boot: subflow A (dl pending)");
    IWDG_Start();

    if ((0 != Flag_GetEaSize(&cipher_len)) ||
        (0 != W25Q64_Init()) ||
        (0 != Boot_Decrypt_Copy(W25Q64_APP_BASE, cipher_len, &fw_size)))
    {
        /* 新包不可用 -> 按流程图转回滚备份区App(子流程C, 不返回) */
        log_e("Boot: subflow A failed, rollback");
        Boot_SubflowC();
        return;
    }

    Boot_RecordAppSize(fw_size);

    /* 置"验证新App" -> 软复位, 下一轮主线走子流程B */
    (void)Flag_SetAppState(APP_ST_VERIFY_NEW);
    NVIC_SystemReset();
}

/*==============================================================================
 * 子流程B: 验证新App
 *============================================================================*/
void Boot_SubflowB(void)
{
    log_i("Boot: subflow B (verify new app)");

    /* 流程图: 设置扇区App标识为(App验证中)。
       "验证新App"和App侧视角的"App验证中"是同一个状态(0x22222222):
       这里重写一遍保证字段完整, App启动后读到它就知道自己在待验证期 */
    (void)Flag_SetAppState(APP_ST_VERIFY_NEW);

    /* 开启看门狗 -> 跳转App(App自检通过后会置"A区搬运至B区"并复位) */
    IWDG_Start();
    JumpToApp();
}

/*==============================================================================
 * 子流程C: 回滚备份区App
 *============================================================================*/
void Boot_SubflowC(void)
{
    uint32_t eb_st = 0, cipher_len = 0, fw_size = 0;

    log_i("Boot: subflow C (rollback)");
    IWDG_Start();

    Flag_GetEb(&eb_st, &cipher_len);

    if ((eb_st != BAK_ST_VALID) || (cipher_len == 0) ||
        (0 != W25Q64_Init()) ||
        (0 != Boot_Decrypt_Copy(W25Q64_BAK_BASE, cipher_len, &fw_size)))
    {
        /* 备份也救不回来: 置"无效"扩展状态, 复位后走路径D待机等Ymodem救援 */
        log_e("Boot: rollback failed");
        (void)Flag_SetAppState(APP_ST_INVALID);
        NVIC_SystemReset();
        return;
    }

    Boot_RecordAppSize(fw_size);
    (void)Flag_SetAppState(APP_ST_VALID);
    NVIC_SystemReset();
}

/*==============================================================================
 * 子流程E: 更新备份区App(A区密文拷到B区, 无解密无校验)
 *============================================================================*/
void Boot_SubflowE(void)
{
    uint32_t cipher_len = 0;
    uint32_t off;

    log_i("Boot: subflow E (backup A->B)");
    IWDG_Start();

    if ((0 != Flag_GetEaSize(&cipher_len)) || (0 != W25Q64_Init()))
    {
        (void)Flag_SetAppState(APP_ST_INVALID);
        NVIC_SystemReset();
        return;
    }

    /* 擦B区域涉及的4KB扇区 */
    for (off = 0; off < cipher_len; off += 4096)
    {
        if (W25Qx_Erase_Block(W25Q64_BAK_BASE + off) != W25Qx_OK)
        {
            (void)Flag_SetAppState(APP_ST_INVALID);
            NVIC_SystemReset();
            return;
        }
    }

    /* 1K分块: A区域 -> B区域, 纯密文拷贝 */
    for (off = 0; off < cipher_len; off += BOOT_COPY_BUF_SIZE)
    {
        uint32_t n = cipher_len - off;

        if (n > BOOT_COPY_BUF_SIZE)
        {
            n = BOOT_COPY_BUF_SIZE;
        }
        if ((W25Qx_Read(s_Boot_CopyBuf, W25Q64_APP_BASE + off, (uint16_t)n) != W25Qx_OK) ||
            (W25Qx_Write(s_Boot_CopyBuf, W25Q64_BAK_BASE + off, n) != W25Qx_OK))
        {
            (void)Flag_SetAppState(APP_ST_INVALID);
            NVIC_SystemReset();
            return;
        }
        IWDG_Feed();
    }

    /* 置B区有效+大小, 并置"有效App" -> 软复位 */
    (void)Flag_SetAll(APP_ST_VALID, cipher_len, BAK_ST_VALID, cipher_len);
    NVIC_SystemReset();
}

/*==============================================================================
 * 主线路径D: 其他/空白状态
 *============================================================================*/

/* 待机循环: LED闪烁指示, 等按键(不开看门狗, 允许无限等待) */
static void Boot_Standby(void)
{
    static uint32_t blink = 0;

    while (1)
    {
        if (Key_Scan())
        {
            return;         /* 按下 -> 返回去走Ymodem下载 */
        }

        LED_OFF;
        Delay(200);
        LED_ON;
        Delay(200);
        if (++blink >= 8)   /* 每隔几秒打点一次日志, 避免刷屏 */
        {
            blink = 0;
            log_i("Boot: standby, press key to update");
        }
    }
}

void Boot_PathD(void)
{
    uint32_t eb_st = 0, eb_size = 0;
    uint32_t fw_size = 0;

    log_i("Boot: path D (idle)");

    /* 关呼吸灯: 后面待机闪烁LED和下载过程都不允许TIM3中断每50us抢写PC13 */
    TIM_Cmd(TIM3, DISABLE);
    TIM_ITConfig(TIM3, TIM_IT_Update, DISABLE);

    /* 1. 检查备份区App标识: 有效 -> 转回滚子流程C(不返回) */
    if (0 == W25Q64_Init())
    {
        Flag_GetEb(&eb_st, &eb_size);
        if ((eb_st == BAK_ST_VALID) && (eb_size != 0))
        {
            Boot_SubflowC();
            return;
        }
    }

    /* 2. 检测按键: 没按下 -> 循环待机; 按下 -> Ymodem下载(子流程D1) */
    Boot_Standby();

    log_i("Boot: ymodem download start");

    /* D1: Ymodem收密文包 -> 写外部Flash A区域(函数内每帧喂狗, 收到首帧后开狗) */
    app_size = Ymodem_Receive(s_Boot_CopyBuf);

    if (app_size <= 0)
    {
        Serial_PutString((uint8_t *)"File update failed\r\n");
        NVIC_SystemReset();             /* 复位回主线 -> 路径D重新待机 */
        return;
    }

    /* 先落标志再搬运: 记录A区密文大小并置"App下载固件待更新",
       搬运途中掉电的话下次上电走子流程A重搬(幂等) */
    (void)Flag_SetEaSize((uint32_t)app_size);
    (void)Flag_SetAppState(APP_ST_DL_PENDING);

    /* D2: 解密搬运A区数据 -> MD5比对 */
    IWDG_Start();
    if (0 != Boot_Decrypt_Copy(W25Q64_APP_BASE, (uint32_t)app_size, &fw_size))
    {
        log_e("Boot: decrypt/copy failed");
        Serial_PutString((uint8_t *)"File update failed\r\n");
        NVIC_SystemReset();
        return;
    }

    Boot_RecordAppSize(fw_size);

    /* 置"验证新App" -> 软复位, 下一轮主线走子流程B */
    (void)Flag_SetAppState(APP_ST_VERIFY_NEW);
    NVIC_SystemReset();
}

/*==============================================================================
 * 跳转App
 *============================================================================*/
void JumpToApp(void)
{
    uint32_t JumpAddress;
    pFunction Jump_To_Application;

    /* 栈顶指针应指向RAM区域(0x20000000附近) */
    if (((*(__IO uint32_t *)ApplicationAddress) & 0x2FFE0000) == 0x20000000)
    {
        /* 跳前喂一口狗: IWDG 一旦开启就关不掉、复位后照样在跑,
           正常启动路径(0x44直接跳)的狗可能还在倒计时 —— App 从启动到
           自己接管喂狗有几秒窗口, 这一口把窗口拉满 */
        IWDG_Feed();

        /* 关呼吸灯: TIM3中断每50us改写PC13, 不停掉会覆盖App对LED的操作 */
        TIM_Cmd(TIM3, DISABLE);
        TIM_ITConfig(TIM3, TIM_IT_Update, DISABLE);

        /* 关全部中断, 向量表重映射到App起始地址(0x0800C000) */
        __disable_irq();
        NVIC_SetVectorTable(NVIC_VectTab_FLASH, 0xC000);
        RCC_DeInit();

        /* 取App复位向量(向量表第2项), 设好MSP后跳转 */
        JumpAddress = *(__IO uint32_t *)(ApplicationAddress + 4);
        __set_MSP(*(__IO uint32_t *)ApplicationAddress);
        Jump_To_Application = (pFunction)JumpAddress;
        Jump_To_Application();
    }
}
