
#ifndef MAX7219_H
#define MAX7219_H

#include "stm32f4xx_hal.h"

void MAX7219_Init(SPI_HandleTypeDef *hspi);
void MAX7219_DisplayPattern(SPI_HandleTypeDef *hspi, const uint8_t rows[8]);
void MAX7219_Clear(SPI_HandleTypeDef *hspi);

extern const uint8_t MAX7219_PATTERN_A[8];
extern const uint8_t MAX7219_PATTERN_B[8];

#endif
