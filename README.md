# OTA 无感升级方案 — 复盘仓库

STM32F401/F411 平台上的 Bootloader + OTA 升级方案，包含两套并存的升级链路：

- **老方案**：Ymodem + AES-256-CBC，固件密文暂存内部 B 区，解密搬运到 A 区
- **新方案**：APP 后台无感下载到外部 W25Q64，EEPROM 状态机驱动，重启后由 Bootloader 搬运

本仓库只保留**自写代码**，已排除 ST 官方库（HAL/CMSIS）、FreeRTOS、编译产物，
体积从 470MB 精简到约 1.2MB。因此**不能直接编译**，仅用于方案复盘与面试准备。

## 学习/面试入口

👉 **[OTA-INTERVIEW-GUIDE.md](OTA-INTERVIEW-GUIDE.md)**
—— 分区布局、两套方案的完整链路、关键技术点、25 处踩坑记录，
以及简历表述与代码实现的逐条对应。

## 目录

```
03_Firmware/01_Bootloader/    Bootloader（标准外设库）
04_Software/                  APP（HAL + FreeRTOS）
AES/                          AES-256 算法与验证工程
OTA-INTERVIEW-GUIDE.md        面试技术档案
```
