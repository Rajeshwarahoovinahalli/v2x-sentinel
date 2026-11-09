
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
    HAL_Delay(10); /* allow the chip's internal oscillator to stabilize */
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
    /* CANCTRL[7:5] selects the operating mode */
    uint8_t current = MCP2515_ReadRegister(h, MCP2515_CANCTRL);
    uint8_t new_val = (uint8_t)((current & 0x1FU) | mode);
    MCP2515_WriteRegister(h, MCP2515_CANCTRL, new_val);
    HAL_Delay(10);
}

uint8_t MCP2515_GetMode(MCP2515_Handle *h)
{
    /* CANSTAT[7:5] reports the CURRENT operating mode */
    return (uint8_t)(MCP2515_ReadRegister(h, MCP2515_CANSTAT) & 0xE0U);
}

/*
 * Bit timing for 500 kbps with an 8 MHz crystal.
 * Tuned to exactly match Linux SocketCAN's auto-calculated timing
 * for `ip link set can0 up type can bitrate 500000` on the same
 * 8MHz-oscillator MCP2515 (prop-seg=2, phase-seg1=3, phase-seg2=2,
 * sjw=1, sample point 75%) - both sides must sample at the same
 * point within each bit, not just agree on the overall bit rate.
 */
HAL_StatusTypeDef MCP2515_SetBitrate500kbps_8MHz(MCP2515_Handle *h)
{
    MCP2515_WriteRegister(h, MCP2515_CNF1, 0x00U);
    MCP2515_WriteRegister(h, MCP2515_CNF2, 0x91U);
    MCP2515_WriteRegister(h, MCP2515_CNF3, 0x01U);
    return HAL_OK;
}

void MCP2515_SendFrame(MCP2515_Handle *h, const CAN_Frame *frame)
{
    /* Standard 11-bit ID goes into SIDH (bits 10-3) and SIDL (bits 2-0, top 3 bits) */
    uint8_t sidh = (uint8_t)(frame->id >> 3);
    uint8_t sidl = (uint8_t)((frame->id & 0x07U) << 5);

    MCP2515_WriteRegister(h, MCP2515_TXB0SIDH, sidh);
    MCP2515_WriteRegister(h, MCP2515_TXB0SIDL, sidl);
    MCP2515_WriteRegister(h, MCP2515_TXB0DLC, frame->dlc & 0x0FU);

    for (uint8_t i = 0U; i < frame->dlc; i++)
    {
        MCP2515_WriteRegister(h, (uint8_t)(MCP2515_TXB0D0 + i), frame->data[i]);
    }

    /* Request-to-send: tells the MCP2515 to actually transmit TXB0 */
    uint8_t cmd = MCP2515_RTS_TXB0;
    CS_Low(h);
    HAL_SPI_Transmit(h->hspi, &cmd, 1, 100);
    CS_High(h);
}

/*
 * MCP2515_GetTxStatus()
 * Reads TXB0CTRL after a send to check what actually happened on
 * the bus - the previous version of this driver never checked this,
 * so it reported "TX sent" even if the frame was never successfully
 * acknowledged by any other node (e.g. no other node present, or a
 * wiring/termination problem prevents the bus from working at all).
 *
 * TXB0CTRL bit meanings:
 *   bit 4 (TXREQ)  - 1 = still pending/in progress, 0 = done (sent or aborted)
 *   bit 5 (TXERR)  - 1 = a bus error occurred during this transmission
 *   bit 6 (MLOA)   - 1 = arbitration was lost
 */
uint8_t MCP2515_GetTxStatus(MCP2515_Handle *h)
{
    return MCP2515_ReadRegister(h, MCP2515_TXB0CTRL);
}

uint8_t MCP2515_FrameAvailable(MCP2515_Handle *h)
{
    uint8_t intf = MCP2515_ReadRegister(h, MCP2515_CANINTF);
    return (intf & 0x01U) != 0U ? 1U : 0U;  /* RX0IF flag */
}

/*
 * MCP2515_ReadFrame() - FIXED VERSION
 * ------------------------------------
 * Previously this read SIDH, SIDL, DLC, and each data byte as
 * SEPARATE SPI transactions (separate CS toggles). This created a
 * "torn read" risk: if the MCP2515's internal state changed between
 * two of those separate reads (e.g. right at the moment the next
 * frame's fields start being written into the same buffer), the ID
 * bytes and the data bytes could end up coming from two DIFFERENT
 * points in time - explaining the observed bug where ID read back
 * as 0x000 while the data still matched a previous, already-reported
 * frame.
 *
 * THE FIX: use the MCP2515's dedicated "Read RX Buffer" burst
 * instruction (0x90 for RXB0). This reads SIDH, SIDL, EID8, EID0,
 * DLC, and all data bytes in ONE continuous SPI transaction, with
 * CS held low the entire time - the MCP2515 guarantees this is
 * read atomically as a single, consistent snapshot of the buffer.
 */
void MCP2515_ReadFrame(MCP2515_Handle *h, CAN_Frame *frame)
{
    uint8_t tx[14] = {0};
    uint8_t rx[14] = {0};
    tx[0] = MCP2515_READ_RXB0;   /* burst-read command, starts at SIDH */

    CS_Low(h);
    HAL_SPI_TransmitReceive(h->hspi, tx, rx, 14, 100);
    CS_High(h);

    /* rx[0] = don't care (echoed during command byte)
     * rx[1] = SIDH, rx[2] = SIDL, rx[3] = EID8, rx[4] = EID0,
     * rx[5] = DLC, rx[6..13] = up to 8 data bytes */
    uint8_t sidh = rx[1];
    uint8_t sidl = rx[2];
    frame->id = (uint16_t)(((uint16_t)sidh << 3) | (sidl >> 5));

    frame->dlc = (uint8_t)(rx[5] & 0x0FU);
    if (frame->dlc > 8U) { frame->dlc = 8U; }  /* defensive clamp */

    for (uint8_t i = 0U; i < frame->dlc; i++)
    {
        frame->data[i] = rx[6U + i];
    }

    /* Clear the RX0IF flag so the next frame can be detected */
    MCP2515_WriteRegister(h, MCP2515_CANINTF, 0x00U);
}
