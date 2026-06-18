#include "ads1292.h"
#include "main.h"

/* CS = PC1 (ADS_SPI_CS), DRDY = PC0 (ADS_SPI_INT) */
#define ADS_CS_LOW()    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_1, GPIO_PIN_RESET)
#define ADS_CS_HIGH()   HAL_GPIO_WritePin(GPIOC, GPIO_PIN_1, GPIO_PIN_SET)
#define ADS_DRDY_LOW()  (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_0) == GPIO_PIN_RESET)

static SPI_HandleTypeDef *_hspi;

static void ads_cmd(uint8_t cmd)
{
    ADS_CS_LOW();
    HAL_Delay(1);
    HAL_SPI_Transmit(_hspi, &cmd, 1, 10);
    HAL_Delay(1);
    ADS_CS_HIGH();
    HAL_Delay(2);
}

/* WREG: write one register. Applies reserved-bit masks per ADS1292R datasheet. */
static void ads_wreg(uint8_t reg, uint8_t val)
{
    switch (reg) {
        case 0x01: val = val & 0x87;          break;  /* CONFIG1 */
        case 0x02: val = (val & 0xFB) | 0x80; break;  /* CONFIG2: bit7 must be 1 */
        case 0x03: val = (val & 0xFD) | 0x10; break;  /* LOFF: bit4 must be 1 */
        case 0x07: val = val & 0x3F;          break;  /* LOFFSENS */
        case 0x08: val = val & 0x5F;          break;  /* LOFFSTAT */
        case 0x09: val = val | 0x02;          break;  /* RESP1: bit1 must be 1 */
        case 0x0A: val = (val & 0x87) | 0x01; break;  /* RESP2: bit0 must be 1 */
        case 0x0B: val = val & 0x0F;          break;  /* GPIO */
        default:                               break;
    }
    uint8_t tx[3] = { 0x40 | reg, 0x00, val };
    ADS_CS_LOW();
    HAL_Delay(1);
    HAL_SPI_Transmit(_hspi, tx, 3, 10);
    HAL_Delay(1);
    ADS_CS_HIGH();
    HAL_Delay(2);
}

void ADS1292_Init(SPI_HandleTypeDef *hspi)
{
    _hspi = hspi;
    ADS_CS_HIGH();
    HAL_Delay(100);
    ads_cmd(0x06);  /* RESET */
    HAL_Delay(100);
    ads_cmd(0x11);  /* SDATAC — exit continuous mode so registers are accessible */
    HAL_Delay(300);

    /* Register configuration — based on ProtoCentral V/Ohm reference setup */
    ads_wreg(ADS1292_REG_CONFIG1, 0x03);  /* CONFIG1:  1 kSPS output data rate */
    HAL_Delay(10);
    ads_wreg(ADS1292_REG_CONFIG2, 0b11100000);  /* CONFIG2:  lead-off comparators ON (PDB_LOFF_COMP, bit6), internal 2.42 V reference, test signal off */
    HAL_Delay(10);
    ads_wreg(ADS1292_REG_LOFF, ADS1292_LOFF_CONFIG);  /* lead-off current/threshold — tune via ADS1292_LOFF_CONFIG in ads1292.h */
    HAL_Delay(10);
    ads_wreg(ADS1292_REG_CH1SET, 0b01000000);  /* CH1SET:   PGA gain 4, normal electrode input */
    HAL_Delay(10);
    ads_wreg(ADS1292_REG_CH2SET, 0b01100000);  /* CH2SET:   PGA gain 8, normal electrode input */
    HAL_Delay(10);
    ads_wreg(ADS1292_REG_RLDSENS, 0x00);  /* RLDSENS:  RLD channel routing off */
    HAL_Delay(10);
    ads_wreg(ADS1292_REG_LOFFSENS, 0x03);  /* LOFFSENS: lead-off detection on IN1P/IN1N (CH1 only — CH2 unused/floating) */
    HAL_Delay(10);
    ads_wreg(ADS1292_REG_RESP1, 0b11110010);  /* RESP1:    demodulator+modulator on, RLDREF internal, 32 kHz clock */
    HAL_Delay(10);
    ads_wreg(ADS1292_REG_RESP2, 0b00000011);  /* RESP2:    clock output enabled, RLDREF internally generated */
    HAL_Delay(10);
}

uint8_t ADS1292_ReadID(void)
{
    uint8_t tx[2] = { 0x20, 0x00 };  /* RREG addr=0x00, count-1=0 */
    uint8_t rx = 0;
    ADS_CS_LOW();
    HAL_Delay(1);
    HAL_SPI_Transmit(_hspi, tx, 2, 10);
    HAL_SPI_Receive(_hspi,  &rx, 1, 10);
    ADS_CS_HIGH();
    return rx;
}

void ADS1292_StartContinuous(void)
{
    ads_cmd(0x08);  /* START */
    ads_cmd(0x10);  /* RDATAC */
}

void ADS1292_Stop(void)
{
    ads_cmd(0x11);  /* SDATAC */
    ads_cmd(0x0A);  /* STOP */
}

void ADS1292_ExitRDATAC(void)
{
    ads_cmd(0x11);  /* SDATAC only — chip keeps converting, START pin stays HIGH */
}

uint8_t ADS1292_ReadReg(uint8_t reg)
{
    uint8_t tx[2] = { 0x20 | (reg & 0x1F), 0x00 };
    uint8_t rx = 0;
    ADS_CS_LOW();
    HAL_Delay(1);
    HAL_SPI_Transmit(_hspi, tx, 2, 10);
    HAL_SPI_Receive(_hspi, &rx, 1, 10);
    ADS_CS_HIGH();
    HAL_Delay(2);
    return rx;
}

void ADS1292_ReadRDATA(uint8_t buf[9])
{
    uint8_t cmd = 0x12;  /* RDATA: read one conversion result, SDATAC mode only */
    ADS_CS_LOW();
    HAL_Delay(1);
    HAL_SPI_Transmit(_hspi, &cmd, 1, 10);
    HAL_SPI_Receive(_hspi, buf, 9, 50);
    ADS_CS_HIGH();
    HAL_Delay(2);
}

void ADS1292_ReadRaw(uint8_t buf[9])
{
    ADS_CS_LOW();
    /* tCSS ≥ 7.8 µs (4×tCLKIN at 512 kHz). ~200 iterations at 16 MHz ≈ 25 µs. */
    for (volatile uint32_t i = 0; i < 200; i++) {}
    HAL_SPI_Receive(_hspi, buf, 9, 50);
    ADS_CS_HIGH();
}

/* Same as ReadRaw but with direct register access — ~40 µs total at 4 MHz
   SPI instead of ~240 µs through the HAL. Used by the DRDY ISR, which must
   stay shorter than the SDMMC FIFO refill deadline (~267 µs) or card
   writes fail with TX underrun. Transmits NOPs (0x00) on MOSI. */
void ADS1292_ReadRawFast(uint8_t buf[9])
{
    SPI_TypeDef *spi = _hspi->Instance;

    ADS_CS_LOW();
    /* tCSS ≥ 7.8 µs: ~64 iterations at 16 MHz ≈ 8 µs */
    for (volatile uint32_t i = 0; i < 64; i++) {}

    if (!(spi->CR1 & SPI_CR1_SPE))
        spi->CR1 |= SPI_CR1_SPE;
    while (spi->SR & SPI_SR_RXNE)              /* drain stale RX FIFO */
        (void)*(volatile uint8_t *)&spi->DR;

    for (uint32_t n = 0; n < 9; n++) {
        *(volatile uint8_t *)&spi->DR = 0x00;  /* 8-bit access — FIFO packs 16-bit otherwise */
        while (!(spi->SR & SPI_SR_RXNE)) {}
        buf[n] = *(volatile uint8_t *)&spi->DR;
    }
    ADS_CS_HIGH();
}

void ADS1292_ReadRDATAFast(uint8_t buf[9])
{
    uint8_t cmd = 0x12;
    ADS_CS_LOW();
    /* tCSS ≥ 7.8µs (4×tCLKIN). ~200 iterations at 16 MHz ≈ 25µs. */
    for (volatile uint32_t i = 0; i < 200; i++) {}
    HAL_SPI_Transmit(_hspi, &cmd, 1, 10);
    HAL_SPI_Receive(_hspi, buf, 9, 50);
    ADS_CS_HIGH();
}

uint8_t ADS1292_ReadSample(ADS1292_Sample_t *s)
{
    uint32_t t0 = HAL_GetTick();
    while (!ADS_DRDY_LOW()) {
        if (HAL_GetTick() - t0 > 20) return 0;  /* timeout ~4ms at 250SPS */
    }

    uint8_t raw[9] = {0};
    ADS_CS_LOW();
    HAL_Delay(1);
    HAL_SPI_Receive(_hspi, raw, 9, 50);
    ADS_CS_HIGH();

    s->status = ((uint32_t)raw[0] << 16) | ((uint32_t)raw[1] << 8) | raw[2];
    /* sign-extend 24-bit two's complement to int32 */
    s->ch1 = ((int32_t)(raw[3] << 24 | raw[4] << 16 | raw[5] << 8)) >> 8;
    s->ch2 = ((int32_t)(raw[6] << 24 | raw[7] << 16 | raw[8] << 8)) >> 8;
    return 1;
}
