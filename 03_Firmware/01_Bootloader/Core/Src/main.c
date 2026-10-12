/**
  ******************************************************************************
	WeAct 微行创新
	>> 标准库实例例程
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "tim.h"
#include "gpio.h"
#include "Debug.h"
#include "usart.h"
#include "elog.h"
#include "ymodem.h"
#include "common.h"   /* [DBG] Serial_PutString: 串口打点(排错完可删) */
#include "Boot_Manager.h"
#include "FlagSector.h"
#include "Eeprom.h"
#include "Spi.h"        /* g_msTick 毫秒时基(Spi.c定义) */
// 全局定义 STM32F411xE 或者 STM32F401xx
// 当前定义 STM32F411xE

// STM32F411 外部晶振25Mhz，考虑到USB使用，内部频率设置为96Mhz
// 需要100mhz,自行修改system_stm32f4xx.c

/** @addtogroup Template_Project
  * @{
  */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
static __IO uint32_t uwTimingDelay;
RCC_ClocksTypeDef RCC_Clocks;

/* Private function prototypes -----------------------------------------------*/

/* Private functions ---------------------------------------------------------*/
 /*
  *power by WeAct Studio
  *The board with `WeAct` Logo && `version number` is our board, quality guarantee.
  *For more information please visit: https://github.com/WeActTC/MiniF4-STM32F4x1
  *更多信息请访问：https://gitee.com/WeActTC/MiniF4-STM32F4x1
  */
/**
  * @brief  Bootloader 主线状态机(按《Bootloader主线流程》流程图)
  *
  *   上电 -> 读标志扇区(0x08008000, 扇区2)的App运行状态 -> 五路分发:
  *     App下载固件待更新(0x11) -> 子流程A: 解密搬运A区->MD5->置"验证新App"->复位
  *     验证新App(0x22)          -> 子流程B: 开IWDG->跳转App
  *     有效App(0x44)            -> 直接跳转App(正常启动)
  *     A区搬运至B区(0x33)       -> 子流程E: A区密文备份到B区->置"有效App"->复位
  *     其他/空白                -> 路径D: 备份有效则回滚C, 否则待机等按键Ymodem下载
  *
  * @param  None
  * @retval None
  */
int main(void)
{
  uint32_t app_state;
  uint8_t  wdg_reset;

	/* Enable Clock Security System(CSS): this will generate an NMI exception
     when HSE clock fails *****************************************************/
  RCC_ClockSecuritySystemCmd(ENABLE);

  /* SysTick end of count event each 1ms */
  SystemCoreClockUpdate();
  RCC_GetClocksFreq(&RCC_Clocks);
  SysTick_Config(RCC_Clocks.HCLK_Frequency / 1000);

  Delay(50);

  /* ==== 复位原因检测(新固件验证失败的唯一线索) ====
     App 在试用期(标志0x22)自检失败时会故意不喂狗, IWDG 超时把芯片
     复位 —— 复位后 RCC->CSR 里的 IWDGRST 标志还在, 这里读出来就知道
     "上次是看门狗咬的"。读完必须清掉, 否则这个标志会一直残留,
     下次上电被误判成"又是狗咬的"。 */
  wdg_reset = (RCC_GetFlagStatus(RCC_FLAG_IWDGRST) == SET) ? 1U : 0U;
  RCC_ClearFlag();

  /* ==== 初始化Mcu驱动(流程图各子流程第一步) ==== */
  Key_IO_Init();
  Led_IO_Init();
  TIM_Config();
  USART1_Init();
  Serial_PutString((uint8_t *)"[BOOT] uart ready\r\n");
  app_elog_init();      /* RTT日志 */

  /* EEPROM只用来记录App真实大小(App侧监控读), 失败不影响主流程 */
  (void)Eeprom_Init();

  /* ==== 检查当前App标识(标志扇区) ==== */
  (void)Flag_Load();
  app_state = Flag_GetAppState();

  switch (app_state)
  {
    case APP_ST_DL_PENDING:     /* App下载固件待更新 -> 子流程A */
      Serial_PutString((uint8_t *)"[BOOT] state=0x11 -> subflow A\r\n");
      Boot_SubflowA();
      break;

    case APP_ST_VERIFY_NEW:     /* 验证新App -> 子流程B */
      Serial_PutString((uint8_t *)"[BOOT] state=0x22 -> subflow B\r\n");
      if (wdg_reset != 0U)
      {
        /* 试用期被狗咬 = 新App没通过自检/跑挂了 -> 回滚备份区旧固件 */
        Boot_SubflowC();
      }
      else
      {
        Boot_SubflowB();
      }
      break;

    case APP_ST_AB_MOVE:        /* A区搬运至B区 -> 子流程E */
      Serial_PutString((uint8_t *)"[BOOT] state=0x33 -> subflow E\r\n");
      Boot_SubflowE();
      break;

    case APP_ST_VALID:          /* 有效App -> 正常启动 */
      Serial_PutString((uint8_t *)"[BOOT] state=0x44 -> jump app\r\n");
      JumpToApp();
      break;

    default:                    /* 其他/空白 -> 主线路径D */
      Serial_PutString((uint8_t *)"[BOOT] state=blank -> path D\r\n");
      Boot_PathD();
      break;
  }

  /* 各分支最终都复位或跳转, 理论到不了这里 */
  while (1)
  {
    Delay(10);
  }
}

/**
  * @brief  Inserts a delay time.
  * @param  nTime: specifies the delay time length, in milliseconds.
  * @retval None
  */
void Delay(__IO uint32_t nTime)
{
  uwTimingDelay = nTime;

  while(uwTimingDelay != 0);
}

/**
  * @brief  Decrements the TimingDelay variable.
  * @param  None
  * @retval None
  */
void TimingDelay_Decrement(void)
{
  g_msTick++;          /* 毫秒时基, 给 Spi.c 的 millis() 用(SysTick 配置成了 1ms) */

  if (uwTimingDelay != 0x00)
  {
    uwTimingDelay--;
  }
}

#ifdef  USE_FULL_ASSERT

/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert error line source number
  * @retval None
  */
void assert_failed(uint8_t* file, uint32_t line)
{
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */

  /* Infinite loop */
  while (1)
  {
  }
}
#endif

/**
  * @}
  */


/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
