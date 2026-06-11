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
 * Data is delivered in 1-second chunks of REC_CHUNK_SAMPLES EMG samples.
 * REC_CHUNKS_PER_BATCH chunks form one independently encrypted batch
 * (AES-256-GCM, hardware accelerated). */

#define REC_CHUNK_SAMPLES     1000  /* EMG samples per chunk (1 s at 1 kHz) */
#define REC_CHUNKS_PER_BATCH  10    /* batch duration in seconds */

/* error_flags bits reported in the control CSV */
#define REC_ERR_WRITE    0x01  /* f_write failed or wrote short */
#define REC_ERR_SYNC     0x02  /* f_sync failed */
#define REC_ERR_RTC      0x04  /* RTC not responding, timestamp is zero */
#define REC_ERR_DROPPED  0x08  /* acquisition overran, samples lost */
#define REC_ERR_AES      0x10  /* AES peripheral error, batch undecryptable */
#define REC_ERR_PARTIAL  0x20  /* batch closed early (recording stopped) */

/* Create session files and write the file header. SD card must already be
   mounted. ads_regs: ADS1292R registers ID,CONFIG1,CONFIG2,LOFF,CH1SET,
   CH2SET,RLDSENS,LOFFSENS,RESP1,RESP2 (stored in the file header).
   Returns 1 on success. */
uint8_t REC_Open(const uint8_t ads_regs[10]);

/* Encrypt and append one chunk. dropped_samples: samples lost since the
   previous chunk (goes to the control CSV). Every REC_CHUNKS_PER_BATCH-th
   chunk finalizes the batch (tag + trailer + CSV row + sync).
   Returns 0 on unrecoverable storage failure. */
uint8_t REC_WriteChunk(const int32_t ch1[REC_CHUNK_SAMPLES],
                       const uint8_t loff[REC_CHUNK_SAMPLES],
                       uint32_t dropped_samples);

/* Finalize an in-progress batch (marked REC_ERR_PARTIAL) and close both
   files. Safe to call when nothing is open. */
void REC_Close(void);

#endif /* RECORDING_H */
