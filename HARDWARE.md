# AstroMowe Hardware Context

MCU: **STM32L462RETx** (LQFP64)  
System clock: **16 MHz HSI** (PLL disabled — `RCC_PLL_NONE`, `RCC_SYSCLKSOURCE_HSI`)  
Project file: `NewProject/NewProject.ioc`

---

## Pin Map

| Pin | Signal | Peripheral | Device | Notes |
|-----|--------|------------|--------|-------|
| PA1  | SPI1_SCK        | SPI1   | ADS1292RIRSMR | |
| PA2  | QUADSPI_BK1_NCS | QUADSPI | MX25R3235FM1xx1 | CS |
| PA3  | QUADSPI_CLK     | QUADSPI | MX25R3235FM1xx1 | SCLK |
| PA6  | QUADSPI_BK1_IO3 | QUADSPI | MX25R3235FM1xx1 | HOLD/SIO3 |
| PA7  | QUADSPI_BK1_IO2 | QUADSPI | MX25R3235FM1xx1 | WP/SIO2 |
| PA9  | I2C1_SCL        | I2C1   | ISM330DHCX, MMC5983MA | |
| PA10 | I2C1_SDA        | I2C1   | ISM330DHCX, MMC5983MA | |
| PA11 | USB_DM          | USB    | — | not connected |
| PA12 | USB_DP          | USB    | — | not connected |
| PA13 | SYS_JTMS-SWDIO  | SWD    | Debugger | |
| PA14 | SYS_JTCK-SWCLK  | SWD    | Debugger | |
| PB0  | QUADSPI_BK1_IO1 | QUADSPI | MX25R3235FM1xx1 | SO/SIO1 |
| PB1  | QUADSPI_BK1_IO0 | QUADSPI | MX25R3235FM1xx1 | SI/SIO0 |
| PB4  | SPI1_MISO       | SPI1   | ADS1292RIRSMR | |
| PB5  | SPI1_MOSI       | SPI1   | ADS1292RIRSMR | |
| PB6  | USART1_TX       | USART1 | — | |
| PB7  | USART1_RX       | USART1 | — | |
| PB10 | I2C2_SCL        | I2C2   | — | |
| PB11 | I2C2_SDA        | I2C2   | — | |
| PC0  | ADS_SPI_INT     | GPIO   | ADS1292RIRSMR | interrupt input |
| PC1  | ADS_SPI_CS      | GPIO   | ADS1292RIRSMR | chip select output |
| PB12 | SD_DET          | GPIO   | Hirose DM3AT | card detect input |
| PC8  | SDMMC1_D0       | SDMMC1 | — | |
| PC9  | SDMMC1_D1       | SDMMC1 | — | |
| PC10 | SDMMC1_D2       | SDMMC1 | — | |
| PC11 | SDMMC1_D3       | SDMMC1 | — | |
| PC12 | SDMMC1_CK       | SDMMC1 | — | |
| PC14 | RCC_OSC32_IN    | RCC    | LSE crystal | 32.768 kHz |
| PD2  | SDMMC1_CMD      | SDMMC1 | — | |

---

## Peripherals

### SPI1 — ADS1292RIRSMR ECG ADC

**Device:** ADS1292RIRSMR (24-bit ADC, ECG/biopotential)  
**Pins:** PA1 (SCK), PB4 (MISO), PB5 (MOSI), PC1 (CS), PC0 (DRDY)  
**SPI config:** CPOL=0, CPHA=1, prescaler=32, 500 kHz  
**Status:** working — ID=0x53, 1 kSPS, CH1 ECG/EMG capture confirmed 2026-05-22

**Register config (validated):**

| Reg | Value | Description |
|-----|-------|-------------|
| CONFIG1  | 0x83 | HR=1 (high-res mode required for 1 kSPS), DR=011 |
| CONFIG2  | 0xA0 | Internal 2.42 V reference, lead-off comparator off |
| LOFF     | 0x10 | DC lead-off detection, threshold 5% |
| CH1SET   | 0x40 | PGA gain 4, normal electrode input |
| CH2SET   | 0x60 | PGA gain 8, normal electrode input |
| RLDSENS  | 0x00 | RLD routing off (enable to cancel 50 Hz common-mode) |
| LOFFSENS | 0x0F | Lead-off detection active on IN1P/IN1N/IN2P/IN2N |
| RESP1    | 0x02 | Respiration modulator/demodulator OFF; bit1 must be 1 |
| RESP2    | 0x03 | Clock output enabled, RLDREF internally generated |

**Known issues — board 04:**
- DRDY (PC0) intermittently stuck HIGH after some power cycles — EXTI0 fires on some boots, isr_count stays 0 on others. Root cause unresolved (hardware issue on board 04).
- Workaround: keep ADS in SDATAC mode and read via RDATA command at 1 kHz from main loop (`ADS1292_ReadRDATAFast`). Acquisition is fully DRDY-independent.
- With RESP modulator enabled (RESP1 bits[7:6] set) and floating inputs, CH1 saturates to +full scale (0x7FFFFF). Keep RESP1=0x02 until proper electrode circuit is connected.

**Pitfall — HR bit:** CONFIG1 bit7 = HR (high-resolution mode). HR=0 (low-power) halves the effective output rate even at the same DR[2:0] setting. Always use 0x83, not 0x03.

<details>
<summary>Acquisition code (SDATAC polling path, 1 kHz SysTick)</summary>

```c
/* In ADS1292_Init: keep chip in SDATAC after RESET — registers accessible, RDATA works */
ads_cmd(0x11);  /* SDATAC */

/* ADS1292_ReadRDATAFast — no HAL_Delay, safe at 1 kHz */
void ADS1292_ReadRDATAFast(uint8_t buf[9])
{
    uint8_t cmd = 0x12;
    ADS_CS_LOW();
    for (volatile uint32_t i = 0; i < 200; i++) {}  /* tCSS ≥ 7.8 µs at 512 kHz tCLKIN */
    HAL_SPI_Transmit(_hspi, &cmd, 1, 10);
    HAL_SPI_Receive(_hspi, buf, 9, 50);
    ADS_CS_HIGH();
}

/* ACQ_Process — call from main loop; samples at 1 kHz via SysTick */
void ACQ_Process(void)
{
    static uint32_t s_last_ms = 0;
    uint32_t now = HAL_GetTick();
    if (now == s_last_ms) return;
    s_last_ms = now;

    uint8_t raw[9] = {0};
    ADS1292_ReadRDATAFast(raw);

    /* sign-extend 24-bit CH1 to int32 */
    int32_t ch1 = ((int32_t)((uint32_t)raw[3] << 24 |
                              (uint32_t)raw[4] << 16 |
                              (uint32_t)raw[5] << 8)) >> 8;
    uint8_t loff_status = raw[0];  /* bits[22:19] of status word */
}
```

</details>

---

### I2C1

**Devices:**
- ISM330DHCX — 6-axis IMU (accelerometer + gyroscope)
- MMC5983MA — 3-axis magnetometer

**Pins:** PA9 (SCL), PA10 (SDA)  
**Timing register:** `0x10D19CE4`  
**Status:** not tested

<details>
<summary>Code snippet</summary>

```c
// add working snippet here
```

</details>

---

### I2C2

**Device:** —  
**Pins:** PB10 (SCL), PB11 (SDA)  
**Timing register:** `0x10D19CE4`  
**Status:** not tested

<details>
<summary>Code snippet</summary>

```c
// add working snippet here
```

</details>

---

### QUADSPI — Single Bank 1

**Device:** MX25R3235FM1xx1 — 32 Mbit NOR flash  
**Pins:** PA2 (CS), PA3 (SCLK), PB1 (SI/SIO0), PB0 (SO/SIO1), PA7 (WP/SIO2), PA6 (HOLD/SIO3)  
**Status:** not tested

<details>
<summary>Code snippet</summary>

```c
// add working snippet here
```

</details>

---

### SDMMC1 — 4-bit wide bus

**Device:** Micro SD card — Hirose DM3AT connector  
**Pins:** PC8 (D0), PC9 (D1), PC10 (D2), PC11 (D3), PC12 (CK), PD2 (CMD)  
**GPIO:** PB12 (SD_DET, card detect input), PB11 (TPS_ON — SD power gate)  
**Clock:** HSI48 source; 480 kHz in 4-bit mode (ClockDiv=50)  
**Notes:** 47 kΩ pull-ups on D0–D3 and CMD limit max clock — see BOARD_SETUP.md §6  
**Status:** working — 4-bit 480 kHz validated, ~395 KB/s. See `Core/Src/acquisition.c` `sd_init_4bit()`.

**Init sequence (critical order):**
1. Init in 1-bit mode at ClockDiv=10 (2.4 MHz) — `HAL_SD_Init`
2. Override GPIO to `GPIO_SPEED_FREQ_VERY_HIGH` + `GPIO_PULLUP` for all 6 pins
3. Wait for TRANSFER state, then `HAL_SD_ConfigWideBusOperation(SDMMC_BUS_WIDE_4B)`
4. Drop clock: `MODIFY_REG(SDMMC1->CLKCR, SDMMC_CLKCR_CLKDIV, 50U)` → 480 kHz

**`DISABLE_SD_INIT` must be defined in `sd_diskio.c`** — prevents FatFS from calling `HAL_SD_Init` a second time on `f_mount`.

<details>
<summary>sd_init_4bit() — working init sequence</summary>

```c
static uint8_t sd_init_4bit(void)
{
    hsd1.Init.ClockDiv = 10;
    hsd1.Init.BusWide  = SDMMC_BUS_WIDE_1B;
    if (HAL_SD_Init(&hsd1) != HAL_OK) return 0;

    /* Override MspInit GPIO speed — MEDIUM is too slow for 4-bit transitions */
    GPIO_InitTypeDef g = {0};
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF12_SDMMC1;
    g.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
    HAL_GPIO_Init(GPIOC, &g);
    g.Pin = GPIO_PIN_2;
    HAL_GPIO_Init(GPIOD, &g);

    if (!sd_wait_transfer(1000)) return 0;
    if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK) return 0;
    MODIFY_REG(SDMMC1->CLKCR, SDMMC_CLKCR_CLKDIV, 50U);  /* 48 MHz / (2×50) = 480 kHz */
    HAL_Delay(20);
    return sd_wait_transfer(500);
}
```

</details>

---

### USART1 — Async

**Device:** —  
**Pins:** PB6 (TX), PB7 (RX)  
**Status:** not tested

<details>
<summary>Code snippet</summary>

```c
// add working snippet here
```

</details>

---

### USB — Device mode

**Device:** —  
**Pins:** PA11 (DM), PA12 (DP)  
**Clock:** 48 MHz (PLLSAI1Q)  
**Status:** not tested

<details>
<summary>Code snippet</summary>

```c
// add working snippet here
```

</details>

---

### AES — Hardware accelerator

**Status:** not tested

<details>
<summary>Code snippet</summary>

```c
// add working snippet here
```

</details>
