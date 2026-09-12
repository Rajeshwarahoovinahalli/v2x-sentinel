
#include "can_transport.h"
#include <string.h>
#include <stdio.h>
#include "stm32f4xx_hal.h"

extern UART_HandleTypeDef huart2;
static void trans_dbg(const char *s)
{
    HAL_UART_Transmit(&huart2, (uint8_t*)s, strlen(s), 100);
}

void CAN_Transport_Send(MCP2515_Handle *h, uint16_t can_id, const uint8_t *data, uint16_t len)
{
    CAN_Frame frame;
    frame.id = can_id;

    char dbg[64];
    snprintf(dbg, sizeof(dbg), "TRANSPORT_SEND: id=0x%03X len=%u\r\n", can_id, len);
    trans_dbg(dbg);

    if (len <= 7U)
    {

        frame.dlc = (uint8_t)(len + 1U);
        frame.data[0] = (uint8_t)(PCI_TYPE_SF | (len & 0x0FU));
        memcpy(&frame.data[1], data, len);
        MCP2515_SendFrame(h, &frame);
        trans_dbg("  -> SF sent\r\n");
    }
    else
    {

        frame.dlc = 8U;
        frame.data[0] = (uint8_t)(PCI_TYPE_FF | ((len >> 8) & 0x0FU));
        frame.data[1] = (uint8_t)(len & 0xFFU);
        memcpy(&frame.data[2], data, 6U);
        MCP2515_SendFrame(h, &frame);
        trans_dbg("  -> FF sent\r\n");

        uint16_t sent = 6U;
        uint8_t seq = 1U;
        while (sent < len)
        {
            uint16_t remaining = (uint16_t)(len - sent);
            uint8_t chunk = (remaining > 7U) ? 7U : (uint8_t)remaining;

            frame.dlc = (uint8_t)(chunk + 1U);
            frame.data[0] = (uint8_t)(PCI_TYPE_CF | (seq & 0x0FU));
            memcpy(&frame.data[1], &data[sent], chunk);
            MCP2515_SendFrame(h, &frame);

            snprintf(dbg, sizeof(dbg), "  -> CF seq=%u chunk=%u sent\r\n", seq, chunk);
            trans_dbg(dbg);

            sent = (uint16_t)(sent + chunk);
            seq = (uint8_t)((seq + 1U) & 0x0FU);
            if (seq == 0U) { seq = 1U; }

            HAL_Delay(2);
        }
        trans_dbg("TRANSPORT_SEND: complete\r\n");
    }
}

uint8_t CAN_Transport_ProcessFrame(CAN_Transport_RxState *rx_state, const CAN_Frame *frame)
{
    if (frame->dlc == 0U) { return 0U; }

    uint8_t pci_type = (uint8_t)(frame->data[0] & 0xF0U);

    if (pci_type == PCI_TYPE_SF)
    {
        uint8_t len = (uint8_t)(frame->data[0] & 0x0FU);
        if (len > (frame->dlc - 1U)) { return 0U; }
        if (len > CAN_TRANSPORT_MAX_MSG) { return 0U; }

        memcpy(rx_state->buf, &frame->data[1], len);
        rx_state->recv_len = len;
        rx_state->total_len = len;
        rx_state->in_progress = 0U;
        return 1U;
    }
    else if (pci_type == PCI_TYPE_FF)
    {
        uint16_t len = (uint16_t)(((uint16_t)(frame->data[0] & 0x0FU) << 8) | frame->data[1]);
        if (len > CAN_TRANSPORT_MAX_MSG) { return 0U; }

        uint8_t first_chunk = (frame->dlc >= 8U) ? 6U : (uint8_t)(frame->dlc - 2U);
        memcpy(rx_state->buf, &frame->data[2], first_chunk);

        rx_state->total_len = len;
        rx_state->recv_len = first_chunk;
        rx_state->expected_seq = 1U;
        rx_state->in_progress = 1U;
        return 0U;
    }
    else if (pci_type == PCI_TYPE_CF)
    {
        if (rx_state->in_progress == 0U) { return 0U; }

        uint8_t seq = (uint8_t)(frame->data[0] & 0x0FU);
        if (seq != rx_state->expected_seq)
        {

            rx_state->in_progress = 0U;
            rx_state->recv_len = 0U;
            return 0U;
        }

        uint8_t chunk_len = (uint8_t)(frame->dlc - 1U);
        uint16_t remaining_space = (uint16_t)(CAN_TRANSPORT_MAX_MSG - rx_state->recv_len);
        if (chunk_len > remaining_space)
        {
            rx_state->in_progress = 0U;
            rx_state->recv_len = 0U;
            return 0U;
        }

        memcpy(&rx_state->buf[rx_state->recv_len], &frame->data[1], chunk_len);
        rx_state->recv_len = (uint16_t)(rx_state->recv_len + chunk_len);

        rx_state->expected_seq = (uint8_t)((rx_state->expected_seq + 1U) & 0x0FU);
        if (rx_state->expected_seq == 0U) { rx_state->expected_seq = 1U; }

        if (rx_state->recv_len >= rx_state->total_len)
        {
            rx_state->in_progress = 0U;
            return 1U;
        }
        return 0U;
    }
    else
    {
        return 0U;
    }
}
