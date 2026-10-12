/**
  ******************************************************************************
  * @file    bsp_cst816s.c
  * @brief   CST816S / CST816T 电容触摸驱动（软件模拟 I2C）。
  *
  *          接线（和 jiemian1.ioc 里的配置一一对应）：
  *
  *            STM32F411          CST816S
  *            ─────────          ───────
  *            PB6              ── SCL
  *            PB7              ── SDA
  *            PB0  (TOUCH_RST) ── RST   低电平复位
  *            (不接)           ── INT   本驱动用轮询，不接中断
  *            3V3              ── VCC
  *            GND              ── GND
  *
  *          PB6/PB7 名义上还是 I2C1 的引脚（CubeMX 里配的），但本驱动
  *          不用那个外设，只把它们当普通开漏 GPIO 用。为什么这么做，
  *          见 bsp_cst816s.h 开头那段。
  *
  *          【为什么用轮询而不是中断】
  *          INT 脚的作用是"有触摸时通知 MCU"，可以省掉空闲时的 I2C 轮询。
  *          但 LVGL 的 read_cb 本来就被 lv_timer_handler() 每 33ms
  *          调一次（LV_DEF_REFR_PERIOD），这个频率足够跟手。
  *          少接一根线就少一个故障点。
  *
  *          【这个文件的阅读顺序】
  *          1. 最上面一堆宏：时序参数，每个都写了"为什么是这个数"
  *          2. 电平原语：bit_delay / scl_* / sda_*，最底层的四根拨杆
  *          3. 协议层：bus_start / bus_stop / bus_write_byte / bus_read_byte
  *          4. 传输层：bus_read_regs / bus_write_reg，一次完整的寄存器读写
  *          5. 对外接口：init / read / keepalive
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "bsp_cst816s.h"
#include "i2c.h"     /* hi2c1 —— HAL I2C 传输层用的总线句柄 */
#include "main.h"    /* TOUCH_RST_Pin / TOUCH_RST_GPIO_Port */
#include "bsp_i2c1_bus.h"

/* Private define ------------------------------------------------------------*/

/**
  * @brief 从机地址的两个方向。
  *
  *        I2C 的第一个字节是"7 位地址 + 1 位读写方向"，
  *        所以把 0x15 左移一位，最低位补 0 是写、补 1 是读。
  */
#define CST816S_ADDR_WRITE        (CST816S_I2C_ADDR_7BIT << 1)        /* 0x2A */
#define CST816S_ADDR_READ         ((CST816S_I2C_ADDR_7BIT << 1) | 1u) /* 0x2B */

/**
  * 022 移植：目标工程的 I2C1 上还挂着 AT24C02（EEPROM，业务侧），
  * **不能**像源工程那样把外设关掉做软件模拟 —— 那会直接弄死 EEPROM。
  * 传输层改走 HAL I2C1（本工程 100kHz，CST816S 兼容），并用
  * bsp_i2c1_bus 的互斥锁与 EEPROM 的事务互斥。
  * 置 0 可切回源工程的软件模拟（仅当 I2C1 上没有其他设备时可用）。
  */
#define BSP_CST816S_TRANSPORT_HAL 1
#define CST816S_I2C_TIMEOUT_MS    20u

/* 寄存器地址 ---------------------------------------------------------------- */

#define CST816S_REG_GESTURE       0x01u  /**< 手势码，也是连读的起始地址 */
#define CST816S_REG_CHIPID        0xA7u  /**< 芯片 ID */
#define CST816S_REG_DISAUTOSLEEP  0xFEu  /**< 写 0x01 关闭自动休眠 */

/**
  * @brief 连读回来的 6 个字节分别是什么。
  *
  *        0x01~0x06 是一次连续读拿回来的，所以解析的时候用的是
  *        "第几个字节"而不是"哪个寄存器地址"。这里把对应关系写死成
  *        一组序号，免得解析处散落一堆 buf[2] 这种魔数。
  *
  *        对应关系（序号 = 寄存器地址 - 0x01）：
  *          0x01 手势     0x02 手指数   0x03 X高
  *          0x04 X低      0x05 Y高      0x06 Y低
  */
enum
{
  CST816S_IDX_GESTURE = 0,
  CST816S_IDX_FINGER  = 1,
  CST816S_IDX_XPOS_H  = 2,
  CST816S_IDX_XPOS_L  = 3,
  CST816S_IDX_YPOS_H  = 4,
  CST816S_IDX_YPOS_L  = 5
};

/**
  * @brief 从 0x01 开始一次读 6 个字节。
  *
  *        手势(0x01) + 手指数(0x02) + X高(0x03) + X低(0x04)
  *                  + Y高(0x05) + Y低(0x06)
  *
  *        合成一次传输而不是读六次，是因为坐标必须来自同一帧 ——
  *        分六次读的话，手指在两次读之间移动会拿到一个
  *        "上一帧的 X 配这一帧的 Y" 的混合坐标，表现为触摸点乱跳。
  */
#define CST816S_READ_LEN          6u

/* ChipID 的接受范围 --------------------------------------------------------- */

/**
  * @brief 认得的 ChipID 区间。
  *
  *        0xB5 = CST816S，0xB6 = CST816T。
  *        上下各放宽一档（0xB4 / 0xB7）是因为市面上有几批兼容片
  *        会落在相邻值上，功能一致。放宽到这个范围之外就大概率
  *        是别的芯片或者读错了，这时候报错比硬撑着跑更有用。
  */
#define CST816S_CHIPID_MIN        0xB4u
#define CST816S_CHIPID_MAX        0xB7u

/* 时序 ---------------------------------------------------------------------- */

#define CST816S_RST_LOW_MS        10u  /**< RST 拉低保持时间 */
#define CST816S_BOOT_MS           60u  /**< 放开 RST 后等芯片就绪 */

/**
  * @brief 软件 I2C 时，每一个半位（SCL 的高电平段或低电平段）保持多久。
  *
  *        【这个数定的不是"多快"，是"多慢可以接受"】
  *        I2C 协议只规定最快能多快，**没有最低频率限制**。所以"慢"永远
  *        是安全的，"快"才是风险。手接的线、模块板上的上拉电阻容差都很大，
  *        宁可跑到几十 kHz，也不要贴着 400kHz 跑 —— 外设那版配的就是
  *        400kHz，这是它容易被打断的原因之一。
  *
  *        【为什么是空循环，而不是算出来的微秒数】
  *        要算出"延时 N 微秒"得知道编译器把这个循环编成了几条指令，
  *        而那个数会随 -O0/-O1/-O3 变。用 DWT 周期计数器能算准，
  *        但 DWT 要先开 TRCENA，万一开不起来就是一个死循环 ——
  *        那正是这个版本要消灭的东西，不能引进来。
  *
  *        所以计数器用 volatile：编译器必须真的跑这个循环，不能优化掉
  *        （被优化掉的后果是时序快到 1MHz 以上，在长线/弱上拉上必然出错）。
  *        volatile 还带来一个好处：循环体是"读内存-加一-写内存-比较"，
  *        实测 -O0 和 -O3 编出来每圈都是 11 个周期，**跟优化等级无关**。
  *
  *        100 圈 × 11 周期 / 100MHz ≈ 11 微秒一个半位，也就是 **约 45kHz**。
  *
  *        【一次读要花多久】
  *        "半位"是这套时序的计价单位。一次 6 字节读的构成：
  *
  *          2 次 START            2 ×  3 =   6
  *          3 个字节的写（含 ACK） 3 × 27 =  81
  *          6 个字节的读（含 ACK） 6 × 19 = 114
  *          1 次 STOP                       3
  *                                      -------
  *                                        204   ← 下限
  *
  *        204 × 11 微秒 ≈ **2.2ms**。但这是**下限**，因为每处
  *        `scl_wait_high()` 在"放开 SCL 之后立刻采样"时通常还没升上去，
  *        要多花一个半位再采一次。把这一项算进去是 204 + 85 = **289 个半位
  *        ≈ 3.2ms**，这才是实际值 —— 上拉越弱、线越长，越靠近这个数。
  *
  *        触摸是 33ms 轮询一次（LVGL 的 LV_DEF_REFR_PERIOD），
  *        所以读坐标占约 **10% 的 CPU**。
  *        保活那次 3 字节写是 87~116 个半位 ≈ **1.0~1.3ms**，
  *        500ms 才一次，可以忽略。
  *
  *        【觉得 10% 太费，想提速的话】
  *        把这个数减半（50）就是约 90kHz、1.6ms、5% CPU。
  *        90kHz 对 I2C 来说仍然非常保守（协议上限是 400kHz），
  *        但**改小之前先确认信号质量顶得住** —— 这个工程就是因为
  *        400kHz 下总线被打断才改到这里来的，别一口气又跑回去。
  */
#define CST816S_BIT_DELAY_LOOPS   100u

/**
  * @brief 等一条线放开的上限，单位是"多少个半位时间"。
  *
  *        200 × 11 微秒 ≈ 2.2 毫秒。I2C 允许从机做时钟拉伸，但触摸芯片
  *        最多拉伸几微秒到几百微秒，2.2ms 是很宽的上限了。
  *
  *        **重点是这个上限本身**：它是"不会永久卡死"这件事的保证所在。
  *        线不放开就报超时返回，而不是在那里一直等 —— 外设那版的问题
  *        恰恰是"卡住之后就一直卡着"。
  */
#define CST816S_LINE_WAIT_LOOPS   200u

/**
  * @brief 判断"总线是不是空闲"之前，给线留多少个半位的上升时间。
  *
  *        8 × 11 微秒 ≈ 88 微秒。
  *
  *        为什么需要它：`bus_stop()` 放开 SDA 之后，线是按
  *        R(上拉) × C(线电容) 的指数曲线爬回高电平的，**不是瞬间**。
  *        模块板上 4.7k 上拉大约 1 微秒就到位，但如果只剩芯片内部
  *        40k 上拉、线又长，可能要几十微秒。这段时间里直接采样会
  *        读到一个还在半路的电平，把"上一次传输刚收尾"误判成
  *        "有从机抱着线" —— 于是 `bs` 涨、解锁流程白跑一趟，
  *        而这次读其实完全正常。
  *
  *        88 微秒对任何合理的上拉都是很宽裕的余量，同时对真正被占住的
  *        总线又足够短：代价只有 88 微秒，不会把故障诊断拖慢。
  *
  *        **和 `CST816S_LINE_WAIT_LOOPS` 不是一回事**：那个等的是
  *        "从机肯不肯放手"（故障判据，所以给到 2.2ms），
  *        这个等的是"线肯不肯爬上去"（物理建立时间，几个微秒就够）。
  *        混用会让正常人品下的故障诊断白白多花 2.2ms。
  */
#define CST816S_IDLE_SETTLE_LOOPS 8u

/**
  * @brief 解锁一个被打断的从机要打多少个 SCL 时钟。
  *
  *        9 = 8 位数据 + 1 个应答位。一帧最多就这么多，所以打完这 9 个时钟，
  *        任何被打断在半个字节中间的从机都必然已经走完当前字节、把 SDA 放开。
  *        少打不行（可能差最后一位没走完），多打无害（空闲的从机不理会多余的时钟）。
  */
#define CST816S_UNWEDGE_CLOCKS    9u

/* 协议层的返回码 ------------------------------------------------------------ */

#define CST816S_ACK       1   /**< 从机应答了（第 9 拍把 SDA 拉低） */
#define CST816S_NACK      0   /**< 从机没应答（第 9 拍 SDA 一直是高） */
#define CST816S_BUS_TO  (-1)  /**< 等 SCL 放开超时，或者数据阶段没走完 */

/* 引脚 ---------------------------------------------------------------------- */

/**
  * @brief 软件 I2C 挂在哪两个脚上。
  *
  *        和 CubeMX 生成的 HAL_I2C_MspInit() 里配的是同一组。
  *        写在 BSP 里而不是复用 CubeMX 的宏，是因为那几个宏是生成代码的
  *        私有写法，BSP 不该依赖它们。
  */
#define CST816S_GPIO              GPIOB
#define CST816S_SCL_PIN           GPIO_PIN_6
#define CST816S_SDA_PIN           GPIO_PIN_7

/* Private variables ---------------------------------------------------------*/

/** init 时读到的原始 ChipID，排错用。init 之前是 0 */
static uint8_t s_chip_id = 0u;

/** init 时"关自动休眠"那次写的结果，见 bsp_cst816s_autosleep_rc() */
static int s_autosleep_rc = BSP_CST816S_ERR_I2C;

/** 最近一次失败的现场 + 累计的总线异常计数，见 bsp_cst816s_get_fault() */
static bsp_cst816s_fault_t s_fault;

/* Private functions ---------------------------------------------------------*/

/* ---- 电平原语 -------------------------------------------------------------
   最底层的四根拨杆。整个驱动只有这一层直接碰硬件 ——
   真换了平台（比如换成推挽 + 外部电平转换），改这四个函数就够了。

   开漏输出下"写 1"等于**放开**（让上拉拉高），"写 0"才是真的拉低。
   读 IDR 拿到的仍然是线上真实的电平：F4 的引脚在输出模式下输入通路
   照样是通的，所以不需要为了读而切换方向。
   这一点很关键 —— 整份驱动的自我保护（等线放开、检测总线被占）
   全都建立在"能读回自己写出去的电平"上。                             */

#if !BSP_CST816S_TRANSPORT_HAL
/**
  * @brief 等半位时间。
  */
static void bit_delay(void)
{
  volatile uint32_t i;

  for (i = 0u; i < CST816S_BIT_DELAY_LOOPS; i++)
  {
    /* 空转。volatile 保证编译器不会把这个循环整个优化掉。 */
  }
}

/** @brief 放开 SCL，靠上拉拉高 */
static void scl_release(void)
{
  HAL_GPIO_WritePin(CST816S_GPIO, CST816S_SCL_PIN, GPIO_PIN_SET);
}

/** @brief 把 SCL 拉低 */
static void scl_drive_low(void)
{
  HAL_GPIO_WritePin(CST816S_GPIO, CST816S_SCL_PIN, GPIO_PIN_RESET);
}

/** @brief 放开 SDA，靠上拉拉高 */
static void sda_release(void)
{
  HAL_GPIO_WritePin(CST816S_GPIO, CST816S_SDA_PIN, GPIO_PIN_SET);
}

/** @brief 把 SDA 拉低 */
static void sda_drive_low(void)
{
  HAL_GPIO_WritePin(CST816S_GPIO, CST816S_SDA_PIN, GPIO_PIN_RESET);
}

/** @brief 读 SCL 线上真实的电平。1 = 高 */
static uint8_t scl_is_high(void)
{
  return (HAL_GPIO_ReadPin(CST816S_GPIO, CST816S_SCL_PIN) == GPIO_PIN_SET)
         ? 1u : 0u;
}

/** @brief 读 SDA 线上真实的电平。1 = 高 */
static uint8_t sda_is_high(void)
{
  return (HAL_GPIO_ReadPin(CST816S_GPIO, CST816S_SDA_PIN) == GPIO_PIN_SET)
         ? 1u : 0u;
}
#endif /* !BSP_CST816S_TRANSPORT_HAL */

/**
  * @brief 把这次失败的现场存下来。
  *
  *        存的是"此刻"两条线的实际电平 —— 这两个值回答一个很基本的问题：
  *        到底是不是**物理上**卡住了。
  *
  *          两条线都是高的 -> 总线电气正常，问题在"没人应答"
  *                            （地址不对 / 芯片没供电 / 芯片坏了）
  *          SDA 低         -> 从机停在半个字节中间，在等时钟
  *          SCL 低         -> 从机在时钟拉伸，而且不放手
  *
  *        这三种的修法完全不同，合并成一个"读失败"就等于把唯一能
  *        定位的东西扔了。
  *
  *        注意只更新 phase 和两个电平，**不动 bus_stuck / unwedge_failed**
  *        —— 那两个是累计计数器，语义是"从开机到现在发生过几次"。
  */
static void cst816s_capture_fault(uint8_t phase)
{
  s_fault.phase = phase;
#if BSP_CST816S_TRANSPORT_HAL
  /* HAL 传输下引脚归 I2C1 外设（AF 复用），IDR 依然反映线上真实电平，
     直接读，语义和软件模拟时一致 */
  s_fault.scl_level = (HAL_GPIO_ReadPin(CST816S_GPIO, CST816S_SCL_PIN) == GPIO_PIN_SET) ? 1u : 0u;
  s_fault.sda_level = (HAL_GPIO_ReadPin(CST816S_GPIO, CST816S_SDA_PIN) == GPIO_PIN_SET) ? 1u : 0u;
#else
  s_fault.scl_level = scl_is_high();
  s_fault.sda_level = sda_is_high();
#endif
}

#if !BSP_CST816S_TRANSPORT_HAL
/* ---- 协议层 --------------------------------------------------------------- */

/**
  * @brief 等 SCL 真的升起来（从机可能正在做时钟拉伸）。
  *
  *        所有"放开 SCL"之后都必须调这个，不能写完 ODR 就当它已经高了 ——
  *        从机可以在任意时刻把 SCL 拉住不放，硬抢就违反了协议。
  *
  * @retval 1 = 升起来了；0 = 超时，从机不放手
  */
static uint8_t scl_wait_high(void)
{
  uint16_t i;

  for (i = 0u; i < CST816S_LINE_WAIT_LOOPS; i++)
  {
    if (scl_is_high() != 0u)
    {
      return 1u;
    }
    bit_delay();
  }

  return 0u;
}

/**
  * @brief 总线此刻是不是空闲的（两条线都被上拉拉高）。
  *
  *        这是一个**瞬时采样**，没有建立时间的概念 ——
  *        要判断"能不能开始一次传输"应该调 `bus_wait_idle()`，
  *        那个才负责给出线爬上去的时间。
  */
static uint8_t bus_is_idle(void)
{
  return ((scl_is_high() != 0u) && (sda_is_high() != 0u)) ? 1u : 0u;
}

/**
  * @brief 等总线变空闲，有上限。
  *
  *        和 `scl_wait_high()` 是同一个形状：放开线之后不能立刻采，
  *        要给足上升时间。区别只在等的是"两条线都高"而不是"SCL 高"。
  *
  *        **为什么要等而不是直接读**：`bus_stop()` 放开 SDA 之后只留了
  *        一个半位（11 微秒）就返回，而 init 里的"读 ChipID -> 写 0xFE"
  *        和每帧的"保活写 -> 读坐标"两处，下一次传输几乎紧接着就开始。
  *        线还在爬的时候采到低电平，就会把正常的紧邻传输误判成总线被占，
  *        凭空记一次 `bus_stuck`、白跑一趟解锁 —— 于是 `bs` 会在
  *        **硬件完全正常**的情况下慢慢往上涨，把排查方向带偏。
  *
  *        这不是打补丁：文件里每一处 `scl_release()` 后面都跟着一个有界的
  *        `scl_wait_high()`，唯独这里假设线是瞬间到位的，本来就自相矛盾。
  *
  * @retval 1 = 空闲了；0 = 等满 CST816S_IDLE_SETTLE_LOOPS 还没空闲
  */
static uint8_t bus_wait_idle(void)
{
  uint8_t i;

  for (i = 0u; i < CST816S_IDLE_SETTLE_LOOPS; i++)
  {
    if (bus_is_idle() != 0u)
    {
      return 1u;
    }
    bit_delay();
  }

  return 0u;
}

/**
  * @brief 补一个 STOP 条件。
  *
  *        STOP 的定义是"SCL 保持高的时候 SDA 从低跳到高"。所以顺序必须是：
  *
  *          1. 先把 SDA 拉低 —— 此刻 SCL 还是低的，从机不会把这个
  *             跳变误判成 START/STOP（SCL 低的时候 SDA 随便动都是数据）
  *          2. 放开 SCL，等它真的升上去
  *          3. 放开 SDA —— SCL 高的时候 SDA 上升，这才是 STOP
  *
  *        顺序错了会变成一个 START（SDA 在 SCL 高的时候下降），
  *        从机不但不会收尾，还会以为要开始新的一帧。
  */
static void bus_stop(void)
{
  sda_drive_low();
  bit_delay();
  scl_release();
  (void)scl_wait_high();
  bit_delay();
  sda_release();
  bit_delay();
}

/**
  * @brief 发一个 START（或者 repeated START）条件。
  *
  *        先把两条线放开再动手，顺序不能反：如果 SDA 还是低的时候
  *        SCL 先升上去，从机看到的是一个 STOP —— 而我们要的是开始。
  *
  * @retval 1 = 建起来了；0 = SCL 拉不起来，或者 SDA 被从机抱着放不开
  */
static uint8_t bus_start(void)
{
  sda_release();
  scl_release();

  if (scl_wait_high() == 0u)
  {
    return 0u;
  }

  /* START 需要 SDA 有一个**下降沿**。它现在是低的话就说明有从机
     抱着 SDA 不放，建不出边沿 —— 这种情况应该由 bus_prepare() 提前拦掉，
     这里是第二道防线。 */
  if (sda_is_high() == 0u)
  {
    return 0u;
  }

  bit_delay();
  sda_drive_low();     /* SCL 高、SDA 降 = START */
  bit_delay();
  scl_drive_low();     /* 钳住总线，准备发第一个字节 */
  bit_delay();

  return 1u;
}

/**
  * @brief 发一个字节，并读从机的应答。
  *
  *        数据位在 SCL 低电平期间变化、高电平期间被采样（协议规定），
  *        所以顺序是"先摆 SDA -> 再放开 SCL -> 等 SCL 升起来 -> 保持"。
  *
  * @retval CST816S_ACK     从机应答了
  *         CST816S_NACK    没人应答（地址不对 / 芯片没响应）
  *         CST816S_BUS_TO  等 SCL 放开超时
  */
static int bus_write_byte(uint8_t b)
{
  uint8_t i;
  int ack;

  for (i = 0u; i < 8u; i++)
  {
    if ((b & 0x80u) != 0u)
    {
      sda_release();
    }
    else
    {
      sda_drive_low();
    }

    b = (uint8_t)(b << 1);

    bit_delay();
    scl_release();
    if (scl_wait_high() == 0u)
    {
      return CST816S_BUS_TO;
    }
    bit_delay();
    scl_drive_low();
    bit_delay();
  }

  /* 第 9 拍：主机放开 SDA，把线让给从机去应答。
     主机不放开的话，从机想拉低也会被主机的高电平顶住，永远应不了答。 */
  sda_release();
  bit_delay();
  scl_release();
  if (scl_wait_high() == 0u)
  {
    return CST816S_BUS_TO;
  }
  bit_delay();

  /* 从机拉低 = ACK；一直是高 = NACK（没人应答） */
  ack = (sda_is_high() != 0u) ? CST816S_NACK : CST816S_ACK;

  scl_drive_low();
  bit_delay();

  return ack;
}

/**
  * @brief 读一个字节，然后由**主机**给应答。
  *
  * @param  out        输出，读到的字节
  * @param  ack_after  1 = 给 ACK（后面还有），0 = 给 NACK（这是最后一个）
  *
  * @retval CST816S_ACK 读成功；CST816S_BUS_TO 等 SCL 放开超时
  */
static int bus_read_byte(uint8_t *out, uint8_t ack_after)
{
  uint8_t i;
  uint8_t b = 0u;

  /* 从机要在这 8 拍里把 SDA 推成它要发的位，所以主机必须先放开 SDA。
     忘了这一条就成了主机和从机同时驱动 SDA，读回来全是 1。 */
  sda_release();

  for (i = 0u; i < 8u; i++)
  {
    bit_delay();
    scl_release();               /* 高电平期间从机把这一位摆到 SDA 上 */
    if (scl_wait_high() == 0u)
    {
      return CST816S_BUS_TO;
    }
    bit_delay();

    b = (uint8_t)(b << 1);
    if (sda_is_high() != 0u)
    {
      b = (uint8_t)(b | 1u);
    }

    scl_drive_low();
  }

  /* 第 9 拍：这一拍的方向和写字节时相反 —— 由**主机**驱动 SDA。
     ACK = 拉低，NACK = 放开（靠上拉拉高）。
     读的最后一个字节必须 NACK，从机看到才明白"发完了，该收尾了"；
     一直 ACK 的话从机会继续往外吐数据。 */
  if (ack_after != 0u)
  {
    sda_drive_low();
  }
  else
  {
    sda_release();
  }

  bit_delay();
  scl_release();
  if (scl_wait_high() == 0u)
  {
    return CST816S_BUS_TO;
  }
  bit_delay();
  scl_drive_low();
  bit_delay();

  *out = b;
  return CST816S_ACK;
}

/**
  * @brief 写一个字节并要求应答，失败时把现场记下来。
  *
  * @param  nack_phase 收到 NACK 时该记成哪一步（地址 / 寄存器号 / 数据）
  * @retval 1 = 成功；0 = 失败（现场已记录）
  */
static uint8_t bus_write_acked(uint8_t b, uint8_t nack_phase)
{
  int rc = bus_write_byte(b);

  if (rc == CST816S_ACK)
  {
    return 1u;
  }

  /* 超时（SCL 被拉死）和 NACK（没人应答）是两回事：
     前者是电气/从机状态问题，后者是"芯片没理我"。
     按最接近的那一步分开记，别糊成一个数。 */
  cst816s_capture_fault((rc == CST816S_BUS_TO) ? BSP_CST816S_PHASE_SCL
                                               : nack_phase);
  return 0u;
}

/**
  * @brief 读一个字节，失败时把现场记下来。
  *
  *        读的方向不会有 NACK（应答位是主机在驱动），唯一的失败
  *        就是等 SCL 放开超时。
  *
  * @retval 1 = 成功；0 = 失败（现场已记录）
  */
static uint8_t bus_read_acked(uint8_t *out, uint8_t ack_after)
{
  if (bus_read_byte(out, ack_after) == CST816S_ACK)
  {
    return 1u;
  }

  cst816s_capture_fault(BSP_CST816S_PHASE_SCL);
  return 0u;
}

/**
  * @brief 从机抱着线不放的时候，用时钟把它"请"下总线。
  *
  *        打时钟而不是直接把线拽高，是因为 I2C 的规矩是任何一方都
  *        只能拉低、放开靠上拉。硬把线推高就是两个输出对打 ——
  *        轻则拉不上去，重则烧引脚。打时钟是**按协议请求**它放手。
  */
static void bus_unwedge(void)
{
  uint8_t i;

  /* 打时钟之前先放开 SDA：现在从机可能正拉着它，主机不该跟着一起拉 */
  sda_release();

  for (i = 0u; i < CST816S_UNWEDGE_CLOCKS; i++)
  {
    scl_drive_low();
    bit_delay();
    scl_release();
    bit_delay();
  }

  /* 补一个 STOP：从机可能停在"等应答"的状态，它需要看到 STOP
     才认为这次传输真的结束了。少了这一下它可能继续占着总线。 */
  bus_stop();
}

/**
  * @brief 一次传输开始前的准备：确认总线空闲，被占着就先解锁。
  *
  *        这是"上一次传输被打断"的唯一补救点，也是**唯一**需要补救的地方
  *        —— 软件模拟的时序不会像外设那样把烂状态留在寄存器里，
  *        从机那边收拾干净之后，这一次传输和平时完全一样。
  *
  * @retval 1 = 总线可以用了；0 = 救不回来（现场已记录）
  */
static uint8_t bus_prepare(void)
{
  if (bus_wait_idle() != 0u)
  {
    return 1u;
  }

  /* 有人抱着线。先按协议打时钟请它放手 */
  s_fault.bus_stuck++;
  bus_unwedge();

  if (bus_wait_idle() != 0u)
  {
    return 1u;
  }

  /**
    * 打完 9 个时钟还是没放开 —— **这是硬件问题，软件救不了。**
    *
    * 解锁时序是 I2C 协议规定的标准做法，从机只要还活着就一定会放手。
    * 它还在拉，只有三种可能：上拉根本不存在（线升不上去）、
    * 线对地短路了、或者从机彻底死了。
    *
    * 这里如实上报，不假装修好了继续往下走 —— 上一版"总线恢复"
    * 就是在这里含糊过去的，结果屏幕上只看到一个一直在涨的计数，
    * 分不出是"救回来了又坏"还是"根本没救回来"。
    */
  s_fault.unwedge_failed++;
  cst816s_capture_fault(BSP_CST816S_PHASE_BUS_STUCK);
  return 0u;
}

/* ---- 传输层 --------------------------------------------------------------- */

/**
  * @brief 读寄存器：START -> 地址(写) -> 寄存器号 -> repeated START
  *                   -> 地址(读) -> 连读 len 个字节 -> STOP
  *
  *        "先写寄存器号、再重新起一次 START 转成读"是 I2C 读寄存器的
  *        标准套路。中间那次叫 repeated START，特点是**不发 STOP**——
  *        发了 STOP 从机就收尾了，接下来那次读会变成"从当前地址指针读"，
  *        拿到的不是我们要的寄存器。
  *
  * @retval BSP_CST816S_OK 或 BSP_CST816S_ERR_I2C
  */
static int bus_read_regs(uint8_t reg, uint8_t *buf, uint16_t len)
{
  uint16_t i;

  if (bus_prepare() == 0u)
  {
    return BSP_CST816S_ERR_I2C;
  }

  if (bus_start() == 0u)
  {
    /* 先采现场再收尾：bus_stop() 会把 SDA 拉低再放开，
       收完尾再采，两条线必然是"空闲"的高电平，
       而 START 失败的原因恰恰就是"哪条线当时是低的"。
       采晚了等于把唯一的线索洗掉。 */
    cst816s_capture_fault(BSP_CST816S_PHASE_START);
    bus_stop();
    return BSP_CST816S_ERR_I2C;
  }

  if (bus_write_acked(CST816S_ADDR_WRITE, BSP_CST816S_PHASE_ADDR) == 0u)
  {
    bus_stop();
    return BSP_CST816S_ERR_I2C;
  }

  if (bus_write_acked(reg, BSP_CST816S_PHASE_REG) == 0u)
  {
    bus_stop();
    return BSP_CST816S_ERR_I2C;
  }

  if (bus_start() == 0u)      /* repeated START，注意中间没有 STOP */
  {
    /* 先采现场再收尾：bus_stop() 会把 SDA 拉低再放开，
       收完尾再采，两条线必然是"空闲"的高电平，
       而 START 失败的原因恰恰就是"哪条线当时是低的"。
       采晚了等于把唯一的线索洗掉。 */
    cst816s_capture_fault(BSP_CST816S_PHASE_START);
    bus_stop();
    return BSP_CST816S_ERR_I2C;
  }

  if (bus_write_acked(CST816S_ADDR_READ, BSP_CST816S_PHASE_ADDR) == 0u)
  {
    bus_stop();
    return BSP_CST816S_ERR_I2C;
  }

  for (i = 0u; i < len; i++)
  {
    /* 最后一个字节要 NACK，从机才知道该收尾 */
    if (bus_read_acked(&buf[i], (uint16_t)(i + 1u) < len ? 1u : 0u) == 0u)
    {
      bus_stop();
      return BSP_CST816S_ERR_I2C;
    }
  }

  bus_stop();
  return BSP_CST816S_OK;
}

/**
  * @brief 写寄存器：START -> 地址(写) -> 寄存器号 -> 数据 -> STOP
  *
  * @retval BSP_CST816S_OK 或 BSP_CST816S_ERR_I2C
  */
static int bus_write_reg(uint8_t reg, uint8_t value)
{
  if (bus_prepare() == 0u)
  {
    return BSP_CST816S_ERR_I2C;
  }

  if (bus_start() == 0u)
  {
    /* 先采现场再收尾：bus_stop() 会把 SDA 拉低再放开，
       收完尾再采，两条线必然是"空闲"的高电平，
       而 START 失败的原因恰恰就是"哪条线当时是低的"。
       采晚了等于把唯一的线索洗掉。 */
    cst816s_capture_fault(BSP_CST816S_PHASE_START);
    bus_stop();
    return BSP_CST816S_ERR_I2C;
  }

  if (bus_write_acked(CST816S_ADDR_WRITE, BSP_CST816S_PHASE_ADDR) == 0u)
  {
    bus_stop();
    return BSP_CST816S_ERR_I2C;
  }

  if (bus_write_acked(reg, BSP_CST816S_PHASE_REG) == 0u)
  {
    bus_stop();
    return BSP_CST816S_ERR_I2C;
  }

  if (bus_write_acked(value, BSP_CST816S_PHASE_DATA) == 0u)
  {
    bus_stop();
    return BSP_CST816S_ERR_I2C;
  }

  bus_stop();
  return BSP_CST816S_OK;
}

/**
  * @brief 读一个寄存器。
  */
static int bus_read_reg(uint8_t reg, uint8_t *value)
{
  return bus_read_regs(reg, value, 1u);
}

#else /* BSP_CST816S_TRANSPORT_HAL —— HAL I2C1 传输层（与 EEPROM 共享总线） */

/**
  * @brief 读寄存器（HAL 版）。
  *        HAL_I2C_Mem_Read 内部就是"写寄存器号 -> repeated START 转读"，
  *        与上面软件时序的注释语义一致。地址用 8 位形式（CST816S_ADDR_WRITE，
  *        HAL 自己管读写方向位）。
  */
static int bus_read_regs(uint8_t reg, uint8_t *buf, uint16_t len)
{
  int rc;

  bsp_i2c1_lock();
  if (HAL_I2C_Mem_Read(&hi2c1, CST816S_ADDR_WRITE, reg, I2C_MEMADD_SIZE_8BIT,
                       buf, len, CST816S_I2C_TIMEOUT_MS) == HAL_OK)
  {
    rc = BSP_CST816S_OK;
  }
  else
  {
    cst816s_capture_fault(BSP_CST816S_PHASE_BUS_STUCK);
    rc = BSP_CST816S_ERR_I2C;
  }
  bsp_i2c1_unlock();
  return rc;
}

static int bus_write_reg(uint8_t reg, uint8_t value)
{
  int rc;

  bsp_i2c1_lock();
  if (HAL_I2C_Mem_Write(&hi2c1, CST816S_ADDR_WRITE, reg, I2C_MEMADD_SIZE_8BIT,
                        &value, 1u, CST816S_I2C_TIMEOUT_MS) == HAL_OK)
  {
    rc = BSP_CST816S_OK;
  }
  else
  {
    cst816s_capture_fault(BSP_CST816S_PHASE_BUS_STUCK);
    rc = BSP_CST816S_ERR_I2C;
  }
  bsp_i2c1_unlock();
  return rc;
}

static int bus_read_reg(uint8_t reg, uint8_t *value)
{
  return bus_read_regs(reg, value, 1u);
}

#endif /* BSP_CST816S_TRANSPORT_HAL */

/* ---- 芯片控制 ------------------------------------------------------------- */

/**
  * @brief 给 CST816S 发一个复位脉冲。
  *
  *        RST 是低电平复位：拉低 -> 保持 -> 放开。
  *        GPIO 的时钟和方向已经在 MX_GPIO_Init() 里配好了，这里只管电平。
  */
static void cst816s_hardware_reset(void)
{
  HAL_GPIO_WritePin(TOUCH_RST_GPIO_Port, TOUCH_RST_Pin, GPIO_PIN_RESET);
  HAL_Delay(CST816S_RST_LOW_MS);
  HAL_GPIO_WritePin(TOUCH_RST_GPIO_Port, TOUCH_RST_Pin, GPIO_PIN_SET);
  HAL_Delay(CST816S_BOOT_MS);
}

#if !BSP_CST816S_TRANSPORT_HAL
/**
  * @brief 把 PB6/PB7 从 I2C1 外设手里收回来，配成软件 I2C 用的开漏 GPIO。
  *
  *        只在 init 里调一次，之后整个程序都只把这两个脚当普通 GPIO 用。
  */
static void cst816s_pins_take_over(void)
{
  GPIO_InitTypeDef gpio = {0};

  /**
    * 先把 I2C1 外设整个关掉。
    *
    * main.c 里 MX_I2C1_Init() 照常被调用 —— 那是 CubeMX 生成的代码，
    * 改了下次重新生成就被覆盖回去，所以更稳的做法是让它照旧执行，
    * 在这里主动把引脚收回来（而不是去删 main.c 里那一行）。
    *
    * **这一步是必须的**：只要外设还占着引脚（AF 复用模式），
    * 往 ODR 写电平根本不会反映到线上，下面所有时序都会是空操作。
    * 而且现象是"读不到芯片"而不是"报错"，最难查。
    *
    * HAL_I2C_DeInit() 内部的 MspDeInit 会把 PB6/PB7 的复用配置解掉，
    * 顺便关掉 I2C1 的时钟。之后这个外设就再也不用了。
    */
  HAL_I2C_DeInit(&hi2c1);

  /* 幂等地补一次 GPIOB 时钟。MspDeInit 把 I2C1 的时钟关了，
     GPIOB 的时钟它没管 —— 正常情况下 MX_GPIO_Init() 已经开过了，
     但万一哪天 MX_I2C1_Init() 从 main.c 里被拿掉，这里就没人保证。
     重复使能同一个时钟没有副作用。 */
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /**
    * 配成**开漏输出**（GPIO_MODE_OUTPUT_OD）。这一条是硬要求：
    * I2C 的电气规矩是任何一方都只能拉低，放开靠上拉。配成推挽的话，
    * 主机想输出高、而从机正在拉低，就是两个输出对打 ——
    * 轻则拉不上去，重则烧引脚。
    *
    * Speed 取 LOW 是有意的。外设那版配的是 VERY_HIGH（边沿最陡），
    * 在长线/弱上拉上反而容易振铃。软件时序只跑几十 kHz，
    * 用最慢的边沿换来最干净的电平，是稳赚的。
    *
    * 内部上拉（约 40k）比正常的 4.7k 弱得多。模块板上一般自带上拉，
    * 这里开着是当备份：万一外部上拉缺失或虚焊，至少还能把线拉起来，
    * 让"上拉不足"表现成"时序不稳"而不是"彻底没反应"——
    * 前者还能从失败现场看出来，后者只能靠万用表。
    */
  gpio.Pin   = CST816S_SCL_PIN | CST816S_SDA_PIN;
  gpio.Mode  = GPIO_MODE_OUTPUT_OD;
  gpio.Pull  = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(CST816S_GPIO, &gpio);

  /* 空闲状态：两条线都放开，等上拉拉高 */
  scl_release();
  sda_release();
}
#endif /* !BSP_CST816S_TRANSPORT_HAL */

/* Exported functions --------------------------------------------------------*/

int bsp_cst816s_init(void)
{
  uint8_t id = 0u;

  /* HAL 传输层下不做引脚接管 —— I2C1 外设保持原样，PB6/PB7 归 HAL 管，
     EEPROM（AT24C02）继续正常工作。 */
  cst816s_hardware_reset();

  /* 读 ChipID。这一步同时验证了两件事：
       I2C 时序通不通（不通返回 ERR_I2C）
       芯片是不是 CST816 系列（不是返回 ERR_CHIPID） */
  if (bus_read_reg(CST816S_REG_CHIPID, &id) != BSP_CST816S_OK)
  {
    return BSP_CST816S_ERR_I2C;
  }

  s_chip_id = id;

  if ((id < CST816S_CHIPID_MIN) || (id > CST816S_CHIPID_MAX))
  {
    return BSP_CST816S_ERR_CHIPID;
  }

  /**
    * 关闭自动休眠。这一步必须成功，原因见下面这段。
    *
    * CST816S 上电后默认**开**着自动休眠：一段时间不碰（从最后一次
    * 触摸算起），芯片进低功耗，触控扫描停下来。
    *
    * 麻烦在于**低功耗状态下它是靠 INT 引脚通知 MCU 的**，而寄存器
    * 不再更新。本工程没有接 INT（为了少一根线），所以一旦它睡下去，
    * 轮询读到的永远是"没手指"，而且再也醒不过来。
    *
    * ⚠️ 但这一条**不是**本工程"用一阵子就永久卡死"的原因 ——
    * 验证过：as=OK（确认关掉了）的情况下照样卡死。真正的原因在
    * bsp_cst816s.h 开头那段，是 HAL 的两条死锁路径。
    * 留着这一句是因为关休眠本身就该做，它只是不解释那个现象。
    */
  s_autosleep_rc = bus_write_reg(CST816S_REG_DISAUTOSLEEP, 0x01u);

  return BSP_CST816S_OK;
}

int bsp_cst816s_autosleep_rc(void)
{
  return s_autosleep_rc;
}

int bsp_cst816s_keepalive(void)
{
  return bus_write_reg(CST816S_REG_DISAUTOSLEEP, 0x01u);
}

int bsp_cst816s_read(bsp_cst816s_touch_t *out)
{
  uint8_t buf[CST816S_READ_LEN];

  if (out == NULL)
  {
    return BSP_CST816S_ERR_PARAM;
  }

  /* 先给一个确定的"没触摸"状态。这样即使下面失败提前返回，
     调用方拿到的也是安全的默认值，而不是上一次的残留。

     raw[] 也必须一起清：它是"失败现场"的证据，而失败路径上
     根本没有往 buf 里填过东西。不清的话，调用方打印出来的是
     上一次成功读的旧字节（甚至栈上的垃圾），会让人以为
     "读到了 0x12 这种可疑值"，把排查方向带偏。 */
  out->x       = 0u;
  out->y       = 0u;
  out->pressed = 0u;
  out->gesture = 0u;

  for (uint8_t i = 0u; i < CST816S_READ_LEN; i++)
  {
    out->raw[i] = 0u;
  }

  if (bus_read_regs(CST816S_REG_GESTURE, buf, CST816S_READ_LEN)
      != BSP_CST816S_OK)
  {
    return BSP_CST816S_ERR_I2C;
  }

  /* 原始字节原样留一份给上层排错用。
     注意这里在"手指数为 0"的提前返回之前 —— 卡死的时候
     这几个字节恰恰是唯一能说明问题的东西，不能被跳过。 */
  for (uint8_t i = 0u; i < CST816S_READ_LEN; i++)
  {
    out->raw[i] = buf[i];
  }

  out->gesture = buf[CST816S_IDX_GESTURE];

  /* 手指数。0 表示这一帧没有手指，后面几个字节无意义，
     不要拿它去拼坐标 —— 抬起瞬间的残留值会是一个假坐标，
     表现成"手抬起来了，触摸点还闪一下"。 */
  if (buf[CST816S_IDX_FINGER] == 0u)
  {
    return BSP_CST816S_OK;
  }

  /* 12 位坐标：高 4 位在 H 寄存器低半字节，低 8 位在 L 寄存器。
     所以是 (H & 0x0F) << 8 | L，不是 (H << 8) | L ——
     H 的高 4 位是保留位，不清掉会得到一个上千的野坐标。 */
  out->x = (uint16_t)(((uint16_t)(buf[CST816S_IDX_XPOS_H] & 0x0Fu) << 8)
                      | (uint16_t)buf[CST816S_IDX_XPOS_L]);
  out->y = (uint16_t)(((uint16_t)(buf[CST816S_IDX_YPOS_H] & 0x0Fu) << 8)
                      | (uint16_t)buf[CST816S_IDX_YPOS_L]);
  out->pressed = 1u;

  return BSP_CST816S_OK;
}

uint8_t bsp_cst816s_chip_id(void)
{
  return s_chip_id;
}

void bsp_cst816s_get_fault(bsp_cst816s_fault_t *out)
{
  if (out == NULL)
  {
    return;
  }

  *out = s_fault;
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
