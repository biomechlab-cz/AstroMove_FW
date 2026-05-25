# AstroMowe Board Setup Procedure

Follow this checklist every time a **new board** is programmed for the first time.

---

## 1. Option bytes — must be done before first flash

The BOOT0 pin (MCU pin 60) is floating on the PCB. By default the STM32L4
reads BOOT0 as HIGH and boots into the ST system bootloader instead of user
flash, so the device appears to do nothing and HAL_Delay never returns.

**Fix (one-time per chip, survives power cycles):**

1. Open **STM32CubeProgrammer** (≥ v2.x).
2. Connect ST-Link V3 MINI via USB, click **Connect**.
3. Go to the **OB** tab (Option Bytes).
4. In the **User Configuration** section set:
   - `nSWBOOT0` (labelled **SW_BOOT0** in CubeProgrammer) = **0**  ← BOOT0 taken from nBOOT0 bit, not the physical pin
   - `nBOOT0`   = **1**  ← effective BOOT0 = 0 → boot from flash
   - `nBOOT1`   — leave unchanged
5. Click **Apply**.
6. The board will reset and now always boots from user flash.

> **Why:** nBOOT_SEL=0 disconnects the physical BOOT0 pin from the boot
> decision. nBOOT0=1 forces BOOT0=0 (flash boot). The floating pin is then
> irrelevant.
>
> **Next PCB revision:** add a 10 kΩ pull-down resistor to GND on pin 60
> (BOOT0) so the default silicon behaviour is correct without option byte
> changes.

---

## 2. Flashing user firmware

Use **STM32CubeIDE** with the debug configuration included in the project:

| Config | File | Use |
|--------|------|-----|
| Debug  | `AstroMoveCubeMX Debug.launch` | Flash + attach debugger |
| Run    | `AstroMoveCubeMX Run.launch`   | Flash only, no debugger |

Or flash manually from CubeProgrammer: **Erasing & Programming** tab →
select `build/Debug/AstroMoveCubeMX.elf` → **Start Programming**.

---

## 3. Verifying the board is alive

After flashing, the LED on **PB10** (solder pad) should blink at 1 Hz
once the while-loop is reached. If the LED does not blink at all the most
likely cause is the option bytes have not been set (see step 1).

---

## 4. SD card

- Format card as **FAT32** before first use.
- Card detect is active LOW on **PB12** — LED on SD_DET net means card is

---

## 5. Known hardware issues (this PCB revision)

| Issue | Symptom | Boards affected | Workaround |
|-------|---------|-----------------|------------|
| BOOT0 floating (pin 60) | Boots to ST bootloader, HAL_Delay never returns | All | Set option bytes (step 1) |
| SDMMC D1-D3 marginal solder joints | 4-bit mode `DATA_TIMEOUT (0x20)` | 05 confirmed, others TBD | Use 4-bit at 480 kHz (ClockDiv=50) — see §6 |
| MMC5983MA dead chip | I2C scan: 0x30 absent | **05 only** | Replace U5; board 04 confirmed working |

---

## 6. SDMMC performance findings

### What was tested (boards 04 and 05)

| Mode | ClockDiv | Clock | Result |
|------|----------|-------|--------|
| 1-bit | 10 | 2.4 MHz | **Working** — baseline confirmed |
| 4-bit | 50 | 480 kHz | **Working** — 395 KB/s |
| 4-bit | 10 | 2.4 MHz | FAIL — FR_DISK_ERR |
| 4-bit | 4 | 6 MHz | FAIL — FR_DISK_ERR |
| 4-bit | 2 | 12 MHz | FAIL — FR_DISK_ERR |
| 4-bit | 1 | 24 MHz | FAIL — FR_DISK_ERR |

SDMMC clock formula: `48 MHz / (2 × ClockDiv)`

### Root cause: 47 kΩ pull-up resistors (R14, R15, R16, R17, R19)

The data and CMD lines are pulled HIGH through 47 kΩ external resistors. With PCB trace + SD card pad + MCU input capacitance (~15 pF), the RC time constant is:

```
τ = 47 kΩ × 15 pF ≈ 705 ns
```

At 2.4 MHz the half-period is only 208 ns (≈ 0.3τ) — the line never rises to a valid logic HIGH before the next clock edge. The STM32 internal pull-up (~40 kΩ) is enabled in firmware and helps slightly (47k ∥ 40k ≈ 21.7 kΩ, τ ≈ 325 ns), which is why 480 kHz barely passes and 2.4 MHz does not.

1-bit mode works at 2.4 MHz because in 1-bit only D0 is active; the card's internal driver dominates data transitions and D1–D3 idle noise is absent.

### Current board workaround

- Use **4-bit at 480 kHz (ClockDiv=50)** → 395 KB/s
- Phase 1 logging needs only ~12 KB/s (64 KB flush every ~14 s), so this is 33× headroom
- Implemented in firmware: `MODIFY_REG(SDMMC1->CLKCR, SDMMC_CLKCR_CLKDIV, 50U)` after `HAL_SD_ConfigWideBusOperation(SDMMC_BUS_WIDE_4B)`
- GPIO must be `GPIO_SPEED_FREQ_VERY_HIGH` (MspInit sets MEDIUM — override after `HAL_SD_Init`)

### Short-term improvement (current PCB)

Replace R14, R15, R16, R17, R19 with **4.7 kΩ**:

```
τ = 4.7 kΩ × 15 pF ≈ 70 ns
At 2.4 MHz (half-period 208 ns ≈ 3τ): signal reaches ~95% → valid
At 6 MHz   (half-period  83 ns ≈ 1.2τ): marginal, likely passes
```

Expected result after reflow: 4-bit at 2.4–6 MHz → ~2–4 MB/s writes.

### Next PCB revision recommendations

| Item | Current | Recommended | Reason |
|------|---------|-------------|--------|
| DAT0–DAT3 pull-ups | 47 kΩ | **2.2 kΩ** | τ ≈ 33 ns; valid up to ~12 MHz |
| CMD pull-up | 47 kΩ | **2.2 kΩ** | same RC constraint |
| Series R (DAT/CMD) | 22 Ω | keep or 0 Ω | 22 Ω negligible vs 2.2 kΩ; remove only if pushing >12 MHz |
| BOOT0 pull-down | missing | 10 kΩ to GND | eliminates option-byte requirement on new chips |
| SD card VDD bypass | 1 cap (placement TBC) | 100 nF + 10 µF close to J3 pin 4 | reduces write-current noise spikes |

With 2.2 kΩ pull-ups: τ ≈ 33 ns → comfortably valid at 12 MHz → expected ~6 MB/s in 4-bit mode.

---

## 7. ADS1292R ECG ADC findings

### What was tested (board 04, 2026-05-25)

| Test | Result |
|------|--------|
| SPI communication | Working — ID register returns 0x53 |
| Register readback | All registers return expected values after `ADS1292_Init` |
| 1 kSPS output rate | Working — isr_count increments by exactly 1 per sample; HAL_GetTick snapshots confirm 100 samples = 100 ms |
| CH1 ECG signal | Working — 50 Hz mains baseline + EMG burst activity captured |
| DRDY / EXTI0 | **Working** — RDATAC + EXTI0 falling-edge ISR confirmed on board 04 |

### DRDY root cause and fix

DRDY was reading permanently LOW before the pull-up was added. Root cause: **DRDY is an open-drain output** on the ADS1292R (datasheet §8.4). Without a pull-up the line floats at ~250 mV, which is below the STM32L4 LOW threshold (~1 V), so it reads as LOW continuously — no falling edge is ever generated for EXTI.

**Fix:** configure PC0 as `GPIO_MODE_IT_FALLING` + `GPIO_PULLUP` (internal ~40 kΩ). The pull-up holds the line at 3.3 V when the ADS releases it; the ADS then drives it LOW when data is ready, creating a clean falling edge.

Previous firmware used `GPIO_NOPULL` (set by `MX_GPIO_Init`). The pull-up must be applied **after** `MX_GPIO_Init` in user code, or `MX_GPIO_Init` must be updated.

### DRDY pitfalls

| Issue | Symptom | Root cause |
|-------|---------|------------|
| No pull-up on PC0 | Line sits at ~250 mV, EXTI never fires | DRDY is open-drain — requires pull-up to define HIGH state |
| `MX_GPIO_Init` sets PC0 `GPIO_NOPULL` | EXTI armed but ISR never fires | CubeMX default; override in USER CODE after `MX_GPIO_Init` |
| Measuring SPI clock with high-impedance probe at low timebase | Scope shows ~1.5 V instead of 3.3 V | Averaging artifact at 500 kHz — not a hardware fault |

### Critical register pitfalls

| Issue | Symptom | Fix |
|-------|---------|-----|
| CONFIG1 HR=0 (default, 0x03) | ~500 SPS instead of 1 kSPS | Use CONFIG1=0x83 (HR=1) |
| RESP1 bits[7:6] set with floating inputs | CH1 saturates to 0x7FFFFF | Use RESP1=0x02; re-enable respiration only with proper electrode circuit |
| LOFFSENS=0x0F with floating inputs | ~700k counts DC offset on CH1 | Expected — lead-off current sources active; not a fault |

---

## 8. Board status log

| Board | MCU option bytes set | All sensors verified | Notes |
|-------|---------------------|----------------------|-------|
| 04    | ✓ | ✓ ISM330DHCX, MMC5983MA, RV-3028-C7, ADS1292R | Reference board |
| 05    | ✓ | ✗ MMC5983MA dead | Dead U5 chip; SDMMC D1-D3 marginal |
