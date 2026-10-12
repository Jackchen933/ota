/**
  ******************************************************************************
  * @file    bsp_st7789.c
  * @brief   ST7789 / ST7789V 液晶驱动本体（平台无关）。
  *
  *          本文件只通过 basic_op_driver_interface_t / timebase / yield 三个
  *          接口与外界打交道，不包含任何 HAL 头文件。换平台时不用动这里。
  *
  *          关于 st7789 的一个基本事实：它无法区分收到的 spi 信号是数据还是
  *          命令，必须靠外部 GPIO（DC 脚）来标记：
  *              DC = 0  ->  命令
  *              DC = 1  ->  数据
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "bsp_st7789.h"

#include <stddef.h>

/* Private define ------------------------------------------------------------*/

/* MADCTL(0x36) 的值。0x00 = 竖屏、RGB 顺序；
   如果红蓝反了（该显红却显蓝），把它改成 0x08（BGR）。
   横屏需要改的是 0x60 / 0x70 等，本轮未实现 set_direction。 */
#define ST7789_MADCTL_DATA        0x00u

/* 像素格式：0x05 = 16bit/pixel (RGB565) */
#define ST7789_COLMOD_DATA        0x05u

/* 是否在初始化末尾发 INVON(0x21)。
   绝大多数 ST7789 模块需要打开反显，否则颜色会整体反相。
   如果你看到的是"反色"画面（黑底变白底），把这个宏改成 0。 */
#define ST7789_CMD_INVON_ENABLE   1

/* 是否对 RGB565 做字节交换。
   ST7789 要求高字节先发，而 Cortex-M 是小端：0xF800(红) 在内存里是 00 F8，
   直接按字节 DMA 出去会变成 0x00F8，颜色全错。必须交换。
   只有在你的屏确实按小端接收时才改成 0。 */
#define ST7789_SWAP_COLOR_BYTES   1

#if ST7789_SWAP_COLOR_BYTES
#define ST7789_COLOR16(c)  ((uint16_t)((((uint16_t)(c) >> 8) & 0x00FFu) | \
                                       (((uint16_t)(c) << 8) & 0xFF00u)))
#else
#define ST7789_COLOR16(c)  ((uint16_t)(c))
#endif

/* Private variables ---------------------------------------------------------*/

/* 刷屏数据缓冲区（条带缓冲）。
   240 * 8 * 2 = 3840 字节，放在 .bss。
   （接了 LVGL 之后刷屏归 LVGL 的显示缓冲管，这里只留给 pf_fill 用，
   所以高度压到了 8 —— 改动理由见 bsp_st7789.h 的 ST7789_HOR_LEN。） */
static uint16_t disp_buf[ST7789_WIDTH * ST7789_HOR_LEN];

/* 未实现功能被调用时记录的出错行号，方便在调试器 Watch 窗口里看 */
volatile uint32_t g_st7789_unimplemented_line = 0u;

/* 驱动实例（静态分配，避免堆） */
static bsp_st7789_driver_t s_st7789_driver;
static st7789_spi_driver_interface_t s_st7789_spi_if;
static uint8_t s_st7789_created = 0u;

/* Private function prototypes -----------------------------------------------*/

/* --- spi 层：命令 / 数据 --- */
static uint8_t st7789_write_command(void * const driver_instance,
                                    const uint8_t *pCmd, uint32_t cmd_length);
static uint8_t st7789_write_simple_data(void * const driver_instance,
                                        const uint8_t *pData, uint32_t data_length);
static uint8_t st7789_write_data(void * const driver_instance,
                                 const uint8_t *pData, uint32_t data_length);

/* --- basic 层 --- */
static uint8_t st7789_init(void * const st7789_instance);
static uint8_t st7789_fill_color(void * const st7789_instance, uint16_t color);
static uint8_t st7789_draw_pixel(void * const st7789_instance,
                                 uint16_t x, uint16_t y, uint16_t color);
static uint8_t st7789_fill(void * const st7789_instance,
                           uint16_t xSta, uint16_t ySta,
                           uint16_t xEnd, uint16_t yEnd, uint16_t color);
static uint8_t st7789_set_addr_window(void * const st7789_instance,
                                      uint16_t x0, uint16_t y0,
                                      uint16_t x1, uint16_t y1);

/* --- 内部辅助 --- */
static uint8_t st7789_write_cmd_with_data(void * const driver_instance,
                                          uint8_t cmd,
                                          const uint8_t *pData, uint32_t data_length);
static void    st7789_fill_buffer(uint16_t *pBuf, uint32_t pixels, uint16_t color16);

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  未实现功能的兜底处理。
  *         停在死循环里，方便在调试器里看 g_st7789_unimplemented_line。
  */
void bsp_st7789_unimplemented_handler(uint32_t line)
{
  g_st7789_unimplemented_line = line;
  while (1)
  {
  }
}

/**
  * @brief  向 st7789 发送命令。DC = 0。
  */
static uint8_t st7789_write_command(void * const driver_instance,
                                    const uint8_t *pCmd, uint32_t cmd_length)
{
  bsp_st7789_driver_t * const p = (bsp_st7789_driver_t *)driver_instance;
  basic_op_driver_interface_t * const op = p->p_st7789_basic_operation;
  uint32_t remain = cmd_length;
  const uint8_t *src = pCmd;
  uint8_t ret = 0u;

  if ((pCmd == NULL) || (cmd_length == 0u))
  {
    return 1u;
  }

  op->pf_write_cs_pin(ST7789_PIN_RESET);      /* chip select   */
  op->pf_write_dc_pin(ST7789_PIN_RESET);      /* dc = 0 -> 命令 */

  while (remain > 0u)
  {
    uint32_t chunk = (remain > (uint32_t)ST7789_DMA_MAX_CHUNK)
                     ? (uint32_t)ST7789_DMA_MAX_CHUNK : remain;
    ret = op->pf_spi_transmit(src, chunk);
    if (ret != 0u)
    {
      break;
    }
    src += chunk;
    remain -= chunk;
  }

  op->pf_write_cs_pin(ST7789_PIN_SET);        /* chip unselect */
  return ret;
}

/**
  * @brief  发送少量数据。DC = 1，polling 方式。
  *         地址窗口这类只有几字节的数据走这里，开 DMA 反而更慢。
  */
static uint8_t st7789_write_simple_data(void * const driver_instance,
                                        const uint8_t *pData, uint32_t data_length)
{
  bsp_st7789_driver_t * const p = (bsp_st7789_driver_t *)driver_instance;
  basic_op_driver_interface_t * const op = p->p_st7789_basic_operation;
  uint32_t remain = data_length;
  const uint8_t *src = pData;
  uint8_t ret = 0u;

  if ((pData == NULL) || (data_length == 0u))
  {
    return 1u;
  }

  op->pf_write_cs_pin(ST7789_PIN_RESET);      /* chip select   */
  op->pf_write_dc_pin(ST7789_PIN_SET);        /* dc = 1 -> 数据 */

  while (remain > 0u)
  {
    uint32_t chunk = (remain > (uint32_t)ST7789_DMA_MAX_CHUNK)
                     ? (uint32_t)ST7789_DMA_MAX_CHUNK : remain;
    ret = op->pf_spi_transmit(src, chunk);
    if (ret != 0u)
    {
      break;
    }
    src += chunk;
    remain -= chunk;
  }

  op->pf_write_cs_pin(ST7789_PIN_SET);        /* chip unselect */
  return ret;
}

/**
  * @brief  发送大批量刷屏数据。DC = 1。
  *
  *         DMA 一次最多发 65535 字节，超过就要分块；
  *         小于 ST7789_DMA_MIN_SIZE 字节时，开 DMA 的开销比 CPU 直接发还大，
  *         所以降级成 polling。
  */
static uint8_t st7789_write_data(void * const driver_instance,
                                 const uint8_t *pData, uint32_t data_length)
{
  bsp_st7789_driver_t * const p = (bsp_st7789_driver_t *)driver_instance;
  basic_op_driver_interface_t * const op = p->p_st7789_basic_operation;
  uint32_t remain = data_length;
  const uint8_t *src = pData;
  uint8_t ret = 0u;

  if ((pData == NULL) || (data_length == 0u))
  {
    return 1u;
  }

  op->pf_write_cs_pin(ST7789_PIN_RESET);      /* chip select   */
  op->pf_write_dc_pin(ST7789_PIN_SET);        /* dc = 1 -> 数据 */

  while (remain > 0u)
  {
    uint32_t chunk = (remain > (uint32_t)ST7789_DMA_MAX_CHUNK)
                     ? (uint32_t)ST7789_DMA_MAX_CHUNK : remain;

    if (chunk >= (uint32_t)ST7789_DMA_MIN_SIZE)
    {
      ret = op->pf_spi_transmit_dma(src, chunk);
    }
    else
    {
      ret = op->pf_spi_transmit(src, chunk);
    }

    if (ret != 0u)
    {
      break;
    }

    src += chunk;
    remain -= chunk;
  }

  op->pf_write_cs_pin(ST7789_PIN_SET);        /* chip unselect */
  return ret;
}

/**
  * @brief  发一条命令 + 若干参数。
  */
static uint8_t st7789_write_cmd_with_data(void * const driver_instance,
                                          uint8_t cmd,
                                          const uint8_t *pData, uint32_t data_length)
{
  uint8_t ret = st7789_write_command(driver_instance, &cmd, 1u);

  if ((ret == 0u) && (pData != NULL) && (data_length > 0u))
  {
    ret = st7789_write_simple_data(driver_instance, pData, data_length);
  }

  return ret;
}

/**
  * @brief  把整个缓冲区填充成同一个颜色（已完成字节交换）。
  */
static void st7789_fill_buffer(uint16_t *pBuf, uint32_t pixels, uint16_t color16)
{
  uint32_t i;

  for (i = 0u; i < pixels; i++)
  {
    pBuf[i] = color16;
  }
}

/**
  * @brief  指定刷屏范围（列地址 + 行地址 + 写 RAM）。
  *         之后的写数据都会落在这个窗口内，超出窗口会自动回卷到窗口起点，
  *         并且行优先递增——这正是条带刷屏能跑通的前提。
  */
static uint8_t st7789_set_addr_window(void * const st7789_instance,
                                      uint16_t x0, uint16_t y0,
                                      uint16_t x1, uint16_t y1)
{
  uint8_t buf[4];

  /* 加上面板在 GRAM 中的偏移 */
  x0 = (uint16_t)(x0 + ST7789_X_SHIFT);
  x1 = (uint16_t)(x1 + ST7789_X_SHIFT);
  y0 = (uint16_t)(y0 + ST7789_Y_SHIFT);
  y1 = (uint16_t)(y1 + ST7789_Y_SHIFT);

  /* 列地址设置 0x2A */
  buf[0] = (uint8_t)(x0 >> 8);
  buf[1] = (uint8_t)(x0 & 0xFFu);
  buf[2] = (uint8_t)(x1 >> 8);
  buf[3] = (uint8_t)(x1 & 0xFFu);
  if (st7789_write_cmd_with_data(st7789_instance, 0x2Au, buf, 4u) != 0u)
  {
    return 1u;
  }

  /* 行地址设置 0x2B */
  buf[0] = (uint8_t)(y0 >> 8);
  buf[1] = (uint8_t)(y0 & 0xFFu);
  buf[2] = (uint8_t)(y1 >> 8);
  buf[3] = (uint8_t)(y1 & 0xFFu);
  if (st7789_write_cmd_with_data(st7789_instance, 0x2Bu, buf, 4u) != 0u)
  {
    return 1u;
  }

  /* 写 RAM 0x2C */
  if (st7789_write_cmd_with_data(st7789_instance, 0x2Cu, NULL, 0u) != 0u)
  {
    return 1u;
  }

  return 0u;
}

/**
  * @brief  全屏刷指定颜色。
  *         没有整屏 framebuffer，所以用条带缓冲循环多次发出去。
  */
static uint8_t st7789_fill_color(void * const st7789_instance, uint16_t color)
{
  bsp_st7789_driver_t * const p = (bsp_st7789_driver_t *)st7789_instance;
  const uint32_t strip_bytes = (uint32_t)ST7789_WIDTH * (uint32_t)ST7789_HOR_LEN * 2u;
  uint32_t remain = (uint32_t)ST7789_WIDTH * (uint32_t)ST7789_HEIGHT * 2u;
  uint16_t color16 = ST7789_COLOR16(color);

  st7789_fill_buffer(disp_buf, (uint32_t)ST7789_WIDTH * (uint32_t)ST7789_HOR_LEN, color16);

  if (st7789_set_addr_window(st7789_instance, 0u, 0u,
                             (uint16_t)(ST7789_WIDTH - 1u),
                             (uint16_t)(ST7789_HEIGHT - 1u)) != 0u)
  {
    return 1u;
  }

  while (remain > 0u)
  {
    uint32_t chunk = (remain > strip_bytes) ? strip_bytes : remain;

    if (p->p_st7789_spi_instance->pf_st7789_write_data(st7789_instance,
                                                       (const uint8_t *)disp_buf,
                                                       chunk) != 0u)
    {
      return 1u;
    }
    remain -= chunk;
  }

  return 0u;
}

/**
  * @brief  矩形区域填充（包含 xEnd / yEnd）。
  *         因为是连续地址窗口，同一颜色重复发送就能铺满整个矩形，
  *         所以可以按条带批量发，不必逐行调用。
  */
static uint8_t st7789_fill(void * const st7789_instance,
                           uint16_t xSta, uint16_t ySta,
                           uint16_t xEnd, uint16_t yEnd, uint16_t color)
{
  bsp_st7789_driver_t * const p = (bsp_st7789_driver_t *)st7789_instance;
  uint16_t w;
  uint16_t h;
  uint16_t tmp;
  uint32_t strip_bytes;
  uint32_t strip_pixels;
  uint32_t remain;
  uint16_t color16;

  if (xSta > xEnd) { tmp = xSta; xSta = xEnd; xEnd = tmp; }
  if (ySta > yEnd) { tmp = ySta; ySta = yEnd; yEnd = tmp; }

  if ((xEnd >= ST7789_WIDTH) || (yEnd >= ST7789_HEIGHT))
  {
    return 1u;
  }

  w = (uint16_t)(xEnd - xSta + 1u);
  h = (uint16_t)(yEnd - ySta + 1u);

  /* w <= ST7789_WIDTH，所以 w * ST7789_HOR_LEN 一定不超过 disp_buf 的容量 */
  strip_pixels = (uint32_t)w * (uint32_t)ST7789_HOR_LEN;
  strip_bytes  = strip_pixels * 2u;
  remain       = (uint32_t)w * (uint32_t)h * 2u;
  color16      = ST7789_COLOR16(color);

  st7789_fill_buffer(disp_buf, strip_pixels, color16);

  if (st7789_set_addr_window(st7789_instance, xSta, ySta, xEnd, yEnd) != 0u)
  {
    return 1u;
  }

  while (remain > 0u)
  {
    uint32_t chunk = (remain > strip_bytes) ? strip_bytes : remain;

    if (p->p_st7789_spi_instance->pf_st7789_write_data(st7789_instance,
                                                       (const uint8_t *)disp_buf,
                                                       chunk) != 0u)
    {
      return 1u;
    }
    remain -= chunk;
  }

  return 0u;
}

/**
  * @brief  单像素绘制。
  *         没有回读能力，所以直接把地址窗口缩到 1x1 再写两个字节。
  */
static uint8_t st7789_draw_pixel(void * const st7789_instance,
                                 uint16_t x, uint16_t y, uint16_t color)
{
  uint8_t buf[2];
  uint16_t color16 = ST7789_COLOR16(color);

  if ((x >= ST7789_WIDTH) || (y >= ST7789_HEIGHT))
  {
    return 1u;
  }

  if (st7789_set_addr_window(st7789_instance, x, y, x, y) != 0u)
  {
    return 1u;
  }

  buf[0] = (uint8_t)(color16 >> 8);
  buf[1] = (uint8_t)(color16 & 0xFFu);

  return st7789_write_simple_data(st7789_instance, buf, 2u);
}

/**
  * @brief  ST7789 初始化序列。
  *
  *         复位时序 + 帧率 / 电源 / Gamma 设置，最后开显示。
  *         这里用的是面板厂给的通用参数，绝大多数 240x320 模块可以直接跑。
  */
static uint8_t st7789_init(void * const st7789_instance)
{
  bsp_st7789_driver_t * const p = (bsp_st7789_driver_t *)st7789_instance;
  basic_op_driver_interface_t   * const op = p->p_st7789_basic_operation;
  st7789_timebase_interface_t   * const tb = p->p_timebase_instance;

  /* 负伽马（NVGAM）14 字节 */
  static const uint8_t nvgam[14] = {
    0xD0u, 0x02u, 0x08u, 0x0Du, 0x12u, 0x2Cu, 0x43u,
    0x55u, 0x53u, 0x1Eu, 0x1Bu, 0x19u, 0x20u, 0x22u
  };
  /* 正伽马（PVGAM）14 字节 */
  static const uint8_t pvgam[14] = {
    0xD0u, 0x03u, 0x09u, 0x0Eu, 0x11u, 0x3Du, 0x47u,
    0x55u, 0x53u, 0x1Au, 0x16u, 0x14u, 0x1Fu, 0x22u
  };
  static const uint8_t porch[5]  = { 0x0Cu, 0x0Cu, 0x00u, 0x33u, 0x33u };
  static const uint8_t pwr_d0[2] = { 0xA4u, 0xA1u };

  /*-------------------- 硬复位 --------------------*/
  op->pf_write_reset_pin(ST7789_PIN_SET);
  tb->pf_delay_no_os(10u);
  op->pf_write_reset_pin(ST7789_PIN_RESET);
  tb->pf_delay_no_os(10u);
  op->pf_write_reset_pin(ST7789_PIN_SET);
  tb->pf_delay_no_os(120u);

  /*-------------------- 软复位 --------------------*/
  if (st7789_write_cmd_with_data(st7789_instance, 0x01u, NULL, 0u) != 0u) { return 1u; }
  tb->pf_delay_no_os(150u);

  /*-------------------- 退出睡眠 -------------------*/
  if (st7789_write_cmd_with_data(st7789_instance, 0x11u, NULL, 0u) != 0u) { return 1u; }
  tb->pf_delay_no_os(120u);

  /*-------------------- Display Setting ----------*/
  {
    const uint8_t madctl = ST7789_MADCTL_DATA;
    if (st7789_write_cmd_with_data(st7789_instance, 0x36u, &madctl, 1u) != 0u) { return 1u; }
  }
  {
    const uint8_t colmod = ST7789_COLMOD_DATA;
    if (st7789_write_cmd_with_data(st7789_instance, 0x3Au, &colmod, 1u) != 0u) { return 1u; }
  }

  /*--------------- ST7789V Frame rate setting ----*/
  if (st7789_write_cmd_with_data(st7789_instance, 0xB2u, porch, 5u) != 0u) { return 1u; }
  {
    const uint8_t gate = 0x35u;
    if (st7789_write_cmd_with_data(st7789_instance, 0xB7u, &gate, 1u) != 0u) { return 1u; }
  }

  /*--------------- ST7789V Power setting ---------*/
  {
    const uint8_t vcom = 0x20u;
    if (st7789_write_cmd_with_data(st7789_instance, 0xBBu, &vcom, 1u) != 0u) { return 1u; }
  }
  {
    const uint8_t lcm = 0x2Cu;
    if (st7789_write_cmd_with_data(st7789_instance, 0xC0u, &lcm, 1u) != 0u) { return 1u; }
  }
  {
    const uint8_t vdvvrh = 0x01u;
    if (st7789_write_cmd_with_data(st7789_instance, 0xC2u, &vdvvrh, 1u) != 0u) { return 1u; }
  }
  {
    const uint8_t vrh = 0x0Bu;
    if (st7789_write_cmd_with_data(st7789_instance, 0xC3u, &vrh, 1u) != 0u) { return 1u; }
  }
  {
    const uint8_t vdv = 0x20u;
    if (st7789_write_cmd_with_data(st7789_instance, 0xC4u, &vdv, 1u) != 0u) { return 1u; }
  }
  {
    const uint8_t vcmoffset = 0x0Fu;
    if (st7789_write_cmd_with_data(st7789_instance, 0xC6u, &vcmoffset, 1u) != 0u) { return 1u; }
  }
  if (st7789_write_cmd_with_data(st7789_instance, 0xD0u, pwr_d0, 2u) != 0u) { return 1u; }

  /*--------------- ST7789V gamma setting ---------*/
  if (st7789_write_cmd_with_data(st7789_instance, 0xE0u, pvgam, 14u) != 0u) { return 1u; }
  if (st7789_write_cmd_with_data(st7789_instance, 0xE1u, nvgam, 14u) != 0u) { return 1u; }

  /*--------------- 反显 --------------------------*/
#if ST7789_CMD_INVON_ENABLE
  if (st7789_write_cmd_with_data(st7789_instance, 0x21u, NULL, 0u) != 0u) { return 1u; }
#else
  if (st7789_write_cmd_with_data(st7789_instance, 0x20u, NULL, 0u) != 0u) { return 1u; }
#endif

  /*--------------- 正常显示模式 ------------------*/
  if (st7789_write_cmd_with_data(st7789_instance, 0x13u, NULL, 0u) != 0u) { return 1u; }

  /*--------------- 开显示 ------------------------*/
  if (st7789_write_cmd_with_data(st7789_instance, 0x29u, NULL, 0u) != 0u) { return 1u; }
  tb->pf_delay_no_os(20u);

  return 0u;
}

/* Exported functions --------------------------------------------------------*/

bsp_st7789_driver_t *bsp_st7789_create(basic_op_driver_interface_t *basic_op,
                                       st7789_timebase_interface_t *timebase,
                                       st7789_yield_interface_t *yield)
{
  uint32_t i;
  uint8_t *raw;

  if ((basic_op == NULL) || (timebase == NULL) || (yield == NULL))
  {
    return NULL;
  }

  /* 先把实例整体清零，未实现的函数指针就都是 NULL 了 */
  raw = (uint8_t *)&s_st7789_driver;
  for (i = 0u; i < (uint32_t)sizeof(s_st7789_driver); i++)
  {
    raw[i] = 0u;
  }

  /* ---- 第 2 节：spi 层 ---- */
  s_st7789_spi_if.pf_st7789_write_data        = st7789_write_data;
  s_st7789_spi_if.pf_st7789_write_simple_data = st7789_write_simple_data;
  s_st7789_spi_if.pf_st7789_write_command     = st7789_write_command;

  /* ---- 第 3/4 节：挂上各层接口 ---- */
  s_st7789_driver.p_st7789_basic_operation = basic_op;
  s_st7789_driver.p_st7789_spi_instance    = &s_st7789_spi_if;
  s_st7789_driver.p_timebase_instance      = timebase;
  s_st7789_driver.p_yield_instance         = yield;

  /* ---- 本轮已实现的功能 ---- */
  s_st7789_driver.pf_st7789_init      = st7789_init;
  s_st7789_driver.pf_set_addr_window  = st7789_set_addr_window;
  s_st7789_driver.pf_fill_color       = st7789_fill_color;
  s_st7789_driver.pf_fill             = st7789_fill;
  s_st7789_driver.pf_draw_pixel       = st7789_draw_pixel;

  /* ---- 以下功能本轮未实现，保持 NULL ----
     pf_set_direction / pf_draw_pixel_4px / pf_draw_line / pf_draw_rectangle
     pf_draw_circle / pf_draw_image / pf_invert_colors / pf_write_char
     pf_write_string / pf_draw_filled_rectangle / pf_draw_triangle
     pf_draw_filled_triangle / pf_draw_filled_circle / pf_tear_effect
     pf_test_your_device
     调用前必须用 ST7789_ASSERT_PTR() 断言，否则会 HardFault。 */

  s_st7789_created = 1u;
  return &s_st7789_driver;
}

bsp_st7789_driver_t *bsp_st7789_get_instance(void)
{
  return (s_st7789_created != 0u) ? &s_st7789_driver : NULL;
}
