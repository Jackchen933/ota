# LVGL 移植层

把 LVGL 9.4 接到已经点亮的那块 ST7789 上。

**本板屏幕：1.69 寸 240×280 竖屏。**

注意不是 240×320 —— ST7789 芯片的 GRAM 是 240×320，但屏厂切出来的是
中间 y=20~299 那一段，剩下的靠偏移掉。所以：

| 宏 | 在哪 | 值 |
|---|---|---|
| `ST7789_WIDTH` / `ST7789_HEIGHT` | `BSP/Inc/bsp_st7789.h` | 240 / 280 |
| `ST7789_X_SHIFT` / `ST7789_Y_SHIFT` | `BSP/Inc/bsp_st7789.h` | 0 / **20** |
| `LV_PORT_DISP_HOR_RES` / `VER_RES` | `UI/Inc/lv_port_disp.h` | 240 / 280 |
| `SCR_W` / `SCR_H` | `UI/Src/ui_test.c` | 240 / 280 |

GUI Guider 里创建工程时也要填 **240 × 280**，四个地方必须一致。

```
Core/Src/main.c
    │
    ├── lv_init()                 LVGL 内核起来
    ├── lv_tick_set_cb()          把 LVGL 的"时间"接到 HAL_GetTick
    │
    ├── lv_port_disp_init()  ──┐
    │                          │  UI/Src/lv_port_disp.c
    │                          │  LVGL 渲染出的像素 → BSP → SPI → 屏幕
    │                          │
    ├── lv_port_indev_init() ──┤
    │                          │  UI/Src/lv_port_indev.c
    │                          │  手指坐标 ← I2C ← CST816S → LVGL
    │                          │
    ├── setup_ui()           ──┤
    │                          │  UI/generated/gg_utils.c
    │                          │  GUI Guider 生成的界面（画什么）
    │                          │
    ├── custom_init()        ──┤
    │                          │  UI/custom/custom.c
    │                          │  你自己的逻辑（交互）
    │                          │
    └── while(1)               │
          lv_timer_handler() ──┘  跑定时器 + 重绘脏区域
```

## 各个文件管什么

| 文件 | 职责 | 什么时候改它 |
|---|---|---|
| `Src/lv_port_disp.c` | LVGL 和屏幕之间的搬运工 | 换屏幕、改分辨率、调刷屏性能 |
| `Src/lv_port_indev.c` | LVGL 和触摸芯片之间的搬运工 | **触摸方向不对时改这里**（顶部三个宏） |
| `generated/` | GUI Guider 生成的界面 | **不要手改**，在 GUI Guider 里改完覆盖 |
| `custom/custom.c` | 你自己的逻辑 | 加交互、绑事件回调 |
| `Src/ui_test.c` | 自检界面（**当前正在跑**） | 屏幕/触摸出问题时就看它 |
| `Middlewares/lvgl/lv_conf.h` | LVGL 的编译期配置 | 开控件、开字体、调内存 |

## 自检界面怎么看

**当前跑的就是这个界面**（正在做触摸移植的第一阶段验证）。

切换方法：改 [main.c:72](../Core/Src/main.c#L72) 的宏，不是倒注释：

```c
#define UI_USE_SELFTEST     1    /* 1 = 自检界面；0 = GUI Guider 的正式界面 */
```

> 为什么用宏而不是"注释掉这两行、放开那一行"：后者改到一半很容易
> 两个都开着或者两个都关着，编译能过但屏幕全黑，白查半天。

自检界面的作用是**把问题分层**，快速区分「屏幕/接线的问题」
「触摸芯片/总线的问题」和「LVGL 这一侧的问题」—— 这三类的下一步动作完全不同。

界面长这样：

```
┌────────────────────────┐
│■红                 绿■│   四个角块 = 硬坐标，不对就是坐标/方向/偏移问题
│      ST7789 + LVGL     │
│  TOUCH OK  id=B5  as=OK│   初始化结果 + 芯片ID + 关自动休眠写成功没有
│  e5=0 bs=0/0  n=1234   │   近5秒失败 · 总线被占次数/没救回次数 · 采样次数
│  raw 123, 45  g=00     │   芯片原始坐标 + 手势码
│  ->  123, 45  DOWN     │   变换后坐标 + 按下状态
│  rc 00 01 7B 2D 00 00  │   芯片原始 6 字节（读失败时全变 00）
│  press 3  hit 123, 45  │   累计按下次数 + 最后一次落点
│        12.3 s          │   秒数在跳 = lv_tick 和定时器通了
│  ▓▓▓▓▓▓░░░░░░░░░░░░░   │   进度条在动 = 局部刷新和 flush 回调通了
│                   ●    │   红点跟着手指跑，松手就消失
│■蓝                 白■│
└────────────────────────┘
```

**都对 = 整条链路通了。** 有一项不对，按下表查：

| 现象 | 原因 | 改哪 |
|---|---|---|
| 全黑，什么都没有 | 卡在 LVGL 之前 | 先确认 `bsp_st7789_port_init()` 返回非 NULL；再看主循环有没有跑到 `lv_timer_handler` |
| **开机量尺画面正常，进界面后一两秒变全黑，背光还亮着** | **栈溢出** —— 1 KB 栈跑 LVGL 会踩坏 `.bss`，而 40 KB 的 LVGL 堆正好在 `.bss` 里 | 见下面「栈为什么必须是 8 KB」。改完还黑就按「卡死了怎么查」判断是不是 HardFault |
| 界面整体偏移 / 边缘有条纹 | 屏幕 GRAM 偏移 | `BSP/Inc/bsp_st7789.h` 的 `ST7789_X_SHIFT` / `Y_SHIFT` |
| 界面上下颠倒 / 左右镜像 | 屏幕扫描方向反了 | `BSP/Src/bsp_st7789.c` 的 `ST7789_MADCTL_DATA` |
| 颜色发蓝发绿、红蓝互换 | RGB565 字节序 | 见下面「最关键的三个坑」第 2 条 |
| **界面只画了一部分 / 右下角空一块 / 控件位置和 GUI Guider 里对不上** | **LVGL 分辨率和 GUI Guider 工程设的不一致** | 两边都必须是 240×280：`UI/Inc/lv_port_disp.h` 的 `VER_RES`、`BSP/Inc/bsp_st7789.h` 的 `ST7789_HEIGHT`、GUI Guider 工程设置里的高度 |
| 有图形但文字空白 | 字体没编进去 | `lv_conf.h` 的 `LV_FONT_MONTSERRAT_14` 要 1 |
| 中文全是空白 | 内置 montserrat 字体没有汉字 | 在 GUI Guider 里选一个中文字体重新生成并导出 |
| 动画不动 / 控件值不变 | `lv_tick` 没接 | `main.c` 里 `lv_tick_set_cb(HAL_GetTick)` |
| 卡死在第一帧 | `flush_ready` 没调 | `lv_port_disp.c` 的 `disp_flush` 每条分支都要调 `lv_display_flush_ready` |
| 编译报 `lv_conf.h: No such file` | 缺宏 | Keil 的 C/C++ 选项卡 Define 里要有 `LV_CONF_INCLUDE_SIMPLE` |
| 链接报 `undefined symbol lv_xxx_create` | 用了 `lv_conf.h` 里没开的控件 | 见下面「加新控件」 |
| 跑一会儿死机 / 界面画到一半卡住 | LVGL 堆不够 | 调大 `lv_conf.h` 的 `LV_MEM_SIZE`（现在 40KB），或精简界面 |

### 四条验收标准

触摸移植算不算成功，按这四条依次过。**必须依次**，因为后一条依赖前一条：

| # | 标准 | 怎么看 | 不过说明什么 |
|---|---|---|---|
| 1 | **认到芯片** | 第二行是 `TOUCH OK  id=B5`（或 `B6`） | 认不到就是在第一行查：`id` 是 `--` 说明 I2C 根本没通，`id` 是别的值说明通到了别的器件。见 [BSP/README.md](../BSP/README.md) 的「调不通怎么查」 |
| 2 | **能读到坐标** | 手指按下去，`raw x,y` 跟着变，松手 `g=00` | `raw` 不动但 `n` 在涨 → 芯片说没手指，查面板排线；`n` 不动 → 回调没跑，跟 I2C 无关 |
| 3 | **方向和范围对** | 四个角块**各自**在对应角上；按住左上角，红点就在左上角 | `raw` 对但 `->` 不对 = 方向变换问题，改 [lv_port_indev.c](Src/lv_port_indev.c) 顶部三个宏；`raw` 本身就超出 0~239 / 0~279 = 驱动读错字节或接线问题 |
| 4 | **长时间不卡死** | 挂着放 10 分钟以上，`n` 一直涨、`e5` 一直是 0、`bs` 不涨 | `e5` 在涨 = 此刻 I2C 坏了（看失败现场那行）；`e5=0` 但触摸没反应 = 不是总线问题，另查 |

> 第 4 条是这个工程最初要解决的问题（外设那版"用一阵子就永久卡死"），
> 所以**必须真的挂够时间**，点两下就走等于没验。
> `bs` 不涨这一条同样重要 —— 它涨说明驱动自己误判过总线状态，
> 会把上面的判断带偏。

## 触摸（CST816S）

分三层，各管各的，出问题先定位是哪一层：

```
CST816S ──I2C──► bsp_cst816s.c ──► lv_port_indev.c ──► LVGL
                        │                  │
                   原始坐标            方向变换
              （BSP_CST816S_ERR_*）   （三个宏）
```

**分工的原则是：驱动层永远吐原始值，方向逻辑只存在于一处。**
这样"驱动读错了"和"方向反了"是两个能分开判断的问题，自检界面把
raw 和变换后的值并排显示出来，就是为了让你一眼看出是哪一层。

### 怎么判读 —— 先看累计量，再看坐标

手指点一下只有一两百毫秒，只盯"当前值"看不出问题。所以界面上摆的是
**只增不减**的累计量（`n`、`press`、`hit`）加一个**滚动窗口**（`e5`）。
累计量不会漏掉任何一次触摸；`e5` 不是累计量，它专门回答"现在坏不坏"
（见第二步）。

**第一步：看 `n` 在不在涨。**

| `n` 的表现 | 说明 | 往哪查 |
|---|---|---|
| 一直 0 / 不动 | `lv_port_indev.c` 的 `read_cb` 根本没被调用 | **跟 I2C、接线、驱动全都无关**。查 `lv_port_indev_init()` 有没有被调到；查 `lv_port_disp_init()` 里有没有 `lv_display_set_default()`（indev 挂不到显示器上就不会被轮询） |
| 在涨 | 驱动在跑 | 继续第二步 |

**第二步：看 `e5`（近 5 秒失败次数）和 `bs`（总线被占的累计次数）。**

其他几个错误相关的量都是**累计**的，分不出"以前失败过"和"现在正在失败"，
而这两个结论指向完全相反的方向 —— 所以看的是 **`e5`**。

`bs=a/b` 里 `a` 是"开始传输时总线不是空闲状态"的累计次数，
`b` 是"打了 9 个时钟还是没放开"的累计次数。它涨说明有东西在**物理上**
拉着线，驱动用 9 个时钟把它请开了：

| 表现 | 说明 | 往哪查 |
|---|---|---|
| `e5` 在涨 | **此刻 I2C 就是坏的** | 看失败现场那行（见下）。**这时候别去查芯片休眠，方向是反的** |
| **`bs` 在涨、`b=0`** | 总线被占住过，但打时钟救回来了，这一次读是**成功的** | 偶发的话可以不管；涨得频繁说明有持续的干扰源（走线、上拉、电源纹波） |
| **`b` 也在涨** | 打完时钟两条线还是没放开 | **硬件问题**：上拉电阻缺失 / SDA 或 SCL 对地短路。软件救不了，去量线 |
| `n` 涨、`e5=0`、`bs=0/0`、`press` 一直 0 | 总线是好的，但芯片报告"没有手指" | 触摸面板本身：芯片到面板的排线；面板是不是独立的、没贴在屏幕上。**也可能真的是芯片休眠了**，见 [BSP/README.md](../BSP/README.md) |
| `press` 在涨 | 芯片确实感觉到手指了 | 进第三步 |

> **为什么要有 `e5` 而不是只看一个累计的失败次数**：这是 `e5` 存在的
> 全部理由。早先界面上有个累计的 `e`，问题是看到 "e=63" 这一屏，
> 没法判断那 63 次是**正在发生**还是**早就发生完了** ——
> 而这两种情况的下一步动作是相反的：前者去修总线，后者去查芯片。
> 一个数分不出两件事，就再加一个数。
>
> （那个累计的 `e` 现在为了让位给 `bs` 已经从界面上删掉了，
> 但 `e5` 留下 —— 因为"现在坏不坏"才是排查时真正要回答的问题。）
>
> **为什么 `bs` 要分成 `a/b` 两个数**：只有"被占住几次"的话，
> 「救回来了」和「救不回来」在屏幕上长得一模一样。
> `b` 在涨 = 线都放不开，那是硬件；`b=0` 就是救回来了。
> 一个数分不出两件事，就再加一个数 —— 和上面同一条道理。
>
> **这两个数在读成功的时候也会涨，所以别只在 `READ FAIL` 那一屏里找它们。**
> `bs` 最常见的形态恰恰是"被占住 → 救回来 → 这次读成功"。

#### 读失败长什么样

任何一次读失败，中间三行会变成：

```
raw --  g=--
->  --, --  READ FAIL
SC0 SD0 SCL
```

**看到 `READ FAIL` 就说明这三行没有任何真实数据** ——
`raw` / `->` / `rc` 本来会保留上一次成功读到的值，看起来像
"芯片说有一根手指但状态是抬起"这种自相矛盾的怪状态，
其实只是残值。照它去查会把方向整个带偏。

第三行是失败现场，最多两段。**前两段（`SC0` / `SD0`）放在最前面是刻意的**
—— 标签定宽、放不下就从右边裁，而"哪条线被拉低"最不能丢：

| 位置 | 含义 |
|---|---|
| `SC0` | SCL 被拉低 = 从机在**时钟拉伸**且不放手。打时钟救不了，只能靠 RST |
| `SD0` | SDA 被拉低 = 从机停在半个字节中间等时钟，打 9 个时钟能让它走完 |
| 阶段 | **断在哪一拍**，见下表 |

阶段比外设那版的 `HAL_BUSY`/`HAL_TIMEOUT` 细得多 ——
软件模拟知道自己是停在哪一步的：

| 显示 | 含义 | 排查方向 |
|---|---|---|
| `STUCK` | 传输还没开始，总线就被占着 | 有东西在拉线 → 量线，看 `SC0`/`SD0` |
| `START` | 发不出 START | 线接反 / 没接 |
| `ADDR` | 地址字节没被应答 | 芯片没上电 / 接线错 / 地址不对 |
| `REG` | 寄存器号没被应答 | 地址认了，寄存器不认 → **型号不对** |
| `DATA` | 数据字节没被应答（写寄存器时） | 同上 |
| `SCL` | 等 SCL 放开超时 | 从机在做时钟拉伸且不放手 |

`SC0` / `SD0` **只有线被拉低时才出现** —— 两条线都是高就是正常空闲状态，
不占字符。

> 原本这行开头有 `FLT ` 四个字符，去掉了 —— 上面那行已经写了
> `READ FAIL`，整块的意思不会看错。省下的位置正好给两个线的标记，
> 这样最坏情况 `SC0 SD0 STUCK`（13 字符）余量很大。

完整对照表见 [BSP/README.md](../BSP/README.md) 的「失败现场」一节。

**第三步：坐标和方向（`press` 涨了才轮到这一步）。**

| # | 操作 | 期望 |
|---|---|---|
| 1 | 点左上角红块 | 红点出现在左上角 |
| 2 | 点右下角白块 | 红点出现在右下角 |
| 3 | 沿四条边划一圈 | 红点跟手、不跑偏、不镜像、不跳 |
| 4 | 看 `raw` 两个数 | 都在 0~239 / 0~279 范围内 |

第 1~3 条不对，改 [lv_port_indev.c](Src/lv_port_indev.c) 顶部三个方向宏。
第 4 条里 `raw` 就不对的话，**是驱动读字节的问题，不是方向问题**，
去 [BSP/README.md](../BSP/README.md) 看寄存器格式，别在这里改宏。

> **为什么要分这三步**：`TOUCH OK` / `id=B5` 只说明**初始化那次** I2C 读通了，
> 它证明接线对，但**不能证明驱动在持续被调用**。`init_rc` 是在
> `lv_port_indev_init()` 里直接赋的，就算 `read_cb` 一次都没跑过，
> 它也照样显示 `TOUCH OK`。`n` 才能区分这两种情况 ——
> 少看这一个数，就会去查一堆本来没坏的东西。

### 方向不对时怎么校准

第 1~3 条不对，改 [lv_port_indev.c](Src/lv_port_indev.c) 顶部三个宏：

| 现象 | 改哪个 |
|---|---|
| 点左上角，红点跑到右上角 | `TOUCH_MIRROR_X` 改 1 |
| 点左上角，红点跑到左下角 | `TOUCH_MIRROR_Y` 改 1 |
| 上下滑动时红点左右跑 | `TOUCH_SWAP_XY` 改 1 |

这三个宏**最多只会调一次**，校准完就固定下来，不用再动。
`TOUCH_AXIS_X_MAX` / `Y_MAX` 会跟着 `SWAP_XY` 自动换，不用手工改。

### 触摸相关排错

| 现象 | 原因 | 改哪 |
|---|---|---|
| 显示 `I2C FAIL` | 线的问题：接反 / 缺上拉 / 没共地 | 见 [BSP/README.md](../BSP/README.md) 的「调不通怎么查」 |
| 显示 `CHIPID BAD` | 线是通的，芯片型号不对 | 同上，**别再去量线了** |
| **`as=FAIL`** | **关自动休眠没写进去，芯片一定会睡** | **先修 I2C。别查别的了** |
| **能用一阵子，然后突然按不动** | 触发了某个**不会自己恢复**的东西：芯片休眠（`as=FAIL`）或线被拉死（`b` 在涨） | 先看 `as=`：`FAIL` 就是没关掉自动休眠；是 `OK` 就看 `e5`/`b`。两者都不符合就再往下查，见 [BSP/README.md](../BSP/README.md) 的「调不通怎么查」 |
| `TOUCH OK` 但红点从不动 | 按到的是空白区，或面板没供电 | 手指按在四个角块上试；量屏幕排线 |
| `n` 一直不动 | `read_cb` 没被调用 | 跟 I2C、驱动无关，查 `lv_display_set_default()` |
| `e5` 在涨 | 此刻 I2C 是坏的 | 看失败现场那行的阶段：`ADDR` 查接线和供电，`REG`/`DATA` 查型号，`SCL`/`STUCK` 量线 |
| `bs` 一直涨，但触摸还是有反应 | 总线在反复被占用又被救回来 | 能用了，但说明有持续的干扰源（走线、上拉、电源纹波），迟早要治本 |
| `bs` 右边那个数（`b`）也在涨 | 打完时钟线还是没放开 | **硬件**：量上拉电阻和对地短路 |
| 红点位置对但会飘 | 采样期间手指移动 | 正常，30ms 采样（`TOUCH_TIMER_MS`）跟不上快速滑动 |
| 红点跟手但有明显延迟 | 刷新周期偏大 | 调小 `TOUCH_TIMER_MS` |
| 红点卡在某个位置不消失 | I²C 偶发错误后状态没复位 | 已在 `indev_read_cb` 里处理：读失败一律返回 `RELEASED` |

## 最关键的三个坑

### 1. 必须定义 `LV_CONF_INCLUDE_SIMPLE`

LVGL 默认去 `Middlewares/lvgl/src/../../lv_conf.h` 找配置，那个路径不存在。

正常 LVGL 会靠 `__has_include` 自动探测，但 **ARMCC V5 不认识 `__has_include`**，
所以必须手动在 Keil 的 `Define` 里加上这个宏，让 LVGL 走 `#include "lv_conf.h"`。

已经加好了（`USE_HAL_DRIVER,STM32F411xE,LV_CONF_INCLUDE_SIMPLE`），别删。

### 2. 颜色格式必须是 `RGB565_SWAPPED`

ST7789 要求每个像素**高字节先发**，Cortex-M 是小端，两者正好相反。

设成 `LV_COLOR_FORMAT_RGB565_SWAPPED` 后，LVGL 渲染时就直接把字节摆成
ST7789 要的顺序，flush 回调拿到什么发什么，不需要逐像素转换。

**如果忘了这一行**，现象是"图形轮廓对、颜色不对"（发蓝发绿、红蓝互换）。
不要去改 BSP 里的 `ST7789_SWAP_COLOR_BYTES`，那是给 BSP 自己画图用的，
改了会把 BSP 那边的颜色弄错。

### 3. 没有整屏 framebuffer

`240 × 280 × 2 = 134KB` > F411 的 128KB RAM。

所以用 `LV_DISPLAY_RENDER_MODE_PARTIAL`：LVGL 把屏幕切成条带，
每次只渲染 40 行（19.2KB）进缓冲，刷完再渲染下一条。

单缓冲而非双缓冲，是因为 BSP 的 `pf_st7789_write_data` 是**阻塞**的
（内部等 DMA 完成才返回），flush 回调没法提前返回，双缓冲只会白占一份内存。

## RAM 都花在哪了

| 用途 | 大小 | 在哪定义 |
|---|---|---|
| LVGL 堆 | 40 KB | `lv_conf.h` 的 `LV_MEM_SIZE` |
| 显示条带缓冲 | 19.2 KB | `lv_port_disp.c` 的 `DISP_BUF_LINES` |
| BSP 条带缓冲 | 3.8 KB | `bsp_st7789.h` 的 `ST7789_HOR_LEN` |
| 栈 | 8 KB | `startup_stm32f411xe.s` 的 `Stack_Size` + `jiemian1.ioc` 的 `StackSize` |
| 堆 | 0.5 KB | 同上 `Heap_Size`（LVGL 不用它，走自己的 40KB） |
| **合计** | **约 72 KB** | 128KB 里还剩 56 KB |

Flash 方面 Keil 工程已经开了 `One ELF Section per Function`，
链接器会把没被引用的函数全丢掉，所以实际占用远小于"编译了多少代码"。
（实测：`Code=161906 RO-data=16130` ≈ 174 KB / 512 KB。）

### ⚠️ 栈为什么必须是 8 KB

CubeMX 默认给 1 KB（`0x400`），**跑 LVGL 一定会炸**，而且症状极具迷惑性。

LVGL 第一次渲染的调用链是：

```
lv_timer_handler → lv_refr_now → lv_refr_area → lv_draw_rect
  → lv_draw_sw_fill → lv_draw_sw_blend → lv_draw_sw_blend_to_rgb565
```

十几层，每层还有几十到几百字节的局部结构体。1 KB 撑不住。

**迷惑的地方在于它不会当场死**：栈向下增长，溢出后踩的是 `.bss` —— 而 LVGL 那 40 KB 堆正好在 `.bss` 里。先踩坏堆，过几帧才崩。表现出来就是"开机正常、进界面一两秒后黑掉"。

**改动位置有两处，必须同时改**，否则 CubeMX 重新生成代码会把 `startup_*.s` 覆盖回去：

- `MDK-ARM/startup_stm32f411xe.s` 的 `Stack_Size`
- `jiemian1.ioc` 的 `ProjectManager.StackSize`

### 卡死了怎么查

`HardFault_Handler`（在 `Core/Src/stm32f4xx_it.c`）现在不会闷声死循环了，它做两件事：

1. 把出错现场存进 `g_hf_msp` / `g_hf_psp` / `g_hf_lr` / `g_hf_pc` / `g_hf_count`
2. 主动复位

**所以看到屏幕周期性重启，就说明确实进了 HardFault**；屏幕黑着不动，则是卡在别处（多半是某个 `while` 等待）。

想看成因，把 handler 里的 `NVIC_SystemReset()` 注释掉，复现后在 Keil 调试器里看 `g_hf_pc`，拿它的值去 `MDK-ARM/jiemian1/jiemian1.map` 里搜，就能定位到出错函数。

## 改界面

现在 `ui_test.c` 里的界面是手写的。自由发挥时注意：

- **中文字显示不出来**：内置的 montserrat 字体里一个汉字都没有。
  要做中文得先生成字体文件（LVGL 官方的 Font Converter，或 GUI Guider 导出），
  把生成的 `.c` 丢进 `UI/Src`，然后在 `lv_conf.h` 里：
  ```c
  #define LV_FONT_CUSTOM_DECLARE   LV_FONT_DECLARE(my_font_cn_16)
  ```
- **字号**：`lv_conf.h` 里只开了 `LV_FONT_MONTSERRAT_14`，
  要大字就把 `LV_FONT_MONTSERRAT_20` 之类改成 1 —— 字体文件一直编译着，
  不用动 Keil 工程。
- **想用新控件**（比如做个下拉框）：要改**两个地方**，
  只改一个会在链接时报 `undefined symbol`：
  1. `lv_conf.h` 里 `LV_USE_DROPDOWN` 改成 1
  2. Keil 工程 `LVGL/widgets` 分组里加上 `lv_dropdown.c`

## GUI Guider 界面

### 现在编进去的是 016，但屏幕上跑的不是它

两个 `generated/` 同时存在，**别搞混**：

| 路径 | 是哪个工程 | 时间 | Keil 编不编它 |
|---|---|---|---|
| [UI/generated/](generated/) | **016**，两个按钮，无图片无自定义字体 | 2026-09-21 | ✅ 在 IncludePath 里，**编译的就是这份** |
| [../generated/](../generated/) | **022**，新界面 | 2026-09-22 | ❌ 不在 IncludePath 里，目前完全不参与编译 |

而 `main.c` 的 `UI_USE_SELFTEST = 1`，所以**此刻屏幕上是自检界面**，
016 和 022 都没在显示。

### 移植 022 = 第二阶段

022 的素材（在工程根目录 `generated/assets/` 下）比 016 重得多：

- **字体 8 个**：`alimama_14/30`、`digitaldreamfatnarrow_30`、
  `interttf_30/40`、`montserratMedium_10/20`
- **图片 22 个**，全部是 `RGB565A8`（带 alpha 通道），其中包括
  整屏背景 `MDLBG_240x280_RGB565A8.c`

Flash 不够，所以要先做**素材精简**再移植：

1. 图片从 `RGB565A8` 转成 `RGB565`（丢掉 alpha 通道）
2. 字体做子集化（只保留界面上真正用到的那几百个字）

> 整屏背景那一张是最大的一块：`240 × 280 × 2 = 134400` 字节的 RGB565，
> 再加上同样大小的 A8 通道。**光砍掉 alpha 就省 67200 字节。**

### 代码分布

```
UI/
├── generated/            ← GUI Guider 自动生成，不要手改
│   ├── gui_guider.h          gg_ui_t 结构体 + guider_ui 全局实例
│   ├── gg_utils.c            setup_ui()：建三个层 + 屏幕，然后显示
│   ├── screens/              setup_screen() / setup_layer_sys|top|bottom()
│   ├── events/               gg_event_init_*()，事件回调
│   └── assets/               图片和字体（每加一个图片/字体就多一个 .c）
├── custom/               ← 你自己的代码，GUI Guider 不会覆盖
│   ├── custom.c              custom_init()，在 setup_ui() 之后被调用
│   └── lv_conf_ext.h         给 lv_conf.h 打补丁用的（现在空着）
├── Src/lv_port_disp.c        显示桥，和 GUI Guider 无关，别动
├── Src/ui_test.c             自检界面，当前启用（UI_USE_SELFTEST=1）
└── Inc/
```

`main.c` 里的调用顺序：

```c
lv_port_disp_init();     /* 屏幕先通 */
lv_port_indev_init();    /* 触摸再通（内部自己复位并校验 CST816S） */

#if UI_USE_SELFTEST       /* ← 现在是 1，所以走这一支 */
  ui_test_init();         /* 自检界面 */
#else
  setup_ui(&guider_ui);    /* GUI Guider 建界面 */
  custom_init(&guider_ui); /* 你自己的逻辑 */
#endif
```

### 在 GUI Guider 里改完界面后怎么同步

1. GUI Guider 里 **Export → C**
2. 把 `016/generated/` 整个覆盖到 `UI/generated/`（删旧文件再拷，别只覆盖同名文件）
3. 如果加了图片/字体，`016/custom/` 也跟着覆盖到 `UI/custom/`
4. Keil 重新编译

**只有"新增了东西"才需要动 Keil 工程：**

| 你在 GUI Guider 里做了 | 还要改什么 |
|---|---|
| 移动/改色/改文字 | 什么都不用改，覆盖 `generated/` 就行 |
| 用了新控件（下拉框、键盘…） | `lv_conf.h` 开 `LV_USE_XXX` + Keil `LVGL/widgets` 分组加对应的 `.c` |
| 加了图片 | 加进 Keil 的 `GUI Guider` 分组（会多出 `assets/images/xxx.c`） |
| 加了自定义字体 | 同上（`assets/fonts/xxx.c`），中文必须走这一步 |
| 加了中文 | 在 GUI Guider 里选一个中文字体生成，内置 montserrat 一个汉字都没有 |

### 自定义逻辑写哪

写在 `UI/custom/custom.c` 的 `custom_init()` 里 —— 这个文件 GUI Guider 不会覆盖。

要操作界面上的控件，通过 `guider_ui` 这个全局实例访问：

```c
/* 控件名 = 你在 GUI Guider 里给控件起的名字，见 generated/gui_guider.h */
lv_obj_add_event_cb(guider_ui.screen.button_1, my_cb, LV_EVENT_CLICKED, NULL);
```

⚠️ 反过来注意：**控件本身的事件回调如果是在 GUI Guider 里配的**，会生成到
`generated/events/gg_event_screen.c`，那个文件每次导出都被覆盖，
所以回调逻辑写在 `custom.c` 里、在 GUI Guider 里只做绑定，或者干脆全部
用上面的方式在 `custom_init()` 里手动绑。

### 别做的事

- **不要整份抄 GUI Guider 那份 `platform/simulator/lv_conf.h`** ——
  那是给模拟器配的，`LV_MEM_SIZE` 是 320KB，比 F411 整个 RAM 还大。
  它用到的控件和字体，对着在自己的 `lv_conf.h` 里开就行。
- **不要手改 `UI/generated/` 下任何文件** —— 下次导出直接覆盖。
