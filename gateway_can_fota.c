#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/can.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include "can_transport_pi.h"

static const unsigned char HMAC_SECRET_KEY[] = "V2X-Sentinel-2026-SharedKey!";
#define HMAC_KEY_LEN (sizeof(HMAC_SECRET_KEY) - 1)

#define CMD_GET_STATUS   0x10U
#define CMD_LED_CTRL     0x20U
#define CMD_FOTA_INIT    0x30U
#define CMD_FOTA_CONFIRM 0x31U
#define CMD_FOTA_CHUNK   0x32U

#define FOTA_CHUNK_SIZE  128U

static int open_can_socket(const char *ifname)
{
    int sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (sock < 0) { perror("socket"); return -1; }

    struct ifreq ifr;
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) { perror("ioctl"); close(sock); return -1; }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(sock);
        return -1;
    }
    return sock;
}

static uint32_t g_next_seq = 1U;

static uint8_t g_last_sent_pkt[256];
static uint16_t g_last_sent_len = 0U;

static void send_v2x_packet(int sock, uint8_t id, const uint8_t *data, uint16_t len, int force_error)
{

    uint8_t payload_with_seq[256];
    payload_with_seq[0] = (uint8_t)((g_next_seq >> 24) & 0xFFU);
    payload_with_seq[1] = (uint8_t)((g_next_seq >> 16) & 0xFFU);
    payload_with_seq[2] = (uint8_t)((g_next_seq >> 8) & 0xFFU);
    payload_with_seq[3] = (uint8_t)(g_next_seq & 0xFFU);
    memcpy(&payload_with_seq[4], data, len);
    uint16_t full_len = (uint16_t)(4U + len);

    uint8_t pkt[256];
    pkt[0] = 0xAAU;
    pkt[1] = id;
    pkt[2] = (uint8_t)((full_len >> 8) & 0xFFU);
    pkt[3] = (uint8_t)(full_len & 0xFFU);

    uint8_t checksum = 0U;
    for (uint16_t i = 0U; i < full_len; i++)
    {
        pkt[4U + i] = payload_with_seq[i];
        checksum = (uint8_t)(checksum + payload_with_seq[i]);
    }
    pkt[4U + full_len] = force_error ? 0x00U : checksum;

    uint16_t total = (uint16_t)(4U + full_len + 1U);
    CAN_Transport_Send(sock, CAN_ID_PI_TO_STM32, pkt, total);
    printf("GATEWAY_TX >> Packet 0x%02X sent over CAN (Len: %u, CRC: 0x%02X, SEQ: %lu)\n",
           id, full_len, pkt[4U + full_len], (unsigned long)g_next_seq);

    memcpy(g_last_sent_pkt, pkt, total);
    g_last_sent_len = total;
    g_next_seq++;
}

static void replay_last_packet(int sock)
{
    if (g_last_sent_len == 0U)
    {
        printf("GATEWAY: Nothing sent yet - nothing to replay.\n");
        return;
    }
    CAN_Transport_Send(sock, CAN_ID_PI_TO_STM32, g_last_sent_pkt, g_last_sent_len);
    printf("GATEWAY: [REPLAY ATTACK SIMULATION] Resent the last captured packet unchanged.\n");
}

static void read_response(int sock, int timeout_ms)
{
    CAN_Transport_RxState rx;
    if (CAN_Transport_Receive(sock, &rx, CAN_ID_STM32_TO_PI, timeout_ms))
    {
        printf("[NODE_TELEMETRY]: %.*s\n", rx.recv_len, rx.buf);
    }
    else
    {
        printf("[NODE_TELEMETRY]: (no response received)\n");
    }
}

static void send_fota_over_can(int sock, const char *bin_path, int simulate_attacker)
{
    FILE *f = fopen(bin_path, "rb");
    if (!f) {
        printf("GATEWAY: ERROR - could not open '%s' (%s)\n", bin_path, strerror(errno));
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 16384) {
        printf("GATEWAY: ERROR - '%s' is %ld bytes (must be 1-16384 bytes)\n", bin_path, fsize);
        fclose(f);
        return;
    }

    uint8_t *fw = malloc(fsize);
    if (!fw) { printf("GATEWAY: ERROR - out of memory\n"); fclose(f); return; }
    size_t read_bytes = fread(fw, 1, fsize, f);
    fclose(f);
    if ((long)read_bytes != fsize) {
        printf("GATEWAY: ERROR - short read on '%s'\n", bin_path);
        free(fw);
        return;
    }

    printf("GATEWAY: Loaded '%s' - %ld bytes.\n", bin_path, fsize);

    uint8_t full_hmac[EVP_MAX_MD_SIZE];
    unsigned int hmac_len = 0;
    HMAC(EVP_sha256(), HMAC_SECRET_KEY, (int)HMAC_KEY_LEN, fw, fsize, full_hmac, &hmac_len);

    if (simulate_attacker)
    {

        full_hmac[0] ^= 0xFFU;
        printf("GATEWAY: [SIMULATED ATTACK] Sending GENUINE firmware bytes with a DELIBERATELY WRONG HMAC.\n");
    }

    printf("GATEWAY_SECURITY: HMAC-SHA256: ");
    for (unsigned int i = 0; i < hmac_len; i++) printf("%02x", full_hmac[i]);
    printf("\n");

    uint8_t init_payload[4 + 32];
    init_payload[0] = (uint8_t)((fsize >> 24) & 0xFF);
    init_payload[1] = (uint8_t)((fsize >> 16) & 0xFF);
    init_payload[2] = (uint8_t)((fsize >> 8) & 0xFF);
    init_payload[3] = (uint8_t)(fsize & 0xFF);
    memcpy(&init_payload[4], full_hmac, 32);

    send_v2x_packet(sock, CMD_FOTA_INIT, init_payload, 4U + 32U, 0);
    read_response(sock, 1500);

    long offset = 0;
    int chunk_num = 0;
    while (offset < fsize)
    {
        int this_chunk = (fsize - offset > FOTA_CHUNK_SIZE) ? FOTA_CHUNK_SIZE : (int)(fsize - offset);
        long expected_cumulative = offset + this_chunk;

        int acked = 0;
        for (int attempt = 1; attempt <= 3 && !acked; attempt++)
        {
            send_v2x_packet(sock, CMD_FOTA_CHUNK, fw + offset, (uint16_t)this_chunk, 0);

            CAN_Transport_RxState rx;
            if (CAN_Transport_Receive(sock, &rx, CAN_ID_STM32_TO_PI, 2000))
            {
                long got = -1;
                if (sscanf((const char*)rx.buf, "CACK:%ld", &got) == 1 && got == expected_cumulative)
                {
                    acked = 1;
                }
                else
                {
                    printf("GATEWAY: Unexpected reply on attempt %d ('%.*s') - retrying chunk...\n",
                           attempt, rx.recv_len, rx.buf);
                }
            }
            else
            {
                printf("GATEWAY: No ack for chunk at offset %ld (attempt %d) - retrying...\n", offset, attempt);
            }
        }

        if (!acked)
        {
            printf("GATEWAY: ERROR - chunk at offset %ld failed after 3 attempts. Aborting.\n", offset);
            free(fw);
            return;
        }

        offset += this_chunk;
        chunk_num++;
        if (chunk_num % 20 == 0 || offset >= fsize)
        {
            printf("GATEWAY: Sent %ld/%ld bytes (acknowledged)...\n", offset, fsize);
        }
    }

    printf("GATEWAY: All chunks sent and acknowledged (%ld bytes total).\n", fsize);
    free(fw);
}

int main(void)
{
    int sock = open_can_socket("can0");
    if (sock < 0) {
        printf("Failed to open can0 - is the interface up? "
               "(sudo ip link set can0 up type can bitrate 500000)\n");
        return -1;
    }
    printf("Connected to can0.\n");

    int choice;
    uint8_t dummy = 0x01U;
    char bin_path[256] = "test_fw.bin";

    while (1)
    {
        printf("\n--- V2X SENTINEL GATEWAY (CAN + FOTA) ---\n");
        printf("1. Node Status Report\n2. Toggle Vehicle LED\n3. Simulate Malicious Attack\n"
               "4. Exit\n5. Start FOTA over CAN\n6. CONFIRM pending FOTA commit\n"
               "7. Set .bin file path (current: %s)\n8. Simulate Malicious FOTA (wrong HMAC)\n"
               "9. Simulate Replay Attack (resend last packet)\nSelection: ", bin_path);
        if (scanf("%d", &choice) != 1) { break; }

        if (choice == 1) {
            send_v2x_packet(sock, CMD_GET_STATUS, &dummy, 1U, 0);
            read_response(sock, 1000);
        }
        else if (choice == 2) {
            printf("LED State (1:ON, 0:OFF): ");
            int s; scanf("%d", &s);
            uint8_t val = (uint8_t)s;
            send_v2x_packet(sock, CMD_LED_CTRL, &val, 1U, 0);
            read_response(sock, 1000);
        }
        else if (choice == 3) {
            printf("Injecting Corrupt Payload...\n");
            send_v2x_packet(sock, CMD_GET_STATUS, &dummy, 1U, 1);
            read_response(sock, 1000);
        }
        else if (choice == 5) {
            send_fota_over_can(sock, bin_path, 0);
        }
        else if (choice == 6) {
            printf("Sending FOTA CONFIRM over CAN - this will commit and reset the STM32.\n");
            send_v2x_packet(sock, CMD_FOTA_CONFIRM, &dummy, 1U, 0);
            read_response(sock, 4500);
        }
        else if (choice == 8) {
            send_fota_over_can(sock, bin_path, 1);
        }
        else if (choice == 9) {
            replay_last_packet(sock);
            read_response(sock, 1000);
        }
        else if (choice == 7) {
            printf("Enter .bin file path: ");
            scanf("%255s", bin_path);
        }
        else {
            break;
        }
    }

    close(sock);
    return 0;
}
