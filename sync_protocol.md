# Sync Protocol

Synchronizes the EMG sample clocks of **N devices** (2–4+ in practice) so their
recordings can be aligned to a common instant. This document covers the hardware
and the on-device handshake; what the **files** store and how a reader aligns
them is in [`FORMAT.md`](FORMAT.md) §10 (authoritative for the file fields).

Implemented in `Core/Src/sync.c` (`SYNC_Run()`), called from `main.c` after the
DRDY ISR is armed and before the recording session is opened.

## Hardware Lines

| Pin | Role        | Description                                                        |
|-----|-------------|--------------------------------------------------------------------|
| PC6 | Sync A      | **Shared signal net** — open-drain + pull-up, common to all devices |
| PC7 | Sync B      | Presence detection — shorted to GND through the connector          |

The sync cable ties **all devices' PC6 together** into one electrical node and
shorts every device's PC7 to GND. The cable provided shorts the red and green
wires; it is made with a right angle right off the connector and the wires run
away from the board, so that the red wire is next to the ST-LINK connector.

**PC6 (Sync A) detail:** open-drain with a pull-up, so the net idles HIGH and any
device can pull it LOW without contention (wired-AND). One device's LOW pulse is
therefore seen by **every** device on the net at the same instant (wire-propagation
skew ≪ one sample period). This shared edge — not any mechanical plug/unplug — is
the only event that is genuinely simultaneous across the group, so it is the
synchronization reference.

**PC7 (Sync B) detail:** pulled high internally. When the connector is plugged in,
Sync B is shorted to GND, so PC7 reads LOW (cable present). When absent, PC7 floats
HIGH via the pull-up.

---

## Sequence

```
Device powers on, peripherals up, DRDY ISR armed (EMG sample clock running)
       │
       ▼
  Read PC7
       │
  PC7 == LOW? ──── No ──► Connector absent → synced = 0, start recording
       │
      Yes  (cable present)
       │
       ▼
  Emit one LOW pulse on PC6, latch own sample index, zero RTC   (LED = SYNC)
       │
       ▼
  ┌─── Re-latch loop (while PC7 == LOW) ───────────────────────┐
  │   On each LOW edge seen on PC6 (a pulse from any device):  │
  │       latch current sample index, zero RTC                 │
  └────────────────────────────────────────────────────────────┘
       │
  PC7 goes HIGH (this device unplugged)
       │
       ▼
  Freeze the last latched index = sync reference
  Reset the sample ring, open session (header stores synced + sync_lead_samples)
  Start recording
```

## State Description

### 1. Connector absent at power-on
PC7 is HIGH → no cable. Sync is skipped, `synced = 0`, recording starts normally.
Single-device operation is unaffected.

### 2. Connector present — arm and listen
PC7 is LOW. The device emits one pulse on the shared net (announcing itself to any
device that armed after it) and latches its own pulse as the initial reference.
It then re-latches on every LOW edge it sees, holding the **last** one. Each device
pulses exactly once on arming, so the last edge on the net comes from the last
device to arm, and every device still on the bus latches that same edge.

### 3. Unplug → record
When **this** device's PC7 goes HIGH, the re-latch loop exits and the last latched
index becomes the sync reference. Recording starts from a clean ring; the header
records `sync_lead_samples = first_recorded_index − reference_index`. Because
alignment is locked to the shared pulse, devices may be unplugged one at a time —
the staggered starts are corrected by each file's `sync_lead_samples`.

---

## Operational requirements

- **Cable must be connected before power-on.** Sync runs once, at boot, only if PC7
  is already low; plugging the cable into already-running devices does nothing. Power
  (or power-cycle) the devices with the cable already attached.
- **Power all devices on within the same window**, before any is unplugged. A device
  unplugged before the last peer has armed freezes on an earlier edge and is misaligned.
  Each device staggers its own pulse by a UID-derived delay (`SYNC_PULSE_STAGGER_MS`),
  so one shared power source / simultaneous power-on is fine — the pulses still separate
  into a clean last edge.
- The **SYNC LED** is **solid** (magenta on RGB) while the cable is present —
  *do not unplug until every device shows solid*, i.e. all have armed. When you unplug,
  the LED drops to the recording wink; that solid→wink transition is the "go" signal.
- If only one device is on the bus it syncs to its own pulse (harmless); with no
  cable it records standalone (`synced = 0`).

## Precision

The reference is one shared electrical edge, latched in firmware on each device, so
alignment is sample-accurate to within each device's pulse-detection latency
(sub-millisecond ≪ one 1 kHz sample). Zeroing the RV-3028 RTC on each pulse gives an
additional **coarse** (~1 s) common timestamp as a cross-check / fallback.
