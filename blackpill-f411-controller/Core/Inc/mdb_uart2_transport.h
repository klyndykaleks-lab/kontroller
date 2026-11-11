#ifndef MDB_UART2_TRANSPORT_H
#define MDB_UART2_TRANSPORT_H
/* Included by ftasks.c only. USART2 owns PA2/PA3, normal UART polarity.
 * ISR only captures complete 9-bit words; task waits without busy GPIO polling. */
typedef struct { uint16_t word; uint16_t errors; uint32_t cycle; } MdbUart2Word;
static osMessageQueueId_t mdbUart2Queue;
static volatile bool mdbUart2Overflow;
static volatile uint32_t mdbUart2RxErrors, mdbUart2Drops;
static uint32_t mdbUart2LowSince, mdbUart2TxStarted;
static bool mdbUart2LowTracking, mdbUart2ResetLatched;
static uint32_t MdbUart2Ticks(uint32_t ms) {
    return (uint32_t)(((uint64_t)ms * osKernelGetTickFreq() + 999U) / 1000U);
}
void MdbUart2IRQ(void) {
    uint32_t status = USART2->SR;
    if (status & (USART_SR_RXNE | USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) {
        MdbUart2Word item;
        item.cycle = DWT->CYCCNT;
        item.word = (uint16_t)(USART2->DR & 0x1FFU); /* SR then DR clears errors. */
        item.errors = (uint16_t)(status & (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE));
        if (item.errors) ++mdbUart2RxErrors;
        if (!mdbUart2Queue || osMessageQueuePut(mdbUart2Queue, &item, 0U, 0U) != osOK) {
            ++mdbUart2Drops;
            mdbUart2Overflow = true;
        }
    }
}
static void MdbUart2Init(void) {
    /* main/MSP already configured USART2 9600/9N1 AF7. Never switch to GPIO. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    mdbUart2Queue = osMessageQueueNew(128U, sizeof(MdbUart2Word), NULL);
    if (!mdbUart2Queue) Error_Handler();
    (void)USART2->SR; (void)USART2->DR;
    HAL_NVIC_SetPriority(USART2_IRQn, 5U, 0U); /* CMSIS ISR queue API permitted. */
    SET_BIT(USART2->CR1, USART_CR1_RXNEIE | USART_CR1_PEIE);
    SET_BIT(USART2->CR3, USART_CR3_EIE);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
}
static bool MdbUart2Read(uint16_t *word, uint32_t timeoutMs, bool ackWindow) {
    uint32_t started = HAL_GetTick();
    /* UART reports completed words, not start edges: allow one word to finish.
     * The per-word compatibility timeout remains bounded; no timer IRQ masking. */
    uint32_t budget = timeoutMs + 2U;
    for (;;) {
        bool low = (GPIOA->IDR & GPIO_PIN_3) == 0U;
        uint32_t now = HAL_GetTick();
        if (!low) { mdbUart2LowTracking = false; mdbUart2ResetLatched = false; }
        else if (!mdbUart2LowTracking) { mdbUart2LowSince = now; mdbUart2LowTracking = true; }
        else if (!mdbUart2ResetLatched && now - mdbUart2LowSince >= 100U) {
            mdbUart2ResetLatched = true;
            mdbBusResetDetected = true;
            (void)osMessageQueueReset(mdbUart2Queue);
            return false;
        }
        if (mdbUart2Overflow) {
            mdbUart2Overflow = false;
            (void)osMessageQueueReset(mdbUart2Queue);
            return false; /* Fail closed; never accept a partial frame. */
        }
        MdbUart2Word item;
        if (osMessageQueueGet(mdbUart2Queue, &item, NULL, MdbUart2Ticks(1U)) == osOK) {
            if (item.errors) return false;
            if (ackWindow && !(item.word & 0x100U)
                    && (item.word == 0U || item.word == 0xAAU || item.word == 0xFFU)
                    && (int32_t)(item.cycle - mdbUart2TxStarted) < 0) {
                if (HAL_GetTick() - started >= budget) return false;
                continue; /* Never retire this reply with a queued old control. */
            }
            *word = item.word;
            mdbDebugLastWord = *word;
            mdbDebugLastRxTick = HAL_GetTick();
            return true;
        }
        if (HAL_GetTick() - started >= budget) return false;
    }
}
static bool mdbUart2TxFailed;
static void MdbUart2Send(const uint16_t *words, uint8_t len) {
    /* HAL 9-bit/no-parity uses uint16_t elements. Waits for TC before return;
     * RXNE IRQ remains enabled, so an immediate ACK is captured in the queue. */
    mdbUart2TxStarted = DWT->CYCCNT;
    if (HAL_UART_Transmit(&huart2, (uint8_t *)(uintptr_t)words, len,
                         (uint32_t)len * 2U + 5U) != HAL_OK)
        mdbUart2TxFailed = true;
}
#endif
