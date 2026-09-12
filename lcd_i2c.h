
#ifndef LCD_I2C_H
#define LCD_I2C_H

#include "stm32f4xx_hal.h"

#define LCD_I2C_ADDR (0x27U << 1)

void LCD_Init(I2C_HandleTypeDef *hi2c);
void LCD_Clear(I2C_HandleTypeDef *hi2c);
void LCD_SetCursor(I2C_HandleTypeDef *hi2c, uint8_t col, uint8_t row);
void LCD_Print(I2C_HandleTypeDef *hi2c, const char *str);

#endif
