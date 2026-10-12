/**
  ******************************************************************************
  * @file    aes.c
  * @brief   AES 加解密算法实现（纯软件，无外部依赖，兼容 51 与 ARM）
  *
  * 支持 128 / 192 / 256 位密钥与 ECB / CBC 两种模式。
  * 密钥长度和加密模式在 aes.h 中通过 AES_KEY_LENGTH / AES_MODE 宏配置，
  * 未使用的代码会被条件编译裁掉，以节省 Flash 和 RAM。
  *
  * 状态矩阵按列优先存放：state[row + 4 * col]
  ******************************************************************************
  */

#include "aes.h"
#include <string.h>

/*==============================================================================
 * 1. 兼容51
 *
 * #ifndef __C51__ :
 * 检查是否定义了 __C51__，用于识别是否在C51编译环境下。C51环境下有一些
 * 特定关键字（如 code、data、idata、xdata、pdata）。
 * 如果不是C51环境（即没有定义 __C51__），就将这些关键字定义为空，
 * 以确保在其他环境中不会出错。
 * 同时，在非C51环境中，将 BOOL 定义为 unsigned char 类型，
 * 而在C51环境中定义为 bit 类型（8051系列）。
 *============================================================================*/
#ifndef __C51__
    #define code
    #define data
    #define idata
    #define xdata
    #define pdata
    typedef unsigned char BOOL;
#else
    typedef bit BOOL;
#endif

/*==============================================================================
 * 2. 密钥长度单位转换
 *
 * Nk 表示AES密钥的长度（单位是字，4字节/字）。
 * 假设 AES_KEY_LENGTH 为 128、192或256，则 Nk 分别为4、6、8。
 *============================================================================*/
#define Nk  (AES_KEY_LENGTH / 32)

/*==============================================================================
 * 3. 定义数据块大小
 *
 * Nb 表示数据块大小（单位是字），固定为4。这是AES算法的固定标准，
 * AES处理的是4x4的字节块。
 *============================================================================*/
#define Nb  4

/*==============================================================================
 * 4. 定义加密轮数
 *
 * Nr 表示AES的轮数，取决于密钥长度。通过 #if 语句确定 AES_KEY_LENGTH
 * 值为 128、192或256时的轮数。
 *
 * 注意：AES_KEY_LENGTH_128 这类取值在 aes.h 中必须是宏而不是枚举成员，
 * 因为枚举常量在预处理阶段一律按 0 处理，会让这里的 #if 判断恒真。
 *============================================================================*/
#if AES_KEY_LENGTH == AES_KEY_LENGTH_128
    #define Nr  10
#elif AES_KEY_LENGTH == AES_KEY_LENGTH_192
    #define Nr  12
#elif AES_KEY_LENGTH == AES_KEY_LENGTH_256
    #define Nr  14
#else
    #error AES_KEY_LENGTH must be 128, 192 or 256 BITS!
#endif

/* 加密模式取值检查，同样比较宏而不是枚举成员 */
#if ((AES_MODE != AES_MODE_ECB) && (AES_MODE != AES_MODE_CBC))
    #error AES_MODE must be AES_MODE_ECB or AES_MODE_CBC!
#endif

/*==============================================================================
 * 5. 定义不可约多项式
 *
 * BPOLY 为 AES 不可约多项式 x^8 + x^4 + x^3 + x + 1 的低 8 位，
 * 用于 xtime 运算（GF(2^8) 域内乘 2）。
 *============================================================================*/
#define BPOLY  0x1B

/*==============================================================================
 * 私有变量
 *============================================================================*/

/* 是否在运行时用算法生成 S 盒 / 逆 S 盒。
 *   0 —— 使用下面预存的静态表（推荐：查表最快，表放代码段还不占 RAM）
 *   1 —— 启动时用 gmul 逐个求逆元再仿射变换，生成到 RAM 中
 *         （省下 512 字节 Flash，代价见第 7 节的说明）
 * 生成算法见文件末尾的 generate_aes_sbox / generate_aes_inverse_sbox。 */
#define AES_SBOX_RUNTIME_GENERATE   0

#if (AES_SBOX_RUNTIME_GENERATE == 0)

/* S 盒：字节代换用。
 * 加 code 修饰是为了在 51 上放进代码段、不占用本就紧张的 RAM；
 * 非 C51 环境下 code 已被定义为空，不影响 ARM/GCC 编译。 */
static const unsigned char code Sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

/* 逆 S 盒：逆字节代换用，同样放代码段 */
static const unsigned char code InvSbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

#else
/* 运行时生成模式：两张表放在 RAM 里，由 AES_Init 首次调用时填好 */
static unsigned char Sbox[256];
static unsigned char InvSbox[256];
#endif

/* AES子密钥表，当密钥长度为128位时，占用176字节空间
 * 共 (Nr + 1) 组，每组 4 * Nb = 16 字节，最多 15 * 16 = 240 字节。
 * 240 字节远超 51 的内部 RAM，所以要放 xdata；
 * 非 C51 环境下 xdata 已被定义为空，不影响 ARM/GCC 编译。 */
static xdata unsigned char g_roundKeyTable[4 * Nb * (Nr + 1)];

/* 密钥扩展是否已完成。未完成时拒绝加解密，避免用全 0 轮密钥静默算错 */
static BOOL bAesReady = 0;

/*==============================================================================
 * 私有函数声明
 *============================================================================*/
static void rotation_word(unsigned char *pWord);
static void xor_bytes(unsigned char *pData1, const unsigned char *pData2,
                      unsigned char nCount);
static void AddRoundKey(unsigned char *pState, const unsigned char *pRoundKey);
static void sub_bytes(unsigned char *pState, unsigned char nCount, BOOL bInvert);
static void shift_rows(unsigned char *pState, BOOL bInvert);
static unsigned char gf_mult_by02(unsigned char num);
static void mix_columns(unsigned char *pState, BOOL bInvert);
static void Cipher(unsigned char *pState);
static void InvCipher(unsigned char *pState);

#if (AES_SBOX_RUNTIME_GENERATE != 0)
static unsigned char ginv(unsigned char x);
static unsigned char affineTransform(unsigned char x);
static void generate_aes_sbox(unsigned char sbox[256]);
static void generate_aes_inverse_sbox(const unsigned char sbox[256],
                                      unsigned char inv_sbox[256]);
#endif

/*==============================================================================
 * 私有函数实现
 *============================================================================*/

/**
  * @brief 在 GF(2^8) 域内进行乘以 2 的运算.
  *
  * 该函数实现 GF(2^8) 的乘2运算。根据有限域的定义，
  * 当乘数的最高位为0时，直接左移一位；当最高位为1时，
  * 左移后需要与多项式 BPOLY 进行异或运算。
  *
  * @param[in] num 需要乘以 2 的数（8 位无符号整数）.
  *
  * @return num 乘以 2 的结果（8 位无符号整数）.
  */
static unsigned char gf_mult_by02(unsigned char num)
{
    /* 检查 num 的最高位（第8位）是否为 0 */
    if ((num & 0x80) == 0)              /* 通过位与操作检查最高位 */
    {
        num = (unsigned char)(num << 1);            /* 如果最高位为 0，左移一位 */
    }
    else
    {
        /* 如果最高位为 1，左移一位并与 GF(2^8) 的模多项式进行异或 */
        num = (unsigned char)((num << 1) ^ BPOLY);  /* 左移后与 BPOLY 异或 */
    }

    return num;                         /* 返回乘以 2 的结果 */
}

/*==============================================================================
 * 6. 有限域乘法
 *
 * AES_MOD 为 AES 的不可约多项式 x^8 + x^4 + x^3 + x + 1。
 * 它比 BPOLY 多一个第 9 位的 1；由于 gmul 里 a 是 unsigned char，
 * 左移后高位会被截掉，所以这里写完整的 0x11B 和写 0x1B 结果一致。
 *
 * gmul 只在运行时生成 S 盒（第 7 节）时用到。列混淆走的是更快的
 * gf_mult_by02，用不到通用乘法，所以查表模式下这里整段不参与编译，
 * 免得 static 函数"定义了却没用"而报 warning。
 *============================================================================*/
#if (AES_SBOX_RUNTIME_GENERATE != 0)

#define AES_MOD  0x11B

/**
  * @brief   计算在 GF(2^8) 中的乘法
  * @param   a, b 参与相乘的两个字节
  * @retval  a * b mod AES_MOD
  *
  * 原理是"俄式乘法"：把 b 逐位拆开，b 的第 i 位为 1 就累加 a * x^i。
  * GF(2^8) 中的加法就是异或，乘以 x 就是左移一位，
  * 左移后若溢出（最高位为 1）则再异或不可约多项式把结果拉回域内。
  */
static unsigned char gmul(unsigned char a, unsigned char b)
{
    unsigned char p = 0;    /* 初始化积为 0 */

    while (b)               /* 当 b 不为 0 时循环 */
    {
        if (b & 1)          /* 如果 b 的最低位为 1，则将 a 累加到 p 中 */
        {
            p ^= a;
        }

        /* 将 a 左移一位，并根据最高位决定是否异或不可约多项式 */
        a = (unsigned char)((a << 1) ^ ((a & 0x80) ? AES_MOD : 0));

        b >>= 1;            /* 将 b 右移一位 */
    }

    return p;               /* 返回计算的结果 */
}

#endif /* AES_SBOX_RUNTIME_GENERATE */

/**
  * @brief 函数名: rotation_word
  *        描述:     定义一个静态函数，循环左移输入的字（4 个字节）
  *        输入参数: pWord -- 要移位的 4 字节数据
  *        输出参数: pWord -- 移位后的 4 字节数据
  *        返回值:   无
  */
static void rotation_word(unsigned char *pWord)
{
    unsigned char temp;                     /* 临时保存第一个字节 */

    /* 检查输入指针是否为空，避免潜在的空指针引用错误 */
    if (pWord == NULL)
    {
        return;                             /* 参数检查，若为空则返回 */
    }

    /* 保存第一个字节到临时变量 */
    temp = pWord[0];

    /* 将字的每个字节向前移动一个位置 */
    pWord[0] = pWord[1];                    /* 第一个字节变为第二个字节 */
    pWord[1] = pWord[2];                    /* 第二个字节变为第三个字节 */
    pWord[2] = pWord[3];                    /* 第三个字节变为第四个字节 */

    /* 将原本的第一个字节放到最后一个位置 */
    pWord[3] = temp;                        /* 第四个字节变为原来的第一个字节 */
}

/**
  * @brief 批量异或两组数据.
  *
  * 此函数将 pData2 中的每个字节与 pData1 中的对应字节进行异或操作，
  * 结果直接存储在 pData1 中。
  * 适用于需要逐字节混淆或合并数据的应用场景，如加密算法中的混淆步骤。
  *
  * @param[out] pData1 要异或的第一组数据，同时用于存储异或结果.
  * @param[in]  pData2 要异或的第二组数据.
  * @param[in]  nCount 要异或的数据字节长度.
  *
  * @note 调用者需确保 pData1 和 pData2 均至少有 nCount 个字节，否则可能导致访问越界。
  *       本函数对指针为空不进行检查。
  */
static void xor_bytes(unsigned char *pData1, const unsigned char *pData2,
                      unsigned char nCount)
{
    /* 定义一个循环计数变量 i */
    unsigned char i;

    /* 循环遍历每个字节，进行逐位异或 */
    for (i = 0; i < nCount; i++)
    {
        /* 将 pData1 的每个字节与 pData2 的相应字节异或
           异或结果直接存储在 pData1 中 */
        pData1[i] ^= pData2[i];
    }
}

/**
  * @brief 将中间状态数据与子密钥进行异或操作.
  *
  * 此函数将状态数据 pState 与子密钥 pRoundKey 进行异或运算，结果存储在 pState 中。
  * 数据长度固定为 16 字节（4 个字，每个字 4 字节），
  * 通常用于 AES 加密算法的轮密钥加操作.
  *
  * @param[in,out] pState     状态数据，经过异或后将更新为新的状态数据.
  * @param[in]     pRoundKey  子密钥数据，用于与状态数据进行异或.
  *
  * @note 此函数对输入指针不进行空值检查，调用者需确保
  *       pState 和 pRoundKey 不为空并具有有效长度.
  */
static void AddRoundKey(unsigned char *pState, const unsigned char *pRoundKey)
{
    xor_bytes(pState, pRoundKey, (unsigned char)(4 * Nb));
}

/* S 盒指针类型：
 * 查表模式下两张表带 code 修饰（51 上放在代码段），指向它们的指针也必须带 code，
 * 否则 51 会按 data 段寻址、取不到正确数据；运行时生成模式下表在 RAM 中，
 * 只能用普通指针。非 C51 环境下 code 已被定义为空，两种情况写法本来就一样。 */
#if (AES_SBOX_RUNTIME_GENERATE == 0)
    #define AES_SBOX_PTR    const unsigned char code *
#else
    #define AES_SBOX_PTR    const unsigned char *
#endif

/**
  * @brief 使用 S-盒对状态数据进行字节替换.
  *
  * 此函数将状态数据中的每个字节用 S-盒进行替换。
  * 可选择使用反向 S-盒（解密时使用）。
  *
  * @param[in,out] pState    状态数据，经过替换后将更新为新的状态数据.
  * @param[in]     nCount    状态数据的长度.
  * @param[in]     bInvert   指示是否使用反向 S-盒.
  *
  * @note 此函数对输入指针不进行空值检查，调用者需确保
  *       pState 不为空且 nCount 有效.
  */
static void sub_bytes(unsigned char *pState, unsigned char nCount, BOOL bInvert)
{
    unsigned char i;                                /* 循环索引 */

    AES_SBOX_PTR pSbox = bInvert ? InvSbox : Sbox;  /* 根据是否反向选择 S-盒 */

    for (i = 0; i < nCount; i++)                    /* 遍历状态数据的每个字节 */
    {
        pState[i] = pSbox[pState[i]];               /* 用 S-盒替换状态数据中的字节 */
    }
}

/**
  * @brief 对状态数据进行行移位操作.
  *
  * 此函数根据 AES 算法的规定，将状态数据中的每一行移位。
  * 可选择反向移位（解密时使用）。
  *
  * @param[in,out] pState    状态数据，经过移位后将更新为新的状态数据.
  * @param[in]     bInvert   指示是否进行反向移位.
  *
  * @note 状态数据以列为主进行存储，调用者需确保 pState 不为空.
  */
static void shift_rows(unsigned char *pState, BOOL bInvert)
{
    /* 注意: 状态数据以列形式存放! */

    unsigned char r;            /* 行索引 */
    unsigned char c;            /* 列索引 */
    unsigned char temp;         /* 用于存储移位值 */
    unsigned char rowData[4];   /* 用于备份当前行数据 */

    for (r = 1; r < 4; r++)     /* 从第二行开始，循环到第四行 */
    {
        /* 备份当前行的数据 */
        for (c = 0; c < 4; c++)
        {
            rowData[c] = pState[r + 4 * c];     /* 将当前行的字节存入 rowData 中 */
        }

        /* 根据是否反向选择移位的步数 */
        temp = (unsigned char)(bInvert ? (4 - r) : r);      /* 计算移位步数 */

        for (c = 0; c < 4; c++)
        {
            /* 更新状态数据，执行移位 */
            pState[r + 4 * c] = rowData[(c + temp) % 4];
        }
    }
}

/**
  * @brief 对状态数据进行列混淆操作.
  *
  * 此函数对状态数据进行列混淆（加密）或逆向列混淆（解密）操作。
  * 列混淆是对状态矩阵的每一列应用线性变换。
  * 可选择反向操作（解密时使用）。
  *
  * @param[in,out] pState    状态数据，经过列混淆后将更新为新的状态数据.
  * @param[in]     bInvert   指示是否进行反向列混淆.
  *
  * @note 调用者需确保 pState 不为空.
  */
static void mix_columns(unsigned char *pState, BOOL bInvert)
{
    unsigned char i;                /* 循环索引 */
    unsigned char a0, a1, a2, a3;   /* 当前列中的四个字节 */

    for (i = 0; i < Nb; i++)        /* 遍历所有列 */
    {
        /* 从状态矩阵中提取当前列的四个字节 */
        a0 = pState[4 * i + 0];
        a1 = pState[4 * i + 1];
        a2 = pState[4 * i + 2];
        a3 = pState[4 * i + 3];

        if (bInvert)                /* 解密时的逆向列混淆 */
        {
            /* 逆列混淆矩阵
             *   [14 11 13  9]
             *   [ 9 14 11 13]
             *   [13  9 14 11]
             *   [11 13  9 14]
             * 可以分解成"一次额外的列变换 + 一次正向列混淆"。这一步就是
             * 那个额外的列变换：把 a0、a2 同 4*(a0^a2) 异或，把 a1、a3 同
             * 4*(a1^a3) 异或；随后走下面正向列混淆的公式，结果正好是逆列
             * 混淆，这样就不用像 14a0^11a1^13a2^9a3 那样做 16 次有限域乘法。 */
            unsigned char a0Pa2_M4 = (unsigned char)(gf_mult_by02(gf_mult_by02((unsigned char)(a0 ^ a2))));
            unsigned char a1Pa3_M4 = (unsigned char)(gf_mult_by02(gf_mult_by02((unsigned char)(a1 ^ a3))));

            a0 = (unsigned char)(a0 ^ a0Pa2_M4);
            a1 = (unsigned char)(a1 ^ a1Pa3_M4);
            a2 = (unsigned char)(a2 ^ a0Pa2_M4);
            a3 = (unsigned char)(a3 ^ a1Pa3_M4);
        }

        /* 正向列混淆：每列的四个字节看作多项式，模 x^4+1 乘以 03 01 01 02
         * 即 result[0] = 2a0 ^ 3a1 ^ a2 ^ a3，其余三行同理循环下移。
         * 3a1 拆成 2a1 ^ a1，于是每行只需 2 次 gf_mult_by02。 */
        pState[4 * i + 0] = (unsigned char)(gf_mult_by02(a0) ^ gf_mult_by02(a1) ^ a1 ^ a2 ^ a3);
        pState[4 * i + 1] = (unsigned char)(a0 ^ gf_mult_by02(a1) ^ gf_mult_by02(a2) ^ a2 ^ a3);
        pState[4 * i + 2] = (unsigned char)(a0 ^ a1 ^ gf_mult_by02(a2) ^ gf_mult_by02(a3) ^ a3);
        pState[4 * i + 3] = (unsigned char)(gf_mult_by02(a0) ^ a0 ^ a1 ^ a2 ^ gf_mult_by02(a3));
    }
}

/**
  * @brief  加密一个分组（已含轮密钥加，但不含 CBC 的异或）
  */
static void Cipher(unsigned char *pState)
{
    unsigned char round;

    /* 第 0 轮：只做轮密钥加，用的是子密钥表开头的 16 字节 */
    AddRoundKey(pState, &g_roundKeyTable[0]);

    /* 第 1 ~ Nr 轮合并到一个循环里。最后一轮（round == Nr）不做列混淆，
       用 if 挡掉，这样就不必把收尾轮再单独写一遍——等价于 FIPS-197 的
       「Nr-1 个完整轮」+「1 个不含列混淆的收尾轮」。 */
    for (round = 1; round <= Nr; round++)
    {
        sub_bytes(pState, (unsigned char)(4 * Nb), 0);  /* 字节代换 */
        shift_rows(pState, 0);                          /* 行移位   */

        if (round != Nr)
        {
            mix_columns(pState, 0);                     /* 列混淆（末轮跳过） */
        }

        AddRoundKey(pState, &g_roundKeyTable[round * 4 * Nb]);  /* 轮密钥加 */
    }
}

/**
  * @brief  解密一个分组（与 Cipher 的步骤完全对称、逆序）
  *
  * 轮密钥的下标方向和 Cipher 相反：Cipher 从 0 走到 Nr，这里从 Nr 走回 0。
  * 每一步都取 Cipher 对应步骤的逆运算——逆行移位、逆字节代换、逆列混淆；
  * 只有轮密钥加不用取逆，因为异或自逆，同一个操作再异或一次就还原。
  */
static void InvCipher(unsigned char *pState)
{
    unsigned char round;

    /* 第 Nr 轮的子密钥先加进去，与 Cipher 最后一轮对应 */
    AddRoundKey(pState, &g_roundKeyTable[Nr * 4 * Nb]);

    /* round 从 Nr 递减到 1，把 Cipher 的操作严格倒着做一遍。
       注意 AddRoundKey 用的是 (round - 1)，比循环变量慢一拍——
       因为 Cipher 里轮密钥加排在列混淆之后，倒推时它要先被解开。 */
    for (round = Nr; round > 0; round--)
    {
        shift_rows(pState, 1);                          /* 逆行移位   */
        sub_bytes(pState, (unsigned char)(4 * Nb), 1);  /* 逆字节代换 */
        AddRoundKey(pState, &g_roundKeyTable[(round - 1) * 4 * Nb]);

        if (round != 1)
        {
            mix_columns(pState, 1);                     /* 逆列混淆（末次跳过） */
        }
    }
}

/*==============================================================================
 * 7. Sbox 的生成
 *
 * 上面两张表是预先算好写死在 Flash 里的，直接用最快。下面给出用算法
 * 实时生成它们的实现，既能说明"表里的数是怎么来的"，也能在不想占用
 * 512 字节 Flash 时改用生成的方式（把 AES_SBOX_RUNTIME_GENERATE 改成 1）。
 *
 * 生成思路：
 *   1) S 盒：对每个字节先求 GF(2^8) 中的乘法逆元，再套一次仿射变换。
 *      0 的逆元按约定取 0，而 affineTransform(0) 恰好等于 0x63，
 *      正好对上 S 盒首字节。
 *   2) 逆 S 盒：把 S 盒的"下标 → 值"反过来填，即 inv_sbox[sbox[i]] = i。
 *
 * 注意：ginv 用的是暴力搜索，256 个字节全生成一遍是 O(256 * 256 * 8) 量级，
 * 在 51 上要好几秒，所以只在启动时做一次，别放进循环里。
 *============================================================================*/

#if (AES_SBOX_RUNTIME_GENERATE != 0)

/**
  * @brief   使用暴力搜索找到 GF(2^8) 中的逆元
  * @param   x 待求逆的字节
  * @retval  x 的乘法逆元（即满足 gmul(x, r) == 1 的那个 r），x 为 0 时返回 0
  *
  * 循环变量用 unsigned int 而不是 unsigned char：char 型 i 加到 255 之后
  * 会回绕成 0，i < 256 永远成立，直接就成死循环了。
  */
static unsigned char ginv(unsigned char x)
{
    unsigned int i;

    if (x == 0)
    {
        return 0;                   /* 0 没有逆元 */
    }

    for (i = 1; i < 256; i++)
    {
        if (gmul(x, (unsigned char)i) == 1)
        {
            return (unsigned char)i;
        }
    }

    return 0;                       /* 不应该到达此行 */
}

/**
  * @brief   仿射变换函数
  * @param   x 待变换的字节
  * @retval  变换结果
  *
  * 即把 x 与它的 1~4 位循环左移版本以及常量 0x63 逐位异或。
  * (x << n) | (x >> (8-n)) 就是 8 位循环左移 n 位。
  */
static unsigned char affineTransform(unsigned char x)
{
    unsigned char result;

    result = (unsigned char)(x ^
                             (unsigned char)((x << 1) | (x >> 7)) ^
                             (unsigned char)((x << 2) | (x >> 6)) ^
                             (unsigned char)((x << 3) | (x >> 5)) ^
                             (unsigned char)((x << 4) | (x >> 4)) ^
                             (unsigned char)0x63);

    return (unsigned char)(result & 0xFF);
}

/**
  * @brief   生成 AES S-Box
  * @param   sbox 输出缓冲区，长度 256 字节
  */
static void generate_aes_sbox(unsigned char sbox[256])
{
    unsigned int i;

    for (i = 0; i < 256; i++)
    {
        unsigned char x = ginv((unsigned char)i);       /* 计算逆元素 */

        sbox[i] = affineTransform(x);                   /* 应用仿射变换 */
    }

    sbox[0] = 0x63;                                     /* S-Box[0] 的特例处理 */
}

/**
  * @brief   生成 AES 逆 S-Box
  * @param   sbox     已生成好的 S-Box
  * @param   inv_sbox 输出缓冲区，长度与 sbox 相同
  */
static void generate_aes_inverse_sbox(const unsigned char sbox[256],
                                      unsigned char inv_sbox[256])
{
    unsigned int i;

    for (i = 0; i < 256; i++)
    {
        inv_sbox[sbox[i]] = (unsigned char)i;
    }
}

#endif /* AES_SBOX_RUNTIME_GENERATE */

/*==============================================================================
 * 公共函数实现
 *============================================================================*/

/**
  * @brief  初始化AES加密，执行密钥扩展操作
  *
  * 密钥扩展就是把原始密钥摊开成 Nr + 1 组轮密钥，规则只有一条：
  *
  *     w[i] = w[i - Nk] ^ w[i - 1]
  *
  * 即每个字 = 「Nk 个字之前的那一个」异或「紧挨着的前一个字」。
  * 只有每 Nk 个字轮到的那个特殊字，才把 w[i-1] 先过一道
  * RotWord + SubWord + 异或轮常量；256 位密钥多一条额外 SubWord 的规则。
  *
  * 下面用 pRoundKey 在子密钥表上逐字往后走，就地改：先把前一个字
  * 复制过来，再在它自己身上做变换，最后和 w[i-Nk] 异或。这样除了
  * 4 字节的轮常量，不再需要额外的临时数组。
  *
  * 轮常量本身也不用查表：它就是 GF(2^8) 里依次乘 2 得到的
  * 01,02,04,08,10,20,40,80,1B,36，正好是列混淆用的那个 gf_mult_by02。
  */
void AES_Init(const unsigned char *pKey)
{
    unsigned char  i;                                       /* 字的序号 */
    unsigned char *pRoundKey;                               /* 指向当前正在生成的那个字 */
    unsigned char  Rcon[4] = {0x01, 0x00, 0x00, 0x00};      /* 轮常量，只有最低字节非 0 */

    if (pKey == NULL)
    {
        return;
    }

#if (AES_SBOX_RUNTIME_GENERATE != 0)
    /* 首次调用时用算法生成 S 盒，之后直接复用 */
    {
        static BOOL bSboxReady = 0;

        if (bSboxReady == 0)
        {
            generate_aes_sbox(Sbox);
            generate_aes_inverse_sbox(Sbox, InvSbox);
            bSboxReady = 1;
        }
    }
#endif

    /* 前 Nk 个字就是原始密钥本身，原样搬进子密钥表开头 */
    memcpy(g_roundKeyTable, pKey, (size_t)(4 * Nk));

    /* 从第 Nk 个字开始往后生成，pRoundKey 始终指向当前这个字 */
    pRoundKey = &g_roundKeyTable[4 * Nk];

    for (i = Nk; i < (Nb * (Nr + 1)); pRoundKey += 4, i++)
    {
        /* 先把前一个字复制到当前位置，后面就在这个副本上做变换 */
        memcpy(pRoundKey, pRoundKey - 4, 4);

        if ((i % Nk) == 0)
        {
            /* 每 Nk 个字轮到一个特殊字：旋转、代换、再异或轮常量 */
            rotation_word(pRoundKey);               /* 循环左移一个字节 */
            sub_bytes(pRoundKey, 4, 0);             /* 逐字节查 S 盒   */
            xor_bytes(pRoundKey, Rcon, 4);          /* 异或轮常量      */

            /* 轮常量翻倍，给下一个特殊字用：01→02→04→…→80→1B→36 */
            Rcon[0] = gf_mult_by02(Rcon[0]);
        }
        else if ((Nk > 6) && ((i % Nk) == Nb))
        {
            /* 仅 256 位密钥需要：特殊字之后再过 4 个字，还要多做一次代换 */
            sub_bytes(pRoundKey, 4, 0);
        }
        else
        {
            /* 其余情况不做额外变换 */
        }

        /* 与 Nk 个字之前的那一个异或，本位才算生成完 */
        xor_bytes(pRoundKey, pRoundKey - 4 * Nk, 4);
    }

    bAesReady = 1;
}

/**
  * @brief  加密数据
  */
void AES_Encrypt(const unsigned char *pPlainText, unsigned char *pCipherText,
                 unsigned int nDataLen, const unsigned char *pIV)
{
    unsigned int i;

#if (AES_MODE == AES_MODE_CBC)
    unsigned char zeroIV[Nb * 4];       /* 只在 pIV 传 NULL 时用来兜底 */
#endif

    if ((pPlainText == NULL) || (pCipherText == NULL) || (bAesReady == 0))
    {
        return;
    }

    /* 长度必须是分组大小的整倍数，否则直接返回，避免越界读写 */
    if ((nDataLen == 0U) || ((nDataLen % (Nb * 4)) != 0U))
    {
        return;
    }

#if (AES_MODE == AES_MODE_CBC)
    /* 链式初值：首分组用 pIV，pIV 为 NULL 时按全 0 处理 */
    if (pIV == NULL)
    {
        for (i = 0; i < (Nb * 4); i++)
        {
            zeroIV[i] = 0x00;
        }
        pIV = zeroIV;
    }
#else
    /* ECB 模式不使用 IV */
    (void)pIV;
#endif

    /* 明文和密文不是同一块内存时，先把明文整体搬过去。
       搬完之后就直接在密文缓冲区上原地加密，不必再开 16 字节的中间状态。 */
    if (pPlainText != pCipherText)
    {
        memcpy(pCipherText, pPlainText, nDataLen);
    }

    for (i = 0; i < nDataLen; i += (Nb * 4))
    {
#if (AES_MODE == AES_MODE_CBC)
        /* 明文先与链值（首分组为 IV）异或 */
        xor_bytes(&pCipherText[i], pIV, (unsigned char)(Nb * 4));
#endif

        /* 对当前分组原地加密 */
        Cipher(&pCipherText[i]);

#if (AES_MODE == AES_MODE_CBC)
        /* 本分组的密文就是下一分组的链值 */
        pIV = &pCipherText[i];
#endif
    }
}

/**
  * @brief  解密数据
  */
void AES_Decrypt(const unsigned char *pCipherText, unsigned char *pPlainText,
                 unsigned int nDataLen, const unsigned char *pIV)
{
    unsigned int   i;                   /* 分组序号，这里是从后往前数 */
    unsigned char *pBlock;              /* 指向当前正在处理的那个分组 */

#if (AES_MODE == AES_MODE_CBC)
    unsigned char zeroIV[Nb * 4];       /* 只在 pIV 传 NULL 时用来兜底 */
#endif

    if ((pCipherText == NULL) || (pPlainText == NULL) || (bAesReady == 0))
    {
        return;
    }

    if ((nDataLen == 0U) || ((nDataLen % (Nb * 4)) != 0U))
    {
        return;
    }

#if (AES_MODE == AES_MODE_CBC)
    if (pIV == NULL)
    {
        for (i = 0; i < (Nb * 4); i++)
        {
            zeroIV[i] = 0x00;
        }
        pIV = zeroIV;
    }
#else
    (void)pIV;
#endif

    /* 同样先整体搬过来，然后在明文缓冲区上原地解密 */
    if (pPlainText != pCipherText)
    {
        memcpy(pPlainText, pCipherText, nDataLen);
    }

    /* ---- 从最后一个分组开始，倒着往前解 ----
       CBC 解密要算 P[i] = D(C[i]) ^ C[i-1]，需要用到前一个分组的密文。
       如果从前往后做，轮到第 i 组时 C[i-1] 已经被 P[i-1] 覆盖掉了，
       就必须额外开 16 字节把密文存一份。倒着走则不存在这个问题：
       处理第 i 组时，第 i-1 组的密文还原封未动地躺在前面，直接拿来用。 */
    for (i = nDataLen / (Nb * 4); i > 0; i--)
    {
        pBlock = &pPlainText[(i - 1) * (Nb * 4)];

        /* 对当前分组原地解密 */
        InvCipher(pBlock);

#if (AES_MODE == AES_MODE_CBC)
        if (i == 1)
        {
            /* 最靠前的那一组（最后才轮到它），链值是初始向量 IV */
            xor_bytes(pBlock, pIV, (unsigned char)(Nb * 4));
        }
        else
        {
            /* 其余分组，链值是它前面那一组的密文 */
            xor_bytes(pBlock, pBlock - (Nb * 4), (unsigned char)(Nb * 4));
        }
#endif
    }
}

/**
  * @brief  带 IV 链值的单分组解密（OTA 逐块搬运固件时用）
  *
  * 这里的难点是 CBC 的链值要跨调用维持。AES_Decrypt 一次只认一个 pIV，
  * 而且声明成 const 不会回写，所以链值只能由本函数替调用者保管：
  * 进来时 pIV 是本组的链值，出去时被改写成"本组的密文"，也就是下一组的链值。
  *
  * 解密是原地的，而本组密文解开后就没了——偏偏下一组的链值正是它，
  * 所以必须先用 cipherBackup 备份一份再解。
  *
  * 分组大小为 16 时，AES_Decrypt 的 CBC 分支走的是"i == 1"那条路径，
  * 即 InvCipher(本组) 再异或 pIV，正好就是单分组 CBC 解密的定义，
  * 所以这里直接复用它，不用把 InvCipher 暴露出来。
  */
void Aes_IV_128bit_Decode(unsigned char *pIV, unsigned char *pData,
                          const unsigned char *pKey)
{
    /* 已经做过密钥扩展的那个密钥是谁。密钥表是固定的静态数组，比指针即可 */
    static const unsigned char *pKeyExpanded = NULL;

    unsigned char cipherBackup[Nb * 4];     /* 备份本组密文，解开后要拿它当链值 */

    if ((pIV == NULL) || (pData == NULL) || (pKey == NULL))
    {
        return;
    }

    /* 密钥扩展只做一次。OTA 搬运一块 16 字节就要调一次本函数，
       96KB 固件会调用六千多次，每次都重算 240 字节轮密钥纯属浪费 */
    if (pKeyExpanded != pKey)
    {
        AES_Init(pKey);
        pKeyExpanded = pKey;
    }

    memcpy(cipherBackup, pData, (size_t)(Nb * 4));

    /* 原地解密一个分组：P = InvCipher(C) ^ pIV */
    AES_Decrypt(pData, pData, (unsigned int)(Nb * 4), pIV);

    /* 链值前移：本组密文成为下一组的 IV */
    memcpy(pIV, cipherBackup, (size_t)(Nb * 4));
}

/************************ (C) COPYRIGHT *****END OF FILE***********************/
