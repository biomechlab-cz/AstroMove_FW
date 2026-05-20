#include "i2c_sensors.h"

/* ISM330DHCX: SDO=GND → 7-bit 0x6A */
#define ISM330_ADDR  0xD4
/* RV-3028-C7:  7-bit 0x52 */
#define RV3028_ADDR  0xA4

static I2C_HandleTypeDef *_hi2c;

void I2C_Sensors_Init(I2C_HandleTypeDef *hi2c)
{
    _hi2c = hi2c;

    /* ISM330DHCX: CTRL1_XL = 0x50 (208Hz, ±2g)
                   CTRL2_G  = 0x50 (208Hz, ±250dps) */
    uint8_t val = 0x50;
    HAL_I2C_Mem_Write(_hi2c, ISM330_ADDR, 0x10, I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
    HAL_I2C_Mem_Write(_hi2c, ISM330_ADDR, 0x11, I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
}

uint8_t ISM330_ReadSample(ISM330_Data_t *out)
{
    uint8_t buf[12];
    /* Burst read: OUTX_L_G (0x22) through OUTZ_H_XL (0x2D) */
    if (HAL_I2C_Mem_Read(_hi2c, ISM330_ADDR, 0x22, I2C_MEMADD_SIZE_8BIT, buf, 12, 100) != HAL_OK)
        return 0;

    out->gx = (int16_t)(buf[1]  << 8 | buf[0]);
    out->gy = (int16_t)(buf[3]  << 8 | buf[2]);
    out->gz = (int16_t)(buf[5]  << 8 | buf[4]);
    out->ax = (int16_t)(buf[7]  << 8 | buf[6]);
    out->ay = (int16_t)(buf[9]  << 8 | buf[8]);
    out->az = (int16_t)(buf[11] << 8 | buf[10]);
    return 1;
}

uint8_t RV3028_ReadTime(RTC_Time_t *out)
{
    uint8_t buf[7];
    if (HAL_I2C_Mem_Read(_hi2c, RV3028_ADDR, 0x00, I2C_MEMADD_SIZE_8BIT, buf, 7, 100) != HAL_OK)
        return 0;

    out->sec  = buf[0];
    out->min  = buf[1];
    out->hr   = buf[2];
    out->date = buf[4];
    out->mon  = buf[5];
    out->yr   = buf[6];
    return 1;
}

uint8_t RV3028_SetTime(const RTC_Time_t *t)
{
    uint8_t buf[7] = { t->sec, t->min, t->hr, 0x00, t->date, t->mon, t->yr };
    return (HAL_I2C_Mem_Write(_hi2c, RV3028_ADDR, 0x00, I2C_MEMADD_SIZE_8BIT, buf, 7, 100) == HAL_OK) ? 1 : 0;
}
