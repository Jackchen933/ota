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
#include "Flash.h"
#include "elog.h"
#include "ymodem.h"
#include "Boot_Manager.h"
#include "Spi.h"
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
uint8_t tab_1024[1024];   /* Ymodem接收RAM缓冲区, ymodem.c中extern引用, 必须定义 */

/* 新方案(APP下载到W25Q64)的升级结果, 取值见 Boot_Ota_Update() 的说明 */
int8_t ota_result = 0;

/* Private function prototypes -----------------------------------------------*/

/* Private functions ---------------------------------------------------------*/
 /*
  *power by WeAct Studio
  *The board with `WeAct` Logo && `version number` is our board, quality guarantee.
  *For more information please visit: https://github.com/WeActTC/MiniF4-STM32F4x1
  *更多信息请访问：https://gitee.com/WeActTC/MiniF4-STM32F4x1
  */
/**
  * @brief  Main program
  * @param  None
  * @retval None
  */
 uint8_t au8_test[1024];
int main(void)
{
	/* Enable Clock Security System(CSS): this will generate an NMI exception
     when HSE clock fails *****************************************************/
  RCC_ClockSecuritySystemCmd(ENABLE);

 /*!< At this stage the microcontroller clock setting is already configured,
       this is done through SystemInit() function which is called from startup
       files before to branch to application main.
       To reconfigure the default setting of SystemInit() function,
       refer to system_stm32f4xx.c file */

  /* SysTick end of count event each 1ms */
  SystemCoreClockUpdate();
  RCC_GetClocksFreq(&RCC_Clocks);
  SysTick_Config(RCC_Clocks.HCLK_Frequency / 1000);


  /* Add your application code here */
  /* Insert 50 ms delay */
  Delay(50);

  Key_IO_Init();
  Led_IO_Init();
  TIM_Config();
  USART1_Init();

  /* ===== 升级流程 =====
     A区(0x08008000,扇区2-4,96KB) = 运行区
     B区(0x08020000,扇区5,128KB)   = 老方案(Ymodem+AES)的备份区
     外部 W25Q64                    = 新方案(后台无感下载)的固件仓库 */

  /* 1. 新方案: APP 已经把固件下到 W25Q64, EEPROM 里记着升级请求 -> 搬到A区
        1  = 搬运完成, 直接跳新App
        0  = 没有升级请求, 继续走下面的老流程
        -1 = 有请求但搬运失败, A区内容已经不可信, 绝不能跳转,
             两条 if 都不进, 直接落到最后的 Ymodem 救援分支 */
  ota_result = Boot_Ota_Update();

  if (1 == ota_result)
  {
    TIM_Cmd(TIM3, DISABLE);
    TIM_ITConfig(TIM3, TIM_IT_Update, DISABLE);
    JumpToApp();
  }

  if (0 == ota_result)
  {
    /* 2. 老方案: B区有固件(上次Ymodem下载的) -> 解密搬运到A区,
          成功后擦掉B区标记"已处理" */
    /* 这里直接调 BackToApp() 而不是先用 Slot_IsValid() 判"B区有没有固件":
       B区开头是AES密文而不是向量表, 密文几乎必然被判成无效, 那条分支等于
       永远进不来, 跨次上电就搬不动了。BackToApp() 自己会解开头部校验大小,
       B区是空白(全0xFF)或残包都会返回 -1, 不会再往下写。
       冷启动时 app_size 为0, BackToApp 会从头部反推密文总长。 */
    if (BackToApp(app_size) == 0)
    {
      Backup_Invalidate();
    }

    /* A区固件有效 -> 直接跳转运行 */
    if (Slot_IsValid(ApplicationAddress))
    {
      TIM_Cmd(TIM3, DISABLE);
      TIM_ITConfig(TIM3, TIM_IT_Update, DISABLE);
      JumpToApp();
    }
  }

  /* 3. A区无固件(第一次使用 / 新方案搬运失败) -> 阻塞等待Ymodem下载, 固件写入B区 */
  FLASH_Unlock();   /* ymodem.c内部直接调FLASH_EraseSector/ProgramWord, 必须先解锁 */
  app_size = Ymodem_Receive(tab_1024);
  FLASH_Lock();

  /* 关闭呼吸灯: 停掉TIM3中断, 否则ISR每50us改写PC13, 主循环的LED_ON/OFF会被覆盖 */
  TIM_Cmd(TIM3, DISABLE);
  TIM_ITConfig(TIM3, TIM_IT_Update, DISABLE);

  app_elog_init();

  /* 串口直接打印(绕过elog): USART1发出两条消息 */
  {
    const char *msg = "Hello LiXin\r\nthisisbootloader\r\n";
    while (*msg) USART_SendChar(USART1, (uint8_t)*msg++);
  }

  /* 4. 下载成功 -> 解密搬运B区到A区 -> 擦B区 -> 跳转A区运行(完整OTA闭环) */
  if (app_size > 0)
  {
    if (BackToApp(app_size) == 0)
    {
      Backup_Invalidate();
      JumpToApp();
    }
  }

  Delay(10);
  /* Infinite loop */
  while (1)
  {
    //如果是按下，则Led翻转
    if(Key_Scan())
    {
      /* Ymodem回传实验: 短按按键, 把A区固件回传给上位机(需先在SecureCRT启动Ymodem接收) */
      if (app_size > 0)
      {
        if (Ymodem_Transmit((uint8_t *)ApplicationAddress, (const uint8_t *)"app.bin", app_size) == 0)
        {
          USART_SendChar(USART1, 'S');   /* 回传成功 */
        }
        else
        {
          USART_SendChar(USART1, 'F');   /* 回传失败 */
        }
      }

      LED_ON;
    }
    else
    {
      LED_OFF;
    }
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
