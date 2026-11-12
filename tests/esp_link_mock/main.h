#ifndef MOCK_HAL_H
#define MOCK_HAL_H
#include <stdint.h>
typedef struct { int unused; } DMA_HandleTypeDef;
typedef struct { void *Instance; } UART_HandleTypeDef;
#define USART1 ((void*)1)
#define RESET 0
#define DMA_FLAG_TCIF2_6 1
#define DMA_IT_HT 1
#define HAL_OK 0
#define HAL_BUSY 2
extern uint32_t mockMask,mockRemaining,mockPendingTc;
#define __get_PRIMASK() mockMask
#define __disable_irq() (mockMask=1)
#define __enable_irq() (mockMask=0)
#define __HAL_DMA_GET_COUNTER(p) mockRemaining
#define __HAL_DMA_GET_FLAG(p,f) mockPendingTc
#define __HAL_DMA_DISABLE_IT(p,f) ((void)0)
uint32_t HAL_GetTick(void);
int HAL_UART_Receive_DMA(UART_HandleTypeDef*,uint8_t*,uint16_t);
int HAL_UART_Transmit_DMA(UART_HandleTypeDef*,uint8_t*,uint16_t);
int HAL_UART_AbortReceive(UART_HandleTypeDef*);
int HAL_UART_AbortTransmit(UART_HandleTypeDef*);
#endif
