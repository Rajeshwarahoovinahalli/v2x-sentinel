#include "stm32f4xx_hal.h"
#include <stdint.h>

#define BOOT_FLAG_SECTOR     FLASH_SECTOR_2
#define BOOT_FLAG_ADDR       0x08008000UL

#define BOOT_FLAG_BANK_A     0xA5A5A5A5UL
#define BOOT_FLAG_BANK_B     0xB6B6B6B6UL

uint32_t boot_flag_read(void)
{
    return *(volatile uint32_t *)BOOT_FLAG_ADDR;
}

HAL_StatusTypeDef boot_flag_write(uint32_t new_flag)
{
    FLASH_EraseInitTypeDef erase_init;
    uint32_t sector_error = 0;

    erase_init.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    erase_init.Sector = BOOT_FLAG_SECTOR;
    erase_init.NbSectors = 1;

    HAL_FLASH_Unlock();

    HAL_StatusTypeDef status =
        HAL_FLASHEx_Erase(&erase_init, &sector_error);

    if (status == HAL_OK)
    {
        status = HAL_FLASH_Program(
            FLASH_TYPEPROGRAM_WORD,
            BOOT_FLAG_ADDR,
            new_flag
        );
    }

    HAL_FLASH_Lock();

    return status;
}

HAL_StatusTypeDef boot_flag_toggle(void)
{
    uint32_t current = boot_flag_read();

    uint32_t new_flag =
        (current == BOOT_FLAG_BANK_B)
        ? BOOT_FLAG_BANK_A
        : BOOT_FLAG_BANK_B;

    return boot_flag_write(new_flag);
}
