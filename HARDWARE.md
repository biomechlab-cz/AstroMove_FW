# AstroMowe Hardware Context

MCU: **STM32L462RETx** (LQFP64)  
System clock: **80 MHz** (PLL from MSI)  
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

### SPI1 — 40 Mbit/s, Full Duplex Master

**Device:** ADS1292RIRSMR (24-bit ADC, ECG/biopotential)  
**Pins:** PA1 (SCK), PB4 (MISO), PB5 (MOSI), PC1 (CS), PC0 (INT)  
**Status:** not tested

<details>
<summary>Code snippet</summary>

```c
// add working snippet here
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
**GPIO:** PB12 (SD_DET, card detect input), TPS_ON/PWR_GATE (pin TBD — see note)  
**Clock:** 48 MHz  
**Notes:** 47k pull-ups on D0–D3 and CMD; ESD protection (PESD3V3U1UL)  
**Status:** not tested

<details>
<summary>Code snippet</summary>

```c
// add working snippet here
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
