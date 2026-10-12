/**
  ******************************************************************************
  * @file    ui_test.h
  * @brief   LVGL 移植自检界面。
  *
  *          这个界面存在的意义是"一眼看出哪一层没通"，不是给人用的产品界面。
  *          以后接了 GUI Guider 生成的界面，把 main.c 里的 ui_test_init()
  *          换成 setup_screen(&guider_ui) 就行，这个文件可以整个删掉。
  ******************************************************************************
  */
#ifndef __UI_TEST_H__
#define __UI_TEST_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  画出自检界面并启动刷新定时器。
  *
  *         调用前提：lv_init() 和 lv_port_disp_init() 都已经执行过。
  */
void ui_test_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_TEST_H__ */
