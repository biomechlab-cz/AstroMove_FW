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
| ISM330DHCX (IMU) | working | I2C addr 0x6A, WHO_AM_I=0x6B, 208Hz ±2g/±250dps |
| MMC5983MA (mag) | working | I2C addr 0x30, board 04 only; board 05 chip dead |
| RV-3028-C7 (RTC) | working | I2C addr 0x52; time not yet set |
| ADS1292R (ECG ADC) | working | SPI1, ID=0x53, CPOL=0 CPHA=1, 500 kHz, 1 kSPS (CONFIG1=0x83 HR=1); DRDY non-functional on board 04 — use SDATAC+RDATA polling; EMG capture confirmed |

## SD card config notes
- **4-bit 480 kHz validated** — `sd_init_4bit()` in `Core/Src/acquisition.c`; see BOARD_SETUP.md §6 for full findings
- Init sequence: 1-bit ClockDiv=10 → GPIO VERY_HIGH → 4-bit → ClockDiv=50 (480 kHz)
- `DISABLE_SD_INIT` defined in sd_diskio.c — `HAL_SD_Init` called manually before `f_mount`
- Do NOT skip the GPIO speed override after `HAL_SD_Init` — MspInit leaves it at MEDIUM which is too slow for 4-bit
- Do NOT use 4-bit at ClockDiv≤10 (2.4 MHz+) — 47 kΩ pull-ups fail at those speeds (see BOARD_SETUP.md §6)

## Build & flash
STM32CubeIDE. Debug config: `NewProject Debug.launch`.
