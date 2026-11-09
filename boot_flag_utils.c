#include "stm32f4xx_hal.h"
#include "boot_flag_utils.h"
#include <string.h>
#include <stdint.h>

#define BOOT_FLAG_SECTOR FLASH_SECTOR_2
#define BOOT_FLAG_ADDR 0x08008000UL

#define BANK_A_SECTOR_START FLASH_SECTOR_4
#define BANK_A_ADDR 0x08010000UL

#define BANK_B_SECTOR_START FLASH_SECTOR_6
#define BANK_B_ADDR 0x08040000UL

#define BANK_A_SIZE_BYTES (192U * 1024U)
#define BANK_B_SIZE_BYTES (256U * 1024U)

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

uint32_t fota_get_inactive_bank_addr(void)
{
    uint32_t current_flag = boot_flag_read();

    if (current_flag == BOOT_FLAG_BANK_B)
    {
        return BANK_A_ADDR;
    }
    else
    {
        return BANK_B_ADDR;
    }
}

HAL_StatusTypeDef fota_erase_inactive_bank(void)
{
    uint32_t current_flag = boot_flag_read();

    uint32_t start_sector =
        (current_flag == BOOT_FLAG_BANK_B)
        ? BANK_A_SECTOR_START
        : BANK_B_SECTOR_START;

    FLASH_EraseInitTypeDef erase_init;
    uint32_t sector_error = 0;

    erase_init.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    erase_init.Sector = start_sector;
    erase_init.NbSectors = 2;

    HAL_FLASH_Unlock();

    HAL_StatusTypeDef status =
        HAL_FLASHEx_Erase(&erase_init, &sector_error);

    HAL_FLASH_Lock();

    return status;
}

HAL_StatusTypeDef fota_write_chunk(
    uint32_t offset,
    const uint8_t *data,
    uint32_t len)
{
    uint32_t target_bank_addr = fota_get_inactive_bank_addr();

    uint32_t target_bank_size =
        (target_bank_addr == BANK_A_ADDR)
        ? BANK_A_SIZE_BYTES
        : BANK_B_SIZE_BYTES;

    if ((offset > target_bank_size) ||
        (len > (target_bank_size - offset)))
    {
        return HAL_ERROR;
    }

    uint32_t target_addr = target_bank_addr + offset;
    HAL_StatusTypeDef status = HAL_OK;

    HAL_FLASH_Unlock();

    uint32_t i = 0U;

    while ((i < len) && (status == HAL_OK))
    {
        uint32_t word = 0xFFFFFFFFU;

        uint32_t remaining =
            ((len - i) >= 4U) ? 4U : (len - i);

        memcpy(&word, &data[i], remaining);

        status = HAL_FLASH_Program(
            FLASH_TYPEPROGRAM_WORD,
            target_addr + i,
            word
        );

        i += 4U;
    }

    HAL_FLASH_Lock();

    return status;
}

HAL_StatusTypeDef fota_commit(void)
{
    uint32_t current_flag = boot_flag_read();

    uint32_t new_flag =
        (current_flag == BOOT_FLAG_BANK_B)
        ? BOOT_FLAG_BANK_A
        : BOOT_FLAG_BANK_B;

    return boot_flag_write(new_flag);
}


