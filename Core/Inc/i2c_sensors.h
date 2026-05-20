#ifndef I2C_SENSORS_H
#define I2C_SENSORS_H

#include "stm32l4xx_hal.h"
#include <stdint.h>

typedef struct {
    int16_t ax, ay, az;  /* accel raw LSB — ±2g: 1g = 16384 */
    int16_t gx, gy, gz;  /* gyro  raw LSB — ±250dps: 1dps = 131 */
} ISM330_Data_t;

typedef struct {
    uint8_t sec, min, hr;       /* BCD */
    uint8_t date, mon, yr;      /* BCD */
} RTC_Time_t;

/* Call once after power-on. Configures ISM330DHCX at 208Hz ±2g/±250dps. */
void    I2C_Sensors_Init(I2C_HandleTypeDef *hi2c);

/* Returns 1 on success, 0 if sensor not responding. */
uint8_t ISM330_ReadSample(ISM330_Data_t *out);

uint8_t RV3028_ReadTime(RTC_Time_t *out);
uint8_t RV3028_SetTime(const RTC_Time_t *t);

#endif /* I2C_SENSORS_H */
