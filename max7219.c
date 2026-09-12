
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
    write_register(hspi, REG_DISPLAY_TEST, 0x00U);
    write_register(hspi, REG_DECODE_MODE, 0x00U);
    write_register(hspi, REG_SCAN_LIMIT, 0x07U);
    write_register(hspi, REG_INTENSITY, 0x08U);
    write_register(hspi, REG_SHUTDOWN, 0x01U);
    MAX7219_Clear(hspi);
}

void MAX7219_DisplayPattern(SPI_HandleTypeDef *hspi, const uint8_t rows[8])
{
    for (uint8_t i = 0U; i < 8U; i++)
    {
        write_register(hspi, (uint8_t)(i + 1U), rows[i]);
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
