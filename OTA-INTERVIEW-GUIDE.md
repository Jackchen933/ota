# OTA 无感升级方案 —— 面试技术档案

> 本文件是给 AI 面试官用的项目背景资料。请基于本文件和候选人简历，
> 对候选人进行模拟面试：深挖实现细节、追问设计取舍、探测是否真正理解。
>
> **请特别注意**：本文件记录的是代码仓库的实际情况。如果与候选人简历的
> 表述有出入，请直接追问差异——这正是真实面试会做的事。

---

## 1. 项目全景

### 1.1 平台

| 项 | 实际情况 |
|---|---|
| MCU | STM32F401xx / STM32F411xE（ARM Cortex-M4） |
| Bootloader 工程 | 标准外设库（SPL），`03_Firmware/01_Bootloader` |
| APP 工程 | HAL 库 + FreeRTOS（CMSIS-RTOS v2），`04_Software` |
| 外部存储 | W25Q64（8MB SPI Flash，硬件 SPI1，软件片选 PA4） |
| 参数存储 | AT24C02（256B EEPROM，硬件 I2C1，PB6/PB7，7 位地址 0x50） |
| 通信 | USART1，115200-8N1 |
| 开发环境 | Keil MDK（ARMCC/AC6） |

> **注**：Bootloader 与 APP 是两个独立 Keil 工程、两套库、两份代码。
> Bootloader 用 SPL，APP 用 HAL —— 同一份 W25Q64 驱动被移植了两遍。

### 1.2 内部 Flash 分区布局

| 区域 | 地址范围 | 扇区 | 大小 | 用途 |
|---|---|---|---|---|
| Bootloader | `0x08000000` – `0x08007FFF` | 0–1 | 32KB | 引导程序 |
| **A 区（运行区）** | `0x08008000` – `0x0801FFFF` | 2–4 | **96KB** (`0x18000`) | 当前运行的 APP |
| **B 区（备份区）** | `0x08020000` – `0x0803FFFF` | 5 | **128KB** (`0x20000`) | 老方案：Ymodem 下载的密文暂存区 |
| 未使用 | `0x08040000` 起 | 6–11 | 384KB | — |

对应宏（`Boot_Manager.h`）：
```c
#define ApplicationAddress     0x8008000   /* A区 */
#define BackApplicationAddress 0x8020000   /* B区 */
#define APP_SLOT_SIZE          0x18000     /* 96KB */
#define BACK_SLOT_SIZE         0x20000     /* 128KB */
```

**外部 W25Q64**：新方案的固件仓库，固件从偏移 0 开始存
（`W25Q64_APP_BASE = 0`，APP 侧 `W25Q64_OTA_BASE_ADDR = 0`，两边必须一致）。

### 1.3 EEPROM 参数区地址表

**这张表是 APP 与 Bootloader 之间唯一的持久化契约，两边逐字节一致：**

| 数据 | 长度 | 起始地址 | 取值 |
|---|---|---|---|
| OTA 状态 | 1 | `0x00` | `0x00` 无更新 / `0x11` 下载中 / `0x22` 请求更新 |
| 新 App 字节大小 | 4 | `0x01` | 小端 |
| 当前 App 字节大小 | 4 | `0x05` | 小端 |

### 1.4 三段存储的分工（面试可讲的核心设计）

```
EEPROM (AT24C02)   —— 升级状态机标志，掉电后 Bootloader 靠它决策
W25Q64 (外部 8MB)  —— 固件仓库，掉电不丢，替代内部 Flash 做备份区
内部 Flash A/B 区  —— 真正运行的代码 / 老方案的密文暂存
```

---

## 2. 方案演进：为什么工程里有两套 OTA

### 2.1 老方案 V1：Ymodem + AES-256-CBC + 内部 B 区

**链路**：上位机 AES 加密 → Ymodem 串口下载 → Bootloader 写 B 区（密文）
→ 解密搬运到 A 区 → 擦 B 区 → 跳转。

**B 区密文布局**（上位机打包工具生成）：
```
[ 0 .. 15]  头部密文 —— 解出来后前 12 字节是自定义内容，
                       第 12~15 字节是固件真实大小（小端）
[16 ..   ]  固件密文 —— 按 16 字节一组，AES-256 + CBC
```

**关键实现**（`Boot_Manager.c` 的 `BackToApp()`）：
- IV 会被解密函数逐组改写，所以只存一份模板 `IV_Init`，每次搬运前重装到工作副本
- 从头部解出真实固件大小，**先卡上界**（`> APP_SLOT_SIZE` 直接返回失败）
- 逐组解密 → 按字写入 A 区 → **每次写完立刻读回校验**
- 冷启动时 `fl_size` 传 0，由头部反推密文总长

**Ymodem 协议**（`Middlewares/Ymodem/`）：
- 停等（stop-and-wait）协议，128B(SOH) / 1KB(STX) 双包型
- CRC16（CCITT，多项式 `0x1021`）
- 出错重发，最多 10 次
- 接收端写入 B 区，**不校验 CRC**（源码事实：`Receive_Packet` 只查包号与反码，
  CRC 只在校验了包号后一并收下；`UpdateCRC16`/`Cal_CRC16` 只在发送侧使用）
- 发送侧修过三处「ACK 流错位」：进函数先清 RX 残留的 `'C'` 邀请、
  `Ymodem_WaitResponse()` 跳过杂散字节、EOT 后吃掉 `'C'`
- 删掉了原 ST 例程尾部多余的 EOT（SecureCRT 会 NAK）

### 2.2 新方案 V2：后台无感下载 + W25Q64 + EEPROM 状态机

**核心目标**：设备在正常业务运行的同时后台完成固件下载，不打断业务。

**APP 侧状态机**（`OtaApp.c`）：
```
WaitReqDownload --收到 0x11 0x22 0x33--> OtaDownload
OtaDownload     --数据传完--------------> WaitReqUpdate
WaitReqUpdate   --收到 0x77 0x88 0x99--> OtaEnd
OtaEnd          --20 秒内按下按键-------> 软复位进 Boot
```

**串口协议**（3 字节定长命令）：
| 命令 | 方向 | 含义 |
|---|---|---|
| `0x11 0x22 0x33` | 服务器 → MCU | 请求进入下载模式 |
| `0x44 0x55 0x66` | MCU → 服务器 | 后台下载完毕，正响应 |
| `0x77 0x88 0x99` | 服务器 → MCU | 请求更新 App |

**数据通路**：
```
USART1 ─DMA2_Stream2_CH4─> 1KB chunk ─IDLE中断─> 4KB 流缓冲
   └─> OTA 线程逐字节喂状态机 ─> 4KB 双缓冲 ─消息队列─> 下载线程
       └─> 擦 4KB 扇区 + 写 W25Q64
```

**结束判定**：上位机是连续推流、没有显式结束命令，所以靠串口空闲时长判断
（`OTA_END_IDLE_MS = 3000ms`）。

**收尾顺序**（掉电安全，顺序不能反）：
1. 冲掉不足一块的尾巴 `Ota_FlushPartial()`
2. **等两块缓冲都落盘** `Ota_WaitAllWritten()`（否则服务器以为好了、实际没落盘）
3. 先写新 App 大小，**再**写状态 `0x22`
4. 回 `0x44 0x55 0x66`

**按键确认**：20 秒窗口内按键稳定 50ms（10ms 轮询 × 5 次）→ `HAL_NVIC_SystemReset()`。
超时不按也没关系——EEPROM 里的 `0x22` 留着，用户下次自己关机重启时 Boot 照样升级。

**Bootloader 侧**（`Boot_Ota_Update()`）：

| EEPROM 状态 | 动作 |
|---|---|
| `0x11` 下载中 | 上次下载中途掉电，W25Q64 里是半包 → 清标志成 `0x00`，**不动 A 区**，跑旧固件 |
| `0x22` 请求更新 | 校验镜像 → 擦 A 区 → 从 W25Q64 搬运 → 写大小、清标志 → 返回 1 |
| 其他 | 无请求，返回 0 |

返回值语义经过设计：
```
 1: 升级完成，调用方跳转 A 区
 0: 没有升级请求，调用方按老流程走（尝试 B 区解密搬运 / 直接跳 A 区）
-1: 有请求但搬运失败 —— A 区内容【不可信】，绝对不能跳转，落到 Ymodem 救援分支
```

### 2.3 为什么从 V1 换到 V2（面试必问）

| 维度 | V1（Ymodem + 内部 B 区） | V2（W25Q64 + EEPROM 状态机） |
|---|---|---|
| 业务影响 | Bootloader 里**阻塞**等待下载，设备升级期间不可用 | APP 后台下载，业务不中断 |
| 占用内部 Flash | B 区吃掉 128KB | 备份移到外部，释放内部空间 |
| 触发方式 | 按键触发进 Bootloader | 服务器下发命令，按键仅用于确认 |
| 传输效率 | 停等协议，每包都要等 ACK | 连续推流，DMA + 双缓冲 |
| 加密 | AES-256-CBC 加密固件 | 无加密（W25Q64 里存 `.bin` 原始字节） |
| 掉电恢复 | 依赖 B 区内容判断 | EEPROM 状态机明确区分「下载中」/「下载完」 |

> V1 的代码**保留且仍可用**：V2 搬运失败时（返回 -1）会落到 V1 的 Ymodem
> 救援分支，作为最后的救砖手段。两套方案是**并存**的，不是简单替换。

---

## 3. 关键技术点（面试官可深挖的方向）

### 3.1 Bootloader 跳转 APP（`JumpToApp()`）

```c
__disable_irq();
NVIC_SetVectorTable(NVIC_VectTab_FLASH, 0x8000);   /* 向量表重映射到 A 区 */
RCC_DeInit();                                       /* 复位时钟 */
JumpAddress = *(__IO uint32_t *)(ApplicationAddress + 4);   /* 复位向量 */
__set_MSP(*(__IO uint32_t *)ApplicationAddress);            /* 主栈指针 */
((pFunction)JumpAddress)();
```

跳转前检查栈顶合法性：`(*ApplicationAddress & 0x2FFE0000) == 0x20000000`。

**可追问**：为什么用 `NVIC_SetVectorTable` 而不是 `SCB->VTOR`？为什么要 `RCC_DeInit`？
跳转前为什么要关中断？APP 侧的 `SCB->VTOR` 是怎么设的？

### 3.2 镜像合法性校验必须在擦除之前

```c
/* Boot_AppImage_Check()：两条判据 */
1. 首字（栈顶指针）必须指向 RAM：(*p & 0x2FFE0000) == 0x20000000
2. 复位向量必须落在 A 区内，且清掉最低位（Thumb 标志）后比较：
   reset_vec &= ~1UL;
   ApplicationAddress <= reset_vec < ApplicationAddress + size
```

> 代码注释原文：这一步必须在【擦 A 区之前】做。源数据是垃圾而我们已经把
> A 区擦了的话，这块板子就只能靠 Ymodem 救了。

### 3.3 Flash 擦除的边界陷阱（两处都有长注释）

`Flash_erase(addr, size)` 内部按 `STMFLASH_GetFlashSector(addr + size)` 算结束扇区。
当 `addr + size` 正好落在下一个扇区起始地址时，会把那个扇区也擦掉。

```c
/* AppSize 恰好等于 A 区容量(0x18000) 时：
   0x08008000 + 0x18000 = 0x08020000 → 正好是 B 区扇区5，
   会把正在读的密文整个擦掉 */
Flash_erase(ApplicationAddress, AppSize - 1);   /* 减 1 规避 */
```

**可追问**：这是修 bug 还是绕过 bug？更根本的修法是什么？

### 3.4 掉电安全设计

1. **`0x11` 语义**：APP 在开始收数据**之前**就写 `0x11`。中途掉电 → Boot 看到 `0x11`
   就知道没下完，直接放弃、保留 A 区的旧固件，绝不会拿半截固件去刷。
2. **写入顺序**：
   - APP 侧：先写大小、**再**写 `0x22`（反了的话掉电会让 Boot 以为固件是完整的）
   - Boot 侧：先写大小、**再**清标志（反了的话「标志清了但大小是旧的」，那一包永远丢）
3. **搬运失败不跳转**：返回 -1 时 A 区内容不可信，落到 Ymodem 救援分支。
4. **APP 上电清理**：启动时若发现 EEPROM 停在 `0x11`，清成 `0x00` 让服务器重来。

**可追问**：这是「幂等重试」还是「事务」？如果 EEPROM 写入本身掉电怎么办？

### 3.5 双缓冲 + 队列 + 信号量的协作模型

```c
#define OTA_BUF_SIZE   4096U    /* 每块正好一个 W25Q 扇区 */
#define OTA_BUF_COUNT  2U       /* RAM 开销 8KB */
```

- OTA 线程填一块（填前 `osSemaphoreAcquire` 取空闲），满 4KB 就投进消息队列
- 下载线程从队列取，擦+写 W25Q64 一个扇区，写完 `osSemaphoreRelease` 还回去
- **必须静态分配**：`configTOTAL_HEAP_SIZE` 只有 15KB，8KB 双缓冲走动态分配会把任务栈挤没

**W25Q64 写一个扇区的耗时**：擦 45ms + 写 84ms ≈ 130ms。
这期间上游数据由 4KB 流缓冲兜住（115200 下 130ms 最多来 1.5KB，够用）。

**可追问**：为什么不用三缓冲？流缓冲满了会怎样？丢数据了怎么办？

### 3.6 FreeRTOS 任务划分与优先级设计

| 任务 | 优先级 | 栈 | 职责 |
|---|---|---|---|
| `defaultTask` | Normal | 512B | 业务（呼吸灯） |
| `ota_task` | Normal | 4KB | 状态机 + 收串口数据 |
| `DownloadAppData_task` | **BelowNormal** | 4KB | 落盘到 W25Q64 |

**下载线程刻意低于 OTA 线程一个优先级**，代码注释原文：
> 写一个 4KB 扇区要 130ms，这期间如果它比 OTA 线程优先级高，OTA 线程就没机会
> 把串口数据从流缓冲里取走，4KB 的流缓冲会被上游灌满并丢数据。
> 让它低一级，OTA 线程随时能抢占它去收数据，写 Flash 慢一点没关系。

### 3.7 中断优先级与 FreeRTOS 约束

```c
configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5
HAL_NVIC_SetPriority(USART1_IRQn,      6, 0);   /* 必须 > 5 */
HAL_NVIC_SetPriority(DMA2_Stream2_IRQn, 6, 0);
```

Cortex-M 优先级**数值越大 = 逻辑优先级越低**。`...FromISR()` API 只允许在
优先级数值 ≥ 6 的中断里调用，设成 5 会在第一次收数据时命中 `configASSERT` 死机。

**可追问**：为什么是 6 而不是 7/8？`configPRIO_BITS` 是多少？再往深：如果
中断里不调 FromISR API 呢？

### 3.8 驱动层的踩坑（都有注释记录）

**AT24C02（I2C）**：
- STM32F4 的 I2C 在器件 NACK 后会卡在半个事务里 → 每个错误分支都要
  `I2c_ReleaseBus()` 放掉总线，否则「第一次失败之后次次失败」
- AT24C02 是 **8 字节一页**，页写**在本页内回绕**而非进位 → 必须按页边界切分
- 读最后一个字节必须**先 NACK + STOP，再等数据**，顺序反了器件会继续占着总线
- 收完必须恢复 ACK，忘了的话「读状态字是对的，读大小全错」（现象隐蔽）
- ACK 轮询代替死等 5ms 写周期（典型 1~2ms 就回来）

**W25Q64（SPI）**：
- `W25Qx_Erase_Block()` 名不副实：函数名是 Block，实际发的是 `SECTOR_ERASE_CMD(0x20)`
  即 **4KB 小扇区**，不是 64KB 大块
- 状态寄存器 1 的 BUSY/WREN 与状态寄存器 2 的 QE **数值都是 0x02 但属于不同寄存器**
- 厂商 ID 不能用来判兼容：Winbond 是 `0xEF`，Puya 是 `0x85`，
  判断容量**只看低字节的 density 码**（`0x13`=8Mbit … `0x18`=256Mbit）
- 容量计算用移位而非 `powf()`（避免把 libm 拖进工程）
- SPI 超时也返回「BUSY」上报，避免调用方把读失败当成「芯片就绪」直接放行
- 页只能写一次，重复写是**按位与**不是覆盖

**Bootloader 侧流式读写封装（`W25Q_Handler.c`）修过三个原始 bug**：
1. 扇区索引原本是 `u8` → 只能数到 255，即 1020KB；W25Q64 有 8MB，
   写到 1MB 后索引回绕到 0，**从第 0 个扇区重新擦写，把开头的数据覆盖掉** → 改成 `u32`
2. `W25Q64_WriteData_End()` 漏了 `write_databuf_index = 0` 和 `write_sector_index++`
   → 不补上要么把空洞当有效数据写下去，要么下次把刚写的扇区当场擦掉
3. `W25Q64_ReadData()` 用 `read_sector_index * SUBSECTOR_SIZE` 算地址 →
   短读之后指针停在扇区中间而扇区号没进位，地址退回扇区开头，
   **把已读过的数据再吐一遍** → 改用 `read_index` 绝对偏移

### 3.9 其他值得追问的实现细节

- **命令滑动窗口**：不能用「攒够 3 个再比」——`[0x11][0x11][0x22][0x33]` 这种流
  会被误判，第二个 `0x11` 才是真正的命令开头。滑动窗口能覆盖重叠。
- **进下载模式时不能 `xStreamBufferReset`**：命令是 3 字节，服务器很可能把
  `0x33` 和数据连着发，匹配上时数据已经在流缓冲里了，一 reset 就全丢。
- **轮次（session）机制**：下载线程不能用「第一次写」这种一次性标志 ——
  那样第二轮会接着上一轮的地址往下写，固件前半段永远不被覆盖。
- **下载线程无论成功失败都必须还缓冲**，否则 OTA 线程永远卡在 `Ota_BufTake`。
- **App 大小用链接器符号算**：`Image$$ER_IROM1$$Limit - 0x08008000`，
  不手工维护数字（改代码会忘）。
- **4KB 搬运缓冲必须放静态区**：启动文件 `Stack_Size` 只有 `0x400`(1KB)，
  4KB 局部数组会踩穿栈底，表现为「进函数就莫名 HardFault」。
- **未对齐访问**：`uint8_t Temp[16]` 强转 `*(uint32_t*)` 在 M4 上是未对齐访问，
  改用 `memcpy` 取字。
- **整段只解锁一次 Flash**：`Flash_Write()` 是「每写一个字解锁/上锁一次」的封装，
  96KB 要重复两万多次 → 改成外层 `Flash_Unlock()` 一次，内层直接 `FLASH_ProgramWord`。
- **写完立刻读回校验**：Flash 编程失败往往「静默写 0」，不校验要到跳转后跑飞才发现。

---

## 4. 简历要点 ↔ 代码实现的对应关系

| 简历表述 | 代码对应 | 备注 |
|---|---|---|
| 分区布局（Bootloader + APP 运行区 + APP 备份区 + 参数区） | A 区 / B 区 + EEPROM 参数区 | 对得上 |
| 向量表重映射 | `NVIC_SetVectorTable(NVIC_VectTab_FLASH, 0x8000)` | 对得上 |
| Bootloader 校验通过后跳转 APP | `Slot_IsValid()` + `JumpToApp()` | 对得上 |
| Ymodem 协议移植 | `Middlewares/Ymodem/` | **代码中属「老方案」**，V2 已替换为连续推流 |
| 串口 DMA 接收 | APP 侧 `HAL_UARTEx_ReceiveToIdle_DMA` | **Bootloader 侧 Ymodem 是轮询，不是 DMA** |
| 分包接收、CRC 校验、丢包重传 | 停等 + 重传 10 次 | **接收侧代码实际未校验 CRC** |
| AES 加密固件下发 | AES-256-CBC，`BackToApp()` 解密搬运 | 对得上（老方案） |
| 密文写备份区 → 解密搬运 → 逐段校验 | `BackToApp()` 边写边读回校验 | 对得上 |
| 移植 W25Q 外部 Flash 驱动 | `w25qxx.c` / `W25q64.c` | 对得上（两个工程各移植一遍） |
| 用外部存储替换 Flash 备份区 | V2 方案：备份移到 W25Q64 | 对得上 |
| APP 侧 OTA 框架与下载线程 | `OtaApp.c` + FreeRTOS 双线程 | 对得上 |
| 升级标志位置位 | EEPROM `0x22` | 对得上 |
| 重启后 Bootloader 完成搬运与跳转 | `Boot_Ota_Update()` | 对得上 |
| **异常回滚** | ⚠️ **代码中没有固件版本回滚机制** | 见下 |

### ⚠️ 需要候选人自己说清的三个点

1. **「异常回滚」**：代码里能做的是——擦 A 区**之前**先校验 W25Q64 里的镜像
   （不合格就返回 -1 不擦）、搬运失败时落到 Ymodem 救援分支。但**没有**保留
   旧版本固件做版本级回滚的能力：一旦开始擦 A 区，旧固件就没了，此时掉电
   只能靠 Ymodem 重刷。这是一个真实的设计局限。

2. **Ymodem 的定位**：简历把它写成主传输方案，但代码演进后它已退居
   「救援通道」。如果被问「为什么后来不用 Ymodem 了」，需要有清晰的答案。

3. **平台**：代码是 STM32F4 + SPL/HAL，简历写的是 TXW81x。两者在
   向量表重映射方式、SDK、驱动接口上都不同，需要候选人自己准备好说法。

---

## 5. 建议的提问方向（供 AI 面试官参考）

**第一层：确认理解**
- 为什么要分区？A 区和 B 区的大小是怎么定的？为什么 A 区是 96KB 而不是 128KB？
- Bootloader 跳转 APP 时做了哪几件事？少做哪一步会出什么问题？
- 固件怎么保证完整性？如果下载了一半掉电会怎样？

**第二层：深挖细节**
- `Flash_erase` 为什么要传 `size - 1`？这是修 bug 还是绕过 bug？
- AES 那套里，为什么 IV 每次搬运前都要重新装填？
- Ymodem 是停等协议，你怎么保证不丢包？接收侧校验 CRC 了吗？
- 双缓冲只有 2 块，如果 W25Q64 写一个扇区要 130ms，上游数据往哪放？放得下吗？
- 下载线程为什么优先级要比 OTA 线程低？

**第三层：设计取舍**
- 为什么新方案放弃了 AES 加密？安全性怎么保证？
- 为什么状态标志放 EEPROM 而不是内部 Flash？会不会有写入寿命问题？
- 如果 W25Q64 坏了，设备还能升级吗？还能正常启动吗？
- 「后台无感」具体无感在哪？下载期间业务真的完全不受影响吗？
- 这个方案有什么你事后觉得可以做得更好的地方？

**第四层：压力测试**
- 你简历写 TXW81x，代码是 STM32，这两者是什么关系？
- 「异常回滚」具体回滚了什么？A 区已经开始擦了还能回滚吗？
- 你说 AES 加密，密钥存在哪里？硬编码在代码里安全吗？
- 这套方案在 TXW81x 上和在 STM32 上有什么本质区别？

---

## 6. 代码索引（供 AI 面试官按需查阅）

```
03_Firmware/01_Bootloader/
├── Core/Src/main.c              启动流程：新方案 → 老方案 → Ymodem 救援
├── Core/Src/Flash.c             内部 Flash 擦写
├── Core/Src/Eeprom.c            AT24C02 驱动（硬件 I2C1）
├── Core/Src/w25qxx.c            W25Q64 裸驱动（硬件 SPI1）
├── Core/Src/W25Q_Handler.c      W25Q64 流式读写封装
├── Tasks/Boot_Manager/Boot_Manager.c   跳转 + 解密搬运 + W25Q64 搬运
├── Bsp/Spi.c                    SPI1 + 毫秒时基
└── Middlewares/Ymodem/ymodem.c  Ymodem 协议

04_Software/
├── USERS/Src/OtaApp.c           后台无感下载：状态机 + 双缓冲 + 下载线程
├── USERS/Src/W25q64.c           W25Q64 驱动（HAL 版）
├── USERS/Src/Eeprom.c           AT24C02 驱动（HAL 版）
└── Core/Src/freertos.c          任务创建 + 串口回调
```
