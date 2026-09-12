
#include "can_transport_pi.h"
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

void CAN_Transport_Send(int sock, uint32_t can_id, const uint8_t *data, uint16_t len)
{
    struct can_frame frame;

    if (len <= 7U)
    {
        memset(&frame, 0, sizeof(frame));
        frame.can_id = can_id;
        frame.can_dlc = (uint8_t)(len + 1U);
        frame.data[0] = (uint8_t)(PCI_TYPE_SF | (len & 0x0FU));
        memcpy(&frame.data[1], data, len);
        write(sock, &frame, sizeof(frame));
    }
    else
    {
        memset(&frame, 0, sizeof(frame));
        frame.can_id = can_id;
        frame.can_dlc = 8U;
        frame.data[0] = (uint8_t)(PCI_TYPE_FF | ((len >> 8) & 0x0FU));
        frame.data[1] = (uint8_t)(len & 0xFFU);
        memcpy(&frame.data[2], data, 6U);
        write(sock, &frame, sizeof(frame));

        uint16_t sent = 6U;
        uint8_t seq = 1U;
        while (sent < len)
        {
            uint16_t remaining = (uint16_t)(len - sent);
            uint8_t chunk = (remaining > 7U) ? 7U : (uint8_t)remaining;

            memset(&frame, 0, sizeof(frame));
            frame.can_id = can_id;
            frame.can_dlc = (uint8_t)(chunk + 1U);
            frame.data[0] = (uint8_t)(PCI_TYPE_CF | (seq & 0x0FU));
            memcpy(&frame.data[1], &data[sent], chunk);
            write(sock, &frame, sizeof(frame));

            sent = (uint16_t)(sent + chunk);
            seq = (uint8_t)((seq + 1U) & 0x0FU);
            if (seq == 0U) { seq = 1U; }

            usleep(8000);
        }
    }
}

static int process_frame(CAN_Transport_RxState *rx_state, const struct can_frame *frame)
{
    if (frame->can_dlc == 0U) { return 0; }

    uint8_t pci_type = (uint8_t)(frame->data[0] & 0xF0U);

    if (pci_type == PCI_TYPE_SF)
    {
        uint8_t len = (uint8_t)(frame->data[0] & 0x0FU);
        if (len > (frame->can_dlc - 1U)) { return 0; }
        memcpy(rx_state->buf, &frame->data[1], len);
        rx_state->recv_len = len;
        rx_state->total_len = len;
        rx_state->in_progress = 0U;
        return 1;
    }
    else if (pci_type == PCI_TYPE_FF)
    {
        uint16_t len = (uint16_t)(((uint16_t)(frame->data[0] & 0x0FU) << 8) | frame->data[1]);
        if (len > CAN_TRANSPORT_MAX_MSG) { return 0; }

        uint8_t first_chunk = (frame->can_dlc >= 8U) ? 6U : (uint8_t)(frame->can_dlc - 2U);
        memcpy(rx_state->buf, &frame->data[2], first_chunk);

        rx_state->total_len = len;
        rx_state->recv_len = first_chunk;
        rx_state->expected_seq = 1U;
        rx_state->in_progress = 1U;
        return 0;
    }
    else if (pci_type == PCI_TYPE_CF)
    {
        if (rx_state->in_progress == 0U) { return 0; }

        uint8_t seq = (uint8_t)(frame->data[0] & 0x0FU);
        if (seq != rx_state->expected_seq)
        {
            rx_state->in_progress = 0U;
            rx_state->recv_len = 0U;
            return 0;
        }

        uint8_t chunk_len = (uint8_t)(frame->can_dlc - 1U);
        memcpy(&rx_state->buf[rx_state->recv_len], &frame->data[1], chunk_len);
        rx_state->recv_len = (uint16_t)(rx_state->recv_len + chunk_len);

        rx_state->expected_seq = (uint8_t)((rx_state->expected_seq + 1U) & 0x0FU);
        if (rx_state->expected_seq == 0U) { rx_state->expected_seq = 1U; }

        if (rx_state->recv_len >= rx_state->total_len)
        {
            rx_state->in_progress = 0U;
            return 1;
        }
        return 0;
    }
    return 0;
}

int CAN_Transport_Receive(int sock, CAN_Transport_RxState *rx_state, uint32_t expected_id, int timeout_ms)
{
    memset(rx_state, 0, sizeof(*rx_state));

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    while (1)
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        int ret = select(sock + 1, &readfds, NULL, NULL, &tv);
        if (ret <= 0)
        {
            return 0;
        }

        struct can_frame frame;
        ssize_t n = read(sock, &frame, sizeof(frame));
        if (n != (ssize_t)sizeof(frame)) { continue; }

        if (frame.can_id != expected_id) { continue; }

        if (process_frame(rx_state, &frame))
        {
            return 1;
        }

    }
}
