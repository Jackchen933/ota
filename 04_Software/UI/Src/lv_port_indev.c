/**
  ******************************************************************************
  * @file    lv_port_indev.c
  * @brief   LVGL <-> CST816S 触摸桥实现。
  *
  *          数据流：
  *
  *            CST816S       bsp_cst816s_read()        read_cb
  *            ───────  ──►  原始 12 位坐标     ──►  方向变换  ──►  LVGL
  *             I2C                                      ▲
  *                                                就在这里，只此一处
  *
  *          【方向不对改哪里】
  *          整个工程里只有下面那三个宏决定触摸方向。
  *          驱动层永远吐原始值，不掺任何方向逻辑 —— 这样
  *          "驱动对不对"和"方向对不对"是两个可以分开判断的问题。
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "lv_port_indev.h"
#include "lv_port_disp.h"     /* LV_PORT_DISP_HOR_RES / VER_RES */
#include "bsp_cst816s.h"

/* Private define ------------------------------------------------------------*/

/**
  * @brief 坐标方向配置。
  *
  *        CST816S 的坐标原点和屏幕的坐标原点不一定一致 ——
  *        触摸屏是贴在显示面板上的，谁旋转了 90 度、谁镜像了，
  *        厂家装配的时候各有各的做法。这三个宏就是用来对齐的。
  *
  *        【怎么判断该改哪个】
  *        跑自检界面（ui_test.c），四角有红绿蓝白四个基准块：
  *
  *          按左上角，高亮的却是右上角   -> TOUCH_MIRROR_X 改 1
  *          按左上角，高亮的却是左下角   -> TOUCH_MIRROR_Y 改 1
  *          按左上角，高亮的却是左上角   -> 方向对了，别动
  *          上下滑动时高亮左右跑         -> TOUCH_SWAP_XY 改 1
  *
  *        这三个宏最多只会改一次，校准完就固定下来了。
  */
#define TOUCH_SWAP_XY    0   /**< 1 = 交换 X/Y 轴（屏幕和触摸差了 90 度） */
#define TOUCH_MIRROR_X   0   /**< 1 = X 轴镜像 */
#define TOUCH_MIRROR_Y   0   /**< 1 = Y 轴镜像 */

/**
  * @brief 坐标轴的最大值，随 SWAP_XY 一起变。
  *
  *        交换之后，触摸的 X 轴对应的其实是屏幕的竖直方向，
  *        镜像时的长度也必须跟着换，不然会镜像到屏幕外面去。
  */
#if TOUCH_SWAP_XY
  #define TOUCH_AXIS_X_MAX   (LV_PORT_DISP_VER_RES - 1)
  #define TOUCH_AXIS_Y_MAX   (LV_PORT_DISP_HOR_RES - 1)
#else
  #define TOUCH_AXIS_X_MAX   (LV_PORT_DISP_HOR_RES - 1)
  #define TOUCH_AXIS_Y_MAX   (LV_PORT_DISP_VER_RES - 1)
#endif

/* Private variables ---------------------------------------------------------*/

static lv_indev_t *s_indev = NULL;

/** 最近一次采样的记录，给 ui_test.c 看 */
static lv_port_indev_sample_t s_last;

/**
  * @brief 保活周期，单位毫秒。
  *
  *        CST816S 从最后一次触摸算起，一段时间不碰就进低功耗。
  *        这个周期的选取有两头要考虑：
  *          太长 -> 芯片已经睡下去了才去叫它，中间那段触摸是丢的
  *          太短 -> 白白多刷 I2C。现在是软件模拟，跑在 45kHz 上，
  *                  一次保活写（3 个字节）约 87~116 个半位 ≈ 1.0~1.3ms。
  *                  每帧读坐标本来就占 2.2~3.2ms，500ms 加一次
  *                  1.3ms 完全可以忽略。
  *        取 500ms 是留足余量：只要芯片的休眠门槛超过 0.5 秒
  *        （数据手册上是秒级），它就永远等不到那一刻。
  */
#define INDEV_KEEPALIVE_MS   500u

/** 上次保活的时间戳 */
static uint32_t s_keepalive_tick = 0u;

/**
  * @brief 失败计数的滚动窗口，单位毫秒。
  *
  *        累计的 errors 分不出"以前失败过"和"现在正在失败"，
  *        而这两个结论指向完全相反的方向：
  *          以前失败过、现在不失败 -> 总线是好的，卡死另有原因
  *          现在还在失败           -> 总线或外设坏了，先修 I2C
  *        所以要有一个"近期"的量。
  *
  *        5 秒是随手定的：足够覆盖一次卡死，又短到能看出趋势变化。
  */
#define INDEV_ERR_WINDOW_MS  5000u

/** 滚动窗口的起点时间戳，以及窗口内累计的失败次数 */
static uint32_t s_err_window_tick  = 0u;
static uint32_t s_err_window_count = 0u;

/* Private function prototypes -----------------------------------------------*/
static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  把芯片原始坐标换算成 LVGL 屏幕坐标。
  *
  *         故意不做范围裁剪（clamp）。裁剪看着更"稳"，实际会把
  *         最有用的线索藏起来 —— 坐标超出屏幕是最典型的
  *         "线接对了但驱动读错字节"的症状，裁掉之后就看不出来了。
  *         超范围的坐标 LVGL 自己会忽略掉，不会出乱子。
  */
static void indev_transform(uint16_t raw_x, uint16_t raw_y,
                            lv_coord_t *out_x, lv_coord_t *out_y)
{
  uint16_t x = raw_x;
  uint16_t y = raw_y;

#if TOUCH_SWAP_XY
  {
    uint16_t tmp = x;
    x = y;
    y = tmp;
  }
#endif

#if TOUCH_MIRROR_X
  x = (uint16_t)(TOUCH_AXIS_X_MAX - x);
#endif

#if TOUCH_MIRROR_Y
  y = (uint16_t)(TOUCH_AXIS_Y_MAX - y);
#endif

  *out_x = (lv_coord_t)x;
  *out_y = (lv_coord_t)y;
}

/**
  * @brief  LVGL 每次要输入状态时回调这里。
  *
  * @param  data 填状态和坐标。状态必须是 LV_INDEV_STATE_PRESSED 或
  *              LV_INDEV_STATE_RELEASED，只填坐标不填状态 LVGL 会当成没按。
  */
static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
  bsp_cst816s_touch_t touch;
  uint32_t now;
  int rc;

  LV_UNUSED(indev);

  s_last.samples++;

  /**
   * 保活：周期性把"关自动休眠"按回去。
   *
   * 为什么不能只在 init 里写一次 —— 那次写成了不代表一直有效。
   * 芯片因为复位、3V3 上的毛刺之类的原因退回默认值时，自动休眠
   * 会重新打开，然后它就会在某次停下手指之后睡过去，从此再也
   * 读不到触摸。而"什么时候睡过去"取决于你最后一次碰它是什么时候，
   * 所以表现出来是随机时间点卡死，最难查。
   *
   * 这里用 lv_tick_get() 而不是数 samples 取模：samples 的增长
   * 速率取决于 lv_timer_handler() 被调用的频率，拿它当时间用
   * 会在负载变化时飘。
   */
  now = lv_tick_get();
  if ((now - s_keepalive_tick) >= INDEV_KEEPALIVE_MS)
  {
    s_keepalive_tick = now;
    s_last.keepalives++;
    if (bsp_cst816s_keepalive() != BSP_CST816S_OK)
    {
      s_last.keepalive_errors++;
    }
  }

  /* 滚动窗口到点就清零重来。用"清零重来"而不是环形缓冲，
     是因为要回答的问题只有"现在坏不坏"，不需要精确的滑动平均。 */
  if ((now - s_err_window_tick) >= INDEV_ERR_WINDOW_MS)
  {
    s_err_window_tick  = now;
    s_err_window_count = 0u;
  }
  s_last.errors_window = s_err_window_count;

  rc = bsp_cst816s_read(&touch);

  /**
   * 失败现场和累计计数**每帧都取一次**，不管这次读成功还是失败。
   *
   * 关键在"成功时也要取"：bus_stuck 统计的是"总线被占住过多少次"，
   * 而"被占住 -> 打 9 个时钟救回来 -> 这次读成功"恰恰是它最常见的
   * 形态。只在失败分支里取的话这个数永远是 0，而它正是判断
   * "有没有东西在物理上拉着线"的唯一依据。
   *
   * 另外，这里不再像以前那样"只记一串失败的头一帧"：那套是给外设用的
   * —— 外设卡住之后会一直刷同一个状态，只有头一帧有信息量。
   * 软件模拟的每一次传输都是独立的，最近一次就是现在正在发生的。
   */
  bsp_cst816s_get_fault(&s_last.fault);

  if (rc != BSP_CST816S_OK)
  {
    /* 读失败就当作"没触摸"，而不是保留上一次的状态。
       否则一次偶发的 I2C 错误会让 LVGL 以为手指一直按着，
       表现成按钮被按住不放、界面卡在按下态。 */
    s_last.errors++;
    s_err_window_count++;
    s_last.errors_window = s_err_window_count;
    s_last.pressed = 0u;
    data->state = LV_INDEV_STATE_RELEASED;
    s_last.read_ok = 0u;

    /**
     * 把"芯片说的话"也一并清掉。
     *
     * 不清的话它们会保持上一次成功读到的值，界面上就会出现
     *   rc 01 01 80 86 00 AE   （第 2 字节 = 1，有手指）
     *   ->  134, 174  UP       （状态却是抬起）
     * 这种自相矛盾的组合 —— 看着像芯片报了个怪状态，
     * 其实是上一次读的残值。
     *
     * 这个坑我自己踩过：照着一张这样的照片去查"芯片为什么不报手指"，
     * 查错了方向。显示成 0（界面上渲染成 --）比显示过期的真值有用。
     */
    s_last.raw_x   = 0u;
    s_last.raw_y   = 0u;
    s_last.gesture = 0u;
    for (uint8_t i = 0u; i < sizeof(s_last.raw); i++)
    {
      s_last.raw[i] = 0u;
    }

    return;
  }

  s_last.read_ok = 1u;
  s_last.raw_x   = touch.x;
  s_last.raw_y   = touch.y;
  s_last.gesture = touch.gesture;
  s_last.pressed = touch.pressed;

  for (uint8_t i = 0u; i < sizeof(s_last.raw); i++)
  {
    s_last.raw[i] = touch.raw[i];
  }

  indev_transform(touch.x, touch.y, &s_last.x, &s_last.y);

  if (touch.pressed != 0u)
  {
    data->point.x = s_last.x;
    data->point.y = s_last.y;
    data->state   = LV_INDEV_STATE_PRESSED;
  }
  else
  {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

/* Exported functions --------------------------------------------------------*/

void lv_port_indev_init(void)
{
  /* 触摸芯片的初始化。失败也继续往下走 ——
     屏幕已经能显示了，让用户看到"触摸初始化失败 + 错误码"
     比直接卡在 Error_Handler 里有用得多。 */
  s_last.init_rc = bsp_cst816s_init();

  /* 把"关自动休眠"那次写的结果也带上来。
     它不是 OK 的话触摸一定会过一会儿就停，那就没必要查别的了。 */
  s_last.autosleep_rc = bsp_cst816s_autosleep_rc();

  s_indev = lv_indev_create();

  if (s_indev == NULL)
  {
    /* LVGL 堆不够，或者 lv_init() 没调 */
    return;
  }

  lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(s_indev, indev_read_cb);

  /* 不给它绑显示器。只有接了物理键盘 / 编码器这类"焦点型"输入设备
     才需要 lv_indev_set_display()，指针型输入跟的是坐标，
     绑不绑都能正常工作。 */
}

void lv_port_indev_get_last(lv_port_indev_sample_t *out)
{
  if (out == NULL)
  {
    return;
  }

  *out = s_last;
}
