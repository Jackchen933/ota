/**
  ******************************************************************************
  * @file    bsp_i2c1_bus.c
  * @brief   I2C1 总线互斥锁实现（FreeRTOS 二值信号量）。
  ******************************************************************************
  */
#include "bsp_i2c1_bus.h"

#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t s_bus_mutex;

void bsp_i2c1_bus_init(void)
{
  if (s_bus_mutex == NULL)
  {
    s_bus_mutex = xSemaphoreCreateMutex();
  }
}

void bsp_i2c1_lock(void)
{
  if (s_bus_mutex != NULL)
  {
    (void)xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
  }
}

void bsp_i2c1_unlock(void)
{
  if (s_bus_mutex != NULL)
  {
    (void)xSemaphoreGive(s_bus_mutex);
  }
}
