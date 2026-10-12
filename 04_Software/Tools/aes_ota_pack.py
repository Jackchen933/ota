#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""OTA 固件加密打包工具（与 Bootloader 的 Boot_Decrypt_Copy 严格对齐）

密文包格式（三段）:
  [16B] 加密头   明文 = 12字节任意(填0) + 4字节固件大小(小端)
  [ NB] 固件密文 固件明文末尾零填充到 16 倍数，与加密头作为"同一条 CBC 流"
                连续加密（Boot 端 IV 链式延续，逐块 Aes_IV_128bit_Decode）
  [16B] MD5 尾  = MD5(固件明文全文)，明文存放，Boot 解密后比对

密钥 / IV: ASCII "1212121212121212"（与 Boot_Manager.c 的 Key / IV_Init 一致）

用法:
  python aes_ota_pack.py <固件.bin> [输出文件.mxe]
输出总长 = 16 + ceil(固件大小/16)*16 + 16，Boot 端按 cipher_len 互验。
"""

import hashlib
import struct
import sys

from Crypto.Cipher import AES

KEY = b"1212121212121212"          # AES-128, 与 Boot 的 Key 一致
IV_INIT = b"1212121212121212"      # 与 Boot 的 IV_Init 一致


def pack(fw: bytes) -> bytes:
    fw_size = len(fw)
    if fw_size == 0:
        raise ValueError("固件为空")

    # 头部明文: 12 字节 0 + fw_size 小端
    header = struct.pack("<12sI", b"", fw_size)

    # 固件零填充到 16 倍数, 与头部拼成同一条 CBC 明文流
    pad = (-fw_size) % 16
    stream = header + fw + b"\x00" * pad

    cipher = AES.new(KEY, AES.MODE_CBC, IV_INIT).encrypt(stream)

    md5_tail = hashlib.md5(fw).digest()
    return cipher + md5_tail


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else (
        src.rsplit(".", 1)[0] + ".mxe")

    with open(src, "rb") as f:
        fw = f.read()

    pkg = pack(fw)

    with open(dst, "wb") as f:
        f.write(pkg)

    print("固件      : %s (%d 字节)" % (src, len(fw)))
    print("MD5       : %s" % hashlib.md5(fw).hexdigest())
    print("密文包    : %s (%d 字节 = 16头 + %d密文体 + 16MD5尾)"
          % (dst, len(pkg), len(pkg) - 32))


if __name__ == "__main__":
    main()
