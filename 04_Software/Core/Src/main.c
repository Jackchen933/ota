/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "cmsis_os.h"
#include "dma.h"
#include "i2c.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "OtaFlag.h"
/* ---- 022 屏幕界面移植（只增不改） ---- */
#include "bsp_st7789_port.h"   /* 屏幕 BSP：bsp_st7789_port_init() */
#include "bsp_st7789.h"        /* ST7789_BLACK */
#include "screen_hw.h"         /* SPI2/DMA/GPIO 上电（屏幕专用，不占 SPI1） */
#include "bsp_i2c1_bus.h"      /* I2C1 总线锁（触摸与 EEPROM 共享总线） */
#include "lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"     /* lv_port_indev_init() —— 触摸 */
#include "gui_guider.h"        /* guider_ui 全局实例 */
#include "gg_utils.h"          /* setup_ui() —— 建界面并显示 */
#include "custom.h"            /* custom_init() —— 用户逻辑 */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* GUI Guider 界面全局实例（gui_guider.h 里是 extern 声明，定义在这） */
gg_ui_t guider_ui;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* [DBG] 临时排错打点: 每个初始化步骤往 USART1 打一行(115200),
   定位"屏幕黑/卡死在初始化"用, 排错完可整段删掉 */
static void dbg_step(const char *tag)
{
  uint16_t len = 0U;
  while (tag[len] != '\0') { len++; }
  (void)HAL_UART_Transmit(&huart1, (const uint8_t *)tag, len, 50U);
}

/**
  * @brief  App 自检: 新固件第一次上电(标志=0x22"验证新App")时跑,
  *         通过就置 0x33 让 Bootloader 把 A 区固件备份进 B 区
  * @retval 0: 自检通过
  * @note   自检内容: 能执行到这里 = 时钟/Flash/内核基本正常;
  *         再验一遍自己向量表的合法性(栈顶指向 RAM、复位向量落在
  *         App 区间且 bit0=1)。业务级的自检(外设探测等)在各自模块
  *         的 Init 里已经隐式做过。
  *
  *         自检【失败】时的处理是"什么都不做、不喂狗":
  *         Boot 跳过来前开了 IWDG(约8.2s), 不喂狗它自己会复位。
  *         复位后 Boot 检测到"标志0x22 + 看门狗复位"即判定新固件
  *         验证失败, 转回滚流程恢复 B 区旧固件。
  */
static uint8_t App_SelfCheck(void)
{
  const uint32_t *vtor = (const uint32_t *)0x0800C000UL;  /* App 向量表 */
  uint32_t sp = vtor[0];
  uint32_t pc = vtor[1];

  /* 栈顶必须指向 RAM(0x2000xxxx) */
  if ((sp & 0x2FFE0000UL) != 0x20000000UL)
  {
    return 1U;
  }

  /* 复位向量必须落在 App 区间 [0x0800C000, 0x08080000) 且 Thumb 位=1 */
  if ((pc < 0x0800C000UL) || (pc >= 0x08080000UL) || ((pc & 1UL) == 0UL))
  {
    return 1U;
  }

  return 0U;
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_SPI1_Init();
  MX_I2C1_Init();
  /* USER CODE BEGIN 2 */

  /* [关键] Boot 的 JumpToApp() 是 __disable_irq() 后直接跳过来的,
     而"跳转"不是复位, PRIMASK 会原样带进 App —— 不在这里重新开中断,
     SysTick 永远不触发, uwTick 不走, 第一处 HAL_Delay 就死循环。
     (Keil 直接下载调试时 PRIMASK 本来就是 0, 这行是给"Boot 跳转"用的) */
  __enable_irq();

  /* ===== 与 Bootloader 的升级交接(见流程图 App侧) =====
     Boot 每次上电按标志扇区分发, 这里是 App 视角的三个入口:
       0x22 验证新App -> 自检 -> 置0x33 -> 复位(Boot 备份 A->B 后跳回)
       0x44/其他      -> 正常运行, 但要接管 Boot 开的 IWDG 持续喂狗 */

  OtaFlag_Load();
  dbg_step("[APP] 1 mx-init done\r\n");

  /* Boot 开的 IWDG 复位后仍在倒计时, 8.2s 内不接管就复位。
     先喂一口, 给下面的自检/初始化买时间 */
  OTA_IWDG_FEED();

  if (OtaFlag_GetAppState() == OTA_FLAG_ST_VERIFY_NEW)
  {
    /* 新固件试用期: 自检通过 -> 置"A区搬运至B区" -> 复位,
       由 Boot 完成备份后置 0x44 再跳回来, 固件正式转正 */
    if (App_SelfCheck() == 0U)
    {
      (void)OtaFlag_SetAppState(OTA_FLAG_ST_AB_MOVE);
      HAL_NVIC_SystemReset();
      for (;;) { }      /* 复位不返回, 兜底 */
    }
    /* 自检失败: 故意不喂狗, 等 IWDG 复位后由 Boot 回滚旧固件 */
    for (;;) { }
  }

  /* ---- 022 屏幕界面移植（业务零改动，全部是追加） ----
     注意：这里在 osKernelInitialize() 之前，lv_init/建界面都没问题；
     LVGL 的循环任务在 freertos.c 的 MX_FREERTOS_Init() 里启动。 */

  /* 1. 屏幕专用硬件：SPI2 + DMA1_Stream4 + CS/DC/RESET。
        与业务的 SPI1(W25Q64) 无关，也不要动这里以外的 SPI 配置 */
  screen_hw_init();
  dbg_step("[APP] 2 screen-hw ok\r\n");

  /* 2. I2C1 总线互斥锁：触摸(CST816S)与 EEPROM(AT24C02) 共享总线，
        两个任务的事务都必须先拿锁（Eeprom.c 里已加） */
  bsp_i2c1_bus_init();
  dbg_step("[APP] 3 i2c-lock ok\r\n");

  /* 3. 屏幕 BSP + 清黑屏（顺带给排错留基准：纯黑=屏好，不亮=背光/复位/时钟） */
  {
    bsp_st7789_driver_t *lcd;
    dbg_step("[APP] 3b lcd-init start\r\n");   /* [DBG] 排错打点, 完了删 */
    lcd = bsp_st7789_port_init();
    if (lcd == NULL)
    {
      dbg_step("[ERR] port_init NULL\r\n");    /* [DBG] 排错打点, 完了删 */
      Error_Handler();
    }
    dbg_step("[APP] 4 lcd-init ok\r\n");
    (void)lcd->pf_fill_color(lcd, ST7789_BLACK);   /* 清黑屏: 第一次大块 DMA 传输 */

    /* 4. LVGL 内核 + 时间基准（不接 tick，动画/定时器全不动） */
    lv_init();
    lv_tick_set_cb(HAL_GetTick);

    /* 5. 显示桥 + 触摸桥。触摸 init 失败不卡程序：屏照常显示，只是没触摸 */
    lv_port_disp_init();
    lv_port_indev_init();
    dbg_step("[APP] 5 lvgl-port ok\r\n");

    /* 6. GUI Guider 022 界面 */
    setup_ui(&guider_ui);
    custom_init(&guider_ui);
    dbg_step("[APP] 6 ui-built ok\r\n");
  }

  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in freertos.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  dbg_step("[APP] 7 kernel-start\r\n");
  osKernelStart();
  /* We should never get here as control is now taken by the scheduler */
  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 100;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  dbg_step("[ERR] Error_Handler!\r\n");   /* [DBG] 排错打点, 完了删 */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
