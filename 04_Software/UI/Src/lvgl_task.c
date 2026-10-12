/**
  ******************************************************************************
  * @file    lvgl_task.c
  * @brief   LVGL 驱动任务：循环调 lv_timer_handler()。
  *
  *          与源工程裸机主循环等价：拿 lv_timer_handler() 的返回值决定
  *          睡多久，最长 10ms，绝不长睡（否则动画一顿一顿）。
  *          栈给 2KB —— lv_timer_handler 的调用链（渲染+flush）不浅。
  ******************************************************************************
  */
#include "lvgl_task.h"

#include "lvgl.h"
#include "cmsis_os2.h"
#include "usart.h"   /* [DBG] huart1: 任务启动打点(排错完可删) */

static void lvgl_loop(void *argument)
{
  static const char run_tag[] = "[LVGL] task run\r\n";
  (void)argument;
  (void)HAL_UART_Transmit(&huart1, (const uint8_t *)run_tag,
                          (uint16_t)(sizeof(run_tag) - 1U), 50U);
  for (;;)
  {
    uint32_t delay_ms = lv_timer_handler();

    if (delay_ms > 10u)
    {
      delay_ms = 10u;
    }
    osDelay((delay_ms == 0u) ? 1u : delay_ms);
  }
}

void lvgl_task_start(void)
{
  static const osThreadAttr_t attr = {
    .name       = "lvglTask",
    .stack_size = 2048u,
    .priority   = osPriorityNormal,
  };
  (void)osThreadNew(lvgl_loop, NULL, &attr);
}
