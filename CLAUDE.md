# AstroMoWe — NewProject

Full hardware context: [`HARDWARE.md`](HARDWARE.md)

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
| GPIO     | PC6  | SYNC_A — shared open-drain multi-device sync signal (sync.c) |
| GPIO     | PC7  | SYNC_B — sync-cable presence (LOW = cable in) |

Disabled in main.c (not yet needed): QUADSPI, USB, I2C2.  
I2C2 permanently disabled — PB11 repurposed as TPS_ON GPIO.

## Active board: **04** (board 05 has dead MMC5983MA — see BOARD_SETUP.md)

## Tested / working

| Feature | Status | Notes |
|---------|--------|-------|
| USART1 Hello World | in progress | 115200 8N1, ST-Link V3 MINI virtual COM |
| SDMMC1 + FatFS write | working | 1-bit mode, ClockDiv=10, GPIO_PULLUP/MEDIUM; D1-D3 bad solder on board 05 |
| ISM330DHCX (IMU) | **not responding** (found 2026-06-11) | NACKs at both 0x6A and 0x6B (addr auto-probed at init); mag+RTC on same bus fine → chip dead or unpowered. Accel/gyro record as zeros. Lowering I2C to ~10 kHz with pull-ups did **not** help (still address-NACK, `g_i2c_last_err`=AF, while mag works on the same bus) → confirmed chip-level, not bus signal integrity; check IMU power/SA0/solder |
| MMC5983MA (mag) | working | I2C addr 0x30, board 04 only; board 05 chip dead |
| RV-3028-C7 (RTC) | working | I2C addr 0x52; time not yet set |
| ADS1292R (ECG ADC) | working | SPI1, ID=0x53, CPOL=0 CPHA=1, 500 kHz, 1 kSPS (CONFIG1=0x83 HR=1); DRDY non-functional on board 04 — use SDATAC+RDATA polling; EMG capture confirmed. **DC lead-off comparators are OFF** (CONFIG2=`0xA0`): on the 2-electrode high-Z textile setup the comparators/current sources unbalance the inputs and convert mains common-mode to differential, swamping the EMG with 50 Hz (confirmed: 50 Hz dropped 2847 µV→230 µV when disabled). Lead-off is instead detected in **software** (signal-based, see acquisition.c + FORMAT.md `ch1_leadoff_chunks`). HW comparator settings (`ADS1292_LOFF_CONFIG` = `ILEAD_OFF` 6 µA, tunable in ads1292.h) are retained for future RLD/3-electrode experiments but currently inert. Signal-saturation detection still runs (`ch1_saturated_samples`) |
| Status LED (PB10) | working | single-colour, active-high; driven by `Core/Src/led.c` from SysTick — see below |

## Status LED indication (`Core/Src/led.c`)
Single LED on PB10 today, written RGB-ready (set `LED_HW_RGB=1` + wire 3 pins later — colour per state already in the pattern table). Driven from `SysTick_Handler` (1 ms) so the rhythm survives blocking SD writes. State set by `main.c` (boot/ready) and `ACQ_Process()` (live signal quality / faults). Fatal states latch.

| State | Rhythm (mono) | Colour (future RGB) | Trigger |
|-------|---------------|---------------------|---------|
| BOOT | solid | blue | init peripherals / mount SD / open session |
| READY | 4 Hz blink (~1 s) | cyan | init OK, acquisition armed |
| RECORDING | 50 ms wink / s | green | acquiring, electrodes good |
| LEADOFF | 2 Hz even blink | yellow | CH1 bad contact: signal-based lead-off (`s_bad_signal_on` from `analyze_signal_quality`), railed input (`g_saturated_active`), or HW comparator (`g_leadoff_active`, currently always 0 — comparators off) |
| WARN | double-blink / 1.5 s | amber | dropped samples (held 3 s) |
| FAULT_STORAGE | triple-blink burst | red | unrecoverable SD/record error (latches, spins) |
| FAULT_INIT | ~6 Hz frantic | red | SD missing / mount / session-open fail (latches) |
| SYNC | **solid** | magenta | multi-device sync: cable in, syncing — do not unplug yet. Mono: solid (not a blink) so it's unmistakable vs the winking RECORDING; solid→wink = recording started. (enum appended last so SWD `s_state` values 2/3/5 stay stable) |

- The ISR builds a normalized lead-off status byte via `ADS1292_NORMALIZE_STATUS` (ads1292.h); `g_leadoff_active` = `status & ADS1292_STATUS_CH1_LEADOFF`. This path is currently inert (HW comparators off, see ADS row) — the live LEADOFF indication comes from the **signal-based** detector in `analyze_signal_quality` (acquisition.c): a 1 s chunk whose sum-of-abs-sample-differences is **below** `QUALITY_LEADOFF_DIFF_SUM_COUNTS` (quiet/open electrode) **or above** `QUALITY_LEADOFF_HI_DIFF_SUM_COUNTS` (wild/floating electrode — see [[astromove-leadoff-two-signatures]]), but isn't flat, sets `bad_contact` → `s_bad_signal_on` → `LED_LEADOFF`, and increments `ch1_leadoff_chunks` in the CSV. Connected EMG sits in the band between the two thresholds. **Both thresholds calibrated in a mains environment — re-tune for battery** (`samples/_leadoff_perchunk.py`). If the HW comparators are re-enabled, verify raw bit positions via the sticky-OR debug globals `g_stat0_or`/`g_stat1_or`. IMU/mag I2C failures are **not** on the LED (ISM330 is dead on board 04 → would mask everything).
- Verified on board 04 over SWD: RECORDING (`s_state`=2) when CH1 connected, LEADOFF (`s_state`=3) when forced, FAULT_STORAGE (`s_state`=5) rendered correctly.

## I2C1 bus
- PA9=SCL and PA10=SDA are configured open-drain with pull-ups enabled (`GPIO_PULLUP` in `Core/Src/stm32l4xx_hal_msp.c` and the CubeMX `.ioc`). Use/keep board-level external pull-up resistors for reliable operation; the MCU pull-ups are only a firmware-side fallback.
- I2C1 uses PCLK1=16 MHz and timing `0x00503D58` (`I2C1_TIMING_100KHZ_16MHZ`) for standard-mode, about 100 kHz. Do not raise this toward fast-mode unless the board pull-ups and all sensors are revalidated.

## Data storage (EMGX format)
- **Authoritative spec: [`FORMAT.md`](FORMAT.md)** (EMGX v1: `version=1`, `payload_type=2`, normalized status byte, entropy nonce). Single source of truth for the format, shared with AstroMoWe_Inspect.
- Writer: `Core/Src/recording.c` — AES-256-GCM (hardware AES peripheral), 10 s batches of 1 s chunks, CRC32 + GCM tag per batch
- Key: `recording.key` (repo root, **placeholder test key** — replace before deployment); CMake generates `recording_key.h` from it at configure time, `decode_emgx.py` reads it directly
- Files per session: `SNNNNNNN.EMX` + `SNNNNNNN.CSV` (8.3 names — FatFS `_USE_LFN=0`; enable LFN in CubeMX for the spec's `SESSION_NNNNNNN.emgx`)
- Desktop decoder: `decode_emgx.py` (needs `pip install cryptography`)
- GCM nonce = 8-byte per-session salt (`nonce_scheme=1`, entropy from ADS analog noise via `ACQ_SeedNonce`) + batch index. Cross-session uniqueness does not depend on the RTC or on monotonic session ids.
- IMU recorded at 100 Hz (payload type 2): accel+gyro int16 (ISM330, currently zeros — chip not responding) + mag 18-bit uint32 (MMC5983 continuous mode); IMU slots derived from the EMG sample clock (every 10th sample) so counts are exact; sample-and-hold across blocking writes and on I2C failure (`g_ism_fail_count`/`g_mag_fail_count`)
- EMG = ADS ch1 int32 only; per-sample lead-off status is **not** stored — the LED gives live feedback and each batch's CH1 quality summary goes to the control CSV: `ch1_saturated_samples`, then the mutually-exclusive per-chunk states `ch1_flatline_chunks` → `ch1_leadoff_chunks` (signal-based, the live indicator) → `ch1_baseline_drift_chunks`, plus the continuous level metric `ch1_diff_abs_sum_min/med/max` (lets thresholds be re-tuned from the CSV without decoding the EMX — see FORMAT.md §8). Do not re-add hardware-comparator sample-count columns or an aggregate quality bitmask unless the firmware and spec are intentionally redesigned. Chunk = ch1[1000] + imu[100] = 6400 B; `payload_type=2` identifies the layout.

## Board 04 quirks found 2026-06-10/11
- **BOOT0 pin reads high** — option bytes set to `nSWBOOT0=0, nBOOT0=1` (always boot main flash, BOOT0 pin ignored). Without this the MCU boots into the ROM bootloader and the firmware never runs. Re-apply after any full chip erase / option-byte reset.
- **SDMMC TX underrun whenever the polled FIFO feed is starved** (also killed the old firmware — see 20-30 s LOG_*.BIN files). HAL aborts the write without CMD12, wedging the card in receive state. Two-layer mitigation, do not remove when regenerating from CubeMX:
  - abort (CMD12) + bounded-wait + retry ×3 in `SD_read`/`SD_write` (sd_diskio.c USER CODE) — residual underruns (~2/batch) recover on first retry
  - SDMMC CLKDIV=100 (≈470 kHz, ~235 KB/s) in `sd_init_4bit()` — at CLKDIV=50 the half-FIFO refill deadline (~70 µs worst case) is shorter than the 94 µs DRDY ISR and every multi-block write fails
  - **Live SWD memory reads halt the core and starve the FIFO too** — polling globals with `STM32_Programmer_CLI ... -r32` during recording spiked `g_sd_retry_count` to ~1000 and forced a (recovered) underrun fault; untouched, the same firmware runs at ~2 retries/batch, 0 fails. Read sparingly, ideally between batches.
- **DRDY edge rate ~1050/s at 1 kSPS** (~5% above spec+tolerance); ISR debounces edges <800 µs apart (DWT-based, `g_spurious_count`).
- **ADS1292R decodes MOSI as commands even in RDATAC** — any SPI read must transmit 0x00. A read that clocks out garbage stops conversions dead (DRDY stuck low) within seconds.

## Acquisition architecture (zero-loss, 2026-06-11)
- DRDY ISR reads the frame itself (`ADS1292_ReadRawFast` — direct-register SPI, ~40 µs; HAL version is ~240 µs and starves SDMMC) into a 1024-sample ring; main loop drains ring → encrypt → SD write. `dropped_samples` in the control CSV = ring overflow only (expected 0).
- SPI1 runs 500 kHz for ADS commands/registers (inter-byte decode time), switched to 4 MHz after RDATAC for data-phase reads (main.c).
- EXTI0 must be armed only **after** `ADS1292_StartContinuous()` — the ISR's SPI reads must never overlap main-thread SPI.
- Debug globals readable over SWD: `g_isr_count/g_spurious_count/g_dropped_count/g_isr_max_cycles/g_sample_index` (acquisition.c), `g_rec_fail_point/code` (recording.c), `g_sd_retry_count/g_sd_fail_count/g_sd_last_stage/g_sd_last_err` (sd_diskio.c).

## Multi-device sync (`Core/Src/sync.c`, FORMAT.md §10, sync_protocol.md)
- Synchronizes N devices' EMG sample clocks over a shared bus: **PC6 (Sync A)** = shared **open-drain** signal net (wired-AND, any device pulls low → all see the edge simultaneously); **PC7 (Sync B)** = presence (LOW = cable in). The shared electrical edge — not a mechanical unplug — is the only simultaneous event across devices.
- `SYNC_Run()` runs in `main.c` **after** EXTI0 is armed (so `g_sample_index` is ticking) and **before** the session opens. Boot order changed: `ACQ_Init()` now only does SD-mount + ADS-reg snapshot; the session is opened by **`ACQ_OpenSession(synced, sync_lead)`** after sync. The documented orderings (reg snapshot before RDATAC, `SeedNonce` before EXTI, SPI 500 k→4 M) are preserved.
- Each device emits one pulse, then re-latches `g_sample_index` (and zeroes the RTC, coarse) on every PC6 edge **until it is unplugged**; the last edge is common to all devices still on the bus. On unplug: `ACQ_ResetRing()` (discard sync-wait samples) → record. Header stores `synced` (byte 39) + `sync_lead_samples` (bytes 60-63) = samples from the pulse to the first recorded sample; subtract across devices to align (FORMAT.md §10).
- Each device staggers its pulse by a UID-derived delay (`SYNC_PULSE_STAGGER_MS`, 0–511 ms) so devices booting together off one power source don't pulse simultaneously (coincident pulses merge on the wired-AND line → no distinct peer edge observed). The last (separated) pulse is the common edge everyone latches.
- **Operational rule:** connect the cable **before** power-on (sync is boot-only — plugging into a running device does nothing), power all devices on together, and don't unplug any until all show **solid** `LED_SYNC` — an early unplug freezes on an earlier pulse. Sample-accurate (sub-ms); RTC zero is the ~1 s fallback. Sync only reconfigures PC6/PC7 by pin (PC0 DRDY / PC1 CS untouched). No cable at boot → `synced=0`, records standalone. NB: on the **mono** LED, SYNC is **solid** and RECORDING is the wink — watch for the solid→wink transition at unplug.

## SD card config notes
- **4-bit ~470 kHz validated** — `sd_init_4bit()` in `Core/Src/acquisition.c`; see BOARD_SETUP.md §6 for full findings
- Init sequence: 1-bit ClockDiv=10 → GPIO VERY_HIGH → 4-bit → ClockDiv=100 (48 MHz/(CLKDIV+2) ≈ 470 kHz; do not lower CLKDIV — see TX underrun note above)
- `DISABLE_SD_INIT` defined in sd_diskio.c — `HAL_SD_Init` called manually before `f_mount`
- Do NOT skip the GPIO speed override after `HAL_SD_Init` — MspInit leaves it at MEDIUM which is too slow for 4-bit
- Do NOT use 4-bit at ClockDiv≤10 (2.4 MHz+) — 47 kΩ pull-ups fail at those speeds (see BOARD_SETUP.md §6)

## Build & flash
STM32CubeIDE. Debug config: `NewProject Debug.launch`.
