#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdint.h>

/* Mount the filesystem. Returns 1 on success. */
uint8_t SD_Card_Init(void);

/* Create or overwrite a file. Returns 1 if all bytes written. */
uint8_t SD_Card_Write(const char *filename, const uint8_t *data, uint32_t len);

/* Append to existing file (creates if not present). Returns 1 on success. */
uint8_t SD_Card_Append(const char *filename, const uint8_t *data, uint32_t len);

/* Unmount the filesystem. */
void SD_Card_Deinit(void);

#endif /* SD_CARD_H */
