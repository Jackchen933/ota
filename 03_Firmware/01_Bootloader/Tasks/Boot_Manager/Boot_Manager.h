#ifndef __BOOT_MANAGER_H
#define __BOOT_MANAGER_H

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx.h"
/* Exported types ------------------------------------------------------------*/
typedef void(*pFunction)(void);
/* Exported constants --------------------------------------------------------*/
/* Exported macro ------------------------------------------------------------*/
#define ApplicationAddress   0x8008000
#define BackApplicationAddress   0x8020000
#define NVIC_VectTab_FLASH   ((uint32_t)0x08000000)

/* 槽位容量 */
#define APP_SLOT_SIZE        0x18000    /* A区(运行区)96KB: 0x08020000 - 0x08008000 */
#define BACK_SLOT_SIZE       0x20000    /* B区(备份区)128KB: 扇区5, 0x08020000 起 */

/* ===== 新方案: APP 后台把固件下到外部 W25Q64, 上电由这里搬到 A 区 =====
   W25Q64 里固件的起始偏移。必须和 APP 侧 USERS/Inc/W25q64.h 的
   W25Q64_OTA_BASE_ADDR 一致(目前都是 0) */
#define W25Q64_APP_BASE      0UL

/* Exported functions --------------------------------------------------------*/

void JumpToApp(void);
uint8_t Slot_IsValid(uint32_t addr);
int8_t BackToApp(int32_t fl_size);
void Backup_Invalidate(void);
extern int32_t app_size;   /* Ymodem收到的密文总长度, main.c赋值 */

/**
  * @brief  检查 EEPROM 里的升级请求, 有的话把 W25Q64 里的新固件搬到 A 区
  * @param  None
  * @retval  1: 升级完成, 调用方应跳转 A 区运行
  *          0: 没有升级请求(或上次下载中途掉电, 已清标志), 调用方按老流程走
  *         -1: 有升级请求但搬运失败 —— A 区内容【不可信】, 绝对不能跳转,
  *             调用方要落到 Ymodem 救援分支
  * @note   本函数会自己 W25Q64_Init() 和 Eeprom_Init(), 调用前不用做任何准备。
  */
int8_t Boot_Ota_Update(void);

#endif /* __BOOT_MANAGER_H */
