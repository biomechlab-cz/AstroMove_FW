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

/* ADS1292R RDATAC status word → normalized lead-off status byte used for the live
   LED and the per-batch lead-off count — it is NOT stored per sample (FORMAT.md §5,
   §8). The first two of the three status bytes carry
   the lead-off comparator outputs (datasheet SBAS502, "Data Output"):
     raw[0] = status[23:16] = 1100 | LOFF_STAT[4] | IN2N_OFF | IN2P_OFF | IN1N_OFF
     raw[1] = status[15: 8] = IN1P_OFF | GPIO1 | GPIO0 | 0 0 0 0 0
   The constant 1100 header and GPIO bits are dropped; the result is:
     bit0 IN1P, bit1 IN1N, bit2 IN2P, bit3 IN2N, bit4 LOFF_STAT[4].
   CH1 carries the EMG signal, so CH1 lead-off = (status & ADS1292_STATUS_CH1_LEADOFF). */
#define ADS1292_NORMALIZE_STATUS(raw0, raw1) \
    ((uint8_t)((((uint8_t)(raw0) & 0x0F) << 1) | (((uint8_t)(raw1) >> 7) & 0x01)))
#define ADS1292_STATUS_CH1_LEADOFF 0x03  /* IN1P | IN1N */

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

/* ----- Lead-off control (LOFF register, 0x03) — adjustable for experiments -----
   DC lead-off injects ILEAD_OFF current into the inputs and flags "lead-off" when
   an input crosses the COMP_TH threshold. The current must develop that threshold
   across the electrode impedance, so a high-impedance / poor contact reads as
   "lead-on" unless the current is large enough:
     6 nA  → flags only a near-total open (~>100 MΩ)
     6 µA  → flags a high-impedance bond   (~>100 kΩ)   <- textile-electrode glue bond
     22 µA → flags a marginal contact      (~>tens of kΩ)
   Larger current costs a small DC offset on a good electrode (≈ I×R; removed by the
   EMG high-pass). Tune ADS1292_LOFF_CONFIG below per experiment. Datasheet SBAS502
   Table 20; bit positions: COMP_TH[7:5], (bit4=1), ILEAD_OFF[3:2], (bit1=0), FLEAD_OFF[0]. */
#define ADS1292_LOFF_COMP_TH_95_5  (0u << 5)  /* comparator threshold pos 95% / neg 5% (default) */
#define ADS1292_LOFF_COMP_TH_92_5  (1u << 5)  /* 92.5% / 7.5% (datasheet lists down to 70%/30%) */
#define ADS1292_LOFF_BIT4          (1u << 4)  /* reserved — must be 1 */
#define ADS1292_LOFF_ILEAD_6NA     (0u << 2)
#define ADS1292_LOFF_ILEAD_22NA    (1u << 2)
#define ADS1292_LOFF_ILEAD_6UA     (2u << 2)
#define ADS1292_LOFF_ILEAD_22UA    (3u << 2)
#define ADS1292_LOFF_FLEAD_DC      (0u << 0)  /* DC lead-off (default) */
#define ADS1292_LOFF_FLEAD_AC      (1u << 0)  /* AC lead-off at f_DR/4 — no DC offset, adds a tone in-band */

/* Active lead-off configuration — change the ILEAD/COMP_TH/FLEAD fields per experiment.
   Current: 6 nA, DC, 95%/5% — recording-safe (too small to rail a poor contact, so the
   real EMG is preserved). 6 µA flags a high-impedance bond but rails it, swamping the
   EMG — only use 6 µA as a contact check or once the electrode bond is reliably low-Z. */
#define ADS1292_LOFF_CONFIG \
    (ADS1292_LOFF_COMP_TH_95_5 | ADS1292_LOFF_BIT4 | ADS1292_LOFF_ILEAD_6NA | ADS1292_LOFF_FLEAD_DC)

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

/* ISR-safe fast variant of ReadRaw: direct register SPI, ~40 µs at 4 MHz.
   Use from the DRDY interrupt — the HAL version is ~6× slower. */
void ADS1292_ReadRawFast(uint8_t buf[9]);

/* Read one register via RREG. Only valid in SDATAC mode (before StartContinuous). */
uint8_t ADS1292_ReadReg(uint8_t reg);

/* Force-read one conversion result with RDATA command. No DRDY required.
   Only valid in SDATAC mode. */
void ADS1292_ReadRDATA(uint8_t buf[9]);

/* Same as ReadRDATA but without HAL_Delay — safe to call at 1kHz from main loop.
   Only valid in SDATAC mode. */
void ADS1292_ReadRDATAFast(uint8_t buf[9]);

#endif /* ADS1292_H */
