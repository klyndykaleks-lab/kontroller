#include "esp_link.h"
#include "main.h"
#include "cmsis_os.h"
#include <string.h>

/* Hardware ownership belongs exclusively to espLinkTask. No protocol/state
 * callbacks execute here or in IRQ context. Queues copy complete lines. */
extern UART_HandleTypeDef huart1;
extern DMA_HandleTypeDef hdma_usart1_rx;
#define TX_DEPTH 16U
#define RX_DEPTH 16U
#define RX_CAPACITY 4096U
#define TX_TIMEOUT_MS 100U
typedef struct { char text[ESP_LINK_LINE_SIZE]; } LinkLine;
static osMessageQueueId_t txQueue, rxQueue;
static uint8_t rxDma[RX_CAPACITY];
static volatile uint32_t rxWraps;
static volatile bool rxError, txComplete;
static uint32_t rxConsumed, txStarted, retryAt;
static bool rxStarted, discardLine, rxPending, txActive;
static volatile bool txPending;
static LinkLine receiveLine, transmitLine;
static uint16_t receiveLength;
/* Leading LF resynchronizes ESP after an interrupted/partial UART frame. */
static uint8_t txWire[ESP_LINK_LINE_SIZE + 3U];
static uint16_t txLength;
static volatile EspLinkStats stats;

bool EspLinkInit(void) {
    txQueue = osMessageQueueNew(TX_DEPTH, sizeof(LinkLine), NULL);
    rxQueue = osMessageQueueNew(RX_DEPTH, sizeof(LinkLine), NULL);
    return txQueue != NULL && rxQueue != NULL;
}

bool EspLinkSendLine(const char *line) {
    LinkLine copy = {{0}};
    if (!line || !txQueue) return false;
    size_t n = strnlen(line, ESP_LINK_LINE_SIZE);
    if (!n || n >= ESP_LINK_LINE_SIZE || strchr(line, '\n') || strchr(line, '\r'))
        return false;
    memcpy(copy.text, line, n);
    /* Thread-safe multi-producer queue; never wait for wire completion. */
    if (osMessageQueuePut(txQueue, &copy, 0U, 0U) != osOK) {
        uint32_t saved = __get_PRIMASK(); __disable_irq();
        ++stats.txBackpressure;
        if (!saved) __enable_irq();
        return false;
    }
    return true;
}

bool EspLinkReceiveLine(char line[ESP_LINK_LINE_SIZE]) {
    return line && rxQueue && osMessageQueueGet(rxQueue, line, NULL, 0U) == osOK;
}

bool EspLinkTxIdle(void) {
    return txQueue && !txPending && osMessageQueueGetCount(txQueue) == 0U;
}

EspLinkStats EspLinkGetStats(void) {
    uint32_t saved = __get_PRIMASK(); __disable_irq();
    EspLinkStats copy = stats;
    if (!saved) __enable_irq();
    return copy;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart) {
    if (uart && uart->Instance == USART1) ++rxWraps;
}
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *uart) {
    if (uart && uart->Instance == USART1) txComplete = true;
}
void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart) {
    if (uart && uart->Instance == USART1) rxError = true;
}

static uint32_t Produced(void) {
    uint32_t saved = __get_PRIMASK(); __disable_irq();
    uint32_t wraps = rxWraps;
    uint32_t left = __HAL_DMA_GET_COUNTER(&hdma_usart1_rx);
    if (__HAL_DMA_GET_FLAG(&hdma_usart1_rx, DMA_FLAG_TCIF2_6) != RESET) {
        left = __HAL_DMA_GET_COUNTER(&hdma_usart1_rx);
        if (left) ++wraps;
    }
    if (!saved) __enable_irq();
    return wraps * RX_CAPACITY + RX_CAPACITY - left;
}

static void ServiceReceive(void) {
    if (rxError || !rxStarted) {
        if (rxStarted) { HAL_UART_AbortReceive(&huart1); ++stats.rxRestarts; }
        /* Never splice a post-error fragment onto a pre-error line. */
        if (rxError) { discardLine = true; receiveLength = 0U; ++stats.rxDrops; }
        rxWraps = 0U; rxConsumed = 0U; rxError = false;
        rxStarted = HAL_UART_Receive_DMA(&huart1, rxDma, RX_CAPACITY) == HAL_OK;
        if (rxStarted) __HAL_DMA_DISABLE_IT(&hdma_usart1_rx, DMA_IT_HT);
        return;
    }
    uint32_t produced = Produced();
    if (produced - rxConsumed >= RX_CAPACITY) {
        rxConsumed = produced; receiveLength = 0U; discardLine = true;
        ++stats.rxDrops;
    }
    if (rxPending) {
        if (osMessageQueuePut(rxQueue, &receiveLine, 0U, 0U) != osOK) return;
        rxPending = false; receiveLength = 0U;
    }
    for (uint32_t budget = 256U; rxConsumed != produced && budget; --budget) {
        char ch = (char)rxDma[rxConsumed++ % RX_CAPACITY];
        if (discardLine) { if (ch == '\n') discardLine = false; continue; }
        if (ch == '\r') continue;
        if (ch == '\n') {
            if (!receiveLength) continue;
            receiveLine.text[receiveLength] = '\0';
            if (osMessageQueuePut(rxQueue, &receiveLine, 0U, 0U) != osOK) {
                rxPending = true; return; /* retain completed line on backpressure */
            }
            receiveLength = 0U;
        } else if (receiveLength < ESP_LINK_LINE_SIZE - 1U) {
            receiveLine.text[receiveLength++] = ch;
        } else {
            receiveLength = 0U; discardLine = true; ++stats.rxDrops;
        }
    }
}

static void ServiceTransmit(void) {
    uint32_t now = HAL_GetTick();
    if (txActive) {
        if (txComplete) {
            txActive = false; txPending = false; ++stats.txCompleted;
        } else if (now - txStarted >= TX_TIMEOUT_MS) {
            /* Abort TX only: RX DMA remains live. Retain exactly the same line. */
            HAL_UART_AbortTransmit(&huart1);
            txActive = false; txComplete = false;
            retryAt = now + 20U; ++stats.txRetries;
        }
        return;
    }
    if ((int32_t)(now - retryAt) < 0) return;
    if (!txPending) {
        /* Publish busy before dequeue: another task must not observe false idle. */
        txPending = true;
        if (osMessageQueueGet(txQueue, &transmitLine, NULL, 0U) != osOK) {
            txPending = false; return;
        }
        size_t n = strlen(transmitLine.text);
        txWire[0] = '\n'; memcpy(txWire + 1U, transmitLine.text, n);
        txWire[n + 1U] = '\r'; txWire[n + 2U] = '\n';
        txLength = (uint16_t)(n + 3U);
    }
    txComplete = false;
    if (HAL_UART_Transmit_DMA(&huart1, txWire, txLength) == HAL_OK) {
        txStarted = now; txActive = true;
    } else { retryAt = now + 20U; ++stats.txRetries; }
}

void espLinkTask(void *argument) {
    (void)argument;
    for (;;) {
        ServiceReceive(); ServiceTransmit();
        osDelay(1U); /* block so manager and lower-priority tasks can run */
    }
}
