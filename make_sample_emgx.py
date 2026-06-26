#!/usr/bin/env python3
"""Generate a small, valid EMGX v1 sample recording (see FORMAT.md).

Produces a deterministic, fully-decodable `.EMX` + control `.CSV` pair encrypted
with the shared recording key, for use as golden test data in both AstroMove_FW
and AstroMoWe_Inspect. The signal is synthetic but realistic enough to exercise
the control-CSV signal-quality columns:

  - ch1: a 30 Hz "EMG-ish" tone whose per-chunk sum-of-abs-differences lands in the
    connected band (~3M), with a deliberate 3-chunk quiet-float window in batch 1
    (near-zero noise, ~470k) so `ch1_leadoff_chunks` comes out 0, 3, 0, 0 and the
    `ch1_diff_abs_sum_*` columns show the connected/quiet spread.
  - IMU: slowly varying synthetic accel/gyro/mag (non-zero, unlike the dead
    ISM330 on board 04) so the all-zero/null diagnostics are exercised as false.

The control-CSV quality columns are computed from the actual synthetic samples
(same diff_abs_sum metric and thresholds as the firmware) so the golden is
internally self-consistent. Generation is seeded so re-running yields
byte-identical files. The real firmware derives the 8-byte nonce prefix from live
analog front-end noise (FORMAT.md §6); here it comes from the fixed seed.

Usage:
    python make_sample_emgx.py [--session N] [--batches K] [-o OUTDIR]
Requires: pip install cryptography
"""
import argparse
import math
import random
import struct
import zlib
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

import decode_emgx as fmt  # reuse the format constants / key loader

ADS_REGS = bytes([0x53, 0x03, 0xE0, 0x10, 0x40, 0x60, 0x00, 0x03, 0xF2, 0x03])
EMG_PGA_GAIN = 4
EMG_REF_MV = 2420
DEVICE_UID = bytes.fromhex("aa55aa55deadbeef00010203")  # synthetic 96-bit UID
SEED = 0xE46  # reproducible "entropy"

EMG_AMP = 24000   # 30 Hz tone amplitude → per-chunk diff_abs_sum ≈ 3M (connected band)
LO_NOISE = 700    # quiet-float window noise → diff_abs_sum ≈ 470k (reads as lead-off)
# Same thresholds as the firmware (acquisition.c) — keep the golden self-consistent.
QUALITY_LEADOFF_LO = 1_000_000
QUALITY_LEADOFF_HI = 12_000_000


def bcd(n):
    return ((n // 10) << 4) | (n % 10)


def bcd_time(sec):
    """seconds-since-epoch -> 6 BCD bytes (sec,min,hr,date,mon,yr), 2000-01-01 base."""
    s = sec % 60
    m = (sec // 60) % 60
    h = (sec // 3600) % 24
    return bytes([bcd(s), bcd(m), bcd(h), bcd(1), bcd(1), bcd(0)])


def bcd_str(b):
    return "".join(f"{x:02x}" for x in (b[5], b[4], b[3], b[2], b[1], b[0]))


def build_chunk(rng, chunk_global_index, leadoff):
    """One 6400-byte plaintext chunk: int32 ch1[1000] + imu[100] (no status byte).

    Returns (plaintext_bytes, diff_abs_sum). diff_abs_sum matches the firmware's
    per-chunk level metric so the control-CSV columns stay self-consistent."""
    samples = []
    base_sample = chunk_global_index * fmt.CHUNK_SAMPLES
    for i in range(fmt.CHUNK_SAMPLES):
        if leadoff:
            val = rng.randint(-LO_NOISE, LO_NOISE)          # open electrode: quiet float
        else:
            t = (base_sample + i) / 1000.0
            val = int(EMG_AMP * math.sin(2 * math.pi * 30 * t) + rng.randint(-400, 400))
        samples.append(val)
    diff = sum(abs(samples[i] - samples[i - 1]) for i in range(1, len(samples)))

    ch1 = bytearray()
    for v in samples:
        ch1 += struct.pack("<i", v)
    imu = bytearray()
    for j in range(fmt.IMU_SAMPLES):
        k = chunk_global_index * fmt.IMU_SAMPLES + j
        ax = int(1000 * math.sin(k / 50.0)); ay = int(800 * math.cos(k / 40.0)); az = 16384
        gx = int(50 * math.sin(k / 30.0)); gy = int(-30 * math.cos(k / 25.0)); gz = 5
        mx = 131072 + int(2000 * math.sin(k / 60.0))
        my = 131072 + int(1500 * math.cos(k / 55.0))
        mz = 131072 + 500
        imu += struct.pack("<6h3I", ax, ay, az, gx, gy, gz, mx, my, mz)
    return bytes(ch1) + bytes(imu), min(diff, 0xFFFFFFFF)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--session", type=int, default=100)
    ap.add_argument("--batches", type=int, default=4)
    ap.add_argument("-o", "--outdir", type=Path, default=Path(__file__).parent / "samples")
    ap.add_argument("--key", help="AES-256 key as 64 hex chars (default: recording.key)")
    args = ap.parse_args()

    key = bytes.fromhex(args.key) if args.key else fmt.load_key_file(fmt.KEY_FILE)
    aes = AESGCM(key)
    rng = random.Random(SEED)
    nonce_prefix = bytes(rng.randrange(256) for _ in range(8))  # stands in for analog entropy

    args.outdir.mkdir(parents=True, exist_ok=True)
    emx = args.outdir / f"S{args.session:07d}.EMX"
    csv = args.outdir / f"S{args.session:07d}.CSV"

    # File header (FORMAT.md §3)
    hdr = bytearray(fmt.FILE_HEADER_SIZE)
    hdr[0:4] = b"EMGX"
    hdr[4] = fmt.FORMAT_VERSION
    hdr[5] = fmt.FILE_HEADER_SIZE
    hdr[6] = fmt.PAYLOAD_TYPE
    hdr[7] = 10
    struct.pack_into("<I", hdr, 8, args.session)
    hdr[12:24] = DEVICE_UID
    hdr[24:30] = bcd_time(0)
    struct.pack_into("<H", hdr, 30, 1000)
    struct.pack_into("<H", hdr, 32, 100)
    hdr[34] = fmt.CIPHER_AES256GCM
    hdr[35] = 1            # key_id
    hdr[36] = 1            # key_version
    hdr[37] = 1            # nonce_scheme = entropy salt
    hdr[38] = EMG_PGA_GAIN
    # byte 39 reserved (0): payload_type alone identifies the chunk layout
    hdr[40:48] = nonce_prefix
    hdr[48:58] = ADS_REGS
    struct.pack_into("<H", hdr, 58, EMG_REF_MV)

    out = bytearray(hdr)
    rows = []
    # Deliberate quiet-float (lead-off) window: 3 full chunks (3 s..6 s) of batch 1.
    leadoff_window = (1 * 10 * fmt.CHUNK_SAMPLES + 3000,
                      1 * 10 * fmt.CHUNK_SAMPLES + 6000)

    chunk_global = 0
    for b in range(args.batches):
        payload = bytearray()
        diffs = []
        leadoff_chunks = 0
        for _ in range(10):
            gs = chunk_global * fmt.CHUNK_SAMPLES
            chunk_leadoff = (gs >= leadoff_window[0] and
                             gs + fmt.CHUNK_SAMPLES <= leadoff_window[1])
            data, diff = build_chunk(rng, chunk_global, chunk_leadoff)
            payload += data
            diffs.append(diff)
            if diff < QUALITY_LEADOFF_LO or diff > QUALITY_LEADOFF_HI:
                leadoff_chunks += 1
            chunk_global += 1
        diffs.sort()
        d_min, d_med, d_max = diffs[0], diffs[len(diffs) // 2], diffs[-1]

        crc = zlib.crc32(payload) & 0xFFFFFFFF
        nonce = nonce_prefix + struct.pack("<I", b)
        ct_tag = aes.encrypt(nonce, bytes(payload), None)
        ct, tag = ct_tag[:-16], ct_tag[-16:]

        bh = bytearray(fmt.BATCH_HEADER_SIZE)
        bh[0:4] = b"BATC"
        struct.pack_into("<I", bh, 4, b)
        bh[8:14] = bcd_time(b * 10)
        bh[14] = 10
        struct.pack_into("<I", bh, 16, 10 * fmt.CHUNK_SAMPLES)
        struct.pack_into("<I", bh, 20, 10 * fmt.IMU_SAMPLES)
        struct.pack_into("<I", bh, 24, len(payload))
        struct.pack_into("<I", bh, 28, len(ct))
        bh[32:44] = nonce
        out += bh + ct + struct.pack("<I", crc) + tag + b"ENDB"

        start_b = bcd_str(bcd_time(b * 10))
        end_b = bcd_str(bcd_time(b * 10 + 10))
        # saturated/flatline/baseline-drift never trip for this synthetic signal;
        # leadoff + the level columns are derived from the actual diff_abs_sum.
        # group_id = 0: this synthetic golden is a standalone (un-synced) session.
        rows.append(f"{args.session},{b},{start_b},{end_b},{10*fmt.CHUNK_SAMPLES},"
                    f"{10*fmt.IMU_SAMPLES},{len(payload)},0,{crc:08X},0,25.3,0x00,OK,"
                    f"0,0,0,{leadoff_chunks},{d_min},{d_med},{d_max},0")

    emx.write_bytes(out)
    csv_header = ("session_id,batch_index,start_timestamp,end_timestamp,emg_sample_count,"
                  "imu_sample_count,payload_bytes,write_time_ms,crc32_plaintext,"
                  "dropped_samples,temperature_c,error_flags,storage_status,"
                  "ch1_saturated_samples,ch1_flatline_chunks,ch1_baseline_drift_chunks,"
                  "ch1_leadoff_chunks,ch1_diff_abs_sum_min,ch1_diff_abs_sum_med,"
                  "ch1_diff_abs_sum_max,group_id")
    csv.write_text(csv_header + "\n" + "\n".join(rows) + "\n")
    print(f"{emx.name}: {args.batches} batches, {args.batches*10*fmt.CHUNK_SAMPLES} EMG / "
          f"{args.batches*10*fmt.IMU_SAMPLES} IMU samples, {len(out)} bytes  (+ {csv.name})")
    print(f"nonce_prefix={nonce_prefix.hex()}  quiet-float window samples "
          f"{leadoff_window[0]}..{leadoff_window[1]} (batch 1, 3 chunks)")


if __name__ == "__main__":
    main()
