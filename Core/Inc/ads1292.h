#ifndef ADS1292_H
#define ADS1292_H

#include "stm32l4xx_hal.h"
#include <stdint.h>

typedef struct {
    uint32_t status;  /* 24-bit status word — header bits [23:20] must be 0xC */
    int32_t  ch1;     /* 24-bit signed — raw ADC counts */
    int32_t  ch2;
} ADS1292_Sample_t;

/* Call once after power-on. Sends RESET + SDATAC. */
void    ADS1292_Init(SPI_HandleTypeDef *hspi);

/* Returns device ID register (0x53 typical for ADS1292R).
   Returns 0x00 or 0xFF on SPI failure. */
uint8_t ADS1292_ReadID(void);

/* Switch chip into RDATAC (continuous output) mode. */
void    ADS1292_StartContinuous(void);

/* Stop conversions and exit RDATAC. */
void    ADS1292_Stop(void);

/* Block until DRDY asserts (max 20ms) then read one 9-byte frame.
   Returns 1 on success, 0 on timeout. Call only while in continuous mode. */
uint8_t ADS1292_ReadSample(ADS1292_Sample_t *s);

#endif /* ADS1292_H */
