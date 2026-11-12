#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "main.h"
#include "cmsis_os.h"
uint32_t mockMask,mockRemaining=4096,mockPendingTc;
static uint32_t tick;
static uint8_t *mockRx;
static const uint8_t *wirePointer;
static uint16_t wireLength;
static unsigned starts,aborts;
static int busy;
UART_HandleTypeDef huart1={USART1};
DMA_HandleTypeDef hdma_usart1_rx;
typedef struct { unsigned head,count,depth,size; unsigned char *data; } Queue;
osMessageQueueId_t osMessageQueueNew(uint32_t depth,uint32_t size,const void *attr) {
    (void)attr; Queue*q=calloc(1,sizeof(*q));q->depth=depth;q->size=size;q->data=calloc(depth,size);return q;
}
osStatus_t osMessageQueuePut(osMessageQueueId_t id,const void *v,uint8_t priority,uint32_t wait) {
    (void)priority;assert(!wait);Queue*q=id;if(q->count==q->depth)return -1;
    memcpy(q->data+((q->head+q->count)%q->depth)*q->size,v,q->size);++q->count;return osOK;
}
osStatus_t osMessageQueueGet(osMessageQueueId_t id,void*v,uint8_t*p,uint32_t wait) {
    (void)p;assert(!wait);Queue*q=id;if(!q->count)return -1;memcpy(v,q->data+q->head*q->size,q->size);q->head=(q->head+1)%q->depth;--q->count;return osOK;
}
uint32_t osMessageQueueGetCount(osMessageQueueId_t id){return ((Queue*)id)->count;}
void osDelay(uint32_t ms){tick+=ms;}
uint32_t HAL_GetTick(void){return tick;}
int HAL_UART_Receive_DMA(UART_HandleTypeDef*u,uint8_t*b,uint16_t n){(void)u;mockRx=b;mockRemaining=n;return HAL_OK;}
int HAL_UART_Transmit_DMA(UART_HandleTypeDef*u,uint8_t*b,uint16_t n){(void)u;if(busy)return HAL_BUSY;wirePointer=b;wireLength=n;++starts;return HAL_OK;}
int HAL_UART_AbortReceive(UART_HandleTypeDef*u){(void)u;return HAL_OK;}
int HAL_UART_AbortTransmit(UART_HandleTypeDef*u){(void)u;++aborts;return HAL_OK;}
#include "../blackpill-f411-controller/Core/Src/esp_link.c"
static void feed(const char*s){while(*s){mockRx[(RX_CAPACITY-mockRemaining)%RX_CAPACITY]=(uint8_t)*s++;if(!--mockRemaining){HAL_UART_RxCpltCallback(&huart1);mockRemaining=RX_CAPACITY;}}ServiceReceive();}
static void complete(void){HAL_UART_TxCpltCallback(&huart1);ServiceTransmit();}
int main(void){
 assert(EspLinkInit());assert(EspLinkTxIdle());ServiceReceive();
 assert(!EspLinkSendLine(NULL));assert(!EspLinkSendLine(""));assert(!EspLinkSendLine("A\nB"));
 char longLine[129];memset(longLine,'x',128);longLine[128]=0;assert(!EspLinkSendLine(longLine));
 assert(EspLinkSendLine("FIRST"));ServiceTransmit();assert(starts==1);assert(!EspLinkTxIdle());
 assert(wireLength==8 && !memcmp(wirePointer,"\nFIRST\r\n",8));
 assert(EspLinkSendLine("SECOND"));ServiceTransmit();assert(starts==1 && !memcmp(wirePointer,"\nFIRST\r\n",8));
 complete();ServiceTransmit();assert(starts==2 && !memcmp(wirePointer,"\nSECOND\r\n",9));complete();
 busy=1;assert(EspLinkSendLine("RETRY"));ServiceTransmit();assert(txPending&&!txActive);tick+=20;busy=0;ServiceTransmit();assert(txActive);
 tick+=100;ServiceTransmit();assert(aborts==1 && txPending && !txActive);tick+=20;ServiceTransmit();assert(!memcmp(wirePointer,"\nRETRY\r\n",8));complete();
 char out[128];feed("PAYMENT_OK,tx=7,");assert(!EspLinkReceiveLine(out));feed("boot=2\r\n");assert(EspLinkReceiveLine(out)&&!strcmp(out,"PAYMENT_OK,tx=7,boot=2"));
 for(unsigned i=0;i<RX_DEPTH+1;i++)feed("F411_QUERY\n");assert(rxPending);
 assert(EspLinkReceiveLine(out));ServiceReceive();assert(!rxPending);unsigned read=1;while(EspLinkReceiveLine(out))++read;assert(read==RX_DEPTH+1);
 feed("OLD_PART");HAL_UART_ErrorCallback(&huart1);ServiceReceive();feed("TAIL\nFRESH\n");assert(EspLinkReceiveLine(out)&&!strcmp(out,"FRESH"));assert(!EspLinkReceiveLine(out));
 feed("PART");rxWraps+=2;mockRemaining=RX_CAPACITY;ServiceReceive();assert(discardLine);feed("TAIL\nOK\n");assert(EspLinkReceiveLine(out)&&!strcmp(out,"OK"));
 mockMask=1;(void)EspLinkGetStats();assert(mockMask==1);mockMask=0;
 for(unsigned i=0;i<TX_DEPTH;i++)assert(EspLinkSendLine("Q"));assert(!EspLinkSendLine("OVERFLOW"));
 assert(EspLinkGetStats().txCompleted==3);assert(EspLinkGetStats().txRetries>=2);
 puts("PASS: async DMA lifetime/order, busy/timeout retry, framing, fragmented RX, RX backpressure, UART error resync, overflow resync, IRQ state, TX saturation");
}
