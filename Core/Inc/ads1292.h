#ifndef ADS1292_H
#define ADS1292_H

#include "stm32l4xx_hal.h"
#include <stdint.h>


// Register Read Commands
#define RREG 0x20; // Read n nnnn registers starting at address r rrrr
// first byte 001r rrrr (2xh)(2) - second byte 000n nnnn(2)
#define WREG 0x40; // Write n nnnn registers starting at address r rrrr
// first byte 010r rrrr (2xh)(2) - second byte 000n nnnn(2)

#define START 0x08  // Start/restart (synchronize) conversions
#define STOP 0x0A   // Stop conversion
#define RDATAC 0x10 // Enable Read Data Continuous mode.

// This mode is the default mode at power-up.
#define SDATAC 0x11 // Stop Read Data Continuously mode
#define RDATA 0x12  // Read data by command; supports multiple read back.

// register address
#define ADS1292_REG_ID 0x00
#define ADS1292_REG_CONFIG1 0x01
#define ADS1292_REG_CONFIG2 0x02
#define ADS1292_REG_LOFF 0x03
#define ADS1292_REG_CH1SET 0x04
#define ADS1292_REG_CH2SET 0x05
#define ADS1292_REG_RLDSENS 0x06
#define ADS1292_REG_LOFFSENS 0x07
#define ADS1292_REG_LOFFSTAT 0x08
#define ADS1292_REG_RESP1 0x09
#define ADS1292_REG_RESP2 0x0A

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
