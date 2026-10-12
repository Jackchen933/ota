/**
  ******************************************************************************
  * @file    lv_port_indev.h
  * @brief   LVGL 输入设备（触摸）适配层声明。
  ******************************************************************************
  */
#ifndef __LV_PORT_INDEV_H__
#define __LV_PORT_INDEV_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "bsp_cst816s.h"   /* bsp_cst816s_fault_t，采样记录里要放一份失败现场 */
#include <stdint.h>

/**
  * @brief  把 CST816S 接到 LVGL 上。
  *
  *         内部会自己调 bsp_cst816s_init()，所以调用前只需要完成
  *         MX_I2C1_Init() 和 lv_init()。
  *         这和 lv_port_disp_init() 自己去取 BSP 实例的做法是一致的 ——
  *         上层不用关心 BSP 的初始化顺序。
  *
  *         触摸初始化失败不会卡住程序，屏幕照常显示，
  *         失败原因记录在 lv_port_indev_get_last()->init_rc 里，
  *         自检界面会把它显示出来。
  */
void lv_port_indev_init(void);

/**
  * @brief 一次采样的完整记录，给自检界面用的。
  *
  *        raw_* 是芯片直接吐出来的值，x/y 是经过方向变换之后、
  *        LVGL 实际拿到的值。两个放在一起看，就能一眼分辨问题出在哪：
  *
  *          raw 就不对（超出 0~239 / 0~279）-> 驱动或接线的问题
  *          raw 对但 x/y 不对                 -> 方向变换的问题，
  *                                               改 lv_port_indev.c 顶部的宏
  */
typedef struct
{
  int       init_rc;    /**< bsp_cst816s_init() 的返回值，BSP_CST816S_OK = 正常 */
  uint16_t  raw_x;      /**< 芯片原始 X */
  uint16_t  raw_y;      /**< 芯片原始 Y */
  lv_coord_t x;         /**< 变换后 X，LVGL 用的就是这个 */
  lv_coord_t y;         /**< 变换后 Y */
  uint8_t   pressed;    /**< 1 = 有手指 */
  uint8_t   gesture;    /**< 手势码 */
  uint32_t  samples;    /**< read_cb 被调用过多少次 */
  uint32_t  errors;     /**< I2C 读失败过多少次（累计） */

  /**
    * @brief 最近一次读成功了没有。
    *
    *        【读失败时下面那些字段是什么】
    *        读失败时 raw[] / raw_x / raw_y / gesture 全部**被清零**，
    *        而不是保留上一次成功读到的值。界面上显示成 0（渲染成 --）。
    *
    *        为什么不保留残值：保留的话，"原始字节"和"读失败"这两件事
    *        会混在一起，一次失败会显示成
    *        "rc 01 01 80 86 00 AE" + "UP" 这种自相矛盾的组合
    *        （第 2 个字节说有手指，状态却是抬起），看起来像芯片
    *        给了个怪状态，其实是过期数据。照着一张这样的照片去查
    *        "芯片为什么不报手指"，会把排查方向带偏。
    *
    *        所以看 raw[] 之前先看这个标志：它是 0 的话，下面全是 0，
    *        那不是"芯片说没有手指"，而是"这一帧根本没读到"。
    *        两者的区别正是 errors_window 存在的意义。
    */
  uint8_t   read_ok;

  /**
    * @brief 最近 5 秒内的读失败次数。
    *
    *        errors 是累计值，分不出"以前失败过"和"现在正在失败"。
    *        卡死的时候要看的是这个数：它在涨 = 此刻总线是坏的，
    *        它不动 = 此刻 I2C 是好的，卡死另有原因。
    */
  uint32_t  errors_window;

  /**
    * @brief 芯片原始 6 字节。
    *
    *        触摸"莫名其妙不动了"的时候，这是唯一能定性的东西：
    *          raw[1] == 0 且一直是 0  -> 芯片在说"没手指"，不是驱动算错
    *       不看这几个字节，就只能对着算出来的 x/y 猜。
    *
    *        ⚠️ 只在 read_ok != 0 时有效。读失败时这里是全 0。
    */
  uint8_t   raw[6];

  /**
    * @brief 最近一次失败的现场 + 累计的总线异常计数。
    *
    *        字段含义见 bsp_cst816s_fault_t。这里只强调一件事：
    *        **这个结构每帧都要取一次，不管这次读成功还是失败。**
    *
    *        phase / scl_level / sda_level 是"最近一次读失败"的现场，
    *        而 bus_stuck / unwedge_failed 是累计计数器 —— 后者在
    *        读**成功**的时候也会变（总线被占住、但打时钟救回来了，
    *        这一次就算成功）。只在失败时取的话，这两个最有价值的数
    *        永远显示不出来。
    */
  bsp_cst816s_fault_t fault;

  /** @brief 发过多少次"关自动休眠"的保活写，以及其中失败多少次 */
  uint32_t  keepalives;
  uint32_t  keepalive_errors;

  /**
    * @brief init 时那次"关自动休眠"的结果，BSP_CST816S_OK = 写进去了。
    *
    *        这个值不是 OK 的话，芯片一定会睡，触摸必然过一会儿就停 ——
    *        那就不用查别的了，先修 I2C。
    */
  int       autosleep_rc;
} lv_port_indev_sample_t;

/**
  * @brief  取最近一次采样。
  *
  *         只在自检界面里用。正常业务代码不需要调它。
  *
  * @param  out 输出，不能为空
  */
void lv_port_indev_get_last(lv_port_indev_sample_t *out);

#ifdef __cplusplus
}
#endif

#endif /* __LV_PORT_INDEV_H__ */
