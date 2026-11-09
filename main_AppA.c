/*
 * AppA_MERGED - main.c  (Bank A: 0x08010000)
 * =============================================
 * This is the FIRST real integration of everything built so far:
 *   - Runs UNDER the dual-bank bootloader (linked at 0x08010000,
 *     requires __enable_irq() at startup - see the interrupt-trap
 *     bug documented in the Claim 3 theory reference)
 *   - Communicates over CAN (MCP2515 + ISO-TP style fragmentation),
 *     not UART
 *   - Implements the FULL FOTA state machine (INIT / CHUNK / CONFIRM)
 *     - buffering firmware in RAM, only touching flash after an
 *     explicit CONFIRM, with the safe-by-default discard-on-timeout
 *     behavior
 *   - On a confirmed commit, resets the chip - the bootloader then
 *     reads the (now updated) boot flag and jumps into the newly
 *     written bank
 *
 * Previously, "dual-bank + FOTA" and "CAN" were only ever proven as
 * two SEPARATE, isolated test projects. This file is where they
 * are actually combined into one working system for the first time.
 */

#include "stm32f4xx_hal.h"
#include "mcp2515.h"
#include "can_transport.h"
#include "boot_flag_utils.h"
#include "hmac_sha256.h"
#include "max7219.h"
#include "lcd_i2c.h"
#include <string.h>
#include <stdio.h>

UART_HandleTypeDef huart2;   /* debug output only, via ST-Link VCP */
UART_HandleTypeDef huart6;   /* NEW: ESP32 WiFi-bridge link (PA11=TX, PA12=RX) */
UART_HandleTypeDef huart1;   /* NEW: direct V2V link to the Black Pill second node (PA9=TX, PA10=RX) */
SPI_HandleTypeDef hspi1;
MCP2515_Handle can1;
CAN_Transport_RxState can_rx_state;
SPI_HandleTypeDef hspi2;   /* NEW: MAX7219 dot matrix */
I2C_HandleTypeDef hi2c1;   /* NEW: 16x2 LCD */

typedef enum { STATE_IDLE, STATE_ID, STATE_LEN_MSB, STATE_LEN_LSB, STATE_DATA, STATE_CRC } v2x_state_t;

#define MAX_PAYLOAD 200U
static v2x_state_t rx_state = STATE_IDLE;
static uint8_t rx_buf[MAX_PAYLOAD];
static uint16_t p_len = 0U, p_idx = 0U;
static uint8_t rx_pkt_id = 0U;
static uint8_t calc_sum = 0U;

typedef struct {
    uint32_t total_pkts;
    uint32_t crc_errs;
    uint32_t hash_fail_count;
    uint32_t fota_commits;
} Stats;
static Stats node_stats = {0U, 0U, 0U, 0U};

/* Replay attack defense: every command payload now carries a 4-byte
   big-endian sequence number as its first 4 bytes, ahead of the
   command's actual content. A packet is only accepted if its
   sequence number is strictly greater than the last one accepted -
   an old, previously-valid packet captured and resent later (a
   replay attack) will always fail this check, since its sequence
   number was already superseded. This check happens once, generically,
   for every command - not per command-type - so it can't be
   bypassed by any individual handler. */
static uint32_t last_accepted_seq = 0U;
static uint32_t node_stats_replay_rejected = 0U;

#define CMD_GET_STATUS   0x10U
#define CMD_LED_CTRL     0x20U
#define CMD_FOTA_INIT    0x30U
#define CMD_FOTA_CONFIRM 0x31U
#define CMD_FOTA_CHUNK   0x32U

/* Shared HMAC secret key - known to both the Gateway and this node,
   never transmitted over the wire. In a production system this would
   be provisioned securely per-device rather than compiled in as a
   literal constant; for this project, given the STM32F401 has no
   secure OTP/HUK key storage, the key is protected via flash
   Read-Out Protection (RDP) rather than hardware-backed key
   derivation - the standard mitigation for MCUs without a Root of
   Trust. */
static const uint8_t HMAC_SECRET_KEY[] = "V2X-Sentinel-2026-SharedKey!";
static uint8_t fota_expected_hmac[HMAC_SHA256_SIZE];

/* ---- FOTA two-phase commit state (identical design to the UART version) ---- */
typedef enum { FOTA_IDLE, FOTA_RECEIVING, FOTA_AWAITING_CONFIRM } fota_state_t;
static volatile fota_state_t fota_state = FOTA_IDLE;
static uint32_t fota_window_start = 0U;
static uint32_t fota_last_progress_print = 0U;
static uint32_t fota_last_chunk_tick = 0U;
#define FOTA_CONFIRM_WINDOW_MS 30000U
#define FOTA_STALL_TIMEOUT_MS 10000U   /* generous margin - a full FOTA chunk now
                                           requires ~20 raw CAN frames; this allows
                                           for normal processing delay without
                                           false-triggering mid-transfer */

#define MAX_FW_SIZE 16384U   /* 16KB staging buffer, same as the UART version. Each
                                individual FOTA_CHUNK (128B) stays well within the CAN
                                transport's 512B per-message limit - only the TOTAL
                                accumulated across multiple chunks needs this larger cap. */
static uint8_t fota_pending_fw[MAX_FW_SIZE];
static uint32_t fota_total_len = 0U;
static uint32_t fota_recv_len = 0U;

static void MX_GPIO_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART6_UART_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_SPI2_Init(void);
static void MX_I2C1_Init(void);
void SystemClock_Config(void);

static void dprint(const char *s)
{
    HAL_UART_Transmit(&huart2, (uint8_t*)s, strlen(s), 100);
}

/* Tracks which transport the CURRENTLY-being-processed command arrived
   on, so its reply goes back the SAME way. Same "feed bytes from
   multiple sources into one parser" pattern already proven for
   UART vs CAN - now extended to a third source (WiFi-via-ESP32). */
typedef enum { REPLY_VIA_CAN, REPLY_VIA_UART6 } ReplyChannel;
static ReplyChannel current_reply_channel = REPLY_VIA_CAN;

/* Sends a response back to the Pi over CAN (fragmented automatically if needed) */
static void can_reply(const char *s)
{
    CAN_Transport_Send(&can1, CAN_ID_STM32_TO_PI, (const uint8_t*)s, (uint16_t)strlen(s));

    /* DIAGNOSTIC: check what the MCP2515 itself believes happened to
       the LAST frame of this reply - "TRANSPORT_SEND: complete" only
       means we finished calling the send function, NOT that the bus
       actually accepted/acknowledged it. This is the same status
       check proven during initial CAN bring-up (status=OK/PENDING). */
    uint8_t txstat = MCP2515_GetTxStatus(&can1);
    char dbg[48];
    const char *result;
    if ((txstat & 0x10U) != 0U) { result = "PENDING(no ACK)"; }
    else if ((txstat & 0x20U) != 0U) { result = "BUS_ERROR"; }
    else if ((txstat & 0x40U) != 0U) { result = "ARB_LOST"; }
    else { result = "OK(acked)"; }
    snprintf(dbg, sizeof(dbg), "  [TX status: %s]\r\n", result);
    dprint(dbg);
}

/*
 * broadcast_accident_alert()
 * =============================
 * The first genuinely PROACTIVE, unsolicited message in this whole
 * project - every other outbound message so far has been a reply to
 * something the Gateway asked for. This is sent the instant the
 * button-triggered "accident" is detected, over BOTH paths
 * simultaneously:
 *   - CAN, to the Raspberry Pi Gateway (V2I - vehicle informs the
 *     control center)
 *   - The direct USART1 wire, straight to the Black Pill second node
 *     (V2V - vehicle informs a nearby vehicle directly, with no
 *     infrastructure in the path at all)
 * Demonstrating both the V2I and V2V patterns from a single real
 * trigger event.
 */
static void broadcast_accident_alert(void)
{
    const char *alert_msg = "ALERT:ACCIDENT_DETECTED|SEVERITY:HIGH|NODE:PRIMARY\n";

    dprint("APPA-CAN: *** ACCIDENT DETECTED - broadcasting alert (V2I + V2V) ***\r\n");

    LCD_Clear(&hi2c1);
    LCD_SetCursor(&hi2c1, 0, 0);
    LCD_Print(&hi2c1, "*ACCIDENT ALERT*");
    LCD_SetCursor(&hi2c1, 0, 1);
    LCD_Print(&hi2c1, "V2I+V2V sent");

    /* V2I: to the Gateway/Control Center, over CAN */
    can_reply(alert_msg);

    /* V2V: directly to the Black Pill, over the dedicated wired link -
       no Gateway involved in this path at all */
    HAL_StatusTypeDef v2v_status = HAL_UART_Transmit(&huart1, (uint8_t*)alert_msg, strlen(alert_msg), 200);
    char dbg[48];
    snprintf(dbg, sizeof(dbg), "APPA-CAN: V2V UART1 transmit status: %d (0=OK)\r\n", (int)v2v_status);
    dprint(dbg);

    dprint("APPA-CAN: Alert broadcast complete (V2I via CAN + V2V via direct UART).\r\n");
}

/* Sends a response back over USART6 - raw bytes, no fragmentation needed
   since it's a direct point-to-point UART link to the ESP32 bridge. */
static void uart6_reply(const char *s)
{
    HAL_UART_Transmit(&huart6, (uint8_t*)s, strlen(s), 200);
}

/* Generic reply - routes to whichever transport the current command
   actually arrived on. */
static void send_reply(const char *s)
{
    if (current_reply_channel == REPLY_VIA_UART6)
    {
        uart6_reply(s);
    }
    else
    {
        can_reply(s);
    }
}

static void feed_byte_to_parser(uint8_t b)
{
    switch (rx_state)
    {
        case STATE_IDLE:
            if (b == 0xAAU) { rx_state = STATE_ID; }
            break;
        case STATE_ID:
            rx_pkt_id = b;
            rx_state = STATE_LEN_MSB;
            break;
        case STATE_LEN_MSB:
            p_len = (uint16_t)((uint16_t)b << 8U);
            rx_state = STATE_LEN_LSB;
            break;
        case STATE_LEN_LSB:
            p_len = (uint16_t)(p_len | b);
            p_idx = 0U;
            calc_sum = 0U;
            rx_state = (p_len > (uint16_t)MAX_PAYLOAD) ? STATE_IDLE : STATE_DATA;
            break;
        case STATE_DATA:
            rx_buf[p_idx] = b;
            p_idx++;
            calc_sum = (uint8_t)(calc_sum + b);
            if (p_idx >= p_len) { rx_state = STATE_CRC; }
            break;
        case STATE_CRC:
            node_stats.total_pkts++;
            if (b == calc_sum)
            {
                /* Every command payload must carry at least the
                   4-byte sequence number prefix. */
                if (p_len < 4U)
                {
                    send_reply("ALERT:BAD_PACKET\n");
                    rx_state = STATE_IDLE;
                    break;
                }

                uint32_t seq = ((uint32_t)rx_buf[0] << 24) | ((uint32_t)rx_buf[1] << 16) |
                               ((uint32_t)rx_buf[2] << 8) | rx_buf[3];

                if (seq <= last_accepted_seq)
                {
                    /* Replay defense: this exact sequence number (or an
                       older one) has already been accepted once before.
                       A legitimate sender never reuses or goes backwards
                       in sequence - this is the signature of a captured
                       packet being resent later by an attacker. Reject
                       outright, without dispatching to any command
                       handler at all. */
                    node_stats_replay_rejected++;
                    dprint("PARSER: REPLAY DETECTED - sequence not newer than last accepted.\r\n");
                    send_reply("ALERT:REPLAY_DETECTED\n");
                    rx_state = STATE_IDLE;
                    break;
                }
                last_accepted_seq = seq;

                /* From here on, "payload"/"payload_len" refer to the
                   command's actual content, with the 4-byte sequence
                   prefix already stripped off - every handler below is
                   otherwise unchanged from before replay protection was
                   added. */
                const uint8_t *payload = &rx_buf[4];
                uint16_t payload_len = (uint16_t)(p_len - 4U);

                if (rx_pkt_id == CMD_GET_STATUS)
                {
                    char rep[110];
                    const char *link_name = (current_reply_channel == REPLY_VIA_UART6) ? "WIFI" : "CAN";
                    snprintf(rep, sizeof(rep),
                             "STATS|PKT:%lu|ERR:%lu|COMMITS:%lu|BANK:A|LINK:%s|FOTA_RX:%lu/%lu|REPLAY:%lu\n",
                             (unsigned long)node_stats.total_pkts, (unsigned long)node_stats.crc_errs,
                             (unsigned long)node_stats.fota_commits, link_name,
                             (unsigned long)fota_recv_len, (unsigned long)fota_total_len,
                             (unsigned long)node_stats_replay_rejected);
                    dprint("PARSER: STATUS cmd, sending reply...\r\n");
                    send_reply(rep);
                    dprint("PARSER: reply sent.\r\n");
                }
                else if (rx_pkt_id == CMD_LED_CTRL)
                {
                    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, (GPIO_PinState)(payload[0] != 0U));
                    dprint("PARSER: LED cmd, sending reply...\r\n");
                    send_reply("EVENT:LED_ACK\n");
                    dprint("PARSER: reply sent.\r\n");
                }
                else if (rx_pkt_id == CMD_FOTA_INIT)
                {
                    /* Payload (after seq prefix): [4-byte total_len big-endian][32-byte expected HMAC-SHA256] */
                    if (payload_len == (4U + HMAC_SHA256_SIZE) && fota_state == FOTA_IDLE)
                    {
                        uint32_t total = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                                         ((uint32_t)payload[2] << 8) | payload[3];
                        if (total > 0U && total <= MAX_FW_SIZE)
                        {
                            fota_total_len = total;
                            fota_recv_len = 0U;
                            fota_state = FOTA_RECEIVING;
                            fota_last_chunk_tick = HAL_GetTick();
                            fota_last_progress_print = 0U;
                            memcpy(fota_expected_hmac, &payload[4], HMAC_SHA256_SIZE);

                            char msg[64];
                            snprintf(msg, sizeof(msg), "APPA-CAN: FOTA_INIT - expecting %lu bytes\r\n",
                                     (unsigned long)total);
                            dprint(msg);
                            send_reply("EVENT:FOTA_INIT_OK\n");
                        }
                        else
                        {
                            send_reply("ALERT:FOTA_SIZE_INVALID\n");
                        }
                    }
                    else
                    {
                        send_reply("ALERT:FOTA_BUSY_OR_BAD_INIT\n");
                    }
                }
                else if (rx_pkt_id == CMD_FOTA_CHUNK)
                {
                    if (fota_state == FOTA_RECEIVING && (fota_recv_len + payload_len) <= fota_total_len)
                    {
                        memcpy(&fota_pending_fw[fota_recv_len], payload, payload_len);
                        fota_recv_len += payload_len;

                        /* Acknowledge this chunk with the new cumulative total,
                           same proven fix as the UART FOTA pipeline - lets the
                           Pi detect and retry a specific lost/corrupted chunk
                           instead of blindly sending all chunks with no
                           confirmation. */
                        char cack[32];
                        snprintf(cack, sizeof(cack), "CACK:%lu\n", (unsigned long)fota_recv_len);
                        send_reply(cack);
                        fota_last_chunk_tick = HAL_GetTick();

                        if (fota_recv_len >= fota_total_len)
                        {
                            /* Independently compute HMAC-SHA256 over the received
                               firmware using the shared secret key, and compare
                               against what the Gateway claimed in FOTA_INIT. This
                               is the actual authenticity check - unlike a bare
                               hash, a match here is only possible if whoever
                               produced this firmware also knew the secret key. */
                            uint8_t computed_hmac[HMAC_SHA256_SIZE];
                            hmac_sha256(HMAC_SECRET_KEY, sizeof(HMAC_SECRET_KEY) - 1U,
                                        fota_pending_fw, fota_recv_len, computed_hmac);

                            if (hmac_sha256_verify(computed_hmac, fota_expected_hmac))
                            {
                                dprint("APPA-CAN: HMAC verified OK. Full firmware received. Awaiting explicit confirm.\r\n");
                                fota_state = FOTA_AWAITING_CONFIRM;
                                fota_window_start = HAL_GetTick();
                                fota_last_progress_print = 0U;
                                send_reply("EVENT:FOTA_BUFFERED_AWAITING_CONFIRM\n");
                            }
                            else
                            {
                                dprint("APPA-CAN: HMAC MISMATCH - rejecting firmware, discarding buffer.\r\n");
                                node_stats.hash_fail_count++;
                                fota_state = FOTA_IDLE;
                                fota_recv_len = 0U;
                                fota_total_len = 0U;
                                send_reply("ALERT:FOTA_HMAC_FAIL\n");
                            }
                        }
                    }
                    else
                    {
                        node_stats.hash_fail_count++;
                        send_reply("ALERT:FOTA_CHUNK_UNEXPECTED\n");
                    }
                }
                else if (rx_pkt_id == CMD_FOTA_CONFIRM)
                {
                    if (fota_state == FOTA_AWAITING_CONFIRM)
                    {
                        dprint("APPA-CAN: FOTA CONFIRM received. Erasing inactive bank...\r\n");
                        fota_erase_inactive_bank();

                        dprint("APPA-CAN: Writing buffered firmware to inactive bank...\r\n");
                        fota_write_chunk(0U, fota_pending_fw, fota_recv_len);

                        dprint("APPA-CAN: Write complete. Committing in 3s - CUT POWER NOW to test rollback.\r\n");
                        MAX7219_DisplayPattern(&hspi2, MAX7219_PATTERN_B);
                        LCD_SetCursor(&hi2c1, 0, 0);
                        LCD_Print(&hi2c1, "SWITCHING TO B  ");
                        HAL_Delay(3000);

                        dprint("APPA-CAN: Committing and resetting now.\r\n");
                        fota_commit();
                        node_stats.fota_commits++;
                        send_reply("EVENT:FOTA_COMMITTED\n");
                        HAL_Delay(150);
                        NVIC_SystemReset();
                    }
                    else
                    {
                        send_reply("ALERT:NO_PENDING_FOTA\n");
                    }
                }
                else
                {
                    send_reply("ALERT:UNKNOWN_CMD\n");
                }
            }
            else
            {
                node_stats.crc_errs++;
                send_reply("ALERT:CRC_FAIL\n");
            }
            rx_state = STATE_IDLE;
            break;
        default:
            rx_state = STATE_IDLE;
            break;
    }
}

int main(void)
{
    __enable_irq();   /* CRITICAL - see Claim 3 theory reference: the bootloader
                          disables interrupts before jumping here; without this,
                          HAL_GetTick() stays frozen and every timeout hangs forever. */

    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_SPI1_Init();
    MX_USART2_UART_Init();
    MX_USART6_UART_Init();
    MX_USART1_UART_Init();
    MX_SPI2_Init();
    MX_I2C1_Init();

    dprint("\r\n*** APPA (MERGED: Bootloader + CAN + FOTA) STARTED ***\r\n");

    MAX7219_Init(&hspi2);
    MAX7219_DisplayPattern(&hspi2, MAX7219_PATTERN_A);   /* Bank A is always active at boot */

    LCD_Init(&hi2c1);
    LCD_SetCursor(&hi2c1, 0, 0);
    LCD_Print(&hi2c1, "BANK:A BOOT OK");
    LCD_SetCursor(&hi2c1, 0, 1);
    LCD_Print(&hi2c1, "PKT:0 ERR:0");

    MCP2515_Init(&can1, &hspi1, GPIOA, GPIO_PIN_4);
    MCP2515_Reset(&can1);
    MCP2515_SetBitrate500kbps_8MHz(&can1);
    MCP2515_SetMode(&can1, MCP2515_MODE_NORMAL);

    uint8_t mode_now = MCP2515_GetMode(&can1);
    char msg[64];
    snprintf(msg, sizeof(msg), "CAN mode: 0x%02X (expect 0x00 = Normal)\r\n", mode_now);
    dprint(msg);
    dprint("Ready - listening for V2X-P packets over CAN (ID 0x100)...\r\n\r\n");

    memset(&can_rx_state, 0, sizeof(can_rx_state));

    uint32_t last_blink = 0U;
    uint32_t last_heartbeat = 0U;
    uint32_t last_lcd_update = 0U;
    const uint32_t BLINK_INTERVAL_MS = 200U;   /* fast blink = Bank A, matches original AppA */

    while (1)
    {
        if (HAL_GetTick() - last_heartbeat >= 3000U)
        {
            const char *tag = (fota_state == FOTA_RECEIVING) ? " [FOTA RECEIVING]" :
                               (fota_state == FOTA_AWAITING_CONFIRM) ? " [FOTA CONFIRM PENDING]" : "";
            snprintf(msg, sizeof(msg), "APPA-CAN: heartbeat%s\r\n", tag);
            dprint(msg);
            last_heartbeat = HAL_GetTick();
        }

        /* Periodic LCD status refresh - live packet/error counts,
           independent of whether anything was recently requested. */
        if (HAL_GetTick() - last_lcd_update >= 2000U)
        {
            char lcd_line1[17];
            char lcd_line2[17];
            snprintf(lcd_line1, sizeof(lcd_line1), "BANK:A PKT:%-5lu", (unsigned long)node_stats.total_pkts);
            snprintf(lcd_line2, sizeof(lcd_line2), "ERR:%-3lu REP:%-3lu", (unsigned long)node_stats.crc_errs,
                     (unsigned long)node_stats_replay_rejected);
            LCD_SetCursor(&hi2c1, 0, 0);
            LCD_Print(&hi2c1, lcd_line1);
            LCD_SetCursor(&hi2c1, 0, 1);
            LCD_Print(&hi2c1, lcd_line2);
            last_lcd_update = HAL_GetTick();
        }

        if (HAL_GetTick() - last_blink >= BLINK_INTERVAL_MS)
        {
            HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0);
            last_blink = HAL_GetTick();
        }

        /* ---- FOTA stall recovery ---- */
        if (fota_state == FOTA_RECEIVING && fota_recv_len > 0U &&
            (HAL_GetTick() - fota_last_chunk_tick) >= FOTA_STALL_TIMEOUT_MS)
        {
            snprintf(msg, sizeof(msg), "APPA-CAN: FOTA stalled (%lu/%lu) - resetting to IDLE\r\n",
                     (unsigned long)fota_recv_len, (unsigned long)fota_total_len);
            dprint(msg);
            fota_state = FOTA_IDLE;
            fota_recv_len = 0U;
            fota_total_len = 0U;
        }

        /* ---- FOTA confirm window (safe-by-default timeout) ---- */
        if (fota_state == FOTA_AWAITING_CONFIRM)
        {
            uint32_t elapsed = HAL_GetTick() - fota_window_start;
            if (HAL_GetTick() - fota_last_progress_print >= 3000U)
            {
                uint32_t remaining_s = (FOTA_CONFIRM_WINDOW_MS - elapsed) / 1000U;
                snprintf(msg, sizeof(msg), "APPA-CAN: Awaiting confirm (%lus left)\r\n",
                         (unsigned long)remaining_s);
                dprint(msg);
                fota_last_progress_print = HAL_GetTick();
            }
            if (elapsed >= FOTA_CONFIRM_WINDOW_MS)
            {
                dprint("APPA-CAN: Confirm window expired - DISCARDED (safe default).\r\n");
                fota_state = FOTA_IDLE;
                fota_recv_len = 0U;
            }
        }

        /* ---- User Button (B1/PC13): dual-purpose, distinguished by press
           duration ----
           SHORT press (<1s): simulated accident detection -> proactive
                               alert broadcast (V2I + V2V)
           LONG press (>=2s, held): original manual bank-switch override
           This reuses the existing button hardware rather than requiring
           a second physical control, distinguishing intent purely by how
           long it is held. */
        {
            static uint8_t button_was_down = 0U;
            static uint32_t press_start_tick = 0U;

            if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_RESET)
            {
                if (!button_was_down)
                {
                    /* Debounce the initial press */
                    HAL_Delay(50);
                    if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_RESET)
                    {
                        button_was_down = 1U;
                        press_start_tick = HAL_GetTick();
                    }
                }
                else if ((HAL_GetTick() - press_start_tick) >= 2000U)
                {
                    /* Long press reached its threshold WHILE still held -
                       act immediately, matching the original behavior of
                       not waiting for release. */
                    if (fota_state == FOTA_IDLE)
                    {
                        dprint("APPA-CAN: Long press - manual override, committing to Bank B and resetting...\r\n");
                        MAX7219_DisplayPattern(&hspi2, MAX7219_PATTERN_B);
                        LCD_SetCursor(&hi2c1, 0, 0);
                        LCD_Print(&hi2c1, "MANUAL -> BANK B");
                        boot_flag_write(BOOT_FLAG_BANK_B);
                        HAL_Delay(200);
                        NVIC_SystemReset();
                    }
                }
            }
            else if (button_was_down)
            {
                /* Button just released - if it was short, treat as the
                   accident-alert trigger. (The long-press case above
                   already acted and reset the board before this could
                   ever be reached for a long press.) */
                uint32_t held_ms = HAL_GetTick() - press_start_tick;
                button_was_down = 0U;
                if (held_ms < 1000U)
                {
                    broadcast_accident_alert();
                }
            }
        }

        /* ---- Poll for incoming CAN frames ---- */
        if (MCP2515_FrameAvailable(&can1))
        {
            CAN_Frame frame;
            MCP2515_ReadFrame(&can1, &frame);

            if (frame.id == CAN_ID_PI_TO_STM32)
            {
                uint8_t complete = CAN_Transport_ProcessFrame(&can_rx_state, &frame);
                if (complete != 0U)
                {
                    snprintf(msg, sizeof(msg), "CAN: reassembled %u bytes, feeding to parser\r\n",
                             can_rx_state.recv_len);
                    dprint(msg);

                    current_reply_channel = REPLY_VIA_CAN;
                    for (uint16_t i = 0U; i < can_rx_state.recv_len; i++)
                    {
                        feed_byte_to_parser(can_rx_state.buf[i]);
                    }
                }
            }
        }

        /* ---- Poll for incoming USART6 bytes (ESP32 WiFi bridge) ----
           IMPORTANT: timeout=0 here, NOT a small nonzero value. Even a
           5ms timeout blocks the ENTIRE main loop for 5ms on every
           single iteration when USART6 has no data waiting (which is
           most of the time) - this was silently starving the CAN
           polling below of CPU time, causing exactly the kind of
           frame loss we fixed once already for a different reason.
           timeout=0 makes this a true non-blocking check: return
           immediately if no byte is ready. */
        {
            uint8_t b;
            if (HAL_UART_Receive(&huart6, &b, 1, 0) == HAL_OK)
            {
                current_reply_channel = REPLY_VIA_UART6;
                feed_byte_to_parser(b);
            }
        }
    }
}

static void MX_SPI1_Init(void)
{
    __HAL_RCC_SPI1_CLK_ENABLE();
    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_MASTER;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi1.Init.NSS = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    HAL_SPI_Init(&hspi1);
}

static void MX_USART2_UART_Init(void)
{
    __HAL_RCC_USART2_CLK_ENABLE();
    huart2.Instance = USART2;
    huart2.Init.BaudRate = 115200;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart2);
}

/* NEW: USART6 - ESP32 WiFi-bridge link. Same 115200 baud, same simple
   8N1 config as every other UART link in this project. */
static void MX_USART6_UART_Init(void)
{
    __HAL_RCC_USART6_CLK_ENABLE();
    huart6.Instance = USART6;
    huart6.Init.BaudRate = 115200;
    huart6.Init.WordLength = UART_WORDLENGTH_8B;
    huart6.Init.StopBits = UART_STOPBITS_1;
    huart6.Init.Parity = UART_PARITY_NONE;
    huart6.Init.Mode = UART_MODE_TX_RX;
    huart6.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart6.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart6);
}

/* NEW: USART1 - direct wired V2V link to the Black Pill second node.
   Unlike USART6 (which carries the full V2X-P command protocol via
   the ESP32 bridge), this link carries only simple, unsolicited
   accident-alert broadcasts - a genuinely different traffic pattern
   from the request/response protocol used everywhere else. */
static void MX_USART1_UART_Init(void)
{
    __HAL_RCC_USART1_CLK_ENABLE();
    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart1);
}

/* NEW: SPI2 - MAX7219 dot matrix. Separate hardware SPI peripheral
   from SPI1 (used by the MCP2515), so no contention between them. */
static void MX_SPI2_Init(void)
{
    __HAL_RCC_SPI2_CLK_ENABLE();
    hspi2.Instance = SPI2;
    hspi2.Init.Mode = SPI_MODE_MASTER;
    hspi2.Init.Direction = SPI_DIRECTION_2LINES;
    hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi2.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi2.Init.NSS = SPI_NSS_SOFT;
    hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
    hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    HAL_SPI_Init(&hspi2);
}

/* NEW: I2C1 - 16x2 LCD */
static void MX_I2C1_Init(void)
{
    __HAL_RCC_I2C1_CLK_ENABLE();
    hi2c1.Instance = I2C1;
    hi2c1.Init.ClockSpeed = 100000;
    hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1 = 0;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2 = 0;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    HAL_I2C_Init(&hi2c1);
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
    GPIO_InitStruct.Pin = GPIO_PIN_0;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    GPIO_InitStruct.Pin = GPIO_PIN_4;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* NEW: USART6 - PA11=TX, PA12=RX (AF8) - ESP32 WiFi-bridge link */
    GPIO_InitStruct.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF8_USART6;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* NEW: USART1 - PA9=TX, PA10=RX (AF7) - direct V2V link to Black Pill */
    GPIO_InitStruct.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* NEW: SPI2 - PB13=SCK, PB14=MISO, PB15=MOSI (AF5) - MAX7219 */
    GPIO_InitStruct.Pin = GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* NEW: MAX7219 CS - PB12, manual chip-select (software-controlled,
       same pattern as the MCP2515's CS on SPI1) */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET);
    GPIO_InitStruct.Pin = GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* NEW: I2C1 - PB6=SCL, PB7=SDA (AF4) - 16x2 LCD */
    GPIO_InitStruct.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;   /* I2C requires open-drain */
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* User Button B1 - PC13, input, no pull (board has external pull already) */
    GPIO_InitStruct.Pin = GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLM = 16;
    RCC_OscInitStruct.PLL.PLLN = 336;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
    RCC_OscInitStruct.PLL.PLLQ = 7;
    HAL_RCC_OscConfig(&RCC_OscInitStruct);
    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                 | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2);
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) { }
}
