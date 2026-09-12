
#include "lcd_i2c.h"

#define LCD_BACKLIGHT 0x08U
#define LCD_ENABLE    0x04U
#define LCD_RS_DATA   0x01U
#define LCD_RS_CMD    0x00U

static void i2c_write_raw(I2C_HandleTypeDef *hi2c, uint8_t data)
{
    HAL_I2C_Master_Transmit(hi2c, LCD_I2C_ADDR, &data, 1, 100);
}

static void write_nibble(I2C_HandleTypeDef *hi2c, uint8_t nibble, uint8_t rs)
{
    uint8_t data = (uint8_t)((nibble & 0xF0U) | LCD_BACKLIGHT | rs);
    i2c_write_raw(hi2c, (uint8_t)(data | LCD_ENABLE));
    i2c_write_raw(hi2c, (uint8_t)(data & (uint8_t)~LCD_ENABLE));
}

static void send_byte(I2C_HandleTypeDef *hi2c, uint8_t value, uint8_t rs)
{
    write_nibble(hi2c, value & 0xF0U, rs);
    write_nibble(hi2c, (uint8_t)((value << 4U) & 0xF0U), rs);
}

static void send_cmd(I2C_HandleTypeDef *hi2c, uint8_t cmd)  { send_byte(hi2c, cmd, LCD_RS_CMD); }
static void send_data(I2C_HandleTypeDef *hi2c, uint8_t data) { send_byte(hi2c, data, LCD_RS_DATA); }

void LCD_Init(I2C_HandleTypeDef *hi2c)
{
    HAL_Delay(50);

    write_nibble(hi2c, 0x30U, LCD_RS_CMD);
    HAL_Delay(5);
    write_nibble(hi2c, 0x30U, LCD_RS_CMD);
    HAL_Delay(1);
    write_nibble(hi2c, 0x30U, LCD_RS_CMD);
    HAL_Delay(1);
    write_nibble(hi2c, 0x20U, LCD_RS_CMD);
    HAL_Delay(1);

    send_cmd(hi2c, 0x28U);
    send_cmd(hi2c, 0x0CU);
    send_cmd(hi2c, 0x06U);
    LCD_Clear(hi2c);
}

void LCD_Clear(I2C_HandleTypeDef *hi2c)
{
    send_cmd(hi2c, 0x01U);
    HAL_Delay(2);
}

void LCD_SetCursor(I2C_HandleTypeDef *hi2c, uint8_t col, uint8_t row)
{
    uint8_t row_offsets[2] = { 0x00U, 0x40U };
    send_cmd(hi2c, (uint8_t)(0x80U | (col + row_offsets[row & 0x01U])));
}

void LCD_Print(I2C_HandleTypeDef *hi2c, const char *str)
{
    while (*str)
    {
        send_data(hi2c, (uint8_t)(*str));
        str++;
    }
}
