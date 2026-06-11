# AstroMowe — NewProject

Full hardware context: [`../HARDWARE.md`](../HARDWARE.md)

## MCU
STM32L462RETx (LQFP64), **16 MHz HSI** (PLL disabled in current firmware), STM32CubeIDE project.

## Active peripherals

| Peripheral | Pins | Device |
|------------|------|--------|
| SPI1 | PA1 SCK, PB4 MISO, PB5 MOSI, PC1 CS, PC0 INT | ADS1292RIRSMR (ECG ADC) |
| I2C1 | PA9 SCL, PA10 SDA | ISM330DHCX (IMU), MMC5983MA (mag), RV-3028-C7 (RTC) |
| QUADSPI | PA2 CS, PA3 CLK, PB1 IO0, PB0 IO1, PA7 IO2, PA6 IO3 | MX25R3235FM1xx1 (32 Mbit NOR flash) |
| SDMMC1 | PC8-11 D0-3, PC12 CK, PD2 CMD | Micro SD — Hirose DM3AT |
| USART1 | PB6 TX, PB7 RX | ST-Link V3 MINI virtual COM |
| GPIO out | PB10 | LED (solder pad) |
| GPIO out | PB11 | TPS_ON — power gate enable |
| GPIO in  | PB12 | SD_DET — card detect |
| GPIO in  | PC0  | ADS_SPI_INT — interrupt |
| GPIO out | PC1  | ADS_SPI_CS — chip select |
| GPIO     | PC6  | SYNC_A (TBD) |
| GPIO     | PC7  | SYNC_B (TBD) |

Disabled in main.c (not yet needed): QUADSPI, USB, I2C2.  
I2C2 permanently disabled — PB11 repurposed as TPS_ON GPIO.

## Active board: **04** (board 05 has dead MMC5983MA — see BOARD_SETUP.md)

## Tested / working

| Feature | Status | Notes |
|---------|--------|-------|
| USART1 Hello World | in progress | 115200 8N1, ST-Link V3 MINI virtual COM |
| SDMMC1 + FatFS write | working | 1-bit mode, ClockDiv=10, GPIO_PULLUP/MEDIUM; D1-D3 bad solder on board 05 |
| ISM330DHCX (IMU) | **not responding** (found 2026-06-11) | NACKs at both 0x6A and 0x6B (addr auto-probed at init); mag+RTC on same bus fine → chip dead or unpowered. Accel/gyro record as zeros |
| MMC5983MA (mag) | working | I2C addr 0x30, board 04 only; board 05 chip dead |
| RV-3028-C7 (RTC) | working | I2C addr 0x52; time not yet set |
| ADS1292R (ECG ADC) | working | SPI1, ID=0x53, CPOL=0 CPHA=1, 500 kHz, 1 kSPS (CONFIG1=0x83 HR=1); DRDY non-functional on board 04 — use SDATAC+RDATA polling; EMG capture confirmed |

## Data storage (EMGX format)
- Spec: "Format specification.docx" (encrypted binary + plaintext control CSV)
- Writer: `Core/Src/recording.c` — AES-256-GCM (hardware AES peripheral), 10 s batches of 1 s chunks, CRC32 + GCM tag per batch
- Key: `Core/Inc/recording_key.h` — **placeholder test key**, replace before deployment
- Files per session: `SNNNNNNN.EMX` + `SNNNNNNN.CSV` (8.3 names — FatFS `_USE_LFN=0`; enable LFN in CubeMX for the spec's `SESSION_NNNNNNN.emgx`)
- Desktop decoder: `decode_emgx.py` (needs `pip install cryptography`); `plot_ecg.py` is for the old pre-EMGX `log_NNNN.bin` format
- GCM nonce = session start time + session id + batch index — RTC must be set for cross-session uniqueness under the fixed key
- IMU recorded at 100 Hz (payload type 2): accel+gyro int16 (ISM330, currently zeros — chip not responding) + mag 18-bit uint32 (MMC5983 continuous mode); IMU slots derived from the EMG sample clock (every 10th sample) so counts are exact; sample-and-hold across blocking writes and on I2C failure (`g_ism_fail_count`/`g_mag_fail_count`)
- EMG = ADS ch1 int32 + lead-off status byte per sample

## Board 04 quirks found 2026-06-10/11
- **BOOT0 pin reads high** — option bytes set to `nSWBOOT0=0, nBOOT0=1` (always boot main flash, BOOT0 pin ignored). Without this the MCU boots into the ROM bootloader and the firmware never runs. Re-apply after any full chip erase / option-byte reset.
- **SDMMC TX underrun whenever the polled FIFO feed is starved** (also killed the old firmware — see 20-30 s LOG_*.BIN files). HAL aborts the write without CMD12, wedging the card in receive state. Two-layer mitigation, do not remove when regenerating from CubeMX:
  - abort (CMD12) + bounded-wait + retry ×3 in `SD_read`/`SD_write` (sd_diskio.c USER CODE) — residual underruns (~2/batch) recover on first retry
  - SDMMC CLKDIV=100 (≈470 kHz, ~235 KB/s) in `sd_init_4bit()` — at CLKDIV=50 the half-FIFO refill deadline (~70 µs worst case) is shorter than the 94 µs DRDY ISR and every multi-block write fails
- **DRDY edge rate ~1050/s at 1 kSPS** (~5% above spec+tolerance); ISR debounces edges <800 µs apart (DWT-based, `g_spurious_count`).
- **ADS1292R decodes MOSI as commands even in RDATAC** — any SPI read must transmit 0x00. A read that clocks out garbage stops conversions dead (DRDY stuck low) within seconds.

## Acquisition architecture (zero-loss, 2026-06-11)
- DRDY ISR reads the frame itself (`ADS1292_ReadRawFast` — direct-register SPI, ~40 µs; HAL version is ~240 µs and starves SDMMC) into a 1024-sample ring; main loop drains ring → encrypt → SD write. `dropped_samples` in the control CSV = ring overflow only (expected 0).
- SPI1 runs 500 kHz for ADS commands/registers (inter-byte decode time), switched to 4 MHz after RDATAC for data-phase reads (main.c).
- EXTI0 must be armed only **after** `ADS1292_StartContinuous()` — the ISR's SPI reads must never overlap main-thread SPI.
- Debug globals readable over SWD: `g_isr_count/g_spurious_count/g_dropped_count/g_isr_max_cycles` (acquisition.c), `g_rec_fail_point/code` (recording.c), `g_sd_retry_count/g_sd_fail_count/g_sd_last_stage/g_sd_last_err` (sd_diskio.c).

## SD card config notes
- **4-bit ~470 kHz validated** — `sd_init_4bit()` in `Core/Src/acquisition.c`; see BOARD_SETUP.md §6 for full findings
- Init sequence: 1-bit ClockDiv=10 → GPIO VERY_HIGH → 4-bit → ClockDiv=100 (48 MHz/(CLKDIV+2) ≈ 470 kHz; do not lower CLKDIV — see TX underrun note above)
- `DISABLE_SD_INIT` defined in sd_diskio.c — `HAL_SD_Init` called manually before `f_mount`
- Do NOT skip the GPIO speed override after `HAL_SD_Init` — MspInit leaves it at MEDIUM which is too slow for 4-bit
- Do NOT use 4-bit at ClockDiv≤10 (2.4 MHz+) — 47 kΩ pull-ups fail at those speeds (see BOARD_SETUP.md §6)

## Build & flash
STM32CubeIDE. Debug config: `NewProject Debug.launch`.
