/**
  ******************************************************************************
  * @file    bsp_i2c1_bus.h
  * @brief   I2C1 总线互斥锁。
  *
  *          本工程的 I2C1 上挂了两个设备：AT24C02（EEPROM，业务侧）和
  *          CST816S（触摸，GUI 侧）。HAL 的 I2C 句柄不是线程安全的，
  *          FreeRTOS 下两个任务并发调用会撞车，所以凡是用 hi2c1 的
  *          事务都必须先拿这把锁。
  *
  *          用法：bsp_i2c1_lock() -> 一次完整事务 -> bsp_i2c1_unlock()。
  ******************************************************************************
  */
#ifndef __BSP_I2C1_BUS_H__
#define __BSP_I2C1_BUS_H__

#ifdef __cplusplus
extern "C" {
#endif

void bsp_i2c1_bus_init(void);   /* 在 RTOS 启动前调用一次 */
void bsp_i2c1_lock(void);       /* 拿锁（锁没建好时空操作，方便裸机复用） */
void bsp_i2c1_unlock(void);     /* 还锁 */

#ifdef __cplusplus
}
#endif

#endif /* __BSP_I2C1_BUS_H__ */
