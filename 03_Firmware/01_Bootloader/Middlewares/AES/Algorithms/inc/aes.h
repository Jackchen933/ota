#ifndef __AES_H
#define __AES_H

/**
  ******************************************************************************
  * @file    aes.h
  * @brief   AES 加解密算法（纯软件实现，兼容 51 与 ARM）
  *          支持 128 / 192 / 256 位密钥，支持 ECB / CBC 两种模式
  *          密钥长度与加密模式在本文件中通过 AES_KEY_LENGTH / AES_MODE 宏配置
  ******************************************************************************
  */

/* AES 分组长度固定为 16 字节（即 Nb * 4，4 个字） */
#define AES_BLOCK_SIZE          16

/*------------------------------------------------------------------------------
 * 密钥长度取值
 *
 * 这里用宏定义，而不是 enum 的枚举成员：宏在预处理阶段就能求值，可以被
 * #if 正确判断；而枚举常量属于 C 语言语法，预处理阶段识别不了，一律被
 * 当作未定义标识符（即 0）处理，写成 #if (AES_KEY_LENGTH == AES_KEY_LENGTH_128)
 * 会恒为真，加密轮数 Nr 的条件编译就失效了，配置永远停在 128 位那一档。
 *------------------------------------------------------------------------------*/
#define AES_KEY_LENGTH_128      128
#define AES_KEY_LENGTH_192      192
#define AES_KEY_LENGTH_256      256

/* 密钥长度枚举类型，便于需要时声明变量，取值与上面的宏保持一致 */
typedef enum {
    KeyLength_128 = AES_KEY_LENGTH_128,
    KeyLength_192 = AES_KEY_LENGTH_192,
    KeyLength_256 = AES_KEY_LENGTH_256
} AES_Key_Length;

/*------------------------------------------------------------------------------
 * 加密模式取值，同样用宏定义，原因同上
 *------------------------------------------------------------------------------*/
#define AES_MODE_ECB            0
#define AES_MODE_CBC            1

/* 加密模式枚举类型，便于需要时声明变量 */
typedef enum {
    AesMode_ECB = AES_MODE_ECB,
    AesMode_CBC = AES_MODE_CBC
} AES_Mode;

/*------------------------------------------------------------------------------
 * 当前配置：改这两行即可切换密钥长度和加密模式
 *
 * 注意：OTA 的密文是用 256 位密钥做的，这里必须跟着改成 256；
 *       改成 128 会让 Nk/Nr 对不上，解出来全是乱码（而且不报错）。
 *------------------------------------------------------------------------------*/
#define AES_KEY_LENGTH          AES_KEY_LENGTH_256
#define AES_MODE                AES_MODE_CBC

/*******************************************************************************
 * @function AES_Init
 * @brief    初始化AES加密，执行密钥扩展操作
 *
 * 此函数基于提供的原始密钥生成AES轮密钥。
 * 加解密前必须先调用本函数，更换密钥时重新调用即可。
 *
 * @param[in]  pKey  指向原始密钥的指针。
 *                   长度为 AES_KEY_LENGTH / 8 字节
 *                   （128位→16字节，192位→24字节，256位→32字节）。
 *
 * @return 无返回值。
 *
 * @note 密钥由调用者保证有效，函数内部只读不修改。
 ******************************************************************************/
void AES_Init(const unsigned char *pKey);

/*******************************************************************************
 * @function AES_Encrypt
 * @brief    加密数据
 *
 * 此函数对给定的明文进行 AES 加密，生成对应的密文。
 *
 * @param[in]  pPlainText   指向明文数据的指针。数据长度为 nDataLen 字节。
 * @param[out] pCipherText  指向用于存储加密后密文的缓冲区的指针，
 *                          长度与 pPlainText 相同。
 *                          允许与 pPlainText 指向同一块内存（原地加密）。
 * @param[in]  nDataLen     数据长度，以字节为单位，必须是
 *                          AES_BLOCK_SIZE（16）的整倍数。
 * @param[in]  pIV          指向初始化向量的指针。长度 AES_BLOCK_SIZE 字节，
 *                          如果使用 ECB 模式，可设为 NULL。
 *
 * @return 无返回值。
 *
 * @note 需先调用 AES_Init 完成密钥扩展；
 *       CBC 模式下各分组会链式加密，pIV 只在首个分组使用。
 ******************************************************************************/
void AES_Encrypt(const unsigned char *pPlainText, unsigned char *pCipherText,
                 unsigned int nDataLen, const unsigned char *pIV);

/*******************************************************************************
 * @function AES_Decrypt
 * @brief    解密数据
 *
 * 此函数对给定的密文进行 AES 解密，生成对应的明文。
 *
 * @param[in]  pCipherText  指向密文数据的指针。数据长度为 nDataLen 字节。
 * @param[out] pPlainText   指向用于存储解密后明文的缓冲区的指针，
 *                          长度与 pCipherText 相同。
 *                          允许与 pCipherText 指向同一块内存（原地解密）。
 * @param[in]  nDataLen     数据长度，以字节为单位，必须是
 *                          AES_BLOCK_SIZE（16）的整倍数。
 * @param[in]  pIV          指向初始化向量的指针。长度 AES_BLOCK_SIZE 字节，
 *                          如果使用 ECB 模式，可设为 NULL。
 *
 * @return 无返回值。
 *
 * @note 需先调用 AES_Init 完成密钥扩展；
 *       CBC 模式下各分组会链式解密，pIV 只在首个分组使用。
 ******************************************************************************/
void AES_Decrypt(const unsigned char *pCipherText, unsigned char *pPlainText,
                 unsigned int nDataLen, const unsigned char *pIV);

/*******************************************************************************
 * @function Aes_IV_256bit_Decode
 * @brief    带 IV 链值的单分组解密（OTA 搬运固件时逐块调用）
 *
 * AES_Decrypt 是"整段一次解完"的接口，而 OTA 搬运是每从 Flash 读 16 字节
 * 就解一次，跨多次调用没法由库内部自己维持 CBC 链值。本函数就是补这一环：
 * 把链值放在调用者自己的缓冲里，每解一组就前移一次。
 *
 * @param[in,out] pIV   指向初始化向量的指针，长度 AES_BLOCK_SIZE(16) 字节。
 *                      进入时是"本组的链值"，返回时被改写为"本组密文"，
 *                      供下一组使用。整段解密开始前须重置为初始 IV。
 * @param[in,out] pData 指向一个分组的缓冲，长度必须是 AES_BLOCK_SIZE 字节。
 *                      进入时是密文，返回时替换为明文（原地解密）。
 * @param[in]     pKey  指向原始密钥的指针，长度 AES_KEY_LENGTH / 8 字节
 *                      （256 位即 32 字节）。首次调用时做密钥扩展。
 *
 * @return 无返回值。
 *
 * @note 本函数只解密"一个分组"，nDataLen 恒为 16，与调用者的分块循环无关。
 *       加密端必须用同样的模式（aes.h 里的 AES_MODE）：CBC 时本函数按
 *       P = InvCipher(C) ^ IV 工作；改成 ECB 后 pIV 被忽略，链值前移无副作用，
 *       同一个函数照样能用。
 ******************************************************************************/
void Aes_IV_256bit_Decode(unsigned char *pIV, unsigned char *pData,
                          const unsigned char *pKey);

#endif /* __AES_H */
