
#include "stm32f4xx_hal.h"
#include <string.h>
#include <stdio.h>

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

typedef enum { STATE_IDLE, STATE_ID, STATE_LEN_MSB, STATE_LEN_LSB, STATE_DATA, STATE_CRC } v2x_state_t;

#define MAX_PAYLOAD 64U
static v2x_state_t rx_state = STATE_IDLE;
static uint8_t rx_buf[MAX_PAYLOAD];
static uint16_t p_len = 0U, p_idx = 0U;
static uint8_t rx_pkt_id = 0U;
static uint8_t calc_sum = 0U;

typedef struct {
    uint32_t total_pkts;
    uint32_t crc_errs;
} Stats;
static Stats node_stats = {0U, 0U};
static volatile uint32_t v2v_bytes_seen = 0U;

static uint32_t last_accepted_seq = 0U;
static uint32_t node_stats_replay_rejected = 0U;

#define CMD_GET_STATUS 0x10U
#define CMD_LED_CTRL   0x20U

static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
void SystemClock_Config(void);

static void reply(const char *s)
{
    HAL_UART_Transmit(&huart1, (uint8_t*)s, strlen(s), 200);
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
                if (p_len < 4U)
                {
                    reply("ALERT:BAD_PACKET\n");
                    rx_state = STATE_IDLE;
                    break;
                }

                uint32_t seq = ((uint32_t)rx_buf[0] << 24) | ((uint32_t)rx_buf[1] << 16) |
                               ((uint32_t)rx_buf[2] << 8) | rx_buf[3];

                if (seq <= last_accepted_seq)
                {
                    node_stats_replay_rejected++;
                    reply("ALERT:REPLAY_DETECTED\n");
                    rx_state = STATE_IDLE;
                    break;
                }
                last_accepted_seq = seq;

                const uint8_t *payload = &rx_buf[4];
                (void)payload;

                if (rx_pkt_id == CMD_GET_STATUS)
                {
                    char rep[100];
                    snprintf(rep, sizeof(rep), "STATS|NODE:BLACKPILL|PKT:%lu|ERR:%lu|REPLAY:%lu|V2V_BYTES:%lu\n",
                             (unsigned long)node_stats.total_pkts, (unsigned long)node_stats.crc_errs,
                             (unsigned long)node_stats_replay_rejected, (unsigned long)v2v_bytes_seen);
                    reply(rep);
                }
                else if (rx_pkt_id == CMD_LED_CTRL)
                {

                    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13,
                                       (payload[0] != 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
                    reply("EVENT:LED_ACK\n");
                }
                else
                {
                    reply("ALERT:UNKNOWN_CMD\n");
                }
            }
            else
            {
                node_stats.crc_errs++;
                reply("ALERT:CRC_FAIL\n");
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
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART1_UART_Init();
    MX_USART2_UART_Init();

    reply("\r\n*** BLACK PILL V2X-P NODE STARTED ***\r\n");

    char v2v_buf[80];
    uint8_t v2v_idx = 0U;

    while (1)
    {
        uint8_t b;
        if (HAL_UART_Receive(&huart1, &b, 1, 0) == HAL_OK)
        {
            feed_byte_to_parser(b);
        }

        uint8_t v2v_byte;
        if (HAL_UART_Receive(&huart2, &v2v_byte, 1, 0) == HAL_OK)
        {
            v2v_bytes_seen++;
            if (v2v_byte == (uint8_t)'\n' || v2v_idx >= (sizeof(v2v_buf) - 1U))
            {
                v2v_buf[v2v_idx] = '\0';
                if (strncmp(v2v_buf, "ALERT:ACCIDENT_DETECTED", 23) == 0)
                {

                    reply("EVENT:V2V_ALERT_RECEIVED - reacting with hazard warning\n");
                    for (uint8_t i = 0U; i < 15U; i++)
                    {
                        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
                        HAL_Delay(100);
                    }
                }
                v2v_idx = 0U;
            }
            else
            {
                v2v_buf[v2v_idx++] = (char)v2v_byte;
            }
        }
    }
}

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

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
    GPIO_InitStruct.Pin = GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
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
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                 | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
    {
        Error_Handler();
    }
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) { }
}
