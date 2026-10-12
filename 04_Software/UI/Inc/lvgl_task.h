/**
  ******************************************************************************
  * @file    lvgl_task.h
  * @brief   LVGL 主循环任务。源工程是裸机主循环里调 lv_timer_handler()，
  *          本工程有 FreeRTOS，改由独立任务驱动，业务代码零改动。
  ******************************************************************************
  */
#ifndef __LVGL_TASK_H__
#define __LVGL_TASK_H__

#ifdef __cplusplus
extern "C" {
#endif

void lvgl_task_start(void);   /* 在 osKernelInitialize() 之后、osKernelStart() 前调用 */

#ifdef __cplusplus
}
#endif

#endif /* __LVGL_TASK_H__ */
