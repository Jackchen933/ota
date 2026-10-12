/*********************************************************************************************************
 * @file    Boot_Manager.h
 *
 * @brief   Bootloader 主状态机(按《Bootloader主线流程》流程图实现)
 *
 *          分区(见 common.h):
 *            内部Flash: 扇区0-1=Bootloader | 扇区2=标志扇区 | 扇区3-7=App运行区(0x0800C000,464KB)
 *            外部W25Q64: A区域(0x000000,512KB)=新固件密文 | B区域(0x080000,512KB)=备份密文
 *
 *          密文包格式(W25Q64 A/B区域里存的文件, 上位机打包工具生成, 双方必须一致):
 *            [0..15]     AES-128-CBC 加密头: 明文=12字节自定义+4字节固件大小(小端)
 *            [16..N-17]  固件密文(按16字节分组)
 *            [N-16..N-1] MD5(解密后的固件明文), 明文存放16字节
 *            总长 N = 16 + roundup16(固件大小) + 16
 *
 *          主线上电按标志扇区状态分发(见 main.c):
 *            APP_ST_DL_PENDING -> Boot_SubflowA()  解密搬运A区到运行区, MD5通过置"验证新App"
 *            APP_ST_VERIFY_NEW -> Boot_SubflowB()  开IWDG后跳转App(App自检后置"A区搬运至B区")
 *            APP_ST_VALID      -> JumpToApp()      正常启动
 *            APP_ST_AB_MOVE    -> Boot_SubflowE()  外部A区密文拷到B区备份, 置"有效App"
 *            其他/空白         -> Boot_PathD()     查B区备份决定回滚或待机/按键Ymodem下载
 *
 * @version V1.0 2026-09-23
 * ********************************************************************************************************/
#ifndef __BOOT_MANAGER_H
#define __BOOT_MANAGER_H

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx.h"
#include "common.h"
#include "FlagSector.h"

/* pFunction 取自 common.h(本文件已包含), 勿在此重复 typedef */

/* Exported functions --------------------------------------------------------*/

/* ---- 主线各分支(main.c调用, 互斥进入) ---- */
void Boot_SubflowA(void);   /* App下载固件待更新: 解密搬运 -> MD5 -> 置"验证新App" -> 软复位 */
void Boot_SubflowB(void);   /* 验证新App: 开IWDG -> 跳转App */
void Boot_SubflowC(void);   /* 回滚备份区: B区解密搬运 -> MD5 -> 置"有效App"/"无效" -> 软复位 */
void Boot_SubflowE(void);   /* A区搬运至B区: 外部Flash内密文备份 -> 置"有效App" -> 软复位 */
void Boot_PathD(void);      /* 其他/空白: 查备份回滚, 否则待机等按键 -> Ymodem下载 -> 搬运 */
void JumpToApp(void);       /* 跳转App运行区(内部关闭TIM3/重映射向量表到0x0800C000) */

/* ---- 看门狗(独立看门狗IWDG, "SS看门狗") ----
   注意: IWDG一旦开启无法关闭, 跳转App后由App侧继续喂狗,
   App自检+启动必须在超时时间内完成。 */
void IWDG_Start(void);      /* 启动IWDG, 超时约8.2秒(LSI 32kHz/64分频/重装4095) */
void IWDG_Feed(void);       /* 喂狗 */

#endif /* __BOOT_MANAGER_H */
