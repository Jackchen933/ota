/**
  ******************************************************************************
  * @file    ui_test.c
  * @brief   LVGL 移植自检界面（显示 + 触摸）。
  *
  *          屏幕布局（240x280）：
  *
  *            ┌────────────────────────┐
  *            │■红                 绿■│  <- 四个角块：屏幕坐标的硬基准
  *            │  ST7789 + LVGL 9.4     │
  *            │  TOUCH OK  id=B5  as=OK│  <- 初始化结果 / ChipID / 关休眠写成功没
  *            │  e5=0 bs=0/0  n=1234   │  <- 近5秒失败, 总线被占/没救回, 采样次数
  *            │  raw 123, 45  g=00     │  <- 芯片原始坐标 + 手势码
  *            │  ->  123, 45  DOWN     │  <- 变换后坐标 + 按下状态
  *            │  rc 00 01 7B 2D 00 00  │  <- 芯片吐的 6 个原始字节
  *            │  press 3  hit 123, 45  │  <- 累计按下次数 + 最后一次落点
  *            │        12.3 s          │  <- 每秒跳，验证 lv_tick + timer
  *            │  ▓▓▓▓▓▓░░░░░░░░░░░░░   │  <- 进度条，验证局部刷新
  *            │   ●                    │  <- 跟手指跑的红点
  *            │■蓝                 白■│
  *            └────────────────────────┘
  *
  *          【屏幕怎么验】
  *            四个角块位置不对 / 屏幕边缘有条纹  -> 调 BSP 的 X_SHIFT / Y_SHIFT
  *            红块跑到右上角                     -> 屏幕方向反了，看 MADCTL
  *            有图形但颜色整体发蓝发绿           -> 字节序问题，见 lv_port_disp.c
  *            角块对、文字是空白                 -> 字体没编进去
  *            角块对、数字不跳                   -> lv_tick 没接上，或主循环没跑
  *                                                  lv_timer_handler()
  *
  *          【触摸怎么验 —— 先看 n 和 press，再看坐标】
  *
  *          第一步：不看坐标，只看这两个累计量。
  *          手指点一下只有一两百毫秒，只盯当前值是看不出问题的。
  *
  *            n 一直是 0 或不动
  *                -> lv_port_indev.c 的 read_cb 根本没被调用。
  *                   I2C、接线、驱动全都无关，问题在 LVGL 这一侧：
  *                   查 lv_port_indev_init() 有没有被调到、
  *                   lv_port_disp_init() 里有没有 lv_display_set_default()
  *                   （indev 挂不到显示器上就不会被轮询）。
  *
  *            n 在涨、press 一直是 0
  *                -> 驱动在跑、I2C 在通（e 也不涨），但芯片报告"没有手指"。
  *                   这时候才轮到查触摸面板本身：芯片和面板之间的排线、
  *                   面板是不是独立的没贴在屏幕上、或者 e 在涨说明 I2C 干扰。
  *
  *            press 在涨
  *                -> 芯片确实感觉到了手指。接下来才轮到坐标和方向：
  *                   1. 点左上角 -> 红点出现在左上角
  *                   2. 点右下角 -> 红点出现在右下角
  *                   3. 沿四条边划一圈 -> 红点不跑偏、不镜像、不跳
  *                   4. raw 和 -> 都在 0~239 / 0~279 之间
  *                   第 1~3 条不对，改 lv_port_indev.c 顶部那三个方向宏。
  *                   第 4 条里 raw 就不对的话，是驱动读字节的问题，不是方向。
  *
  *          【读失败长什么样 —— 最容易被误读的一屏】
  *
  *          一旦某次读失败，中间三行会变成这样：
  *
  *            raw --  g=--
  *            ->  --, --  READ FAIL
  *            SD0 ADDR
  *
  *          出现这个就说明**这一帧没有任何真实数据**，
  *          raw / -> / rc 全都不能信（它们本来会显示上一次成功读到的值，
  *          看起来像芯片报了个怪状态，其实只是残值）。
  *
  *          最后那一行是失败现场，两块：
  *
  *          前面可能有 SC0 / SD0 —— 失败那一刻**线被拉低**的标记。
  *            空闲的 I2C 总线两条线都该是高电平。谁被拉着不放，
  *            谁就是卡住的那一方。什么都不显示就是这个正常状态。
  *              SC0 = SCL 被拉低（从机在做时钟拉伸，而且不放手）
  *              SD0 = SDA 被拉低（从机停在半个字节中间，在等时钟）
  *
  *          后面是**卡在哪一步**，取值：
  *            STUCK  传输还没开始线就被占着，打 9 个时钟也没放开。
  *                   **这是硬件问题**，软件救不了（上拉缺失 / 对地短路 /
  *                   从机死了）—— 这时候该动烙铁，不是改代码。
  *            START  建不起 START 条件。SDA 空闲时应该是高的，
  *                   一直低就没有下降沿可用。
  *            ADDR   从机地址没人应答。线是通的但芯片没响应 ——
  *                   地址不对、芯片没供电、芯片在复位中。
  *            REG    地址应答了、寄存器号没应答。这种"半通不通"
  *                   几乎总是时序或走线问题。
  *            DATA   寄存器号也应答了，最后那个数据字节没应答 ——
  *                   通常是写了一个芯片不认的寄存器。
  *            SCL    等 SCL 放开超时，从机在时钟拉伸不放手。
  *
  *          【为什么分这么细，而不是像以前那样只报一个 HAL 错误码】
  *          HAL 的 TIMEOUT 一个值可能对应"线被拉死"和"没人应答"
  *          两件完全相反的事 —— 前者要查上拉和短路，后者要查供电和地址。
  *          分不开就只能猜，而猜错的代价是烧一次固件。
  *
  *          然后看第二排：
  *            e5 在涨 -> 此刻就是坏的
  *            e5 = 0  -> 此刻是好的，上面那行的现场是"上一次"失败留下的
  *            bs 的第二个数在涨 -> 打时钟都救不回来 = 硬件问题
  *
  *          【注意：这里所有文字都必须是 ASCII】
  *          内置的 montserrat 字体里一个汉字都没有，写中文会显示成空白。
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "ui_test.h"
#include "lvgl.h"
#include "lv_port_indev.h"
#include "bsp_cst816s.h"

/* 注意这里**不再** #include "stm32f4xx_hal.h" 了。
   上一版为了把 HAL 的返回码翻译成文字，让这一屏依赖了 HAL。
   驱动改成软件模拟之后，失败现场里不再有 HAL 的任何东西，
   这一层对 HAL 的依赖也就自然没了 —— 少一个不该有的依赖。 */

/* Private define ------------------------------------------------------------*/

/* 四角验证块的边长，单位像素 */
#define CORNER_SIZE     20

/* 屏幕分辨率，和 lv_port_disp.h 保持一致 */
#define SCR_W           240
#define SCR_H           280

/* 跟随手指的红点直径，单位像素 */
#define TOUCH_DOT_SIZE  16

/* 运行秒数 / 进度条的刷新周期。100ms 足够，偶尔跳一下也看得清 */
#define TEST_TIMER_MS   100

/* 触摸红点的刷新周期。
   比上面那个快得多，因为红点要跟手 —— 100ms 的话就是 10fps，
   手指一动红点就明显拖在后面，看着像触摸卡顿，其实是刷新慢。 */
#define TOUCH_TIMER_MS  30

/* Private variables ---------------------------------------------------------*/

static lv_obj_t *s_label_uptime = NULL;   /* 显示运行秒数 */
static lv_obj_t *s_label_info   = NULL;   /* 显示状态文字 */
static lv_obj_t *s_label_touch  = NULL;   /* 显示触摸芯片状态和坐标 */
static lv_obj_t *s_bar          = NULL;   /* 进度条 */
static lv_obj_t *s_dot          = NULL;   /* 跟手指跑的红点 */

static lv_timer_t *s_timer       = NULL;
static lv_timer_t *s_touch_timer = NULL;

/* Private function prototypes -----------------------------------------------*/
static void test_timer_cb(lv_timer_t *timer);
static void touch_timer_cb(lv_timer_t *timer);
static void create_block(lv_obj_t *parent, int32_t x, int32_t y,
                         int32_t w, int32_t h, uint32_t rgb, int32_t radius);

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  在指定位置画一个纯色块（圆角，radius 决定圆不圆）。
  *
  *         用 lv_obj 当色块而不是 lv_canvas，是因为 lv_canvas 会多占一份
  *         和画布等大的内存，而这里只需要验证"某个坐标能不能刷成某个颜色"。
  */
static void create_block(lv_obj_t *parent, int32_t x, int32_t y,
                         int32_t w, int32_t h, uint32_t rgb, int32_t radius)
{
  lv_obj_t *block = lv_obj_create(parent);

  /* 先把主题带来的默认样式（圆角、边框、内边距、阴影）全清掉，
     否则色块不会严丝合缝贴在角上，会看不准 */
  lv_obj_remove_style_all(block);

  lv_obj_set_pos(block, x, y);
  lv_obj_set_size(block, w, h);

  lv_obj_set_style_bg_color(block, lv_color_hex(rgb), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(block, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(block, radius, LV_PART_MAIN);

  /* 色块不参与点击、也不该滚动。
     红点尤其要注意这一条：它跟着手指跑，如果它是可点击的，
     LVGL 会把它当成手指下面的目标控件，虽然这里没有绑事件不会出错，
     但多一个"红点自己挡住自己"的嫌疑，不如干脆去掉。 */
  lv_obj_remove_flag(block, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(block, LV_OBJ_FLAG_CLICKABLE);
}

/**
  * @brief  把触摸初始化错误码翻译成人能看懂的一行 ASCII 文字。
  *
  *         ASCII 的原因见文件头：字体里没有汉字。
  */
static const char *touch_rc_text(int rc)
{
  switch (rc)
  {
    case BSP_CST816S_OK:         return "TOUCH OK";
    case BSP_CST816S_ERR_I2C:    return "I2C FAIL";
    case BSP_CST816S_ERR_CHIPID: return "CHIPID BAD";
    case BSP_CST816S_ERR_PARAM:  return "BAD PARAM";
    default:                     return "UNKNOWN";
  }
}

/**
  * @brief  初始化失败时给一句"下一步该干什么"，正常时返回空串。
  *
  *        故意做得这么短，是因为屏幕只有 240px 宽。
  *        之前把提示拼在状态行里（"I2C FAIL - check SDA/SCL/pullup/3V3"）
  *        加上 id 之后就超过 240px 会折行，把下面的坐标挤掉。
  *        宁可分一行，也不要文字挤成一团看不懂。
  */
static const char *touch_hint(int rc)
{
  switch (rc)
  {
    case BSP_CST816S_ERR_I2C:    return "check SDA/SCL/pullup";
    case BSP_CST816S_ERR_CHIPID: return "chip not CST816 series?";
    case BSP_CST816S_ERR_PARAM:  return "param bug in caller";
    default:                     return "";
  }
}

/**
  * @brief  把"卡在哪一步"翻译成短标签。
  *
  *        这几个值指向的根因完全不同，上一版把它们压成一个 HAL
  *        错误码就没法定位了（HAL 的 TIMEOUT 一个值同时覆盖
  *        "线被拉死"和"没人应答"两件相反的事）。
  *        详细解释见文件头那段。
  */
static const char *phase_text(uint8_t phase)
{
  switch (phase)
  {
    case BSP_CST816S_PHASE_BUS_STUCK: return "STUCK";
    case BSP_CST816S_PHASE_START:     return "START";
    case BSP_CST816S_PHASE_ADDR:      return "ADDR";
    case BSP_CST816S_PHASE_REG:       return "REG";
    case BSP_CST816S_PHASE_DATA:      return "DATA";
    case BSP_CST816S_PHASE_SCL:       return "SCL";
    default:                          return "?";
  }
}

/**
  * @brief  定时器回调：每 100ms 跑一次。
  *
  *         它同时在验证两件事：
  *           1. lv_tick 有在走 —— 秒数会涨
  *           2. 重绘链路是通的   —— 数字和进度条会变
  *         如果不跳，要么 lv_tick_set_cb 没接，要么主循环里没调
  *         lv_timer_handler()。
  */
static void test_timer_cb(lv_timer_t *timer)
{
  static uint32_t tick_count = 0u;
  int32_t bar_value;

  LV_UNUSED(timer);

  tick_count++;

  /* 每 10 次 = 1 秒 */
  lv_label_set_text_fmt(s_label_uptime, "%u.%u s",
                        (unsigned int)(tick_count / 10u),
                        (unsigned int)(tick_count % 10u));

  /* 进度条来回跑，0 -> 100 -> 0，走一轮 5 秒 */
  if (s_bar != NULL)
  {
    bar_value = lv_bar_get_value(s_bar);
    bar_value += 2;
    if (bar_value > 100)
    {
      bar_value = 0;
    }
    /* LV_ANIM_ON：让 LVGL 自己插值，这样每帧都在变，更能看出刷新是连续的 */
    lv_bar_set_value(s_bar, bar_value, LV_ANIM_ON);
  }
}

/**
  * @brief  定时器回调：每 30ms 跑一次，更新红点和触摸读数。
  *
  *         读的是 lv_port_indev.c 里缓存的那份采样结果，
  *         而不是自己再去读一次 I2C —— 否则一条总线上会有两个读取者，
  *         更容易出现"读到一半被另一处打断"的怪现象。
  */
static void touch_timer_cb(lv_timer_t *timer)
{
  lv_port_indev_sample_t s;

  /* ---- 累计量 ----
     手指点一下只有一两百毫秒，30ms 采样很容易整帧错过。
     只显示"当前值"会造成"我明明点了，界面什么都没变"的假象。
     下面这几个量只增不减，不会漏掉任何一次触摸。 */
  static uint32_t s_press_count = 0u;   /* 见过几次"有手指" */
  static uint16_t s_last_press_x = 0u;  /* 最后一次按下时的原始 X */
  static uint16_t s_last_press_y = 0u;  /* 最后一次按下时的原始 Y */

  LV_UNUSED(timer);

  lv_port_indev_get_last(&s);

  if (s.pressed != 0u)
  {
    s_press_count++;
    s_last_press_x = s.raw_x;
    s_last_press_y = s.raw_y;
  }

  if (s_dot != NULL)
  {
    if (s.pressed != 0u)
    {
      /* 让红点的中心落在手指位置上，所以要减掉半个边长 */
      lv_obj_set_pos(s_dot,
                     s.x - (TOUCH_DOT_SIZE / 2),
                     s.y - (TOUCH_DOT_SIZE / 2));
      lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
      lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (s_label_touch != NULL)
  {
    /* 中间三行先各自拼好。因为"读到数据"和"读失败"要显示完全不同的东西，
       失败时绝不能把上一次的残值当成本次读数摆出来。 */
    char line_raw[32];
    char line_xy[32];
    char line_rc[32];

    if (s.read_ok != 0u)
    {
      lv_snprintf(line_raw, sizeof(line_raw), "raw %u, %u  g=%02X",
                  (unsigned int)s.raw_x, (unsigned int)s.raw_y,
                  (unsigned int)s.gesture);

      lv_snprintf(line_xy, sizeof(line_xy), "->  %d, %d  %s",
                  (int)s.x, (int)s.y,
                  (s.pressed != 0u) ? "DOWN" : "UP");

      lv_snprintf(line_rc, sizeof(line_rc),
                  "rc %02X %02X %02X %02X %02X %02X",
                  (unsigned int)s.raw[0], (unsigned int)s.raw[1],
                  (unsigned int)s.raw[2], (unsigned int)s.raw[3],
                  (unsigned int)s.raw[4], (unsigned int)s.raw[5]);
    }
    else
    {
      /* 读失败了。这三行原来会保持上一次成功读到的值不变，于是屏幕上
         出现 "rc 01 01 80 86 00 AE"（第 2 字节 = 1，有手指）配
         "-> 134, 174 UP"（抬起）这种自相矛盾的组合 ——
         看着像芯片报了个怪状态，其实只是过期的残值，
         照它去查会把方向整个带偏（这个坑我已经踩过一次）。
         所以宁可用 "--" 把"这里没有真数据"直接写在脸上。 */
      lv_snprintf(line_raw, sizeof(line_raw), "raw --  g=--");
      lv_snprintf(line_xy, sizeof(line_xy), "->  --, --  READ FAIL");

      /* 先放两条线的实际电平，再放卡在哪一步。
         线只有"被拉低"才显示 —— 空闲的总线两条线都该是高，
         什么都不显示就是这个正常状态，不占字符。

         **两条线的标记放在最前面**是排版上的取舍：这一行放不下
         会从右边裁掉，而"哪条线被拉低"恰恰是最不能丢的一条。
         最坏情况 "SC0 SD0 STUCK" 是 13 字符，在 14px 字体下
         只占约 100px，余量很大。 */
      lv_snprintf(line_rc, sizeof(line_rc), "%s%s%s",
                  (s.fault.scl_level == 0u) ? "SC0 " : "",
                  (s.fault.sda_level == 0u) ? "SD0 " : "",
                  phase_text(s.fault.phase));
    }

    /* 第二排三个数的排列顺序是按"卡死时先看谁"来的，不是按重要程度
       随手排的。标签是定宽的，左边数位一多右边就会被挤出屏幕 ——
       所以最该看的必须放在最前面。

       e5  = 最近 5 秒内的失败次数。它回答的是"现在坏不坏"：
               e5 在涨 -> 此刻就是坏的
               e5 = 0  -> 此刻是好的，上面那行的现场是上一次失败留下的
             累计的 errors 分不出"以前失败过"和"现在正在失败"，
             而这两个结论指向完全相反的方向，所以只看 e5。

       bs  = bus_stuck / unwedge_failed，两个都是累计值。
               bs 的第一个数：传输还没开始线就被占着的次数。
                 它不为 0 就说明有从机在正常通信之外抱着线不放。
               bs 的第二个数：打了 9 个时钟还是没放开的次数 ——
                 **这是最硬的一条"是硬件问题"的证据**。
                 协议规定的解锁对它无效，那就是上拉缺失、
                 线对地短路、或者芯片死了。
             只看第一个数的话，"每次都被救回来了"和"根本没救回来"
             在屏幕上长得一模一样，而这两种的处置完全相反。

       n   = 采样次数。判断"驱动到底有没有被调用"的唯一依据：
             n 一直不动就是 lv_port_indev.c 的回调根本没跑，
             问题在 LVGL 那一侧，跟 I2C、跟接线都无关。

       （原来这里还有 wk = 保活次数/失败次数，以及累计的 e。
         这一版让位给 bs 了 —— "总线被物理占用"没有别的地方能看到，
         而保活次数不像它那么关键。

         ⚠️ 注意 e5 **不包含**保活失败：e5 只统计读坐标那一次传输。
         保活失败只记在结构体的 keepalive_errors 里，屏幕上没有显示。
         真要查保活是不是在失败，得临时把这个字段加回显示 ——
         别看着 e5=0 就断定"一次都没失败过"。） */
    lv_label_set_text_fmt(s_label_touch,
                          "%s  id=%02X  as=%s\n"
                          "e5=%lu bs=%lu/%lu  n=%lu\n"
                          "%s\n"
                          "%s\n"
                          "%s\n"
                          "press %lu  hit %u, %u",
                          touch_rc_text(s.init_rc),
                          (unsigned int)bsp_cst816s_chip_id(),
                          (s.autosleep_rc == BSP_CST816S_OK) ? "OK" : "FAIL",
                          (unsigned long)s.errors_window,
                          (unsigned long)s.fault.bus_stuck,
                          (unsigned long)s.fault.unwedge_failed,
                          (unsigned long)s.samples,
                          line_raw, line_xy, line_rc,
                          (unsigned long)s_press_count,
                          (unsigned int)s_last_press_x,
                          (unsigned int)s_last_press_y);
  }
}

/* Exported functions --------------------------------------------------------*/

/**
  * @brief  画出整个自检界面
  */
void ui_test_init(void)
{
  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *label;
  lv_port_indev_sample_t s0;

  if (scr == NULL)
  {
    /* lv_port_disp_init() 里没调 lv_display_set_default，或者根本没调 */
    return;
  }

  /* 取一次触摸初始化的结果。init_rc / autosleep_rc 开机后不会再变，
     所以这里读一次就够，不用每帧刷。 */
  lv_port_indev_get_last(&s0);

  /* ---------------- 屏幕底色 ---------------- */
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

  /* ---------------- 四角验证块 ----------------
     红在左上、绿在右上、蓝在左下、白在右下。
     这四个位置是硬坐标，不对就是屏幕坐标/方向/偏移有问题。
     验触摸方向时也靠它们：手指点红块，红点就该出现在红块上。 */
  create_block(scr, 0, 0, CORNER_SIZE, CORNER_SIZE, 0xFF0000, 0); /* 左上 红 */
  create_block(scr, SCR_W - CORNER_SIZE, 0, CORNER_SIZE, CORNER_SIZE,
               0x00FF00, 0);                                      /* 右上 绿 */
  create_block(scr, 0, SCR_H - CORNER_SIZE, CORNER_SIZE, CORNER_SIZE,
               0x0000FF, 0);                                      /* 左下 蓝 */
  create_block(scr, SCR_W - CORNER_SIZE, SCR_H - CORNER_SIZE,
               CORNER_SIZE, CORNER_SIZE, 0xFFFFFF, 0);            /* 右下 白 */

  /* ---------------- 标题 ---------------- */
  label = lv_label_create(scr);
  lv_obj_set_pos(label, 0, 28);
  lv_obj_set_width(label, SCR_W);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_label_set_text(label, "ST7789 + LVGL 9.4");

  /* ---------------- 触摸读数（会跳） ----------------
     三行：芯片状态 / 原始坐标 / 变换后坐标。
     raw 和 -> 分开显示是关键 —— 一眼就能分辨是驱动错了还是方向错了。 */
  s_label_touch = lv_label_create(scr);
  lv_obj_set_pos(s_label_touch, 6, 46);
  lv_obj_set_style_text_color(s_label_touch, lv_color_hex(0xFFD54F), LV_PART_MAIN);
  lv_label_set_text(s_label_touch, "TOUCH ...");

  /* ---------------- 运行秒数（会跳） ---------------- */
  s_label_uptime = lv_label_create(scr);
  lv_obj_set_pos(s_label_uptime, 0, 152);
  lv_obj_set_width(s_label_uptime, SCR_W);
  lv_obj_set_style_text_align(s_label_uptime, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_label_uptime, lv_color_hex(0x4FC3F7), LV_PART_MAIN);
  lv_label_set_text(s_label_uptime, "0.0 s");

  /* ---------------- 进度条（会动） ---------------- */
  s_bar = lv_bar_create(scr);
  lv_obj_set_pos(s_bar, 20, 176);
  lv_obj_set_size(s_bar, SCR_W - 40, 20);
  lv_bar_set_range(s_bar, 0, 100);
  lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

  /* ---------------- 底部状态文字 ----------------
     正常时显示分辨率，初始化失败时改成"下一步该干什么"。
     提示语放在这一行、而不是挤在触摸读数里，是因为读数那 6 行
     每行都接近 240px 的宽度上限，多塞一个字符就折行，会把布局顶乱。 */
  s_label_info = lv_label_create(scr);
  lv_obj_set_pos(s_label_info, 0, 202);
  lv_obj_set_width(s_label_info, SCR_W);
  lv_obj_set_style_text_align(s_label_info, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_label_info, lv_color_hex(0x808080), LV_PART_MAIN);

  if ((s0.init_rc == BSP_CST816S_OK) && (s0.autosleep_rc == BSP_CST816S_OK))
  {
    lv_label_set_text_fmt(s_label_info, "%dx%d  partial buf", SCR_W, SCR_H);
  }
  else if (s0.init_rc != BSP_CST816S_OK)
  {
    lv_label_set_text(s_label_info, touch_hint(s0.init_rc));
  }
  else
  {
    /* 触摸本身通了，但"关自动休眠"没写进去 —— 这一条会直接导致
       触摸过一会儿就停，而且光看触摸读数看不出来。 */
    lv_label_set_text(s_label_info, "autosleep write FAILED");
  }

  /* ---------------- 跟手指跑的红点 ----------------
     最后一个创建，所以它在最上层，不会被别的控件盖住。
     一开始是隐藏的，手指按下去才出现。 */
  s_dot = lv_obj_create(scr);
  lv_obj_remove_style_all(s_dot);
  lv_obj_set_size(s_dot, TOUCH_DOT_SIZE, TOUCH_DOT_SIZE);
  lv_obj_set_style_bg_color(s_dot, lv_color_hex(0xFF3030), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);

  /* ---------------- 启动定时器 ---------------- */
  s_timer       = lv_timer_create(test_timer_cb, TEST_TIMER_MS, NULL);
  s_touch_timer = lv_timer_create(touch_timer_cb, TOUCH_TIMER_MS, NULL);

  LV_UNUSED(s_timer);
  LV_UNUSED(s_touch_timer);
}
