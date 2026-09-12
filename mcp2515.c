
#include "mcp2515.h"

static void CS_Low(MCP2515_Handle *h)  { HAL_GPIO_WritePin(h->cs_port, h->cs_pin, GPIO_PIN_RESET); }
static void CS_High(MCP2515_Handle *h) { HAL_GPIO_WritePin(h->cs_port, h->cs_pin, GPIO_PIN_SET); }

void MCP2515_Init(MCP2515_Handle *h, SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin)
{
    h->hspi = hspi;
    h->cs_port = cs_port;
    h->cs_pin = cs_pin;
    CS_High(h);
}

HAL_StatusTypeDef MCP2515_Reset(MCP2515_Handle *h)
{
    uint8_t cmd = MCP2515_RESET;
    CS_Low(h);
    HAL_StatusTypeDef status = HAL_SPI_Transmit(h->hspi, &cmd, 1, 100);
    CS_High(h);
    HAL_Delay(10);
    return status;
}

uint8_t MCP2515_ReadRegister(MCP2515_Handle *h, uint8_t addr)
{
    uint8_t tx[3] = { MCP2515_READ, addr, 0x00U };
    uint8_t rx[3] = { 0 };
    CS_Low(h);
    HAL_SPI_TransmitReceive(h->hspi, tx, rx, 3, 100);
    CS_High(h);
    return rx[2];
}

void MCP2515_WriteRegister(MCP2515_Handle *h, uint8_t addr, uint8_t value)
{
    uint8_t tx[3] = { MCP2515_WRITE, addr, value };
    CS_Low(h);
    HAL_SPI_Transmit(h->hspi, tx, 3, 100);
    CS_High(h);
}

void MCP2515_SetMode(MCP2515_Handle *h, uint8_t mode)
{

    uint8_t current = MCP2515_ReadRegister(h, MCP2515_CANCTRL);
    uint8_t new_val = (uint8_t)((current & 0x1FU) | mode);
    MCP2515_WriteRegister(h, MCP2515_CANCTRL, new_val);
    HAL_Delay(10);
}

uint8_t MCP2515_GetMode(MCP2515_Handle *h)
{

    return (uint8_t)(MCP2515_ReadRegister(h, MCP2515_CANSTAT) & 0xE0U);
}

HAL_StatusTypeDef MCP2515_SetBitrate500kbps_8MHz(MCP2515_Handle *h)
{
    MCP2515_WriteRegister(h, MCP2515_CNF1, 0x00U);
    MCP2515_WriteRegister(h, MCP2515_CNF2, 0x91U);
    MCP2515_WriteRegister(h, MCP2515_CNF3, 0x01U);
    return HAL_OK;
}

void MCP2515_SendFrame(MCP2515_Handle *h, const CAN_Frame *frame)
{

    uint32_t wait_start = HAL_GetTick();
    while ((MCP2515_ReadRegister(h, MCP2515_TXB0CTRL) & 0x08U) != 0U)
    {
        if ((HAL_GetTick() - wait_start) > 50U)
        {
            break;
        }
    }

    uint8_t sidh = (uint8_t)(frame->id >> 3);
    uint8_t sidl = (uint8_t)((frame->id & 0x07U) << 5);

    MCP2515_WriteRegister(h, MCP2515_TXB0SIDH, sidh);
    MCP2515_WriteRegister(h, MCP2515_TXB0SIDL, sidl);
    MCP2515_WriteRegister(h, MCP2515_TXB0DLC, frame->dlc & 0x0FU);

    for (uint8_t i = 0U; i < frame->dlc; i++)
    {
        MCP2515_WriteRegister(h, (uint8_t)(MCP2515_TXB0D0 + i), frame->data[i]);
    }

    uint8_t cmd = MCP2515_RTS_TXB0;
    CS_Low(h);
    HAL_SPI_Transmit(h->hspi, &cmd, 1, 100);
    CS_High(h);
}

uint8_t MCP2515_GetTxStatus(MCP2515_Handle *h)
{
    return MCP2515_ReadRegister(h, MCP2515_TXB0CTRL);
}

uint8_t MCP2515_FrameAvailable(MCP2515_Handle *h)
{
    uint8_t intf = MCP2515_ReadRegister(h, MCP2515_CANINTF);
    return (intf & 0x01U) != 0U ? 1U : 0U;
}

void MCP2515_ReadFrame(MCP2515_Handle *h, CAN_Frame *frame)
{
    uint8_t tx[14] = {0};
    uint8_t rx[14] = {0};
    tx[0] = MCP2515_READ_RXB0;

    CS_Low(h);
    HAL_SPI_TransmitReceive(h->hspi, tx, rx, 14, 100);
    CS_High(h);

    uint8_t sidh = rx[1];
    uint8_t sidl = rx[2];
    frame->id = (uint16_t)(((uint16_t)sidh << 3) | (sidl >> 5));

    frame->dlc = (uint8_t)(rx[5] & 0x0FU);
    if (frame->dlc > 8U) { frame->dlc = 8U; }

    for (uint8_t i = 0U; i < frame->dlc; i++)
    {
        frame->data[i] = rx[6U + i];
    }

    MCP2515_WriteRegister(h, MCP2515_CANINTF, 0x00U);
}
