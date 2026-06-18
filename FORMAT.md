# EMGX recording format — v1 (authoritative specification)

This is the single source of truth for the AstroMoWe encrypted recording format,
shared by the firmware writer (`AstroMove_FW`) and the inspector/decoder
(`AstroMoWe_Inspect`). It supersedes the prose in the `.docx` design note and the
inline comment in `Core/Src/recording.c`. Implementations (firmware C, Python
`decode_emgx.py`, Rust `astromowe-format`) **must** match this document.

> **Compatibility:** This is the canonical format, identified by `version == 1` and
> `payload_type == 2` (which alone fixes the chunk layout). Earlier development
> recordings are an obsolete, incompatible format: the pre-`BATC` files have no batch
> markers, and the early raw-status recordings (a per-sample status byte → 7400-byte
> chunks, RTC/session-id nonce) reused `payload_type == 2` — they are no longer
> distinguished and simply fail to decode (their batch payload length is not a multiple
> of the 6400-byte chunk).

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
| 39 | 1  | — | reserved (0) |
| 40 | 8  | `nonce_prefix` | per-session nonce salt (§6) |
| 48 | 10 | `ads_regs` | ADS1292R snapshot: `ID,CONFIG1,CONFIG2,LOFF,CH1SET,CH2SET,RLDSENS,LOFFSENS,RESP1,RESP2` |
| 58 | 2  | `emg_ref_mv` | ADC reference in millivolts (2420 = internal 2.42 V) — for scaling (§9) |
| 60 | 4  | — | reserved (0) |

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
feedback while recording, and a per-batch count of CH1 lead-off samples is written
to the control CSV (`ch1_leadoff_samples`, §8). The firmware still derives the
ADS1292R lead-off comparator outputs each sample (CH1 = IN1P/IN1N off, with
`CONFIG2.PDB_LOFF_COMP = 1` and `LOFFSENS` enabling CH1) — it summarizes them per
batch rather than storing one byte per sample.

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

  > **Why not the RTC/session-id (the obsolete `nonce_scheme = 0`)?** The RV-3028 RTC
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
imu_sample_count,unencrypted_payload_bytes,encrypted_payload_bytes,write_time_ms,
crc32_plaintext,dropped_samples,temperature_c,error_flags,storage_status,
ch1_leadoff_samples
```

- Timestamps are BCD `YYMMDDhhmmss` (session-relative; see RTC note in §6).
- `error_flags` (hex, bit field): `0x01` WRITE, `0x02` SYNC, `0x04` RTC,
  `0x08` DROPPED, `0x10` AES, `0x20` PARTIAL.
- `storage_status` is `OK` or `ERR`.
- `ch1_leadoff_samples`: count of CH1 (IN1P/IN1N) lead-off samples in the batch —
  the per-batch summary that replaces the dropped per-sample status byte. `0` means
  the electrodes were attached for the whole batch.

## 9. Physical scaling (for analysis / ML)

The header carries everything needed to convert raw counts to physical units:

- **EMG:** `emg_uV = ch1 * emg_ref_mv * 1000.0 / ((1 << 23) * emg_pga_gain)`
  (with `emg_ref_mv = 2420`, `emg_pga_gain = 4` → ≈ 0.0721 µV / count).
- **Accelerometer:** `g = raw / 16384` (±2 g range).
- **Gyroscope:** `dps = raw / 131` (±250 dps range).
- **Magnetometer:** 18-bit unsigned; null-field (no field / not measuring) = `131072`.
- **Time:** `t_emg[i] = i / emg_rate_hz`, `t_imu[i] = i / imu_rate_hz`.

## 10. Versioning policy

`version` gates header semantics; `payload_type` gates the chunk layout. A reader
must require `version == 1` and `payload_type == 2`; anything else is incompatible.
Future additive changes should bump `payload_type` (new chunk layout) or `version`
(new header) and update this document and all three implementations together.


## 11. Reference implementations

- **Writer:** `AstroMove_FW/Core/Src/recording.c` (+ `acquisition.c` for the
  per-batch lead-off count and the nonce entropy).
- **Decoder (reference):** `AstroMove_FW/decode_emgx.py`.
- **Sample generator:** `AstroMove_FW/make_sample_emgx.py` (deterministic golden data).
- **Rust:** `AstroMoWe_Inspect/crates/astromowe-format`.
