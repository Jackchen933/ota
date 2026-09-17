/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __W25QXX_H
#define __W25QXX_H

#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
/* 本工程用的是标准外设库 + Bsp/Spi.c, 不是 HAL:
   原来这里引的 cmsis_os.h(FreeRTOS) 本工程没有, "spi.h" 也已并到 Bsp/Spi.h */
#include "main.h"
#include "Spi.h"

/** @addtogroup BSP
  * @{
  */

/** @addtogroup Components
  * @{
  */

/** @addtogroup W25QXXXX
  * @{
  */

/** @defgroup W25QXXXX_Exported_Types
  * @{
  */

/**
  * @}
  */

/** @defgroup W25QXXXX_Exported_Constants
  * @{
  */

/**
  * @brief  W25QXX Configuration
  *
  * 下面这组是"W25Q32"的静态默认值, 只作参考。
  * 实际容量由 W25Qx_Get_Parameter() 在运行时读 ID 后填进 W25Qx_Para,
  * 所以换更大的芯片不用改这里。真正被用到的是 W25QXXXX_PAGE_SIZE。
  */

/* W25Q32 */
#define W25QXXXX_FLASH_SIZE                  0x0400000 /* 32 MBits => 4MBytes */
#define W25QXXXX_SECTOR_COUNT                0x40      /* 64 sectors of 64KBytes */
#define W25QXXXX_SECTOR_SIZE                 0x10000   /* 64 sectors of 64KBytes */
#define W25QXXXX_SUBSECTOR_COUNT             0x0400    /* 1024 subsectors of 4kBytes */
#define W25QXXXX_SUBSECTOR_SIZE              0x1000    /* 1024 subsectors of 4kBytes */
#define W25QXXXX_PAGE_SIZE                   0x100     /* 16384 pages of 256 bytes */

/* 0x90(读厂商/器件ID)命令返回的 16 位 ID 中的低字节, 即容量代码 */
#define W25Q80  0XEF13
#define W25Q16  0XEF14
#define W25Q32  0XEF15
#define W25Q64  0XEF16
#define W25Q128 0XEF17

#define W25QXXXX_DUMMY_CYCLES_READ           4
#define W25QXXXX_DUMMY_CYCLES_READ_QUAD      10

#define W25QXXXX_BULK_ERASE_MAX_TIME         250000
#define W25QXXXX_SECTOR_ERASE_MAX_TIME       3000
#define W25QXXXX_SUBSECTOR_ERASE_MAX_TIME    800
#define W25QXXXX_TIMEOUT_VALUE 1000

/**
  * @brief  W25QXX Commands
  */
/* Reset Operations */
#define RESET_ENABLE_CMD                     0x66
#define RESET_MEMORY_CMD                     0x99

#define ENTER_QPI_MODE_CMD                   0x38
#define EXIT_QPI_MODE_CMD                    0xFF

/* Identification Operations */
#define READ_ID_CMD                          0x90
#define DUAL_READ_ID_CMD                     0x92
#define QUAD_READ_ID_CMD                     0x94
#define READ_JEDEC_ID_CMD                    0x9F

/* Read Operations */
#define READ_CMD                             0x03
#define FAST_READ_CMD                        0x0B
#define DUAL_OUT_FAST_READ_CMD               0x3B
#define DUAL_INOUT_FAST_READ_CMD             0xBB
#define QUAD_OUT_FAST_READ_CMD               0x6B
#define QUAD_INOUT_FAST_READ_CMD             0xEB

/* Write Operations */
#define WRITE_ENABLE_CMD                     0x06
#define WRITE_DISABLE_CMD                    0x04

/* Register Operations */
#define READ_STATUS_REG1_CMD                  0x05
#define READ_STATUS_REG2_CMD                  0x35
#define READ_STATUS_REG3_CMD                  0x15

#define WRITE_STATUS_REG1_CMD                 0x01
#define WRITE_STATUS_REG2_CMD                 0x31
#define WRITE_STATUS_REG3_CMD                 0x11


/* Program Operations */
#define PAGE_PROG_CMD                        0x02
#define QUAD_INPUT_PAGE_PROG_CMD             0x32


/* Erase Operations */
#define SECTOR_ERASE_CMD                     0x20      /* 擦除 4KB 小扇区 */
#define BLOCK_ERASE_CMD                      0xD8      /* 擦除 64KB 大块, 本文件暂未使用 */
#define CHIP_ERASE_CMD                       0xC7

#define PROG_ERASE_RESUME_CMD                0x7A
#define PROG_ERASE_SUSPEND_CMD               0x75


/* Flag Status Register */
/* 注意: BUSY 在状态寄存器1(write in progress), WREN 也在状态寄存器1,
   QE 则是在状态寄存器2 的 bit1 —— 两者数值都是 0x02, 但属于不同的寄存器,
   别混用。 */
#define W25QXXXX_FSR_BUSY                    ((uint8_t)0x01)    /*!< busy */
#define W25QXXXX_FSR_WREN                    ((uint8_t)0x02)    /*!< write enable */
#define W25QXXXX_FSR_QE                      ((uint8_t)0x02)    /*!< quad enable */


/* 片选: 标准库的 GPIO_WriteBit, 原来用的 HAL_GPIO_WritePin 本工程没有 */
#define W25Qx_Enable() 			GPIO_WriteBit(F_CS_GPIO_Port, F_CS_Pin, Bit_RESET)
#define W25Qx_Disable() 		GPIO_WriteBit(F_CS_GPIO_Port, F_CS_Pin, Bit_SET)

#define W25Qx_OK            ((uint8_t)0x00)
#define W25Qx_ERROR         ((uint8_t)0x01)
#define W25Qx_BUSY          ((uint8_t)0x02)
#define W25Qx_TIMEOUT				((uint8_t)0x03)

typedef struct
{
	uint32_t FLASH_ID;
	uint32_t FLASH_SIZE;
	uint32_t SECTOR_COUNT;
	uint32_t SECTOR_SIZE;
	uint32_t SUBSECTOR_COUNT;
	uint32_t SUBSECTOR_SIZE;
	uint32_t PAGE_SIZE;
}W25Qx_Parameter;
extern W25Qx_Parameter W25Qx_Para;

/* 注意: W25Qx_Reset / W25Qx_GetStatus 在 .c 里是 static, 属于文件内部函数,
   原来在头文件里也声明了一遍 static, 那会让每个 include 本头文件的 .c
   都各自看到一份"声明了却没有定义"的静态函数, 所以这里删掉了。 */
uint8_t W25Qx_Init(void);
uint8_t W25Qx_WriteEnable(void);
void W25Qx_Read_ID(uint16_t *ID);
uint8_t W25Qx_Read(uint8_t* pData, uint32_t ReadAddr, uint32_t Size);
uint8_t W25Qx_Write(uint8_t* pData, uint32_t WriteAddr, uint32_t Size);
uint8_t W25Qx_Erase_Block(uint32_t Address);
uint8_t W25Qx_Erase_Chip(void);

uint8_t W25Qx_Get_Parameter(W25Qx_Parameter *Para);
/**
  * @}
  */

/** @defgroup W25QXXXX_Exported_Functions
  * @{
  */
/**
  * @}
  */

/**
  * @}
  */

/**
  * @}
  */

/**
  * @}
  */

#ifdef __cplusplus
}
#endif

#endif /* __W25QXX_H */
