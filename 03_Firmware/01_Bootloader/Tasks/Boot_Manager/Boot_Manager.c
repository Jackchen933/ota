#include "Boot_Manager.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_flash.h"
#include "Flash.h"
#include "Eeprom.h"
#include "W25Q_Handler.h"
#include "misc.h"
#include "aes.h"
#include <string.h>

int32_t app_size = 0;   /* Ymodem收到的密文总长度, BackToApp按此大小搬运 */

/* ===== AES-256 解密参数 =====
   必须与上位机打包工具用的 IV/Key 完全一致, 差一个字节解出来就全是乱码。
   IV 会被 Aes_IV_256bit_Decode 改写(每解一组就前移一次), 所以这里只存一份
   模板 IV_Init, 每次搬运前重新装填到工作副本 AES_IV。 */
static const uint8_t Key[32] =
{
    0x31,0x32,0x31,0x32,0x31,0x32,0x31,0x32,
    0x31,0x32,0x31,0x32,0x31,0x32,0x31,0x32,
    0x31,0x32,0x31,0x32,0x31,0x32,0x31,0x32,
    0x31,0x32,0x31,0x32,0x31,0x32,0x31,0x32
};
static const uint8_t IV_Init[16] =
{
    0x31,0x32,0x31,0x32,0x31,0x32,0x31,0x32,
    0x31,0x32,0x31,0x32,0x31,0x32,0x31,0x32
};
static uint8_t AES_IV[16];      /* IV工作副本, 每次搬运前由 IV_Init 重装 */

/**
  * @brief  判断某个槽位的固件是否有效
  *         依据: 槽位第一个字是栈顶指针, 合法的栈顶应指向RAM(0x2000xxxx)
  * @param  addr: 槽位起始地址
  * @retval 1: 有效  0: 无效(空白或非法)
  */
uint8_t Slot_IsValid(uint32_t addr)
{
    uint32_t stack_top = *(__IO uint32_t *)addr;

    if (stack_top == 0xFFFFFFFF)          /* 扇区是擦除后的空白状态 */
    {
        return 0;
    }
    return ((stack_top & 0x2FFE0000) == 0x20000000) ? 1 : 0;
}

/*==============================================================================
 * 旧版: 明文搬运。B区存的是未加密的固件, 直接按字拷到A区。
 * 现在B区存的是AES密文, 改用下面带解密的版本, 这段保留作对照, 不再参与编译。
 *============================================================================*/
#if 0
/**
  * @brief  把B区(备份区 BackApplicationAddress)的固件拷贝到A区(运行区 ApplicationAddress)
  * @retval 0: 成功  -1: 大小非法或校验失败
  */
int8_t BackToApp(void)
{
    int32_t j = 0;
    uint32_t FlashDestination = ApplicationAddress;
    uint32_t BackflashSource;

    /* 固件大小不能超过A区槽位(0x18000=96KB), 也不能是非法负值 */
    if ((app_size > (0x18000 - 1)) ||\
        (app_size < (0)))
    {
        return -1;
    }

    /* 擦除A区中覆盖本次固件大小的所有扇区 */
    Flash_erase(ApplicationAddress, app_size);

    /* 从B区起始地址开始, 按字(4字节)搬运到A区 */
    BackflashSource = BackApplicationAddress;
    for (j = 0;j < app_size; j+= 4)
    {
        Flash_Write(FlashDestination,*(uint32_t*)BackflashSource);

        /* 写入后读回校验, 不一致则失败 */
        if (*(uint32_t*)FlashDestination != *(uint32_t*)BackflashSource)
        {
            return -1;
        }
        FlashDestination += 4;
        BackflashSource += 4;
    }

    return 0;
}
#endif /* 旧版明文搬运结束 */

/*==============================================================================
 * 新版: 带 AES-256 解密的搬运
 *
 * B区密文布局(由上位机打包工具生成):
 *     [ 0 .. 15]  头部密文 —— 解出来后前12字节是自定义内容,
 *                 第12~15字节是固件真实大小(小端)
 *     [16 ..   ]  固件密文 —— 按16字节一组, AES-256 + CBC
 *
 * 搬运时先从Flash读16字节解一组, 校验着写进A区, 再读下一组。
 *============================================================================*/

/**
  * @brief  把B区(备份区)的密文解密后搬到A区(运行区)
  * @param  fl_size: B区中密文的总长度(含16字节头部)。
  *                  冷启动时固件还没下载、长度未知, 传0即可——这种情况会先
  *                  解开头部, 用头部里的固件大小反推密文总长。
  * @retval 0: 成功  -1: 长度非法、头部非法或写入校验失败
  */
int8_t BackToApp(int32_t fl_size)
{
    int32_t  j = 0;
    uint32_t FlashDestination = ApplicationAddress;
    uint32_t BackflashSource  = BackApplicationAddress;
    uint8_t  Temp[16];          /* 解密用的16字节暂存区 */
    uint8_t *pTemp = Temp;
    uint16_t readTime = 0, readDataCount = 0;   /* 需要解密的次数(每次16字节) */
    uint32_t AppSize = 0;                       /* 解密出的固件真实大小 */
    uint32_t cipherLen = 0;                     /* 密文总长 = 头部 + 数据块 */

    if (fl_size < 0)
    {
        return -1;
    }

    /* 密文整体要装得进B区。放在解密之前先挡一道, 挡掉明显不合法的长度 */
    if ((uint32_t)fl_size > (BACK_SLOT_SIZE - 1))
    {
        return -1;
    }

    /* 解头部: 16字节密文 -> 明文, 从中取出固件真实大小 */
    memcpy(AES_IV, IV_Init, sizeof(AES_IV));        /* 链值复位, 每次搬运都要重来 */
    memcpy(pTemp, (uint8_t *)BackflashSource, 16);
    BackflashSource += 16;
    Aes_IV_256bit_Decode(AES_IV, pTemp, Key);

    AppSize = ((uint32_t)pTemp[15] << 24) | ((uint32_t)pTemp[14] << 16) |
              ((uint32_t)pTemp[13] <<  8) |  (uint32_t)pTemp[12];

    /* AppSize 是从密文里解出来的, 内容不可信, 必须先卡住上界再拿去算擦除范围:
       它一旦是个垃圾大数, Flash_erase 会把A区以外的扇区(B区、甚至整片Flash)擦掉 */
    if ((AppSize == 0) || (AppSize > APP_SLOT_SIZE))
    {
        return -1;
    }

    readDataCount = (uint16_t)(AppSize / 16);
    if ((AppSize % 16) != 0)
    {
        readDataCount += 1;         /* 末尾不足16字节也要解一整组 */
    }

    cipherLen = 16 + ((uint32_t)readDataCount * 16);

    if (fl_size == 0)
    {
        /* 冷启动: 固件还没下载过, 长度只能由头部反推 */
        fl_size = (int32_t)cipherLen;
    }
    else if ((uint32_t)fl_size < cipherLen)
    {
        /* 收到带长度的调用, 但B区里的数据不够解出完整固件, 说明包不完整 */
        return -1;
    }

    /* 擦除A区中覆盖本次固件的所有扇区。
       这里传 AppSize - 1 而不是 AppSize, 是为了对付 Flash_erase 的边界问题:
       它按 STMFLASH_GetFlashSector(addr + size) 算结束扇区, 当 addr + size 正好
       落在下一个扇区的起始地址时会把那个扇区也算进去。AppSize 恰好等于 A区容量
       (0x18000) 时, 0x08008000 + 0x18000 = 0x08020000 就是B区扇区5, 会把正在读的
       密文整个擦掉。减1让结束地址停在A区内部, 擦除范围不变。 */
    if (1 == Flash_erase(ApplicationAddress, AppSize - 1))
    {
        return -1;
    }

    /* 逐组解密并写入A区: 每组16字节, 分4次按字写, 每次写完读回校验 */
    for (readTime = 0; readTime < readDataCount; readTime++)
    {
        pTemp = Temp;
        memcpy(pTemp, (uint8_t *)BackflashSource, 16);      /* 读一组密文到暂存区 */
        BackflashSource += 16;
        Aes_IV_256bit_Decode(AES_IV, pTemp, Key);           /* 原地解密成明文 */

        for (j = 0; j < 16; j += 4)
        {
            uint32_t word;

            /* 用memcpy取字而不是 *(uint32_t*)pTemp: Temp 是 uint8_t 数组,
               起始地址不保证4字节对齐, 直接强转取值在M4上属于未对齐访问 */
            memcpy(&word, pTemp, 4);

            Flash_Write(FlashDestination, word);

            if (*(__IO uint32_t *)FlashDestination != word)
            {
                return -1;
            }
            FlashDestination += 4;
            pTemp += 4;
        }
    }

    return 0;
}

/**
  * @brief  使B区失效(整槽擦除): 表示"该备份已被处理过",
  *         下次上电不会再重复拷贝, 直到Ymodem重新下载新固件
  * @param  None
  * @retval None
  */
void Backup_Invalidate(void)
{
    Flash_Unlock();
    FLASH_EraseSector(FLASH_Sector_5, VoltageRange_3);   /* B区=扇区5(0x08020000,128KB) */
    Flash_Lock();
}

/*==============================================================================
 * 新方案: 从外部 W25Q64 搬运到 A 区
 *
 * 和上面 Ymodem/AES 那套的区别:
 *   - 固件不再是主机通过 Ymodem 直接写进内部 B 区, 而是 APP 在运行时
 *     "后台无感"地转存到外部 W25Q64;
 *   - 上电时由这里把 W25Q64 的镜像搬到 A 区, 全程没有加解密 ——
 *     W25Q64 里存的就是 .bin 原始字节;
 *   - 是否需要搬运由 EEPROM(AT24C02) 里的状态字节决定, 不由按键决定。
 *
 * 掉电安全性:
 *   EEPROM 里的状态是 0x11(下载中) 时说明上次下载没下完, 这一轮直接放弃、
 *   什么都不动 —— 这时 A 区里还是完整的旧固件, 不能为了一个新包把它擦了。
 *   只有状态是 0x22(下载完毕) 才动手。
 *============================================================================*/

/* 搬运用的中转缓冲。
   必须放静态区: 启动文件里 Stack_Size 只有 0x400(1KB), 4KB 的局部数组
   会直接把栈踩穿, 而且踩的是栈底, 表现是"进函数就莫名 HardFault"。 */
#define BOOT_COPY_BUF_SIZE   (4096U)
static u8 s_Boot_CopyBuf[BOOT_COPY_BUF_SIZE];

/**
  * @brief  检查 W25Q64 里的镜像像不像一份能跑起来的固件
  * @param  size: 预期长度(字节)
  * @retval 0: 像   1: 不像 / 读失败
  * @note   这一步必须在【擦 A 区之前】做。源数据是垃圾而我们已经把 A 区
  *         擦了的话, 这块板子就只能靠 Ymodem 救了。
  *         判据用的是和 JumpToApp() 完全相同的两条: 首字是合法栈顶,
  *         复位向量落在 A 区内且带 Thumb 位。
  */
static u8 Boot_AppImage_Check(u32 size)
{
    u32 stack_top = 0;
    u32 reset_vec = 0;

    if (W25Qx_Read((u8 *)&stack_top, W25Q64_APP_BASE, 4) != W25Qx_OK)
    {
        return 1;
    }
    if (W25Qx_Read((u8 *)&reset_vec, W25Q64_APP_BASE + 4, 4) != W25Qx_OK)
    {
        return 1;
    }

    /* 栈顶指针应该指向 RAM(0x2000xxxx)。空白(0xFFFFFFFF)或乱码都在这里被挡掉 */
    if ((stack_top & 0x2FFE0000) != 0x20000000)
    {
        return 1;
    }

    reset_vec &= ~1UL;      /* 最低位是 Thumb 标志, 比较地址时要先去掉 */
    if ((reset_vec < ApplicationAddress) ||
        (reset_vec >= (ApplicationAddress + size)))
    {
        return 1;
    }

    return 0;
}

/**
  * @brief  把 W25Q64 里的新固件搬到 A 区
  * @retval 见 Boot_Manager.h 的说明
  */
int8_t Boot_Ota_Update(void)
{
    u8  state = OTA_STATE_NONE;
    u32 new_size = 0;
    u32 offset;

    if (0 != Eeprom_Init())
    {
        /* EEPROM 不通(没焊 / 没上拉)时不能当"有请求"处理, 否则会拿着
           读不出来的 0 去擦 A 区。直接当作没有升级请求, 让设备正常跑旧固件 */
        return 0;
    }

    if (0 != Eeprom_ReadByte(EE_ADDR_OTA_STATE, &state))
    {
        return 0;
    }

    if (OTA_STATE_DOWNLOADING == state)
    {
        /* 上次下载到一半掉电了, W25Q64 里是半包。A 区里还是完整的旧固件,
           这一轮不能动它。把标志清成"无请求", 让本轮正常跳旧 App;
           APP 下次连上服务器会重新下一整包 */
        (void)Eeprom_WriteByte(EE_ADDR_OTA_STATE, OTA_STATE_NONE);
        return 0;
    }

    if (OTA_STATE_REQ_UPDATE != state)
    {
        return 0;       /* 0x00: 没有升级请求, 走老流程 */
    }

    /*---- 到这里说明确实有升级请求 ----*/

    if (0 != Eeprom_ReadU32(EE_ADDR_NEW_APP_SIZE, &new_size))
    {
        return -1;
    }

    /* AppSize 来自外部, 内容不可信。它一旦是个大数, Flash_erase 会把
       A 区以外的扇区(B 区, 甚至 Bootloader 自己所在的扇区 0/1)一起擦掉 */
    if ((new_size == 0) || (new_size > APP_SLOT_SIZE))
    {
        return -1;
    }

    /* 初始化外部 Flash 并确认芯片在位。这一步放在最后: 没有升级请求时
       不该为它浪费时间 */
    if (0 != W25Q64_Init())
    {
        return -1;
    }

    if (0 != Boot_AppImage_Check(new_size))
    {
        return -1;
    }

    /* 擦除 A 区中覆盖本次固件的所有扇区。
       传 new_size - 1 而不是 new_size, 是为了绕过 Flash_erase 的边界问题:
       它按 STMFLASH_GetFlashSector(addr + size) 算结束扇区, 当 addr + size
       正好落在下一个扇区起始地址时会把那个扇区也算进去。new_size 恰好等于
       A 区容量(0x18000) 时, 0x08008000 + 0x18000 = 0x08020000, 会把 B 区
       整个擦掉。减 1 让结束地址停在 A 区内部, 擦除范围不变 */
    if (1 == Flash_erase(ApplicationAddress, new_size - 1))
    {
        return -1;
    }

    /* 整段只解锁一次。Flash_Write() 是"每写一个字就解锁/上锁一次"的封装,
       96KB 要重复两万多次, 每次还带一次 BUSY 轮询, 又慢又没有意义 */
    Flash_Unlock();

    for (offset = 0; offset < new_size; offset += BOOT_COPY_BUF_SIZE)
    {
        u32 remain = new_size - offset;
        u16 want   = (remain > BOOT_COPY_BUF_SIZE) ? (u16)BOOT_COPY_BUF_SIZE
                                                   : (u16)remain;
        u16 i;

        if (W25Qx_Read(s_Boot_CopyBuf, W25Q64_APP_BASE + offset, want) != W25Qx_OK)
        {
            Flash_Lock();
            return -1;
        }

        for (i = 0; i < want; i += 4)
        {
            u32 word;
            u32 addr = ApplicationAddress + offset + i;

            if ((u32)(i + 4) <= (u32)want)
            {
                memcpy(&word, &s_Boot_CopyBuf[i], 4);
            }
            else
            {
                /* 末尾不足 4 字节: 直接从 W25Q64 再读一个整字补上。
                   AppSize 一般总是 4 字节对齐(链接器给的区段长度), 这里不
                   做这个假设。多写的那几个字节落在 App 结尾之后, 没人会读 */
                if (W25Qx_Read((u8 *)&word, W25Q64_APP_BASE + offset + i, 4) != W25Qx_OK)
                {
                    Flash_Lock();
                    return -1;
                }
            }

            if (FLASH_ProgramWord(addr, word) != FLASH_COMPLETE)
            {
                Flash_Lock();
                return -1;
            }

            /* 写完立刻读回校验。Flash 编程失败不会报错到"明显能看出来"的程度,
               不校验的话会写进去一堆 0, 要到跳转过去跑飞了才发现 */
            if (*(__IO u32 *)addr != word)
            {
                Flash_Lock();
                return -1;
            }
        }
    }

    Flash_Lock();

    /* 先记大小再清标志, 顺序不能反:
       万一在两句之间掉电, 状态还是 0x22, 下次上电会重搬一遍(重搬是幂等的),
       反过来就是"标志清了但大小还是旧的", 那一包就永远丢了 */
    (void)Eeprom_WriteU32(EE_ADDR_CUR_APP_SIZE, new_size);
    (void)Eeprom_WriteByte(EE_ADDR_OTA_STATE, OTA_STATE_NONE);

    return 1;
}

void JumpToApp(void)
{
    /* 定义跳转地址变量和函数指针 */
    uint32_t JumpAddress;
    pFunction Jump_To_Application;

    /* 检查APP栈顶地址是否合法：栈顶指针应指向RAM区域(0x20000000附近) */
    if(((* (__IO uint32_t *)ApplicationAddress) & 0x2FFE0000) == 0x20000000)
    {
        /* 关闭全部中断，将中断向量表映射到APP起始地址(偏移0x8000)，并复位时钟 */
        __disable_irq();
        NVIC_SetVectorTable(NVIC_VectTab_FLASH, 0x8000);
        RCC_DeInit();

        /* 取APP复位向量地址(APP起始地址+4，即向量表第2项，APP的入口点) */
        JumpAddress = *(__IO uint32_t *) (ApplicationAddress + 4);

        /* Initialize user application's Stack Pointer */
        /* 设置APP的主栈指针MSP(APP向量表第1项为初始栈顶地址) */
        __set_MSP(*(__IO uint32_t *) ApplicationAddress);

        /* 将入口地址转为函数指针 */
        Jump_To_Application = (pFunction) JumpAddress;

        /* 跳转到APP执行 */
        Jump_To_Application();
    }
}
