#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>

static const unsigned char HMAC_SECRET_KEY[] = "V2X-Sentinel-2026-SharedKey!";
#define HMAC_KEY_LEN (sizeof(HMAC_SECRET_KEY) - 1)

#define CMD_GET_STATUS   0x10U
#define CMD_LED_CTRL     0x20U
#define CMD_FOTA_INIT    0x30U
#define CMD_FOTA_CONFIRM 0x31U
#define CMD_FOTA_CHUNK   0x32U

#define FOTA_CHUNK_SIZE  128U

static int sock = -1;

static int connect_to_esp32(const char *ip, uint16_t port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        printf("GATEWAY: ERROR - invalid IP address '%s'\n", ip);
        close(s);
        return -1;
    }

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(s);
        return -1;
    }
    return s;
}

static uint32_t g_next_seq = 1U;
static uint8_t g_last_sent_pkt[256];
static uint16_t g_last_sent_len = 0U;

static void send_v2x_packet(uint8_t id, const uint8_t *data, uint16_t len, int force_error)
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
    ssize_t sent = send(sock, pkt, total, 0);
    if (sent != (ssize_t)total) {
        printf("GATEWAY: WARNING - only sent %zd of %u bytes\n", sent, total);
    }
    printf("GATEWAY_TX >> Packet 0x%02X sent over WiFi (Len: %u, CRC: 0x%02X, SEQ: %lu)\n",
           id, full_len, pkt[4U + full_len], (unsigned long)g_next_seq);

    memcpy(g_last_sent_pkt, pkt, total);
    g_last_sent_len = total;
    g_next_seq++;
}

static void replay_last_packet(void)
{
    if (g_last_sent_len == 0U) {
        printf("GATEWAY: Nothing sent yet - nothing to replay.\n");
        return;
    }
    send(sock, g_last_sent_pkt, g_last_sent_len, 0);
    printf("GATEWAY: [REPLAY ATTACK SIMULATION] Resent the last captured packet unchanged.\n");
}

static void read_response(int timeout_ms)
{
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(sock + 1, &readfds, NULL, NULL, &tv);
    if (ret > 0)
    {
        char buf[512];
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            printf("[NODE_TELEMETRY]: %s\n", buf);
            return;
        }
    }
    printf("[NODE_TELEMETRY]: (no response received)\n");
}

static void send_fota_over_wifi(const char *bin_path, int simulate_attacker)
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

    send_v2x_packet(CMD_FOTA_INIT, init_payload, 4U + 32U, 0);
    read_response(1500);

    long offset = 0;
    int chunk_num = 0;
    while (offset < fsize)
    {
        int this_chunk = (fsize - offset > FOTA_CHUNK_SIZE) ? FOTA_CHUNK_SIZE : (int)(fsize - offset);
        long expected_cumulative = offset + this_chunk;

        int acked = 0;
        for (int attempt = 1; attempt <= 3 && !acked; attempt++)
        {
            send_v2x_packet(CMD_FOTA_CHUNK, fw + offset, (uint16_t)this_chunk, 0);

            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sock, &readfds);
            struct timeval tv = {2, 0};

            if (select(sock + 1, &readfds, NULL, NULL, &tv) > 0)
            {
                char buf[64];
                ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
                if (n > 0)
                {
                    buf[n] = '\0';
                    long got = -1;
                    if (sscanf(buf, "CACK:%ld", &got) == 1 && got == expected_cumulative)
                    {
                        acked = 1;
                    }
                    else
                    {
                        printf("GATEWAY: Unexpected reply on attempt %d ('%s') - retrying chunk...\n", attempt, buf);
                    }
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
        if (chunk_num % 20 == 0 || offset >= fsize) {
            printf("GATEWAY: Sent %ld/%ld bytes (acknowledged)...\n", offset, fsize);
        }
    }

    printf("GATEWAY: All chunks sent (%ld bytes total). Waiting for final confirmation...\n", fsize);
    read_response(1500);

    free(fw);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: %s <ESP32_IP> [port]\n", argv[0]);
        return -1;
    }
    const char *esp32_ip = argv[1];
    uint16_t port = (argc > 2) ? (uint16_t)atoi(argv[2]) : 8888;

    printf("Connecting to ESP32 bridge at %s:%u...\n", esp32_ip, port);
    sock = connect_to_esp32(esp32_ip, port);
    if (sock < 0) {
        printf("Failed to connect. Check the ESP32 is powered, on WiFi, and the IP/port are correct.\n");
        return -1;
    }
    printf("Connected via WiFi.\n");

    int choice;
    uint8_t dummy = 0x01U;
    char bin_path[256] = "firmware.bin";

    while (1)
    {
        printf("\n--- V2X SENTINEL GATEWAY (WiFi via ESP32) ---\n");
        printf("1. Node Status Report\n2. Toggle Vehicle LED\n3. Simulate Malicious Attack\n"
               "4. Exit\n5. Start FOTA over WiFi\n6. CONFIRM pending FOTA commit\n"
               "7. Set .bin file path (current: %s)\n8. Simulate Malicious FOTA (wrong HMAC)\n"
               "9. Simulate Replay Attack (resend last packet)\nSelection: ", bin_path);
        if (scanf("%d", &choice) != 1) { break; }

        if (choice == 1) {
            send_v2x_packet(CMD_GET_STATUS, &dummy, 1U, 0);
            read_response(1000);
        }
        else if (choice == 2) {
            printf("LED State (1:ON, 0:OFF): ");
            int s; scanf("%d", &s);
            uint8_t val = (uint8_t)s;
            send_v2x_packet(CMD_LED_CTRL, &val, 1U, 0);
            read_response(1000);
        }
        else if (choice == 3) {
            printf("Injecting Corrupt Payload...\n");
            send_v2x_packet(CMD_GET_STATUS, &dummy, 1U, 1);
            read_response(1000);
        }
        else if (choice == 5) {
            send_fota_over_wifi(bin_path, 0);
        }
        else if (choice == 6) {
            printf("Sending FOTA CONFIRM over WiFi - this will commit and reset the STM32.\n");
            send_v2x_packet(CMD_FOTA_CONFIRM, &dummy, 1U, 0);
            read_response(4500);
        }
        else if (choice == 8) {
            send_fota_over_wifi(bin_path, 1);
        }
        else if (choice == 9) {
            replay_last_packet();
            read_response(1000);
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
