/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "OtaApp.h"
#include "OtaFlag.h"    /* OTA_IWDG_FEED: defaultTask 周期喂狗用 */
#include "lvgl_task.h"  /* 022 屏幕界面：LVGL 主循环任务 */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* ---- OTA 相关任务 ----
   线程分配(见设计文档):
     线程1 Default_taskHandle        —— 应用业务(呼吸灯)
     线程2 OTA_taskHandle            —— OTA 状态管理 + 串口收包
     线程3 DownloadAppData_taskHandle—— 把数据落盘到外部 W25Q64 */

/* OTA 线程：状态机 + 收串口，栈上只有 128 字节的 drain 缓冲，2KB 足够 */
#define OTA_TASK_STACK_WORDS        (1024U)

/* 下载线程：栈上要放 OtaFrame_t 和一个 4096 字节的擦写路径，
   但真正的大缓冲(buffer1/buffer2)是模块内的静态区，不在栈上。
   留 2KB 余量给 W25q64 的调用层级 */
#define OTA_DL_TASK_STACK_WORDS     (1024U)
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
/* 串口数据由 OtaApp.c 内部的 DMA 接收区 + 流缓冲承载，这里不另开缓冲。
   中断回调 HAL_UARTEx_RxEventCallback 直接把数据交给 OtaApp（见文件末尾）。 */
/* USER CODE END Variables */

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Definitions for ota_task */
osThreadId_t ota_taskHandle;
const osThreadAttr_t ota_task_attributes = {
  .name = "ota_task",
  .stack_size = OTA_TASK_STACK_WORDS * 4,
  /* 状态机要跟上串口节奏，给 Normal。和 defaultTask 同优先级，
     靠 xStreamBufferReceive 的阻塞让出 CPU，不会互相饿死 */
  .priority = (osPriority_t) osPriorityNormal,
};

/* Definitions for DownloadAppData_task */
osThreadId_t DownloadAppData_taskHandle;
const osThreadAttr_t DownloadAppData_task_attributes = {
  .name = "DownloadAppData_task",
  .stack_size = OTA_DL_TASK_STACK_WORDS * 4,
  /* 刻意【低于】OTA 线程一个优先级。
     写一个 4KB 扇区要 130ms(W25Q64 擦 45ms + 写 84ms)，这期间
     如果它比 OTA 线程优先级高，OTA 线程就没机会把串口数据从流缓冲里取走，
     4KB 的流缓冲会被上游灌满并丢数据。
     让它低一级，OTA 线程随时能抢占它去收数据，写 Flash 慢一点没关系。 */
  .priority = (osPriority_t) osPriorityBelowNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* OTA 模块的内核对象(流缓冲/消息队列/信号量)统一在 OtaApp_Init() 里建。
     流缓冲和双缓冲都是静态分配的，不占 FreeRTOS 堆 ——
     configTOTAL_HEAP_SIZE 只有 15KB，8KB 的双缓冲走动态分配会把任务栈挤没 */
  if (OtaApp_Init() != 0)
  {
    Error_Handler();
  }
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  ota_taskHandle = osThreadNew(OtaApp_Task, NULL, &ota_task_attributes);
  if (ota_taskHandle == NULL)
  {
    Error_Handler();
  }

  DownloadAppData_taskHandle = osThreadNew(OtaApp_DownloadTask, NULL,
                                           &DownloadAppData_task_attributes);
  if (DownloadAppData_taskHandle == NULL)
  {
    Error_Handler();
  }

  /* ---- 022 屏幕界面移植：LVGL 主循环任务（业务零改动，只追加） ----
     main.c 的 USER CODE 2 里已完成 lv_init/显示桥/触摸桥/建界面，
     这里只负责启动循环任务（在 osKernelInitialize 之后，合法） */
  lvgl_task_start();
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  (void)argument;

  /* 应用业务线程。OTA 全程在后台跑，这条线程不受影响 ——
     这就是"后台无感下载"要的效果：升级期间设备照常工作 */
  for(;;)
  {
    HAL_GPIO_TogglePin(Led_GPIO_Port, Led_Pin);

    /* 接管 Boot 开的 IWDG(8.2s 超时, 关不掉): 每 500ms 喂一次,
       即使本线程被高优先级任务抢占 ~7s 也不会被狗咬 */
    OTA_IWDG_FEED();

    osDelay(500);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
/**
  * @brief  串口接收事件回调（DMA 半满/全满 或 IDLE 空闲中断触发）
  * @param  huart: 串口句柄
  * @param  Size : 本次实际收到的字节数
  * @retval None
  * @note   在 USART1 中断上下文里执行，只能调带 FromISR 后缀的 FreeRTOS API。
  *         前提是 USART1 的抢占优先级(6)数值上大于
  *         configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY(5) —— 否则调
  *         xStreamBufferSendFromISR 会命中 configASSERT 直接死机。
  *         优先级在 usart.c 的 MX_USART1_UART_Init 里设置，改动需同步 .ioc。
  */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
  if (huart->Instance != USART1)
  {
    return;
  }

  /* 一帧收完，DMA 已被 HAL 停掉、RxState 已回到 READY。
     先清 IDLE 标志：它是"总线空闲"标志，不清的话只要总线一直空闲，
     中断就会反复触发 */
  __HAL_UART_CLEAR_IDLEFLAG(huart);

  /* 数据是【拷贝】进流缓冲的，本函数返回后 DMA 接收区就空了，
     可以立刻重挂下一次接收，任务那边读的是流缓冲，不抢同一块内存 */
  OtaApp_OnRxEvent(Size);
}

/**
  * @brief  串口错误回调：出错后接收会停摆，交给任务重新挂起
  */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance != USART1)
  {
    return;
  }

  /* 只置标志，重挂接收交给任务做。
     原因：HAL_DMA_Abort 内部是"关流 + 轮询等待"的阻塞实现，
     在中断里做会长时间占着 CPU，还可能等不到 EN 位清零而超时。 */
  OtaApp_RxError();
}
/* USER CODE END Application */
