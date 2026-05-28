#!/usr/bin/env python3
"""
Plot ADS1292R ECG data from AstroMowe binary log files (.bin).

Binary file layout
------------------
File header  32 bytes  "AMS1" + ADS register dump
Data block 5040 bytes  repeated per 1000-sample batch:
  tick_snap[10]  uint32 LE  HAL_GetTick() at each 100-sample boundary
  ch1[1000]      int32  LE  24-bit ADC counts
  loff[1000]     uint8       raw ADS status byte 0

Usage
-----
  python plot_ecg.py log_0000.bin [log_0001.bin ...]
  python plot_ecg.py              # opens file dialog
"""
import sys
import struct
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from pathlib import Path

FILE_HEADER   = 32
BLK_HDR_BYTES = 8              # per-block RTC header (6 bytes BCD time + 2 reserved)
TICK_BYTES    = 10 * 4         # 40
CH1_BYTES     = 1000 * 4       # 4000
LOFF_BYTES    = 1000           # 1000
BLOCK_SIZE    = BLK_HDR_BYTES + TICK_BYTES + CH1_BYTES + LOFF_BYTES  # 5048
SAMPLES_PER_BLOCK = 1000


def bcd(b):
    return (b >> 4) * 10 + (b & 0x0F)

# ADS1292R: Vref = 2.42 V, PGA gain = 4 (CH1SET = 0x40), 24-bit
LSB_UV = (2 * 2.42) / (4 * (2**24)) * 1e6  # µV per count ≈ 0.0722 µV


def parse_bin(path):
    data = path.read_bytes()

    # --- file header ---
    if len(data) < FILE_HEADER or data[:4] != b'AMS1':
        raise ValueError(f"{path.name}: not a valid AMS1 binary file")
    regs = {
        'id':       data[4],  'config1': data[5],
        'config2':  data[6],  'loff':    data[7],
        'ch1set':   data[8],  'ch2set':  data[9],
        'rldsens':  data[10], 'loffsens':data[11],
        'resp1':    data[12], 'resp2':   data[13],
    }
    # RTC start time stored at bytes [14..19] (BCD)
    rtc_start = tuple(data[14:20])

    # --- data blocks ---
    all_ts, all_ch1, all_loff, all_rtc = [], [], [], []
    offset = FILE_HEADER
    block_idx = 0
    while offset + BLOCK_SIZE <= len(data):
        rtc  = struct.unpack_from('8B', data, offset)
        tick = struct.unpack_from('<10I', data, offset + BLK_HDR_BYTES)
        ch1  = struct.unpack_from('<1000i', data, offset + BLK_HDR_BYTES + TICK_BYTES)
        loff = struct.unpack_from('1000B',  data, offset + BLK_HDR_BYTES + TICK_BYTES + CH1_BYTES)

        # per-sample timestamp: tick boundary + intra-group ms offset (1 ms per sample assumed)
        ts = [tick[i // 100] + (i % 100) for i in range(SAMPLES_PER_BLOCK)]

        all_ts.extend(ts)
        all_ch1.extend(ch1)
        all_loff.extend(loff)
        all_rtc.append(rtc)
        offset += BLOCK_SIZE
        block_idx += 1

    return regs, np.array(all_ts), np.array(all_ch1), np.array(all_loff), block_idx, rtc_start, all_rtc


def check_timing(ts, label):
    diffs = np.diff(ts)
    gaps  = np.where(diffs > 1)[0]
    dups  = np.where(diffs == 0)[0]
    print(f"  {label}: {len(ts)} samples over {len(ts)//SAMPLES_PER_BLOCK} blocks, "
          f"span {ts[-1]-ts[0]} ms (expected {len(ts)-1} ms)")
    if len(gaps):
        print(f"    timing gaps (>1 ms): {len(gaps)} — worst {diffs[gaps].max()} ms")
    if len(dups):
        print(f"    duplicate timestamps: {len(dups)}")
    if not len(gaps) and not len(dups):
        print("    timing OK — no gaps or duplicates")


def plot_files(paths):
    fig, axes = plt.subplots(len(paths), 1,
                             figsize=(14, 4 * len(paths)),
                             squeeze=False)

    for ax_row, path in zip(axes, paths):
        ax = ax_row[0]
        try:
            regs, ts, ch1, loff, n_blocks, rtc_start, all_rtc = parse_bin(path)
        except Exception as e:
            ax.set_title(f"{path.name} — {e}")
            continue

        s, m, h, d, mo, yr = [bcd(b) for b in rtc_start]
        print(f"\n{path.name}:")
        print(f"  RTC start: 20{yr:02d}-{mo:02d}-{d:02d} {h:02d}:{m:02d}:{s:02d}")
        print(f"  ADS: ID={regs['id']:02X} CONFIG1={regs['config1']:02X} "
              f"CONFIG2={regs['config2']:02X} CH1SET={regs['ch1set']:02X} "
              f"RESP1={regs['resp1']:02X}")
        for i, rtc in enumerate(all_rtc):
            rs, rm, rh = bcd(rtc[0]), bcd(rtc[1]), bcd(rtc[2])
            fsync_ms  = rtc[6]
            fwrite_ms = rtc[7]
            extras = ""
            if fsync_ms:  extras += f"  fsync={fsync_ms}ms"
            if fwrite_ms: extras += f"  fwrite={fwrite_ms}ms"
            print(f"  block {i}: {rh:02d}:{rm:02d}:{rs:02d}{extras}")
        check_timing(ts, path.name)

        t_sec = (ts - ts[0]) / 1000.0
        signal_uv = ch1 * LSB_UV

        ax.plot(t_sec, signal_uv, lw=0.5, color='steelblue')

        # Vertical lines at every 1-second tick boundary
        for s in range(0, int(t_sec[-1]) + 2):
            ax.axvline(x=s, color='red', alpha=0.25, linewidth=0.8, linestyle='--')

        ax.set_title(f"{path.name}  |  {len(ts)} samples  |  "
                     f"{n_blocks} blocks  |  span {ts[-1]-ts[0]} ms",
                     fontsize=10)
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("µV")
        ax.grid(True, alpha=0.25)
        ax.xaxis.set_minor_locator(ticker.MultipleLocator(0.1))
        ax.grid(True, which='minor', alpha=0.1)

        stats = (f"mean {signal_uv.mean():.0f} µV\n"
                 f"std  {signal_uv.std():.0f} µV\n"
                 f"p-p  {signal_uv.max()-signal_uv.min():.0f} µV")
        ax.text(0.99, 0.97, stats, transform=ax.transAxes,
                fontsize=8, va='top', ha='right',
                bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))

    plt.tight_layout()
    plt.show()


if __name__ == '__main__':
    if len(sys.argv) > 1:
        files = [Path(p) for p in sys.argv[1:]]
    else:
        try:
            import tkinter as tk
            from tkinter import filedialog
            root = tk.Tk(); root.withdraw()
            chosen = filedialog.askopenfilenames(
                title="Select .bin log files",
                filetypes=[("AstroMowe binary log", "*.bin"), ("All files", "*.*")])
            files = [Path(p) for p in chosen]
        except Exception:
            print("Usage: python plot_ecg.py <file.bin> [file.bin ...]")
            sys.exit(1)

    if not files:
        print("No files selected.")
        sys.exit(0)

    plot_files(files)
