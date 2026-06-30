# Sync Protocol

Synchronizes the EMG sample clocks of **N devices** (2–4+ in practice) so their
recordings can be aligned to a common instant, **and stamps them with a shared
`group_id`** so the recordings from one run can be paired. This document covers the
hardware and the on-device handshake; what the **files** store and how a reader
aligns/pairs them is in [`FORMAT.md`](FORMAT.md) §10 (authoritative for the file fields).

Two things ride the shared line, told apart by how long it is held LOW: a short
(~20 ms) **sample-sync pulse** (the alignment reference) and a longer (~60 ms start)
**group-id frame** (the pairing id). The group-id layer never disturbs the alignment.

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
  LED = SYNC (solid); wait a per-device UID stagger
  (so simultaneous boots don't collide on the wired-AND net)
       │
       ▼
  Emit one LOW pulse on PC6, latch own sample index, zero RTC
       │
       ▼
  ┌─── Listen loop (while PC7 == LOW) ─────────────────────────────────────┐
  │   classify each LOW by duration:                                       │
  │     short (~20 ms) = sample pulse  → latch sample index, zero RTC       │
  │     long  (~60 ms) = group-id frame → decode 16-bit id → group_id       │
  │   leader only (shortest stagger), once the bus is quiet (all armed):    │
  │     broadcast the group-id frame ×3, then go silent                     │
  └────────────────────────────────────────────────────────────────────────┘
       │
  PC7 goes HIGH (this device unplugged)
       │
       ▼
  Freeze the last latched index = sync reference
  Reset the sample ring, open session
    (header: synced + sync_lead_samples;  control CSV: group_id)
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
When **this** device's PC7 goes HIGH, the listen loop exits and the last latched
index becomes the sync reference. Recording starts from a clean ring; the header
records `sync_lead_samples = first_recorded_index − reference_index`. Because
alignment is locked to the shared pulse, devices may be unplugged one at a time —
the staggered starts are corrected by each file's `sync_lead_samples`.

### 4. Group id (pairing)
Runs concurrently with the listen loop, on the same line, without disturbing the
sample reference (the frame's long start marker is classified as a frame, never a
sample pulse).

- **Leader election** is implicit: a device that heard the line go low during its
  stagger is a **follower**; the one whose stagger elapsed with the line quiet (the
  shortest stagger) is the **leader**. The stagger is an FNV-1a hash of the **full 96-bit
  UID**, so no two boards — even from the same wafer/lot, which share the low UID bits — tie
  for shortest. With simultaneous power-on the leader fires while every other device is still
  staggering = listening, so all become followers.
- **Broadcast:** once the bus has been quiet for ~400 ms (all devices have emitted
  their announce pulse = all armed), the leader sends the **frame** — a ~60 ms start
  marker, then 16 id bits + an 8-bit checksum, NRZ at ~8 ms/bit — **three times**
  back-to-back (covers loss and a late joiner). The id is 16 bits of the leader's
  per-session ADS analog-noise entropy (`ACQ_SyncSeed()`), so it differs every run.
- **Receive:** followers (and the leader's own loop) classify the long start, sample
  the bits at their mid-points, verify the checksum, and store the id as `group_id`.
- **Collision (safety net):** ties for shortest stagger are now unlikely (the stagger is a
  full-UID hash, not `w0 % 512`), but if two devices still coincide and both lead, each reads
  the bus back while driving a `1` bit; a mismatch means another driver → both abort and
  `group_id` collapses to `0` (consistent across all → heuristic-pairing fallback).
- `group_id` is written to the **control CSV** (plaintext), so the desktop tool pairs
  cards by equal `group_id` without decrypting (FORMAT.md §10.4).

---

## Operational requirements

- **Cable must be connected before power-on.** Sync runs once, at boot, only if PC7
  is already low; plugging the cable into already-running devices does nothing. Power
  (or power-cycle) the devices with the cable already attached.
- **Power all devices on within the same window**, before any is unplugged. A device
  unplugged before the last peer has armed freezes on an earlier edge and is misaligned.
  Each device staggers its own pulse by a delay hashed (FNV-1a) from its **full 96-bit UID**
  (`SYNC_PULSE_STAGGER_MS` max), so one shared power source / simultaneous power-on is fine —
  the pulses still separate into a clean last edge, and even same-wafer boards (which share the
  low UID bits) get distinct delays instead of tying.
- The **SYNC LED** is **solid** (magenta on RGB) while the cable is present —
  *do not unplug until every device shows solid*, i.e. all have armed. When you unplug,
  the LED drops to the recording wink; that solid→wink transition is the "go" signal.
- If only one device is on the bus it syncs to its own pulse (harmless); with no
  cable it records standalone (`synced = 0`).
- **`group_id` is most reliable with simultaneous power-on** (shared source): the
  leader election needs the other devices to still be staggering = listening when the
  leader fires. With widely staggered power-on a late device can self-elect as a second
  leader → the id collapses to `0` and pairing falls back to the heuristic. **Sample
  alignment is unaffected** either way (it rides the announce pulses, not the frame).

## Precision

The reference is one shared electrical edge, latched in firmware on each device, so
alignment is sample-accurate to within each device's pulse-detection latency
(sub-millisecond ≪ one 1 kHz sample). Zeroing the RV-3028 RTC on each pulse gives an
additional **coarse** (~1 s) common timestamp as a cross-check / fallback.
