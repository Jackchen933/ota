/**
  ******************************************************************************
  * @file    lv_port_disp.c
  * @brief   LVGL <-> ST7789 显示桥实现。
  *
  *          数据流：
  *
  *            LVGL 渲染            flush_cb               BSP               屏幕
  *            ──────────►  s_disp_buf  ──────►  set_addr_window  ──►  SPI/DMA
  *                                              + write_data
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "lv_port_disp.h"
#include "bsp_st7789.h"

/* Private define ------------------------------------------------------------*/

/**
  * 显示缓冲的高度，单位：行。
  *
  * 【为什么不用整屏缓冲】
  *   240 * 280 * 2 = 134400 字节，超过 STM32F411CE 的 128KB RAM，放不下。
  *   所以用 LV_DISPLAY_RENDER_MODE_PARTIAL —— LVGL 把屏幕切成若干条带，
  *   每次只渲染一条带进这个缓冲，刷完再渲染下一条。
  *
  * 【为什么是单缓冲不是双缓冲】
  *   双缓冲的意义是"一块在 DMA 传输时，另一块同时在渲染"，前提是 flush 回调
  *   能立刻返回、由 DMA 完成中断去通知 LVGL。而 BSP 的 pf_st7789_write_data
  *   是阻塞实现（内部等 DMA 完成才返回），flush 回调没法提前返回，
  *   双缓冲只会白白多占一份内存，不会变快。
  *
  * 【取值权衡】
  *   缓冲越大，flush 次数越少（每次 flush 都要重发 0x2A/0x2B/0x2C 三条命令）。
  *   240*40*2 = 19200 字节，全屏 280 行要刷 7 次，命令开销可忽略。
  *   RAM 实在紧张可以调到 20 行，性能要求高可以调到 60 行。
  */
#define DISP_BUF_LINES   40

/* 单条带像素数 */
#define DISP_BUF_PIXELS  (LV_PORT_DISP_HOR_RES * DISP_BUF_LINES)
/* 单条带字节数。必须 <= ST7789_DMA_MAX_CHUNK(65535)，DMA 的 NDTR 是 16 位的 */
#define DISP_BUF_BYTES   (DISP_BUF_PIXELS * 2)

/* Private variables ---------------------------------------------------------*/

/**
  * @brief 显示缓冲。
  *
  *        声明成 uint32_t 数组而不是 uint8_t 数组，纯粹是为了拿到 4 字节对齐——
  *        LVGL 渲染时按 32bit 批量写这个缓冲，不对齐在 Cortex-M4 上虽然不会
  *        HardFault（M4 支持非对齐访问），但会明显变慢。
  *        用数组类型来保证对齐，就不必依赖 __attribute__ / __align 这类
  *        各编译器写法不一的修饰符。DISP_BUF_BYTES 必须是 4 的倍数。
  */
static uint32_t s_disp_buf_mem[DISP_BUF_BYTES / 4u];

/* BSP 驱动实例，lv_port_disp_init 里从 bsp_st7789_get_instance() 取 */
static bsp_st7789_driver_t *s_lcd = NULL;

/* Private function prototypes -----------------------------------------------*/
static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  LVGL 要求刷屏时回调这里。
  *
  * @param  disp    显示器对象
  * @param  area    要刷新的矩形区域（屏幕坐标，闭区间）
  * @param  px_map  已经渲染好的像素数据，RGB565，连续排布，
  *                 大小 = (x2-x1+1) * (y2-y1+1) * 2 字节
  */
static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
  uint32_t width;
  uint32_t height;
  uint32_t nbytes;

  /* 没拿到驱动实例就直接放行，否则 LVGL 会一直等 flush_ready 卡死 */
  if (s_lcd == NULL)
  {
    lv_display_flush_ready(disp);
    return;
  }

  width  = (uint32_t)(area->x2 - area->x1 + 1);
  height = (uint32_t)(area->y2 - area->y1 + 1);
  nbytes = width * height * 2u;

  /* 单次传输超过 DMA 上限就放弃这一帧，总比发出去一半花屏强 */
  if (nbytes > ST7789_DMA_MAX_CHUNK)
  {
    lv_display_flush_ready(disp);
    return;
  }

  /* 1. 告诉屏幕接下来这一块像素要画到哪 */
  (void)s_lcd->pf_set_addr_window(s_lcd,
                                  (uint16_t)area->x1, (uint16_t)area->y1,
                                  (uint16_t)area->x2, (uint16_t)area->y2);

  /* 2. 把像素灌进去。
        px_map 里的字节序已经是"高字节在前"了，这里不需要再做任何转换 ——
        原因见 lv_port_disp_init 里设置 LV_COLOR_FORMAT_RGB565_SWAPPED 的注释。 */
  (void)s_lcd->p_st7789_spi_instance->pf_st7789_write_data(
          s_lcd, px_map, nbytes);

  /* 3. 必须调，否则 LVGL 认为这块还没刷完，不会继续渲染下一块 */
  lv_display_flush_ready(disp);
}

/* Exported functions --------------------------------------------------------*/

/**
  * @brief  把 LVGL 接到 ST7789 上
  */
void lv_port_disp_init(void)
{
  lv_display_t *disp;

  s_lcd = bsp_st7789_get_instance();
  if (s_lcd == NULL)
  {
    /* 说明调用顺序错了：必须先 bsp_st7789_port_init() */
    return;
  }

  /* 1. 创建显示器对象 */
  disp = lv_display_create(LV_PORT_DISP_HOR_RES, LV_PORT_DISP_VER_RES);

  /**
   * 2. 指定颜色格式为 RGB565_SWAPPED —— 这一行是整个移植里最容易搞错的地方。
   *
   *    ST7789 要求每个像素**高字节先发**。
   *    Cortex-M 是小端：lv_color_hex(0xF800)（红）在内存里是 00 F8，
   *    如果照原样按字节 DMA 出去，屏幕收到的是 0x00F8，颜色全错。
   *
   *    设成 RGB565_SWAPPED 之后，LVGL 渲染时就直接把字节写成 F8 00，
   *    flush 回调拿到什么就发什么，不需要再逐像素转换（那会白白拖慢刷屏）。
   *
   *    如果忘了这一行、又在 flush 里也不做转换，现象是：
   *    颜色整体偏蓝绿、红蓝互换之类的"颜色不对但图形轮廓是对的"。
   */
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);

  /* 3. 挂缓冲。单缓冲 + PARTIAL 模式（第二个参数传 NULL 就是单缓冲）。
        注意这里的 size 单位是**字节**，不是像素数。 */
  lv_display_set_buffers(disp, (void *)s_disp_buf_mem, NULL,
                         sizeof(s_disp_buf_mem),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  /* 4. 挂刷屏回调 */
  lv_display_set_flush_cb(disp, disp_flush);

  /* 5. 设为默认显示器。不做这一步 lv_screen_active() 返回 NULL，
        后面 lv_obj_create(NULL) 创建屏幕会失败 */
  lv_display_set_default(disp);
}
