/*********************************************************************************************************
 * @file    md5.h
 * @brief   软件MD5(RFC 1321), 用于OTA固件完整性校验
 *
 *          用法(流式, 适合边搬运边计算):
 *              MD5_CTX ctx;
 *              uint8_t digest[16];
 *              MD5Init(&ctx);
 *              MD5Update(&ctx, data1, len1);   // 可分多次, 次数不限
 *              MD5Update(&ctx, data2, len2);
 *              MD5Final(digest, &ctx);
 *
 *          约定: 上位机打包工具在固件密文包末尾附加16字节
 *          MD5(解密后的固件明文), Bootloader搬运完成后比对。
 * @version V1.0 2026-09-23
 * ********************************************************************************************************/
#ifndef __MD5_H
#define __MD5_H

#include <stdint.h>

typedef struct
{
    uint32_t state[4];      /* A/B/C/D 四个32位寄存器 */
    uint32_t count[2];      /* 已处理的总位数(低32位/高32位) */
    uint8_t  buffer[64];    /* 64字节分组缓冲 */
} MD5_CTX;

void MD5Init(MD5_CTX *ctx);
void MD5Update(MD5_CTX *ctx, const uint8_t *input, uint32_t len);
void MD5Final(uint8_t digest[16], MD5_CTX *ctx);

#endif /* __MD5_H */
