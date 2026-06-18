/* =====================================================================
 * EMGX encrypted recording — writer for the EMGX v1 format.
 *
 * The byte layout (file header, batch header, payload type 2 chunk = ch1 + IMU,
 * trailer), the AES-256-GCM scheme and the entropy nonce are the
 * single-source-of-truth specification in FORMAT.md at the repo root. Keep this
 * writer, decode_emgx.py and the AstroMoWe_Inspect Rust crate in step with it.
 *
 * Quick recall:
 *   - 64 B plaintext file header, then a sequence of batches.
 *   - Chunk (1 s) = int32 ch1[1000] + REC_ImuSample imu[100] = 6400 B. Per-sample
 *     lead-off status is NOT stored — the LED shows it live and a per-batch CH1
 *     lead-off and signal-quality summaries go to the control CSV.
 *   - Each batch = 48 B plaintext header ("BATC") + GCM ciphertext + 24 B
 *     trailer (CRC32 of plaintext, GCM tag, "ENDB"); one GCM stream per batch.
 *   - GCM nonce = 8 B per-session salt (entropy, FORMAT.md §6) + 4 B batch index.
 *   - A power-loss-interrupted batch has no trailer and is discarded by readers;
 *     earlier batches stay decryptable. An early REC_Close() rewrites the batch
 *     header with the real counts.
 *
 * Decode on the desktop with decode_emgx.py.
 * ===================================================================== */
#include "recording.h"
#include "recording_key.h"
#include "i2c_sensors.h"
#include "fatfs.h"
#include "stm32l4xx_hal.h"
#include <stdio.h>
#include <string.h>

extern CRYP_HandleTypeDef hcryp; /* defined in main.c */

#define FILE_HEADER_SIZE   64
#define BATCH_HEADER_SIZE  48
#define BATCH_TRAILER_SIZE 24
#define FORMAT_VERSION     1
#define PAYLOAD_TYPE_CH1_IMU 2          /* EMG int32 + IMU (no per-sample status; FORMAT.md §5) */
#define CIPHER_AES256GCM   1
#define NONCE_SCHEME_ENTROPY 1          /* per-session random salt + batch counter */
#define EMG_PGA_GAIN       4            /* ADS1292R CH1 PGA gain (CH1SET=0x40) — for scaling */
#define EMG_REF_MV         2420         /* ADC reference, mV (internal 2.42 V) — for scaling */
#define EMG_RATE_HZ        1000
#define IMU_RATE_HZ        100

#define CHUNK_EMG_BYTES    (REC_CHUNK_SAMPLES * 4)             /* int32 ch1 per sample (no status byte) */
#define CHUNK_IMU_BYTES    (REC_CHUNK_IMU_SAMPLES * sizeof(REC_ImuSample))
#define CHUNK_BYTES        (CHUNK_EMG_BYTES + CHUNK_IMU_BYTES)
#define BATCH_EMG_SAMPLES  (REC_CHUNK_SAMPLES * REC_CHUNKS_PER_BATCH)
#define BATCH_IMU_SAMPLES  (REC_CHUNK_IMU_SAMPLES * REC_CHUNKS_PER_BATCH)
#define BATCH_PAYLOAD_BYTES (CHUNK_BYTES * REC_CHUNKS_PER_BATCH)

_Static_assert(sizeof(REC_ImuSample) == 24, "REC_ImuSample is stored as-is");

#define GCM_TIMEOUT_MS     100

/* Debug (read over SWD): first FatFS failure since boot, never cleared.
   point: 1=open hdr write, 2=open hdr sync, 3=open csv, 4=batch hdr,
   5=payload, 6=trailer, 7=batch sync */
volatile uint32_t g_rec_fail_point = 0;
volatile uint32_t g_rec_fail_code  = 0;

static void rec_note_failure(uint32_t point, uint32_t code)
{
    if (g_rec_fail_point == 0) {
        g_rec_fail_point = point;
        g_rec_fail_code  = code;
    }
}

/* ---- session state ---- */
static FIL      s_emgx;
static FIL      s_csv;
static uint8_t  s_open = 0;
static uint32_t s_session_id;
static uint8_t  s_nonce_prefix[8];

/* ---- current batch state ---- */
static uint8_t    s_batch_open = 0;
static uint32_t   s_batch_index;
static FSIZE_t    s_batch_hdr_pos;   /* file offset of the batch header, for early-stop patching */
static uint8_t    s_nonce[12];
static RTC_Time_t s_batch_start;
static uint32_t   s_chunk_count;
static uint32_t   s_payload_bytes;   /* plaintext bytes encrypted so far */
static uint32_t   s_crc;
static uint32_t   s_dropped;
static uint32_t   s_leadoff;         /* CH1 lead-off samples accumulated this batch (CSV summary) */
static uint32_t   s_saturated;       /* CH1 near-full-scale (railed) samples this batch (CSV summary) */
static uint32_t   s_signal_quality;  /* OR of REC_SIGQ_* flags observed in this batch */
static uint32_t   s_flatline_chunks;
static uint32_t   s_baseline_drift_chunks;
static uint32_t   s_write_ms;        /* f_write + f_sync time spent on this batch */
static uint8_t    s_error_flags;

/* ---- AES-GCM streaming state ----
 * The AES peripheral accepts payload only in 16-byte blocks (except the
 * very last block of a batch), so input is staged in `s_gcm_pending`
 * until a full block is available. */
static uint8_t  s_gcm_iv[16];
static uint8_t  s_gcm_pending[16] __attribute__((aligned(4)));
static uint32_t s_gcm_pending_len;
static uint8_t  s_ct[CHUNK_BYTES + 16] __attribute__((aligned(4))); /* ciphertext staging, one chunk */

/* ====================================================================
 * Helpers
 * ==================================================================== */
static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;        p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* CRC32, standard zlib flavour (reflected polynomial 0xEDB88320, init
 * 0xFFFFFFFF, final XOR with 0xFFFFFFFF) — matches Python's zlib.crc32 so
 * the desktop decoder can verify payloads directly.
 *
 * Computed with a nibble (4-bit) lookup table as a size/speed compromise:
 * the usual byte-wide table costs 1 KB of flash, bit-by-bit costs 8 loop
 * iterations per byte. A CRC processes input lowest-bit-first; dividing
 * 4 bits at a time means each step shifts the register right by 4 and
 * XORs a precomputed remainder selected by the 4 bits shifted out.
 * crc32_nibble[n] is exactly the CRC remainder of the 4-bit value n, so
 * two table steps consume one byte. */
static const uint32_t crc32_nibble[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t len)
{
    while (len--) {
        crc ^= *p++;                                  /* fold byte into low bits */
        crc = (crc >> 4) ^ crc32_nibble[crc & 0x0F];  /* divide low nibble */
        crc = (crc >> 4) ^ crc32_nibble[crc & 0x0F];  /* divide high nibble */
    }
    return crc;
}

/* RTC BCD prints as decimal digits with %02x → "YYMMDDhhmmss" */
static void format_timestamp(char out[13], const RTC_Time_t *t)
{
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x",
             t->yr, t->mon, t->date, t->hr, t->min, t->sec);
}

/* f_write to the .EMX file, timed into s_write_ms. Returns 1 on success.
   point identifies the call site for the g_rec_fail_* debug variables. */
static uint8_t timed_write(const void *buf, uint32_t len, uint32_t point)
{
    UINT bw;
    uint32_t t0 = HAL_GetTick();
    FRESULT fr = f_write(&s_emgx, buf, len, &bw);
    s_write_ms += HAL_GetTick() - t0;
    if (fr != FR_OK || bw != len) {
        rec_note_failure(point, fr);
        s_error_flags |= REC_ERR_WRITE;
        return 0;
    }
    return 1;
}

/* ====================================================================
 * AES-256-GCM via the hardware AES peripheral (HAL CRYP, phase API)
 * ==================================================================== */
static uint8_t gcm_phase(uint32_t phase, const uint8_t *in, uint32_t len, uint8_t *out)
{
    hcryp.Init.GCMCMACPhase = phase;
    if (HAL_CRYPEx_AES_Auth(&hcryp, in, len, out, GCM_TIMEOUT_MS) != HAL_OK) {
        s_error_flags |= REC_ERR_AES;
        return 0;
    }
    return 1;
}

static uint8_t gcm_start(const uint8_t nonce[12])
{
    memcpy(s_gcm_iv, nonce, 12);
    s_gcm_iv[12] = 0; s_gcm_iv[13] = 0; s_gcm_iv[14] = 0;
    s_gcm_iv[15] = 2; /* GCM block counter starts at 2 (1 is reserved for the tag) */
    s_gcm_pending_len = 0;

    HAL_CRYP_DeInit(&hcryp);
    hcryp.Init.DataType      = CRYP_DATATYPE_8B;
    hcryp.Init.KeySize       = CRYP_KEYSIZE_256B;
    hcryp.Init.OperatingMode = CRYP_ALGOMODE_ENCRYPT;
    hcryp.Init.ChainingMode  = CRYP_CHAINMODE_AES_GCM_GMAC;
    hcryp.Init.KeyWriteFlag  = CRYP_KEY_WRITE_ENABLE;
    hcryp.Init.GCMCMACPhase  = CRYP_GCM_INIT_PHASE;
    hcryp.Init.pKey          = (uint8_t *)REC_AES_KEY;
    hcryp.Init.pInitVect     = s_gcm_iv;
    hcryp.Init.Header        = NULL;  /* no additional authenticated data */
    hcryp.Init.HeaderSize    = 0;
    if (HAL_CRYP_Init(&hcryp) != HAL_OK) {
        s_error_flags |= REC_ERR_AES;
        return 0;
    }
    /* HAL_CRYP_Init started the GCM init phase; this call waits for it */
    if (!gcm_phase(CRYP_GCM_INIT_PHASE, NULL, 0, NULL))
        return 0;

    /* Zero-length header phase (we use no AAD). Required even so: the HAL
       forces no-swap data mode during init and only applies DataType (8-bit)
       on header-phase entry — skipping straight to payload would emit a
       non-standard word-swapped GCM stream undecryptable by standard tools. */
    return gcm_phase(CRYP_GCMCMAC_HEADER_PHASE, NULL, 0, NULL);
}

/* Encrypt len bytes into out. Up to 15 bytes stay pending until the next
   call or gcm_finish(). Returns the ciphertext length produced. */
static uint32_t gcm_update(const uint8_t *in, uint32_t len, uint8_t *out)
{
    uint32_t produced = 0;

    if (s_gcm_pending_len) {
        uint32_t take = 16 - s_gcm_pending_len;
        if (take > len) take = len;
        memcpy(s_gcm_pending + s_gcm_pending_len, in, take);
        s_gcm_pending_len += take;
        in += take;
        len -= take;
        if (s_gcm_pending_len < 16)
            return 0;
        gcm_phase(CRYP_GCM_PAYLOAD_PHASE, s_gcm_pending, 16, out);
        s_gcm_pending_len = 0;
        out += 16;
        produced = 16;
    }

    uint32_t aligned = len & ~15U;
    if (aligned) {
        gcm_phase(CRYP_GCM_PAYLOAD_PHASE, in, aligned, out);
        produced += aligned;
    }

    uint32_t rem = len - aligned;
    if (rem) {
        memcpy(s_gcm_pending, in + aligned, rem);
        s_gcm_pending_len = rem;
    }
    return produced;
}

/* Encrypt the final partial block (if any) into out and read the
   authentication tag. total_len = plaintext bytes of the whole batch.
   Returns the ciphertext length flushed into out. */
static uint32_t gcm_finish(uint8_t *out, uint8_t tag[16], uint32_t total_len)
{
    uint32_t flushed = 0;
    if (s_gcm_pending_len) {
        gcm_phase(CRYP_GCM_PAYLOAD_PHASE, s_gcm_pending, s_gcm_pending_len, out);
        flushed = s_gcm_pending_len;
        s_gcm_pending_len = 0;
    }
    gcm_phase(CRYP_GCMCMAC_FINAL_PHASE, NULL, total_len, tag);
    return flushed;
}

/* ====================================================================
 * Batch handling
 * ==================================================================== */
static void build_batch_header(uint8_t h[BATCH_HEADER_SIZE], uint32_t chunks)
{
    memset(h, 0, BATCH_HEADER_SIZE);
    memcpy(h, "BATC", 4);
    put_u32(h + 4, s_batch_index);
    h[8]  = s_batch_start.sec;  h[9]  = s_batch_start.min; h[10] = s_batch_start.hr;
    h[11] = s_batch_start.date; h[12] = s_batch_start.mon; h[13] = s_batch_start.yr;
    h[14] = REC_CHUNKS_PER_BATCH;
    put_u32(h + 16, chunks * REC_CHUNK_SAMPLES);     /* EMG sample count */
    put_u32(h + 20, chunks * REC_CHUNK_IMU_SAMPLES); /* IMU sample count */
    put_u32(h + 24, chunks * CHUNK_BYTES);           /* plaintext payload bytes */
    put_u32(h + 28, chunks * CHUNK_BYTES);           /* ciphertext size equals plaintext */
    memcpy(h + 32, s_nonce, 12);
}

static uint8_t rec_begin_batch(void)
{
    memset(&s_batch_start, 0, sizeof(s_batch_start));
    if (!RV3028_ReadTime(&s_batch_start))
        s_error_flags |= REC_ERR_RTC;

    memcpy(s_nonce, s_nonce_prefix, 8);
    put_u32(s_nonce + 8, s_batch_index);
    if (!gcm_start(s_nonce))
        return 0;

    /* Header is written up front with nominal full-batch counts so a batch
       in progress is already scannable; rewritten only on early stop. */
    s_batch_hdr_pos = f_tell(&s_emgx);
    uint8_t hdr[BATCH_HEADER_SIZE];
    build_batch_header(hdr, REC_CHUNKS_PER_BATCH);
    if (!timed_write(hdr, sizeof(hdr), 4))
        return 0;

    s_crc           = 0xFFFFFFFF;
    s_chunk_count   = 0;
    s_payload_bytes = 0;
    s_batch_open    = 1;
    return 1;
}

static void rec_append_csv_row(void)
{
    RTC_Time_t end_time = {0};
    if (!RV3028_ReadTime(&end_time))
        s_error_flags |= REC_ERR_RTC;
    if (s_dropped)
        s_error_flags |= REC_ERR_DROPPED;

    char ts_start[13], ts_end[13], temp_str[8] = "";
    format_timestamp(ts_start, &s_batch_start);
    format_timestamp(ts_end, &end_time);

    int16_t temp = ISM330_ReadTemperatureTenths();
    if (temp != INT16_MIN)
        snprintf(temp_str, sizeof(temp_str), "%d.%d", temp / 10,
                 (temp < 0 ? -temp : temp) % 10);

    char row[256];
    int len = snprintf(row, sizeof(row),
        "%lu,%lu,%s,%s,%lu,%lu,%lu,%lu,%lu,%08lX,%lu,%s,0x%02X,%s,%lu,%lu,0x%02lX,%lu,%lu\r\n",
        (unsigned long)s_session_id,
        (unsigned long)s_batch_index,
        ts_start, ts_end,
        (unsigned long)(s_chunk_count * REC_CHUNK_SAMPLES),
        (unsigned long)(s_chunk_count * REC_CHUNK_IMU_SAMPLES),
        (unsigned long)s_payload_bytes,
        (unsigned long)s_payload_bytes,
        (unsigned long)s_write_ms,
        (unsigned long)(s_crc ^ 0xFFFFFFFF),
        (unsigned long)s_dropped,
        temp_str,
        s_error_flags,
        (s_error_flags & (REC_ERR_WRITE | REC_ERR_SYNC)) ? "ERR" : "OK",
        (unsigned long)s_leadoff,
        (unsigned long)s_saturated,
        (unsigned long)s_signal_quality,
        (unsigned long)s_flatline_chunks,
        (unsigned long)s_baseline_drift_chunks);

    UINT bw;
    f_write(&s_csv, row, (UINT)len, &bw);
    f_sync(&s_csv);
}

static uint8_t rec_end_batch(void)
{
    /* flush the final ciphertext block (if any) and get the tag */
    uint8_t tag[16];
    uint32_t flushed = gcm_finish(s_ct, tag, s_payload_bytes);
    if (flushed && !timed_write(s_ct, flushed, 5))
        return 0;

    uint8_t trailer[BATCH_TRAILER_SIZE];
    put_u32(trailer, s_crc ^ 0xFFFFFFFF);
    memcpy(trailer + 4, tag, 16);
    memcpy(trailer + 20, "ENDB", 4);
    if (!timed_write(trailer, sizeof(trailer), 6))
        return 0;

    if (s_chunk_count < REC_CHUNKS_PER_BATCH) {
        /* stopped early — rewrite the batch header with the real counts */
        s_error_flags |= REC_ERR_PARTIAL;
        uint8_t hdr[BATCH_HEADER_SIZE];
        build_batch_header(hdr, s_chunk_count);
        FSIZE_t end_pos = f_tell(&s_emgx);
        UINT bw;
        f_lseek(&s_emgx, s_batch_hdr_pos);
        f_write(&s_emgx, hdr, sizeof(hdr), &bw);
        f_lseek(&s_emgx, end_pos);
    }

    uint32_t t0 = HAL_GetTick();
    FRESULT fr = f_sync(&s_emgx); /* batch boundary = power-loss-safe point */
    s_write_ms += HAL_GetTick() - t0;
    if (fr != FR_OK) {
        rec_note_failure(7, fr);
        s_error_flags |= REC_ERR_SYNC;
    }

    rec_append_csv_row();

    s_batch_index++;
    s_batch_open  = 0;
    s_dropped     = 0;
    s_leadoff     = 0;
    s_saturated   = 0;
    s_signal_quality = 0;
    s_flatline_chunks = 0;
    s_baseline_drift_chunks = 0;
    s_write_ms    = 0;
    s_error_flags = 0;
    return fr == FR_OK;
}

/* ====================================================================
 * Public API
 * ==================================================================== */
static uint8_t find_free_session(char emgx_name[13], char csv_name[13])
{
    for (uint32_t n = 0; n <= 9999999; n++) {
        snprintf(emgx_name, 13, "S%07lu.EMX", (unsigned long)n);
        FILINFO fi;
        if (f_stat(emgx_name, &fi) != FR_OK) {
            snprintf(csv_name, 13, "S%07lu.CSV", (unsigned long)n);
            s_session_id = n;
            return 1;
        }
    }
    return 0;
}

uint8_t REC_Open(const uint8_t ads_regs[10])
{
    char emgx_name[13], csv_name[13];
    if (!find_free_session(emgx_name, csv_name)) return 0;
    if (f_open(&s_emgx, emgx_name, FA_CREATE_NEW | FA_WRITE) != FR_OK) return 0;
    if (f_open(&s_csv, csv_name, FA_CREATE_NEW | FA_WRITE) != FR_OK) {
        f_close(&s_emgx);
        return 0;
    }

    RTC_Time_t start = {0};
    RV3028_ReadTime(&start); /* stays zero if RTC does not respond */

    /* Nonce prefix: device-UID placeholder until REC_SetNonceSalt() installs the
       per-session entropy salt (FORMAT.md §6). The batch index fills the low 4
       nonce bytes per batch; no dependence on the RTC. */
    put_u32(s_nonce_prefix + 0, HAL_GetUIDw0());
    put_u32(s_nonce_prefix + 4, HAL_GetUIDw1());

    uint8_t hdr[FILE_HEADER_SIZE] = {0};
    memcpy(hdr, "EMGX", 4);
    hdr[4] = FORMAT_VERSION;
    hdr[5] = FILE_HEADER_SIZE;
    hdr[6] = PAYLOAD_TYPE_CH1_IMU;
    hdr[7] = REC_CHUNKS_PER_BATCH;
    put_u32(hdr + 8, s_session_id);
    put_u32(hdr + 12, HAL_GetUIDw0());
    put_u32(hdr + 16, HAL_GetUIDw1());
    put_u32(hdr + 20, HAL_GetUIDw2());
    hdr[24] = start.sec;  hdr[25] = start.min; hdr[26] = start.hr;
    hdr[27] = start.date; hdr[28] = start.mon; hdr[29] = start.yr;
    put_u16(hdr + 30, EMG_RATE_HZ);
    put_u16(hdr + 32, IMU_RATE_HZ);
    hdr[34] = CIPHER_AES256GCM;
    hdr[35] = REC_KEY_ID;
    hdr[36] = REC_KEY_VERSION;
    hdr[37] = NONCE_SCHEME_ENTROPY;
    hdr[38] = EMG_PGA_GAIN;
    /* byte 39 reserved (0): payload_type is the sole chunk-layout discriminator */
    memcpy(hdr + 40, s_nonce_prefix, 8);
    memcpy(hdr + 48, ads_regs, 10);
    put_u16(hdr + 58, EMG_REF_MV);

    UINT bw;
    FRESULT fr = f_write(&s_emgx, hdr, sizeof(hdr), &bw);
    if (fr != FR_OK || bw != sizeof(hdr)) {
        rec_note_failure(1, fr);
        return 0;
    }
    fr = f_sync(&s_emgx);
    if (fr != FR_OK) {
        rec_note_failure(2, fr);
        return 0;
    }

    static const char csv_header[] =
        "session_id,batch_index,start_timestamp,end_timestamp,"
        "emg_sample_count,imu_sample_count,unencrypted_payload_bytes,"
        "encrypted_payload_bytes,write_time_ms,crc32_plaintext,"
        "dropped_samples,temperature_c,error_flags,storage_status,"
        "ch1_leadoff_samples,ch1_saturated_samples,ch1_quality_flags,"
        "ch1_flatline_chunks,ch1_baseline_drift_chunks\r\n";
    fr = f_write(&s_csv, csv_header, sizeof(csv_header) - 1, &bw);
    if (fr == FR_OK)
        fr = f_sync(&s_csv);
    if (fr != FR_OK) {
        rec_note_failure(3, fr);
        return 0;
    }

    s_batch_index = 0;
    s_batch_open  = 0;
    s_dropped     = 0;
    s_leadoff     = 0;
    s_saturated   = 0;
    s_signal_quality = 0;
    s_flatline_chunks = 0;
    s_baseline_drift_chunks = 0;
    s_write_ms    = 0;
    s_error_flags = 0;
    s_open        = 1;
    return 1;
}

void REC_SetNonceSalt(const uint8_t nonce_salt[8])
{
    if (!s_open) return;
    memcpy(s_nonce_prefix, nonce_salt, 8);
    /* Rewrite the nonce_prefix field [40..47] of the already-written file
       header. Safe only before the first batch, when the .EMX file is still
       just the 64-byte header. */
    UINT bw;
    FSIZE_t end = f_tell(&s_emgx);
    if (f_lseek(&s_emgx, 40) == FR_OK &&
        f_write(&s_emgx, s_nonce_prefix, 8, &bw) == FR_OK && bw == 8) {
        f_lseek(&s_emgx, end);
        f_sync(&s_emgx);
    }
}

uint8_t REC_WriteChunk(const int32_t ch1[REC_CHUNK_SAMPLES],
                       const REC_ImuSample imu[REC_CHUNK_IMU_SAMPLES],
                       uint32_t dropped_samples,
                       uint32_t leadoff_samples,
                       uint32_t saturated_samples,
                       uint32_t signal_quality_flags,
                       uint32_t flatline_chunks,
                       uint32_t baseline_drift_chunks)
{
    if (!s_open) return 0;
    s_dropped += dropped_samples;
    s_leadoff += leadoff_samples;
    s_saturated += saturated_samples;
    s_signal_quality |= signal_quality_flags;
    s_flatline_chunks += flatline_chunks;
    s_baseline_drift_chunks += baseline_drift_chunks;

    if (!s_batch_open && !rec_begin_batch())
        return 0;

    const uint8_t *ch1_bytes = (const uint8_t *)ch1; /* int32 LE on Cortex-M */
    const uint8_t *imu_bytes = (const uint8_t *)imu;
    s_crc = crc32_update(s_crc, ch1_bytes, REC_CHUNK_SAMPLES * 4);
    s_crc = crc32_update(s_crc, imu_bytes, CHUNK_IMU_BYTES);

    uint32_t n = gcm_update(ch1_bytes, REC_CHUNK_SAMPLES * 4, s_ct);
    n += gcm_update(imu_bytes, CHUNK_IMU_BYTES, s_ct + n);
    if (!timed_write(s_ct, n, 5))
        return 0;
    s_payload_bytes += CHUNK_BYTES;

    if (++s_chunk_count >= REC_CHUNKS_PER_BATCH)
        return rec_end_batch();
    return 1;
}

void REC_Close(void)
{
    if (!s_open) return;
    if (s_batch_open)
        rec_end_batch();
    f_close(&s_emgx);
    f_close(&s_csv);
    s_open = 0;
}
