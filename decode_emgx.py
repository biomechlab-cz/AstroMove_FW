#!/usr/bin/env python3
"""Decode an encrypted EMGX recording (format v1, see recording.c).

Decrypts each batch with AES-256-GCM, verifies the authentication tag and
the plaintext CRC32, and exports samples to CSV.

Usage:
    python decode_emgx.py S0000001.EMX
    python decode_emgx.py S0000001.EMX --key 000102...1f -o out.csv

Requires: pip install cryptography
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.exceptions import InvalidTag

# Must match Core/Inc/recording_key.h
TEST_KEY = bytes(range(32))

FILE_HEADER_SIZE = 64
BATCH_HEADER_SIZE = 48
TRAILER_SIZE = 24
CHUNK_SAMPLES = 1000
CHUNK_BYTES = CHUNK_SAMPLES * 5  # int32 ch1 + uint8 loff per sample


def bcd_timestamp(b):
    """6 BCD bytes (sec,min,hr,date,mon,yr) -> 'YYMMDDhhmmss'."""
    sec, mn, hr, date, mon, yr = b
    return f"{yr:02x}{mon:02x}{date:02x}{hr:02x}{mn:02x}{sec:02x}"


def parse_file_header(h):
    if h[0:4] != b"EMGX":
        sys.exit("Not an EMGX file (bad magic)")
    info = {
        "version": h[4],
        "header_size": h[5],
        "payload_type": h[6],
        "batch_duration_s": h[7],
        "session_id": struct.unpack_from("<I", h, 8)[0],
        "device_uid": h[12:24].hex(),
        "start_time": bcd_timestamp(h[24:30]),
        "emg_rate_hz": struct.unpack_from("<H", h, 30)[0],
        "imu_rate_hz": struct.unpack_from("<H", h, 32)[0],
        "cipher_id": h[34],
        "key_id": h[35],
        "key_version": h[36],
        "nonce_prefix": h[40:48].hex(),
        "ads_regs": h[48:58].hex(),
    }
    if info["version"] != 1:
        sys.exit(f"Unsupported format version {info['version']}")
    if info["cipher_id"] != 1:
        sys.exit(f"Unsupported cipher id {info['cipher_id']} (expected 1 = AES-256-GCM)")
    if info["payload_type"] != 1:
        sys.exit(f"Unsupported payload type {info['payload_type']}")
    return info


def swap32(b):
    """Reverse byte order within each 32-bit word."""
    return b"".join(b[i:i + 4][::-1] for i in range(0, len(b), 4))


def legacy_decrypt(key, nonce, ct):
    """Recordings made before the firmware's DataType fix (sessions 0-4)
    were encrypted with the AES peripheral in no-swap mode: a word-swapped
    GCM stream. Decrypt via CTR with swapped words; the GCM tag of those
    files is non-standard and is NOT verified — CRC32 still is."""
    ctr0 = nonce + (2).to_bytes(4, "big")  # GCM payload counter starts at 2
    dec = Cipher(algorithms.AES(key), modes.CTR(ctr0)).decryptor()
    return swap32(dec.update(swap32(ct)) + dec.finalize())


def decode_batches(data, key, csv_out, legacy=False):
    aes = AESGCM(key)
    pos = FILE_HEADER_SIZE
    sample_index = 0
    n_ok = n_bad = 0

    while pos + BATCH_HEADER_SIZE <= len(data):
        if data[pos:pos + 4] != b"BTCH":
            # fault recovery: scan forward for the next sync marker
            nxt = data.find(b"BTCH", pos + 1)
            if nxt < 0:
                print(f"  no further sync marker after offset {pos}, stopping")
                break
            print(f"  corruption at offset {pos}, resyncing at {nxt}")
            pos = nxt
            continue

        h = data[pos:pos + BATCH_HEADER_SIZE]
        batch_index = struct.unpack_from("<I", h, 4)[0]
        start_time = bcd_timestamp(h[8:14])
        emg_n, imu_n, pt_len, ct_len = struct.unpack_from("<IIII", h, 16)
        nonce = h[32:44]

        end = pos + BATCH_HEADER_SIZE + ct_len + TRAILER_SIZE
        if end > len(data):
            print(f"  batch {batch_index}: truncated (power loss?), discarded")
            break
        ct = data[pos + BATCH_HEADER_SIZE:pos + BATCH_HEADER_SIZE + ct_len]
        trailer = data[end - TRAILER_SIZE:end]
        crc_stored = struct.unpack_from("<I", trailer, 0)[0]
        tag = trailer[4:20]
        if trailer[20:24] != b"ENDB":
            print(f"  batch {batch_index}: missing end marker, discarded")
            pos += 4  # step past this marker and resync
            continue

        status = []
        if legacy:
            pt = legacy_decrypt(key, nonce, ct)
            status.append("legacy: tag not verified")
        else:
            try:
                pt = aes.decrypt(nonce, ct + tag, None)
            except InvalidTag:
                print(f"  batch {batch_index}: AUTH TAG FAILED (wrong key or tampered), discarded")
                n_bad += 1
                pos = end
                continue
        if zlib.crc32(pt) != crc_stored:
            status.append("CRC MISMATCH")

        for chunk_off in range(0, len(pt), CHUNK_BYTES):
            chunk = pt[chunk_off:chunk_off + CHUNK_BYTES]
            ch1 = struct.unpack_from(f"<{CHUNK_SAMPLES}i", chunk, 0)
            loff = chunk[CHUNK_SAMPLES * 4:]
            for i in range(CHUNK_SAMPLES):
                csv_out.write(f"{sample_index},{batch_index},{ch1[i]},0x{loff[i]:02X}\n")
                sample_index += 1

        n_ok += 1
        note = f" [{', '.join(status)}]" if status else ""
        print(f"  batch {batch_index}: {emg_n} EMG / {imu_n} IMU samples, "
              f"start {start_time}, tag OK{note}")
        pos = end

    return n_ok, n_bad, sample_index


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", type=Path, help=".EMX recording file")
    ap.add_argument("--key", help="AES-256 key as 64 hex chars (default: built-in test key)")
    ap.add_argument("-o", "--output", type=Path,
                    help="output CSV (default: <input>_data.csv)")
    ap.add_argument("--legacy-swap32", action="store_true",
                    help="decode pre-DataType-fix recordings (sessions made "
                         "before 2026-06-11; word-swapped GCM, tag not verified)")
    args = ap.parse_args()

    key = bytes.fromhex(args.key) if args.key else TEST_KEY
    if len(key) != 32:
        sys.exit("Key must be 32 bytes (64 hex chars)")

    data = args.file.read_bytes()
    if len(data) < FILE_HEADER_SIZE:
        sys.exit("File too short")
    info = parse_file_header(data[:FILE_HEADER_SIZE])

    print(f"Session {info['session_id']}, started {info['start_time']}, "
          f"EMG {info['emg_rate_hz']} Hz, device UID {info['device_uid']}")
    print(f"Key id {info['key_id']} v{info['key_version']}, "
          f"ADS regs {info['ads_regs']}")

    out_path = args.output or args.file.with_name(args.file.stem + "_data.csv")
    with open(out_path, "w", newline="") as csv_out:
        csv_out.write("sample_index,batch_index,ch1,loff\n")
        n_ok, n_bad, n_samples = decode_batches(data, key, csv_out,
                                                legacy=args.legacy_swap32)

    print(f"Done: {n_ok} batches OK, {n_bad} failed, "
          f"{n_samples} samples -> {out_path}")


if __name__ == "__main__":
    main()
