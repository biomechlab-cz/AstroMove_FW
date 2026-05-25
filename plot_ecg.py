#!/usr/bin/env python3
"""
Plot ADS1292R ECG data from InfluxDB line-protocol log files.
Usage:  python plot_ecg.py log_0000.lp [log_0001.lp ...]
        python plot_ecg.py          # opens file dialog
"""
import sys
import re
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from pathlib import Path

LP_LINE = re.compile(r'^ecg ch1=(-?\d+)i,loff=(\d+)i (\d+)$')

def parse_lp(path):
    ch1, loff, ts = [], [], []
    with open(path, 'r') as f:
        for line in f:
            m = LP_LINE.match(line.strip())
            if m:
                ch1.append(int(m.group(1)))
                loff.append(int(m.group(2)))
                ts.append(int(m.group(3)))
    return np.array(ts), np.array(ch1), np.array(loff)

def check_timing(ts, label):
    diffs = np.diff(ts)
    gaps  = np.where(diffs > 1)[0]
    dups  = np.where(diffs == 0)[0]
    print(f"  {label}: {len(ts)} samples, "
          f"span {ts[-1]-ts[0]} ms (expected {len(ts)-1} ms)")
    if len(gaps):
        print(f"    timing gaps (>1ms): {len(gaps)} — "
              f"worst {diffs[gaps].max()} ms at sample {gaps[0]}")
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
        ts, ch1, loff = parse_lp(path)

        if len(ts) == 0:
            ax.set_title(f"{path.name} — no data")
            continue

        print(f"\n{path.name}:")
        check_timing(ts, path.name)

        # Relative time in seconds from block start
        t = (ts - ts[0]) / 1000.0

        # Scale ADC counts to µV  (Vref=2.42V, PGA=4, 24-bit)
        # LSB = 2*Vref / (PGA * 2^24) = 4.84 / (4 * 16777216) ≈ 72.2 nV
        LSB_UV = (2 * 2.42) / (4 * (2**24)) * 1e6  # µV per count
        signal_uv = ch1 * LSB_UV

        # Lead-off overlay: shade regions where loff bit 6 set (IN1P off)
        lead_off = (loff & 0x40).astype(bool)

        ax.plot(t, signal_uv, lw=0.6, color='steelblue', label='CH1')

        # Shade lead-off regions
        if lead_off.any():
            for start_idx in np.where(np.diff(lead_off.astype(int)) > 0)[0]:
                end_candidates = np.where(np.diff(lead_off.astype(int)) < 0)[0]
                end_idx = end_candidates[end_candidates > start_idx][0] \
                          if end_candidates[end_candidates > start_idx].size else len(t)-1
                ax.axvspan(t[start_idx], t[end_idx],
                           alpha=0.15, color='red', label='lead-off')

        ax.set_title(f"{path.name}  |  {len(ts)} samples  "
                     f"|  span {ts[-1]-ts[0]} ms",
                     fontsize=10)
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("µV")
        ax.grid(True, alpha=0.25)
        ax.xaxis.set_minor_locator(ticker.MultipleLocator(0.1))
        ax.grid(True, which='minor', alpha=0.1)

        # Stats box
        stats = (f"mean {signal_uv.mean():.0f} µV\n"
                 f"std  {signal_uv.std():.0f} µV\n"
                 f"p-p  {signal_uv.max() - signal_uv.min():.0f} µV")
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
                title="Select .lp log files",
                filetypes=[("InfluxDB line protocol", "*.lp"), ("All files", "*.*")])
            files = [Path(p) for p in chosen]
        except Exception:
            print("Usage: python plot_ecg.py <file.lp> [file.lp ...]")
            sys.exit(1)

    if not files:
        print("No files selected.")
        sys.exit(0)

    plot_files(files)
