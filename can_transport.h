#ifndef CAN_TRANSPORT_H
#define CAN_TRANSPORT_H

#include "mcp2515.h"
#include <stdint.h>

#define CAN_ID_PI_TO_STM32   0x100U
#define CAN_ID_STM32_TO_PI   0x101U

#define PCI_TYPE_SF  0x00U
#define PCI_TYPE_FF  0x10U
#define PCI_TYPE_CF  0x20U

#define CAN_TRANSPORT_MAX_MSG 512U

typedef struct {
    uint8_t  buf[CAN_TRANSPORT_MAX_MSG];
    uint16_t total_len;
    uint16_t recv_len;
    uint8_t  expected_seq;
    uint8_t  in_progress;
} CAN_Transport_RxState;

void CAN_Transport_Send(MCP2515_Handle *h, uint16_t can_id, const uint8_t *data, uint16_t len);

uint8_t CAN_Transport_ProcessFrame(CAN_Transport_RxState *rx_state, const CAN_Frame *frame);

#endif
