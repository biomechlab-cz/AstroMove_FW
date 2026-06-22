#ifndef RECORDING_H
#define RECORDING_H

#include <stdint.h>

/* Encrypted EMG recording per "Format specification" document.
 *
 * Each session creates two files on the SD card (8.3 names — FatFS LFN
 * is disabled; enable _USE_LFN to get the spec's SESSION_NNNNNNN.emgx):
 *   SNNNNNNN.EMX — encrypted binary measurement file
 *   SNNNNNNN.CSV — plaintext control file, one row per batch
 *
 * Data is delivered in 1-second chunks of REC_CHUNK_SAMPLES EMG samples
 * plus REC_CHUNK_IMU_SAMPLES IMU samples. REC_CHUNKS_PER_BATCH chunks form
 * one independently encrypted batch (AES-256-GCM, hardware accelerated). */

#define REC_CHUNK_SAMPLES      1000 /* EMG samples per chunk (1 s at 1 kHz) */
#define REC_CHUNK_IMU_SAMPLES  100  /* IMU samples per chunk (1 s at 100 Hz) */
#define REC_CHUNKS_PER_BATCH   10   /* batch duration in seconds */

/* One IMU sample, stored in the payload as-is (little-endian, 24 bytes). */
typedef struct {
    int16_t  ax, ay, az;  /* accel — ±2 g, 16384 LSB/g */
    int16_t  gx, gy, gz;  /* gyro  — ±250 dps, 131 LSB/dps */
    uint32_t mx, my, mz;  /* mag   — 18-bit unsigned, null field = 131072 */
} REC_ImuSample;

/* error_flags bits reported in the control CSV */
#define REC_ERR_WRITE    0x01  /* f_write failed or wrote short */
#define REC_ERR_SYNC     0x02  /* f_sync failed */
#define REC_ERR_RTC      0x04  /* RTC not responding, timestamp is zero */
#define REC_ERR_DROPPED  0x08  /* acquisition overran, samples lost */
#define REC_ERR_AES      0x10  /* AES peripheral error, batch undecryptable */
#define REC_ERR_PARTIAL  0x20  /* batch closed early (recording stopped) */

/* Per-chunk signal-quality flags, used internally by the acquisition quality check
   to classify each chunk (no longer a CSV column — the per-batch *_chunks counts are). */
#define REC_SIGQ_FLATLINE        0x01  /* CH1 is stuck or almost flat for a full chunk */
#define REC_SIGQ_BASELINE_DRIFT  0x02  /* CH1 baseline moved too far within a chunk */
#define REC_SIGQ_LEADOFF         0x04  /* CH1 outside the connected band for a full chunk → electrode likely
                                          disconnected: too quiet OR too wild (signal-based lead-off; the HW
                                          comparators are off — see ads1292.c) */

/* Create session files and write the file header. SD card must already be
   mounted. ads_regs: ADS1292R registers ID,CONFIG1,CONFIG2,LOFF,CH1SET,
   CH2SET,RLDSENS,LOFFSENS,RESP1,RESP2 (stored in the file header). The nonce
   prefix starts as a device-UID placeholder; call REC_SetNonceSalt() before the
   first chunk to install the entropy salt. Returns 1 on success. */
uint8_t REC_Open(const uint8_t ads_regs[10]);

/* Install the 8-byte per-session AES-GCM nonce prefix (entropy salt from the
   analog front-end; see FORMAT.md §6) and rewrite it into the file header.
   Must be called after REC_Open() and before the first REC_WriteChunk(). */
void REC_SetNonceSalt(const uint8_t nonce_salt[8]);

/* Encrypt and append one chunk (ch1 + IMU; no per-sample status is stored).
   dropped_samples / saturated_samples: samples lost and CH1 near-full-scale (railed)
   samples since the previous chunk. flatline_chunks / baseline_drift_chunks /
   leadoff_chunks: 1 if this chunk tripped that quality check, else 0. diff_abs_sum:
   this chunk's sum-of-abs-sample-differences — the continuous level metric the lead-off
   thresholds compare against (its batch min/median/max go to the CSV so the thresholds
   stay re-tunable without decoding the EMX). All are accumulated/summarized over the
   batch into the control CSV (FORMAT.md §8); the encrypted payload stays ch1 + IMU only.
   Every REC_CHUNKS_PER_BATCH-th chunk finalizes the batch (tag + trailer + CSV row +
   sync). Returns 0 on unrecoverable storage failure. */
uint8_t REC_WriteChunk(const int32_t ch1[REC_CHUNK_SAMPLES],
                       const REC_ImuSample imu[REC_CHUNK_IMU_SAMPLES],
                       uint32_t dropped_samples,
                       uint32_t saturated_samples,
                       uint32_t flatline_chunks,
                       uint32_t baseline_drift_chunks,
                       uint32_t leadoff_chunks,
                       uint32_t diff_abs_sum);

/* Finalize an in-progress batch (marked REC_ERR_PARTIAL) and close both
   files. Safe to call when nothing is open. */
void REC_Close(void);

#endif /* RECORDING_H */
