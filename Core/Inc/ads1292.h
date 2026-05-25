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

/* Exit RDATAC mode without stopping conversions. Use when falling back to RDATA polling. */
void    ADS1292_ExitRDATAC(void);

/* Block until DRDY asserts (max 20ms) then read one 9-byte frame.
   Returns 1 on success, 0 on timeout. Call only while in continuous mode. */
uint8_t ADS1292_ReadSample(ADS1292_Sample_t *s);

/* Read 9 raw bytes from the chip in RDATAC mode.
   Call from main loop after DRDY flag is set. Includes tCSS (7.8 µs) delay after CS. */
void ADS1292_ReadRaw(uint8_t buf[9]);

/* Read one register via RREG. Only valid in SDATAC mode (before StartContinuous). */
uint8_t ADS1292_ReadReg(uint8_t reg);

/* Force-read one conversion result with RDATA command. No DRDY required.
   Only valid in SDATAC mode. */
void ADS1292_ReadRDATA(uint8_t buf[9]);

/* Same as ReadRDATA but without HAL_Delay — safe to call at 1kHz from main loop.
   Only valid in SDATAC mode. */
void ADS1292_ReadRDATAFast(uint8_t buf[9]);

#endif /* ADS1292_H */
