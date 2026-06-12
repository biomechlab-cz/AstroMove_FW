# AstroMoWe Firmware Plan

---

## Phase 1 — Sensor acquisition and SD logging

This is the core feature. All other phases build on top of this.

---

### Sensors and sample rates

| Sensor | Interface | Rate | Data per sample | Throughput |
|--------|-----------|------|-----------------|------------|
| ADS1292R CH1 + lead-off status | SPI1, DRDY interrupt | 1000 Hz | 6 bytes (3 status + 3 CH1) | 6.0 KB/s |
| ISM330DHCX accel + gyro | I2C1, burst read | 100 Hz | 12 bytes (3 × accel + 3 × gyro, 16-bit each) | 1.2 KB/s |
| MMC5983MA magnetometer | I2C1, burst read | 100 Hz | 9 bytes (3 × 24-bit axis, packed) | 0.9 KB/s |

**Total raw throughput: ~8.1 KB/s**

Lead-off detection for ADS1292R comes from bits [22:19] of the 24-bit status header — no extra acquisition needed, it is part of every 9-byte frame.

---

### Log file format — InfluxDB line protocol

One line per ADS1292R sample, written in 1000-sample blocks (~1 s each):

```
ecg ch1=<signed_int>i,loff=<uint8>i <timestamp_ms>
```

- `ch1` — 24-bit signed ADC count (CH1 only; CH2 not used yet)
- `loff` — raw status byte from the 9-byte ADS frame (bits 22:19 are lead-off flags)
- `timestamp_ms` — `HAL_GetTick()` value at the nearest 100-sample boundary + intra-group ms offset; starts from 0 at each boot

File header (written by `ACQ_WriteDiagnostics`, ignored by InfluxDB):
```
# ID=53 CONFIG1=03 CONFIG2=A0 ... RESP1=F2 RESP2=03
# DRDY_SDATAC=1 RDATA=C00000 ...
```

Filename: `log_NNNN.lp` (NNNN increments by finding the first unused filename on the SD).

Import command: `influx write --precision ms -f log_0000.lp`

Visualisation: `python plot_ecg.py log_0000.lp` (see `plot_ecg.py` in project root).

**Future:** when IMU data is added, a second measurement (`imu`) will be interleaved at 100 Hz. When Phase 2 sync is implemented, `timestamp_ms` will reset to 0 at the sync trigger so all devices share the same time base.

---

### RAM buffer

STM32L462 has **160 KB SRAM**. Deducting ~25 KB for stack, HAL, FatFS, and other allocations, approximately **128 KB** is available for the acquisition buffer.

```
128 KB / 95 bytes per frame = ~1,380 frames ≈ 13.8 seconds of data
```

The buffer is a **flat byte array** used as a ring/ping-pong buffer:
- Half the buffer fills while the other half is being written to SD.
- Flush is triggered when the active half is full (~6.9 s of data per flush, ~64 KB write).

64 KB written every ~7 s to SD → average SD write load is very low, SD can be idle most of the time.

---

### SD card write strategy

- Keep SD card in **idle/standby** between flushes; do not power it off (FatFS needs the file handle).
- Write one large contiguous block per flush (~64 KB) rather than many small writes. Large writes are faster and the SD card sleeps longer between them.
- If write takes longer than the buffer can absorb (unlikely at 9.5 KB/s, SD writes typically 1-4 MB/s), the QSPI flash can act as overflow (see below).
- File opened once at startup, appended continuously, closed on power-off or graceful stop.

---

### QSPI flash cache (MX25R3235FM1xx1, 4 MB) — deferred

> Testing required to determine if this is needed.

The QSPI flash can hold **4 MB / 9.5 KB/s ≈ 7 minutes** of raw data. Possible roles:

| Option | Description |
|--------|-------------|
| **Overflow buffer** | Spill to QSPI if RAM buffer fills before SD write completes |
| **SD duty-cycle reducer** | Fill QSPI for several minutes, then batch-flush to SD in one long write; SD stays off the rest of the time |

The SD duty-cycle option could significantly reduce average power (SD write current ~100 mA, QSPI write ~5 mA). Evaluate after baseline SD-only implementation is working.

---

### Power budget

**Battery:** 800 mAh @ 3 V with step-up. Assuming ~87% step-up efficiency → ~696 mAh effective at 3.3 V rail.

| Consumer | Estimate | Notes |
|----------|----------|-------|
| STM32L462 at 80 MHz | ~15 mA | Can reduce by lowering clock or using LP run mode between flushes |
| ADS1292R | ~1 mA | Always on during measurement |
| ISM330DHCX | ~0.5 mA | 104 Hz ODR (closest to 100 Hz) |
| MMC5983MA | ~0.4 mA | Continuous measurement mode |
| SD card idle | ~0.5 mA | Between flushes |
| SD card writing | ~100 mA | Active for <50 ms per 7 s cycle → avg ~0.7 mA |
| **Total estimate** | **~18–20 mA** | |

**Estimated runtime: 800 mAh / 19 mA ≈ 42 hours** at continuous acquisition.

Key optimisation levers (in priority order):
1. **MCU clock scaling** — drop to 16 or 4 MHz between the 1 kHz ADS interrupts if possible (saves ~8-10 mA).
2. **SD card power gate** — TPS_ON (PB11) cuts SD card VDD. Pull LOW to power off SD between flush cycles; pulse HIGH, re-init, write, power off again. Pairs naturally with the QSPI buffer option.
3. **QSPI as SD replacement** — log to QSPI only, batch transfer to SD once per hour; SD off the rest of the time.

---

### Acquisition loop structure

```
Startup
├── ADS1292_Init (SPI, SDATAC, register config)
├── TPS_ON → SD card power, 500 ms settle
├── Arm PC0 as EXTI0 falling-edge (GPIO_PULLUP — DRDY is open-drain)
├── ACQ_Init: SD 4-bit 480 kHz, f_mount, open log_NNNN.lp
├── ACQ_WriteDiagnostics: register readback → log file header comments
└── ADS1292_StartContinuous: START + RDATAC

EXTI0 ISR (fires on every DRDY falling edge, ~1 kHz)
└── g_isr_count++; g_drdy_flag = 1

Main loop
├── ACQ_Process:
│   ├── if !g_drdy_flag → return
│   ├── g_drdy_flag = 0
│   ├── ADS1292_ReadRaw (9 bytes, RDATAC mode)
│   ├── store CH1 + loff + isr_count snapshot
│   ├── capture HAL_GetTick() at every 100th sample
│   └── when 1000 samples full → flush block to SD as LP lines, blink LED
└── 1 Hz LED heartbeat toggle

100 Hz IMU read  [NOT YET IMPLEMENTED]
├── Read ISM330DHCX burst (12 bytes)
├── Read MMC5983MA burst (9 bytes)
└── Append imu measurement lines to log
```

**Current status (2026-05-25):** ADS1292R acquisition working at 1 kSPS via RDATAC + DRDY interrupt. 1000-sample blocks flush to SD every ~1 s in InfluxDB line-protocol format. Timing verified: `isr_count` increments by 1 per sample; `HAL_GetTick()` boundary snapshots confirm 100 samples = 100 ms. IMU acquisition not yet implemented.

ADS interrupt timing is tight (1 kHz = 1 ms budget). Keep the ISR minimal — only flag set. All data read and frame assembly happens in the main loop.

---

### Open questions before implementation

- [x] ISM330DHCX ODR → **104 Hz** confirmed (register value 0x40 for CTRL1_XL/CTRL2_G). Slight mismatch with ADS; one extra I2C read every ~10 s, acceptable.
- [x] TPS_ON (PB11) controls **SD card VDD** — use it to fully power-gate the SD between flush cycles.
- [ ] Decide whether to timestamp per-frame (ms resolution) or per-sample (interpolated between 100 Hz ticks). Per-frame is simpler and sufficient.
- [ ] Define final file format. **TBD — likely encrypted binary.** Implement as human-readable CSV first; swap format later.

---

## Phase 2 — Multi-device synchronisation

Deferred — implement after Phase 1 is stable.

### Summary

All devices share SYNC_A (PC6) and SYNC_B (PC7) as an open-drain bus. The first device to power on becomes master, claims SYNC_A HIGH, and waits. Each subsequent device pulses SYNC_B for 10 ms to announce itself. After a configurable timeout the master pulls SYNC_A LOW — all devices latch their timer at that moment, establishing a shared t = 0.

Any number of devices supported (2, 3, 4 …). Sync timeout TBD.

Full details preserved in git history — see earlier revision of this file.

---

## Phase 3 — Post-acquisition

> To be defined.
