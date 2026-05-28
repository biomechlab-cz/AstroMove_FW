#ifndef SD_DMA_H
#define SD_DMA_H

#include "stm32l4xx_hal.h"

/* Init SD in 4-bit 480 kHz mode (no FatFS involved). */
HAL_StatusTypeDef SD_DMA_Init(void);

/* Write/read count sectors at sector_addr via DMA. Blocks until done (or timeout). */
HAL_StatusTypeDef SD_DMA_Write(uint32_t sector_addr, const uint8_t *buf, uint32_t count);
HAL_StatusTypeDef SD_DMA_Read (uint32_t sector_addr,       uint8_t *buf, uint32_t count);

/* FreeRTOS task entry point. Write pattern, read back, verify, blink result. */
void SD_DMA_Test(void *arg);

#endif /* SD_DMA_H */
