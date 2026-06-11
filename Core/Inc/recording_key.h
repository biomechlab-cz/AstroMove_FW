#ifndef RECORDING_KEY_H
#define RECORDING_KEY_H

#include <stdint.h>

/* AES-256 recording key.
 *
 * !!! PLACEHOLDER TEST KEY — replace with a securely provisioned key
 * !!! before any real deployment. Anyone with this repository can
 * !!! decrypt recordings made with this key.
 *
 * Key bytes are used in array order (matches the byte order expected by
 * standard AES-GCM implementations, e.g. Python "cryptography").
 * decode_emgx.py uses this same test key as its default. */
#define REC_KEY_ID      0x01
#define REC_KEY_VERSION 0x01

static const uint8_t REC_AES_KEY[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};

#endif /* RECORDING_KEY_H */
