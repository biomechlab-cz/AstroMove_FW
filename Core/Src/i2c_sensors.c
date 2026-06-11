#include "i2c_sensors.h"

/* ISM330DHCX: 7-bit 0x6A (SDO=GND) or 0x6B (SDO=VDD) — probed at init */
#define ISM330_ADDR_L 0xD4
#define ISM330_ADDR_H 0xD6
/* MMC5983MA: 7-bit 0x30 */
#define MMC5983_ADDR 0x60
/* RV-3028-C7:  7-bit 0x52 */
#define RV3028_ADDR  0xA4

static I2C_HandleTypeDef *_hi2c;
static uint8_t _ism_addr = ISM330_ADDR_L;

void I2C_Sensors_Init(I2C_HandleTypeDef *hi2c)
{
    _hi2c = hi2c;

    /* The SDO address strap differs between board builds — probe it */
    if (HAL_I2C_IsDeviceReady(_hi2c, ISM330_ADDR_L, 2, 10) == HAL_OK)
        _ism_addr = ISM330_ADDR_L;
    else if (HAL_I2C_IsDeviceReady(_hi2c, ISM330_ADDR_H, 2, 10) == HAL_OK)
        _ism_addr = ISM330_ADDR_H;

    /* ISM330DHCX: CTRL1_XL = 0x50 (208Hz, ±2g)
                   CTRL2_G  = 0x50 (208Hz, ±250dps) */
    uint8_t val = 0x50;
    HAL_I2C_Mem_Write(_hi2c, _ism_addr, 0x10, I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
    HAL_I2C_Mem_Write(_hi2c, _ism_addr, 0x11, I2C_MEMADD_SIZE_8BIT, &val, 1, 100);

    /* MMC5983MA: SET pulse to initialize magnetization, then continuous
       measurement at 100 Hz with automatic set/reset — MMC5983_ReadSample
       just fetches the latest result. */
    val = 0x08;  /* Ctrl0: DO_SET */
    HAL_I2C_Mem_Write(_hi2c, MMC5983_ADDR, 0x09, I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
    HAL_Delay(1);
    val = 0x20;  /* Ctrl0: Auto_SR_en — periodic set/reset against drift */
    HAL_I2C_Mem_Write(_hi2c, MMC5983_ADDR, 0x09, I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
    val = 0x00;  /* Ctrl1: BW[1:0]=00 → 8 ms measurement time */
    HAL_I2C_Mem_Write(_hi2c, MMC5983_ADDR, 0x0A, I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
    val = 0x0D;  /* Ctrl2: Cmm_en | CM_FREQ=100 Hz */
    HAL_I2C_Mem_Write(_hi2c, MMC5983_ADDR, 0x0B, I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
}

uint8_t ISM330_ReadSample(ISM330_Data_t *out)
{
    uint8_t buf[12];
    /* Burst read: OUTX_L_G (0x22) through OUTZ_H_XL (0x2D) */
    if (HAL_I2C_Mem_Read(_hi2c, _ism_addr, 0x22, I2C_MEMADD_SIZE_8BIT, buf, 12, 100) != HAL_OK)
        return 0;

    out->gx = (int16_t)(buf[1]  << 8 | buf[0]);
    out->gy = (int16_t)(buf[3]  << 8 | buf[2]);
    out->gz = (int16_t)(buf[5]  << 8 | buf[4]);
    out->ax = (int16_t)(buf[7]  << 8 | buf[6]);
    out->ay = (int16_t)(buf[9]  << 8 | buf[8]);
    out->az = (int16_t)(buf[11] << 8 | buf[10]);
    return 1;
}

int16_t ISM330_ReadTemperatureTenths(void)
{
    uint8_t buf[2];
    /* OUT_TEMP_L/H (0x20/0x21): 25 °C + raw/256 per LSB */
    if (HAL_I2C_Mem_Read(_hi2c, _ism_addr, 0x20, I2C_MEMADD_SIZE_8BIT, buf, 2, 100) != HAL_OK)
        return INT16_MIN;
    int16_t raw = (int16_t)(buf[1] << 8 | buf[0]);
    return (int16_t)(250 + ((int32_t)raw * 10) / 256);
}

uint8_t MMC5983_ReadSample(MMC5983_Data_t *out)
{
    /* Continuous mode at 100 Hz — output registers always hold the latest
       measurement. Burst read registers 0x00-0x06: X[17:10], X[9:2],
       Y[17:10], Y[9:2], Z[17:10], Z[9:2], {X[1:0],Y[1:0],Z[1:0]} */
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
