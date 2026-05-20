# AstroMowe — NewProject

Full hardware context: [`../HARDWARE.md`](../HARDWARE.md)

## MCU
STM32L462RETx (LQFP64), 80 MHz, STM32CubeIDE project.

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

## Tested / working

| Feature | Status | Notes |
|---------|--------|-------|
| USART1 Hello World | in progress | 115200 8N1, ST-Link V3 MINI virtual COM |
| SDMMC1 + FatFS write | working | 1-bit mode, ClockDiv=10, GPIO_PULLUP/MEDIUM; D1-D3 suspected bad solder joints so staying in 1-bit |

## SD card config notes
- `SDMMC_BUS_WIDE_1B` permanently — D1/D2/D3 (PC9-PC11) suspected bad solder joints on Hirose DM3AT
- `ClockDiv = 10` → 2.4 MHz (HSI48 source)
- GPIO pull: `GPIO_PULLUP`, speed: `GPIO_SPEED_FREQ_MEDIUM` on all SDMMC pins
- `DISABLE_SD_INIT` defined in sd_diskio.c — manual `HAL_SD_Init` in main.c before `f_mount`
- Do NOT call `HAL_SD_ConfigWideBusOperation` — will break D0 reads if D1-D3 are flaky

## Build & flash
STM32CubeIDE. Debug config: `NewProject Debug.launch`.
