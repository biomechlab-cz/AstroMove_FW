# EMGX recording format — v1 (authoritative specification)

This is the single source of truth for the AstroMoWe encrypted recording format,
shared by the firmware writer (`AstroMove_FW`) and the inspector/decoder
(`AstroMoWe_Inspect`). It supersedes the prose in the `.docx` design note and the
inline comment in `Core/Src/recording.c`. Implementations (firmware C, Python
`decode_emgx.py`, Rust `astromowe-format`) **must** match this document.

All integers are **little-endian** unless stated otherwise.

---

## 1. Files

Each session produces two files that share a base name:

| File | Contents |
|------|----------|
| `SNNNNNNN.EMX` | encrypted binary measurement file (this spec, §3–§7) |
| `SNNNNNNN.CSV` | plaintext control file, one row per batch (§8) |

`NNNNNNN` is the zero-padded session id. Names are 8.3 because FatFS long-file-name
support is disabled (`_USE_LFN=0`); the design note's `SESSION_NNNNNNN.emgx` long
form is an optional future change (enable LFN in CubeMX) and does not affect the
byte format.

## 2. Layout overview

```
File header (64 B, plaintext)
repeat:
  Batch header (48 B, plaintext)
  Encrypted payload (ciphertext, = plaintext size; GCM does not expand)
  Batch trailer (24 B, plaintext: CRC32 + GCM tag + end marker)
[EOF]
```

A batch is the unit of encryption, integrity and fault recovery. A power loss only
risks the batch currently being written; all earlier batches stay decodable.

## 3. File header (64 bytes, plaintext)

| Offset | Size | Field | value / meaning |
|-------:|-----:|-------|-----------------|
| 0  | 4  | `magic` | `"EMGX"` |
| 4  | 1  | `version` | **1** |
| 5  | 1  | `header_size` | 64 |
| 6  | 1  | `payload_type` | **2** = EMG ch1 + IMU chunk (§5) — the sole chunk-layout discriminator |
| 7  | 1  | `batch_duration_s` | 10 |
| 8  | 4  | `session_id` | u32, matches the filename |
| 12 | 12 | `device_uid` | STM32 96-bit unique device ID |
| 24 | 6  | `start_time_bcd` | RTC BCD: `sec,min,hr,date,mon,yr` |
| 30 | 2  | `emg_rate_hz` | 1000 |
| 32 | 2  | `imu_rate_hz` | 100 |
| 34 | 1  | `cipher_id` | 1 = AES-256-GCM |
| 35 | 1  | `key_id` | from `recording.key` |
| 36 | 1  | `key_version` | from `recording.key` |
| 37 | 1  | `nonce_scheme` | **1 = entropy-seeded random salt + batch counter** (§6) |
| 38 | 1  | `emg_pga_gain` | ADS1292R CH1 PGA gain (4) — for scaling (§9) |
| 39 | 1  | `synced` | **1 = this session was multi-device synchronized** (§10); 0 = standalone |
| 40 | 8  | `nonce_prefix` | per-session nonce salt (§6) |
| 48 | 10 | `ads_regs` | ADS1292R snapshot: `ID,CONFIG1,CONFIG2,LOFF,CH1SET,CH2SET,RLDSENS,LOFFSENS,RESP1,RESP2` |
| 58 | 2  | `emg_ref_mv` | ADC reference in millivolts (2420 = internal 2.42 V) — for scaling (§9) |
| 60 | 4  | `sync_lead_samples` | u32: EMG samples from the shared sync pulse to the first recorded sample (§10); 0 if `synced = 0` |

## 4. Batch header (48 bytes, plaintext)

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 0  | 4  | `marker` | `"BATC"` |
| 4  | 4  | `batch_index` | u32, from 0 |
| 8  | 6  | `start_time_bcd` | RTC BCD at batch start |
| 14 | 1  | `batch_duration_s` | 10 |
| 15 | 1  | — | reserved |
| 16 | 4  | `emg_sample_count` | u32 |
| 20 | 4  | `imu_sample_count` | u32 |
| 24 | 4  | `plaintext_payload_bytes` | u32 |
| 28 | 4  | `ciphertext_payload_bytes` | u32 (equals plaintext) |
| 32 | 12 | `nonce` | GCM nonce = `nonce_prefix` (8) ‖ `batch_index` as u32 LE (4) |
| 44 | 4  | — | reserved |

The header is written up-front with nominal full-batch counts so an in-progress
recording is already scannable. If a batch is closed early it is rewritten with the
real counts.

## 5. Payload — type 2 chunk (6400 bytes = 1 second)

The plaintext payload is a whole number of **chunks**, one per second. Each chunk is
two contiguous columnar sections (convenient for array/columnar loading):

| Section | Type | Count | Bytes |
|---------|------|------:|------:|
| `ch1` | `int32` LE | 1000 | 4000 |
| `imu` | `ImuSample` (§5.1) | 100 | 2400 |

`ch1` is the ADS1292R channel-1 EMG conversion: a 24-bit two's-complement count
sign-extended to `int32`. (Channel 2 exists in hardware but is not recorded.)

**Per-sample lead-off status is not stored.** The status LED gives live lead-off
feedback while recording, and the current firmware reports signal-based per-chunk
lead-off summaries in the control CSV (`ch1_leadoff_chunks`, §8). The ADS1292R
hardware lead-off comparator path is intentionally disabled for the high-Z
two-electrode textile setup; the payload stores neither per-sample status bytes nor
hardware-comparator sample counts.

### 5.1 `ImuSample` (24 bytes, packed little-endian)

| Offset | Type | Fields |
|-------:|------|--------|
| 0  | `int16`×3 | `ax, ay, az` (accelerometer) |
| 6  | `int16`×3 | `gx, gy, gz` (gyroscope) |
| 12 | `uint32`×3 | `mx, my, mz` (magnetometer, 18-bit unsigned) |

## 6. Encryption and nonce

- **Cipher:** AES-256-GCM (`cipher_id = 1`), key from `recording.key`. Each batch is
  encrypted independently as a single GCM stream over its whole plaintext payload.
- **Nonce (96-bit):** `nonce_prefix` (8 B) ‖ `batch_index` (u32 LE, 4 B). The decoder
  reads the stored 12-byte nonce from each batch header verbatim — it never
  re-derives it — so the prefix derivation is a firmware-only concern.
- **`nonce_scheme = 1` (entropy salt):** `nonce_prefix` is a per-session random salt
  derived at session start from an **analog entropy source** — the LSB noise of the
  ADS1292R analog front-end (128 conversions), mixed (FNV-1a 64-bit) with the
  free-running CPU cycle counter and the 96-bit device UID. The `batch_index`
  guarantees per-batch uniqueness within a session; the random salt makes the prefix
  unique across sessions **without** depending on the RTC or on monotonic session ids.

  > **Why not RTC/session-id nonce derivation?** The RV-3028 RTC
  > resets to ~2000-01-01 on battery disconnect, so that prefix (BCD time + low-16
  > session id) was near-constant; reformatting the SD restarted session ids and
  > recreated earlier prefixes → **GCM nonce reuse under a fixed key** (catastrophic).
  > A single raw ADC reading is *not* a safe nonce either (too little entropy →
  > collisions); this scheme gathers a 64-bit random salt and keeps the batch counter.

## 7. Trailer (24 bytes, plaintext)

| Offset | Size | Field |
|-------:|-----:|-------|
| 0  | 4  | CRC32 of the plaintext payload (zlib/IEEE, `0xEDB88320`, init/final `0xFFFFFFFF`) |
| 4  | 16 | AES-GCM authentication tag |
| 20 | 4  | end marker `"ENDB"` |

A batch missing the trailer / `ENDB` (power loss) is discarded by readers; scanning
resynchronizes to the next `BATC`.

## 8. Control CSV (plaintext, one row per batch)

Header row, then one row per completed batch:

```
session_id,batch_index,start_timestamp,end_timestamp,emg_sample_count,
imu_sample_count,payload_bytes,write_time_ms,crc32_plaintext,
dropped_samples,temperature_c,error_flags,storage_status,
ch1_saturated_samples,ch1_flatline_chunks,ch1_baseline_drift_chunks,
ch1_leadoff_chunks,ch1_diff_abs_sum_min,ch1_diff_abs_sum_med,ch1_diff_abs_sum_max
```

Readers must key columns by **header name** and tolerate columns being added/removed
across firmware revisions — do not rely on fixed positions.

- Timestamps are BCD `YYMMDDhhmmss` (session-relative; see RTC note in §6).
- `payload_bytes`: plaintext bytes encrypted in the batch (`chunk_count × 6400`).
  AES-GCM ciphertext length equals plaintext length, so this is also the encrypted
  payload size; the 16-byte tag and CRC live in the batch trailer, not here.
- `error_flags` (hex, bit field): `0x01` WRITE, `0x02` SYNC, `0x04` RTC,
  `0x08` DROPPED, `0x10` AES, `0x20` PARTIAL.
- `storage_status` is `OK` or `ERR` (set when `error_flags` has WRITE or SYNC).
- `ch1_saturated_samples`: count of CH1 samples railed to within 8 LSB of ±full-scale.
  Sample-resolution clipping — catches a brief rail inside an otherwise-good chunk
  (the LED also lights for this). `0` is normal.

The next four columns classify each 1-second chunk into one of the mutually
exclusive CH1 quality states (a chunk is counted in at most one of them, in this
priority): **flatline → lead-off → baseline-drift → clean**.

- `ch1_flatline_chunks`: chunks where CH1 was stuck, nearly flat, or repeated the
  same ADC code for almost the whole second — hard dead/stuck signal.
- `ch1_leadoff_chunks`: chunks flagged as electrode-disconnected by the **signal-based**
  lead-off detector (the ADS hardware comparators are off, so this replaces them). A
  disconnected electrode shows one of two signatures, and a connected electrode's
  sum-of-abs-sample-differences sits in a band between them, so a chunk is lead-off when
  that sum is **below** `QUALITY_LEADOFF_DIFF_SUM_COUNTS` (quiet/open electrode) **or
  above** `QUALITY_LEADOFF_HI_DIFF_SUM_COUNTS` (wild/floating electrode picking up
  interference) — but not flat. Treated as hard bad contact; uses the lead-off LED.
- `ch1_baseline_drift_chunks`: chunks (not already flat/lead-off) where the mean of the
  last 100 samples moved ≥ 100000 ADC counts from the first 100 — connected-but-drifting
  (motion / sweat / half-cell drift). Treated as a noisy/motion-artifact warning.
- `ch1_diff_abs_sum_min` / `_med` / `_max`: the **continuous level metric** — the min,
  median, and max across the batch's chunks of each chunk's sum-of-abs-sample-differences
  (the exact quantity the two lead-off thresholds compare against). This makes the
  thresholds re-tunable from the CSV alone (no EMX decode) and exposes signal magnitude:
  quiet float ≈ hundreds of k, connected EMG ≈ low millions, wild float ≈ tens of M
  (mains environment — all shift on battery). Clamped to 32-bit.

## 9. Physical scaling (for analysis / ML)

The header carries everything needed to convert raw counts to physical units:

- **EMG:** `emg_uV = ch1 * emg_ref_mv * 1000.0 / ((1 << 23) * emg_pga_gain)`
  (with `emg_ref_mv = 2420`, `emg_pga_gain = 4` → ≈ 0.0721 µV / count).
- **Accelerometer:** `g = raw / 16384` (±2 g range).
- **Gyroscope:** `dps = raw / 131` (±250 dps range).
- **Magnetometer:** 18-bit unsigned; null-field (no field / not measuring) = `131072`.
- **Time:** `t_emg[i] = i / emg_rate_hz`, `t_imu[i] = i / imu_rate_hz`.

## 10. Multi-device synchronization

Several devices can be recorded as one session and aligned to a common timebase.
The full hardware/handshake spec is in [`SYNC_PROTOCOL.md`](SYNC_PROTOCOL.md); this
section defines what the **files** carry and how a reader uses it.

### 10.1 How devices share a clock

Devices are joined by a sync cable on two lines (per device):

- **Sync A (PC6)** — a **shared open-drain signal net** common to all devices,
  idling high via pull-ups. Any device can pull it low (wired-AND, no contention).
- **Sync B (PC7)** — **presence**: shorted to GND through the connector, so LOW
  means the cable is plugged in.

At power-up, with the cable present, each device starts its EMG sample clock,
emits one LOW pulse on Sync A, and then — until it is unplugged — **re-latches its
current EMG sample index on every LOW edge it sees on the shared net**. Because the
net is one electrical node, a pulse is seen by every device on the bus at the same
instant (wire-propagation skew, far below one sample period); this is the **only**
event that is simultaneous across the group — an individual connector unplug is
not. Each device pulses once on arming, so the last edge before unplug is common to
every device still on the bus. That latched index is the per-device **sync
reference**. The RTC is also zeroed on each pulse as a coarse (1 s) cross-check.

> Operational requirement: connect the cable **before** power-on (sync runs only at
> boot), power all devices on within the same window, and **do not unplug any device
> until all have armed** (LED `SYNC` = **solid**, magenta on RGB) — an early-unplugged
> device freezes on an earlier pulse. Each device staggers its pulse by a UID-derived
> delay, so one shared power source is fine.
> Recording on each device begins when **that** device is unplugged; staggered
> unplugs are fine because alignment is locked to the pulse, not the unplug.

### 10.2 What the file carries

Two header fields (§3):

- **`synced`** (byte 39): 1 if a sync pulse was latched this session, else 0.
- **`sync_lead_samples`** (bytes 60–63, u32): the number of EMG samples between the
  shared sync pulse and the **first recorded sample** of this file
  (`first_recorded_index − pulse_index`). 0 when `synced = 0`.

The sync pulse itself is **before** the recording starts (recording begins at
unplug), so it does not appear in the samples — `sync_lead_samples` says how far
ahead of sample 0 it was. It is exact to ±1 sample under healthy SD; if the session
shows non-zero early `dropped_samples`, treat it as ±`dropped`.

### 10.3 Aligning streams (reader)

For each device *d*, recorded sample *k* maps to a **pulse-relative** index:

```
rel_d(k) = k + sync_lead_samples[d]          # samples since the common pulse
```

Samples from different devices with the **same `rel`** are simultaneous. To align a
set of files:

1. Require `synced == 1` and equal `emg_rate_hz` on all of them (mixed/standalone
   files cannot be sample-aligned — fall back to the RTC `start_time_bcd`, ~1 s).
2. Shift each device by `sync_lead_samples[d]`; the common origin is `rel = 0`
   (the pulse). The overlapping region is `rel ∈ [max_d(lead_d), …]` truncated to
   the shortest stream.
3. Resample/trim to the overlap and emit one aligned, multi-channel matrix.

Precision is one shared electrical edge latched in firmware per device — sample-
accurate to within each device's pulse-detection latency (sub-millisecond), i.e. ≪
one EMG sample at 1 kHz. The RTC zero (`start_time_bcd`) is only a coarse fallback.

## 11. Versioning policy

`version` gates header semantics; `payload_type` gates the chunk layout. A reader
must require `version == 1` and `payload_type == 2`; anything else is unsupported.
The `synced` / `sync_lead_samples` fields are additive within `version = 1`: a reader
that ignores them still decodes single-device data correctly. Future additive changes
should bump `payload_type` (new chunk layout) or `version` (new header) and update
this document and all three implementations together.


## 12. Reference implementations

- **Writer:** `AstroMove_FW/Core/Src/recording.c` (+ `acquisition.c` for the
  per-batch quality summary and the nonce entropy, `sync.c` for the sync sequence).
- **Decoder (reference):** `AstroMove_FW/decode_emgx.py`.
- **Sample generator:** `AstroMove_FW/make_sample_emgx.py` (deterministic golden data).
- **Rust:** `AstroMoWe_Inspect/crates/astromowe-format`.
