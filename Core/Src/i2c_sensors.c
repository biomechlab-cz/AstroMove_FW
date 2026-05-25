#include "i2c_sensors.h"

/* ISM330DHCX: SDO=GND → 7-bit 0x6A */
#define ISM330_ADDR  0xD4
/* MMC5983MA: 7-bit 0x30 */
#define MMC5983_ADDR 0x60
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

    /* MMC5983MA: SET pulse to initialize magnetization, then BW=00 (100 Hz bandwidth) */
    val = 0x08;  /* Ctrl0: DO_SET */
    HAL_I2C_Mem_Write(_hi2c, MMC5983_ADDR, 0x09, I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
    HAL_Delay(1);
    val = 0x00;  /* Ctrl1: BW[1:0]=00 → 100 Hz bandwidth */
    HAL_I2C_Mem_Write(_hi2c, MMC5983_ADDR, 0x0A, I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
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

uint8_t MMC5983_ReadSample(MMC5983_Data_t *out)
{
    /* Trigger single measurement */
    uint8_t ctrl = 0x01;  /* Ctrl0: TM_M */
    HAL_I2C_Mem_Write(_hi2c, MMC5983_ADDR, 0x09, I2C_MEMADD_SIZE_8BIT, &ctrl, 1, 10);

    /* Poll Meas_M_Done (Status reg 0x08, bit 0) — measurement takes <1 ms at BW=00 */
    uint8_t status = 0;
    uint32_t t0 = HAL_GetTick();
    while (!(status & 0x01)) {
        HAL_I2C_Mem_Read(_hi2c, MMC5983_ADDR, 0x08, I2C_MEMADD_SIZE_8BIT, &status, 1, 10);
        if (HAL_GetTick() - t0 > 5) return 0;
    }

    /* Burst read registers 0x00-0x06: X[17:10], X[9:2], Y[17:10], Y[9:2],
       Z[17:10], Z[9:2], {X[1:0],Y[1:0],Z[1:0]} */
    uint8_t buf[7];
    if (HAL_I2C_Mem_Read(_hi2c, MMC5983_ADDR, 0x00, I2C_MEMADD_SIZE_8BIT, buf, 7, 20) != HAL_OK)
        return 0;

    out->mx = ((uint32_t)buf[0] << 10) | ((uint32_t)buf[1] << 2) | ((buf[6] >> 6) & 0x03);
    out->my = ((uint32_t)buf[2] << 10) | ((uint32_t)buf[3] << 2) | ((buf[6] >> 4) & 0x03);
    out->mz = ((uint32_t)buf[4] << 10) | ((uint32_t)buf[5] << 2) | ((buf[6] >> 2) & 0x03);
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
