/*
 * main_sync_demo.c — sync + RTC blink demo
 *
 * Sequence (cable connected at power-on):
 *   1. 1 blink  → sending sync pulse, set RTC = 0
 *   2. Dark     → listening for peer's pulse (up to 3 s)
 *   3. 5 blinks → peer's pulse received, re-set RTC = 0
 *   4. Dark     → waiting for sync cable to be unplugged
 *   5. Blink    → LED ON on odd RTC seconds, OFF on even (both devices in sync)
 *
 * Without cable: RTC is not reset, blink starts immediately from whatever RTC holds.
 *
 * Hardware:
 *   PC6  Sync A  — output (pulse), then input pull-up (listen)
 *   PC7  Sync B  — input pull-up; LOW = cable present
 *   PB10 LED
 *   PA9/PA10  I2C1 → RV-3028-C7 RTC
 */

#include "stm32l4xx_hal.h"
#include "i2c_sensors.h"

/* ---- handles ---- */
I2C_HandleTypeDef hi2c1;

/* ---- pins ---- */
#define LED_PIN     GPIO_PIN_10
#define LED_PORT    GPIOB
#define SYNC_A_PIN  GPIO_PIN_6
#define SYNC_B_PIN  GPIO_PIN_7
#define SYNC_PORT   GPIOC

/* ---- forward decls ---- */
void SystemClock_Config(void);
static void    gpio_init(void);
static void    i2c1_init(void);
static void    sync_a_output(void);
static void    sync_a_input(void);
static int     connector_present(void);
static int     pulse_received(void);
static void    send_pulse(void);
static void    set_rtc_zero(void);
static uint8_t read_rtc_sec(void);
static void    signal_blinks(uint8_t n);

/* ==================================================================== */

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    gpio_init();

    /* Immediate startup blink — confirms HAL_Delay works before any I2C */
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);
    HAL_Delay(300);
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
    HAL_Delay(200);

    /* TPS_ON: power gate — needed for I2C sensors on some boards */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_11, GPIO_PIN_SET);
    HAL_Delay(50);

    i2c_bus_recovery();
    i2c1_init();

    /* Enable internal 40kΩ pull-ups on PA9(SCL) and PA10(SDA).
       MspInit uses GPIO_NOPULL assuming external pull-ups exist.
       Internal pull-ups let the bus work even if external ones are absent. */
    GPIO_InitTypeDef g2 = {0};
    g2.Pin       = GPIO_PIN_9 | GPIO_PIN_10;
    g2.Mode      = GPIO_MODE_AF_OD;
    g2.Pull      = GPIO_PULLUP;
    g2.Speed     = GPIO_SPEED_FREQ_LOW;
    g2.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOA, &g2);

    HAL_Delay(100);

    I2C_Sensors_Init(&hi2c1);  /* sets internal _hi2c pointer; IMU/mag NAKs are fine */

    /* ---- sync sequence ---- */
    if (connector_present()) {
        signal_blinks(1);       /* 1 blink = sending pulse */
        send_pulse();
        set_rtc_zero();

        /* go dark, listen for peer */
        HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
        uint32_t deadline = HAL_GetTick() + 3000;
        while (HAL_GetTick() < deadline && connector_present()) {
            if (pulse_received()) {
                while (pulse_received()) {}
                set_rtc_zero();
                signal_blinks(5);   /* 5 blinks = synced */
                break;
            }
        }

        /* stay dark until cable is removed */
        HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
        while (connector_present()) {}
    }

    signal_blinks(3);   /* 3 blinks = starting */

    /* if RTC not responding, rapid blink as error */
    if (read_rtc_sec() == 0xFF) {
        while (1) { HAL_GPIO_TogglePin(LED_PORT, LED_PIN); HAL_Delay(100); }
    }



    /* LED ON on odd RTC seconds, OFF on even */
    while (1) {
        uint8_t sec = read_rtc_sec();
        HAL_GPIO_WritePin(LED_PORT, LED_PIN, (sec & 1) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_Delay(50);
    }
}



static void set_rtc_zero(void)
{
    RTC_Time_t t = {0};
    RV3028_SetTime(&t);
}

static uint8_t read_rtc_sec(void)
{
    RTC_Time_t t = {0};
    if (!RV3028_ReadTime(&t)) return 0xFF;
    return (uint8_t)((t.sec >> 4) * 10 + (t.sec & 0x0F));
}

static void signal_blinks(uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);
        HAL_Delay(80);
        HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
        HAL_Delay(80);
    }
}

static void sync_a_output(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin   = SYNC_A_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(SYNC_PORT, &g);
    HAL_GPIO_WritePin(SYNC_PORT, SYNC_A_PIN, GPIO_PIN_SET);
}

static void sync_a_input(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin  = SYNC_A_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(SYNC_PORT, &g);
}

static int connector_present(void)
{
    return HAL_GPIO_ReadPin(SYNC_PORT, SYNC_B_PIN) == GPIO_PIN_RESET;
}

static int pulse_received(void)
{
    return HAL_GPIO_ReadPin(SYNC_PORT, SYNC_A_PIN) == GPIO_PIN_RESET;
}

static void send_pulse(void)
{
    sync_a_output();
    HAL_GPIO_WritePin(SYNC_PORT, SYNC_A_PIN, GPIO_PIN_RESET);  /* drive LOW */
    HAL_Delay(20);
    HAL_GPIO_WritePin(SYNC_PORT, SYNC_A_PIN, GPIO_PIN_SET);
    HAL_Delay(2);
    sync_a_input();
}

/* ==================================================================== */

static void gpio_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};

    /* PB10 LED — output, starts OFF */
    g.Pin   = LED_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_PORT, &g);
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);

    /* PC7 Sync B — input pull-up (LOW = cable) */
    g.Pin  = SYNC_B_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(SYNC_PORT, &g);

    /* PC6 Sync A — starts as input pull-up */
    sync_a_input();
}

static void i2c1_init(void)
{
    hi2c1.Instance             = I2C1;
    hi2c1.Init.Timing          = 0xF0413131;  /* ~10 kHz — tolerates weak/missing pull-ups */
    hi2c1.Init.OwnAddress1     = 0;
    hi2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2     = 0;
    hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
    HAL_I2C_Init(&hi2c1);
}

/* ==================================================================== */

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSICalibrationValue = 64;
    osc.PLL.PLLState        = RCC_PLL_NONE;
    HAL_RCC_OscConfig(&osc);

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                       | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0);
}

void Error_Handler(void)
{
    while (1) {
        HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
        HAL_Delay(100);
    }
}
