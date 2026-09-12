#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#define SYNC_BYTE 0xAA
#define MAX_BUF 128

#define CMD_GET_STATUS 0x10
#define CMD_LED_CTRL   0x20

static int open_serial(const char *device)
{
    int fd = open(device, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) { perror("UART Open Failed"); return -1; }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) { perror("tcgetattr failed"); close(fd); return -1; }

    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    cfmakeraw(&tty);
    tty.c_cflag |= (CLOCAL | CREAD | CS8);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 5;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) { perror("tcsetattr failed"); close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static uint32_t g_next_seq = 1;
static uint8_t g_last_sent_pkt[MAX_BUF];
static int g_last_sent_len = 0;

static void send_packet(int fd, uint8_t id, const uint8_t *data, uint16_t len, int force_error)
{
    uint8_t payload_with_seq[MAX_BUF];
    payload_with_seq[0] = (uint8_t)((g_next_seq >> 24) & 0xFF);
    payload_with_seq[1] = (uint8_t)((g_next_seq >> 16) & 0xFF);
    payload_with_seq[2] = (uint8_t)((g_next_seq >> 8) & 0xFF);
    payload_with_seq[3] = (uint8_t)(g_next_seq & 0xFF);
    memcpy(&payload_with_seq[4], data, len);
    uint16_t full_len = 4 + len;

    uint8_t pkt[MAX_BUF];
    pkt[0] = SYNC_BYTE;
    pkt[1] = id;
    pkt[2] = (full_len >> 8) & 0xFF;
    pkt[3] = full_len & 0xFF;

    uint8_t checksum = 0;
    for (int i = 0; i < full_len; i++) {
        pkt[4 + i] = payload_with_seq[i];
        checksum += payload_with_seq[i];
    }
    pkt[4 + full_len] = force_error ? 0x00 : checksum;

    int total = 4 + full_len + 1;
    write(fd, pkt, total);
    printf("GATEWAY_TX >> [BLACKPILL] Packet 0x%02X sent. (Len: %d, CRC: 0x%02X, SEQ: %u)\n",
           id, full_len, pkt[4 + full_len], g_next_seq);

    memcpy(g_last_sent_pkt, pkt, total);
    g_last_sent_len = total;
    g_next_seq++;
}

static void replay_last_packet(int fd)
{
    if (g_last_sent_len == 0) {
        printf("GATEWAY: Nothing sent yet - nothing to replay.\n");
        return;
    }
    write(fd, g_last_sent_pkt, g_last_sent_len);
    printf("GATEWAY: [REPLAY ATTACK SIMULATION] Resent the last captured packet unchanged.\n");
}

static void read_response(int fd, int total_wait_ms)
{
    char rx[256];
    int total = 0;
    int max_empty_reads = (total_wait_ms / 500) + 1;
    int empty_reads = 0;
    while (empty_reads < max_empty_reads && total < (int)sizeof(rx) - 1) {
        int bytes = read(fd, rx + total, sizeof(rx) - 1 - total);
        if (bytes > 0) {
            total += bytes;
            empty_reads = 0;
        } else {
            empty_reads++;
        }
    }
    if (total > 0) {
        rx[total] = '\0';
        printf("[BLACKPILL_TELEMETRY]: %s\n", rx);
    } else {
        printf("[BLACKPILL_TELEMETRY]: (no response received)\n");
    }
}

int main(int argc, char *argv[])
{
    const char *device = (argc > 1) ? argv[1] : "/dev/ttyUSB0";

    printf("Opening %s for Black Pill (second node)...\n", device);
    int fd = open_serial(device);
    if (fd < 0) {
        printf("Failed to open %s - check the USB-TTL adapter is connected and the device path is correct.\n", device);
        return -1;
    }
    printf("Connected to Black Pill node.\n");

    int choice;
    uint8_t dummy = 0x01;

    while (1) {
        printf("\n--- V2X SENTINEL GATEWAY (Black Pill - Second Node) ---\n");
        printf("1. Node Status Report\n2. Toggle LED\n3. Simulate Malicious Attack\n"
               "4. Exit\n5. Simulate Replay Attack (resend last packet)\nSelection: ");
        if (scanf("%d", &choice) != 1) break;

        if (choice == 1) {
            send_packet(fd, CMD_GET_STATUS, &dummy, 1, 0);
            read_response(fd, 1000);
        }
        else if (choice == 2) {
            printf("LED State (1:ON, 0:OFF): ");
            int s; scanf("%d", &s);
            uint8_t val = (uint8_t)s;
            send_packet(fd, CMD_LED_CTRL, &val, 1, 0);
            read_response(fd, 1000);
        }
        else if (choice == 3) {
            printf("Injecting Corrupt Payload...\n");
            send_packet(fd, CMD_GET_STATUS, &dummy, 1, 1);
            read_response(fd, 1000);
        }
        else if (choice == 5) {
            replay_last_packet(fd);
            read_response(fd, 1000);
        }
        else {
            break;
        }
    }

    close(fd);
    return 0;
}
