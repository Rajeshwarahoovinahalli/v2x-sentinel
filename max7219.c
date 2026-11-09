/*
 * max7219.c
 * ===========
 * CS is driven manually (SPI2's hardware NSS is not used), matching
 * the same manual-CS pattern already proven for the MCP2515 driver
 * on SPI1.
 */

#include "max7219.h"

#define MAX7219_CS_PORT GPIOB
#define MAX7219_CS_PIN  GPIO_PIN_12

#define REG_DECODE_MODE  0x09U
#define REG_INTENSITY    0x0AU
#define REG_SCAN_LIMIT   0x0BU
#define REG_SHUTDOWN     0x0CU
#define REG_DISPLAY_TEST 0x0FU

static void cs_low(void)  { HAL_GPIO_WritePin(MAX7219_CS_PORT, MAX7219_CS_PIN, GPIO_PIN_RESET); }
static void cs_high(void) { HAL_GPIO_WritePin(MAX7219_CS_PORT, MAX7219_CS_PIN, GPIO_PIN_SET); }

static void write_register(SPI_HandleTypeDef *hspi, uint8_t reg, uint8_t data)
{
    uint8_t tx[2] = { reg, data };
    cs_low();
    HAL_SPI_Transmit(hspi, tx, 2, 100);
    cs_high();
}

void MAX7219_Init(SPI_HandleTypeDef *hspi)
{
    cs_high();
    write_register(hspi, REG_DISPLAY_TEST, 0x00U);  /* test mode off */
    write_register(hspi, REG_DECODE_MODE, 0x00U);   /* no BCD decode - raw LED addressing */
    write_register(hspi, REG_SCAN_LIMIT, 0x07U);    /* scan all 8 digits/rows */
    write_register(hspi, REG_INTENSITY, 0x08U);     /* medium brightness */
    write_register(hspi, REG_SHUTDOWN, 0x01U);      /* leave shutdown mode - normal operation */
    MAX7219_Clear(hspi);
}

void MAX7219_DisplayPattern(SPI_HandleTypeDef *hspi, const uint8_t rows[8])
{
    for (uint8_t i = 0U; i < 8U; i++)
    {
        write_register(hspi, (uint8_t)(i + 1U), rows[i]);   /* digit registers are 0x01-0x08 */
    }
}

void MAX7219_Clear(SPI_HandleTypeDef *hspi)
{
    for (uint8_t i = 1U; i <= 8U; i++)
    {
        write_register(hspi, i, 0x00U);
    }
}

const uint8_t MAX7219_PATTERN_A[8] = {
    0b00111100,
    0b01000010,
    0b10000001,
    0b10000001,
    0b11111111,
    0b10000001,
    0b10000001,
    0b10000001
};

const uint8_t MAX7219_PATTERN_B[8] = {
    0b11111110,
    0b10000001,
    0b10000001,
    0b11111110,
    0b10000001,
    0b10000001,
    0b10000001,
    0b11111110
};
