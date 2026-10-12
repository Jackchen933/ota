# OTA 无感升级方案 — 复盘仓库

STM32F411（WeAct MiniF4）平台上的 **Bootloader + OTA 升级**完整实现：
App 业务运行状态下经 UART 用 **Ymodem 协议**接收 **AES-128-CBC 加密固件**，
写入外部 SPI Flash（W25Q64），重启后由 Bootloader 解密搬运、MD5 校验、
带**试用期回滚**的固件换装方案。

## 升级链路（当前实现）

```
上位机 aes_ota_pack.py 打包                 Boot 上电
  │ 16B加密头+固件密文+16B MD5               │ 读标志扇区(0x08008000)
  ▼                                         ▼
App 双线程后台接收 ──Ymodem──► W25Q64 A区   0x11待更新 → 解密搬运到运行区
  OTA线程: DMA+空闲中断+流缓冲               │ 边搬边 AES-128-CBC 解密
  下载线程: 4KB块擦写(信号量+队列)           │ 边喂 MD5，校验不过不转正
  │                                         ▼
  └─ 收完置标志(原码+反码双写) ──重启──► 0x22验证中: 自检通过→0x44 有效
                                            │ 自检挂掉(IWDG复位)→回滚 B区备份
```

## 关键设计

- **标志扇区**（0x08008000, 16KB）：App状态/EaSize/EbSt/EbSize 四字段，
  每个按"原码+反码"双 u32 存储——任意时刻掉电都能判定有效或走回滚
- **双线程流水线**：串口 4KB≈357ms、W25Q64 擦写≈130ms，双缓冲+信号量
  把擦写时间藏进收包窗口；就算 Flash 成为瓶颈，缓冲信号量自动限流
- **Ymodem 停等协议**（手写接收状态机）：帧级 CRC16-XMODEM 坏帧重传，
  CRC 过了才落 4KB 缓冲、才回 ACK；重复序号=ACK 丢失，重 ACK 不重填
- **三层校验**：Ymodem 帧 CRC（传输层）→ 加密头固件大小与包长互验
  （结构层）→ 解密后 MD5 比对（文件层，不对不转正）
- **试用期机制**：新固件自检失败 → 不接管喂狗 → IWDG 复位 →
  Boot 检测 0x22+IWDGRSTF → 从 B 区恢复旧版本
- **AES-128-CBC 软件实现**：查表法，分块解密由调用方维护 IV 链值；
  MD5 按 RFC 1321 自实现（流式 Init/Update/Final，内存占用与固件大小无关）

## 目录

```
03_Firmware/01_Bootloader/     Bootloader（标准外设库）
  ├ Tasks/Boot_Manager/        搬运状态机 + FlagSector标志扇区 + MD5
  ├ Middlewares/AES/           AES-128-CBC 查表实现
  └ Middlewares/Ymodem/        Boot 侧 Ymodem（烧录通道）
04_Software/                   APP（HAL + FreeRTOS）
  ├ USERS/Src/OtaApp.c         双线程无感下载 + Ymodem 接收状态机 V2.1
  ├ USERS/Src/OtaFlag.c        App 侧标志扇区读写（与 Boot 格式对齐）
  ├ BSP/                       ST7789 屏 + CST816S 触摸自写驱动
  ├ UI/                        LVGL 业务界面
  └ Tools/aes_ota_pack.py      上位机打包工具（生成加密固件包）
AES/                           AES 算法验证工程
流程图/                        Bootloader 主线流程图
CRC与MD5-逐步拆解.html         算法学习笔记（CRC16手算 + MD5步骤）
双线程Ymodem流水线-精讲.html    架构学习笔记（时间线逐帧推演）
```

## 说明

本仓库只保留**自写代码**，已排除 ST 官方库（HAL/CMSIS）、FreeRTOS、LVGL、
编译产物，体积从 470MB 精简到约 1.3MB。因此**不能直接编译**，
仅用于方案复盘与学习笔记。

## 学习/面试入口

👉 **[OTA-INTERVIEW-GUIDE.md](OTA-INTERVIEW-GUIDE.md)**
—— 分区布局、升级链路、关键技术点、踩坑记录，
以及简历表述与代码实现的逐条对应。
