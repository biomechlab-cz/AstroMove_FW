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
  present.
- Known hardware limitation on this PCB revision: **D1/D2/D3 data lines**
  (PC9/PC10/PC11) have marginal solder joints on the Hirose DM3AT connector.
  Firmware is locked to **1-bit SDMMC mode** until those joints are reflowed.
  4-bit mode returns `HAL_SD_ERROR_DATA_TIMEOUT (0x20)`.

---

## 5. Known hardware issues (this PCB revision)

| Issue | Symptom | Boards affected | Workaround |
|-------|---------|-----------------|------------|
| BOOT0 floating (pin 60) | Boots to ST bootloader, HAL_Delay never returns | All | Set option bytes (step 1) |
| SDMMC D1-D3 marginal solder joints | 4-bit mode `DATA_TIMEOUT (0x20)` | 05 confirmed, others TBD | Stay in 1-bit mode |
| MMC5983MA dead chip | I2C scan: 0x30 absent | **05 only** | Replace U5; board 04 confirmed working |

---

## 6. Board status log

| Board | MCU option bytes set | All sensors verified | Notes |
|-------|---------------------|----------------------|-------|
| 04    | ✓ | ✓ ISM330DHCX, MMC5983MA, RV-3028-C7, ADS1292R | Reference board |
| 05    | ✓ | ✗ MMC5983MA dead | Dead U5 chip; SDMMC D1-D3 marginal |
