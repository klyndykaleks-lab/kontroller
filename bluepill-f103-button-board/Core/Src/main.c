#include "main.h"
#include "button_control.h"
#include <string.h>
#include "../../../common/wire_codec.h"
static I2C_HandleTypeDef bus;
static ButtonControl buttons;
static uint8_t snapshot[12], tx[28], control[20];
static volatile bool recovery;
static uint32_t nextRecovery;
static volatile uint32_t transferStarted;
static volatile bool transferring;
static const uint16_t pins[10] = {GPIO_PIN_6,GPIO_PIN_5,GPIO_PIN_4,GPIO_PIN_3,GPIO_PIN_2,
    GPIO_PIN_1,GPIO_PIN_0,GPIO_PIN_15,GPIO_PIN_14,GPIO_PIN_13};
static volatile uint32_t maintenanceRebootToken,maintenanceRebootAt,maintenanceExpiredToken;
static volatile uint8_t maintenanceQuery;
static uint32_t maintenanceCompletedToken;
static void MaintenanceBootInit(void) {
    __HAL_RCC_PWR_CLK_ENABLE(); __HAL_RCC_BKP_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    if (BKP->DR3 == 0xA610U) { BKP->DR3 = 0xA611U; }
    if (BKP->DR3 == 0xA611U)
        maintenanceCompletedToken = (uint32_t)BKP->DR1 | ((uint32_t)BKP->DR2 << 16);
}

static void MaintenanceRebootService(void) {
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    uint32_t token = maintenanceRebootToken;
    if (!token) {__set_PRIMASK(mask); return;}
    if ((uint32_t)(HAL_GetTick() - maintenanceRebootAt) >= 15000U) {
        maintenanceExpiredToken = token; maintenanceRebootToken = 0U; __set_PRIMASK(mask); return;
    }
    /* This read-only board has no actuator outputs. */

    BKP->DR3 = 0U;
    BKP->DR1 = token & 0xFFFFU; BKP->DR2 = token >> 16;
    __DMB(); BKP->DR3 = 0xA610U; __DSB();
    NVIC_SystemReset();
}

static uint16_t ReadButtons(void) {
    uint16_t mask = 0;
    for (unsigned i = 0; i < 10; ++i)
        if (HAL_GPIO_ReadPin(i < 7 ? GPIOA : GPIOC, pins[i]) == GPIO_PIN_RESET) mask |= 1U << i;
    return mask;
}
void HAL_MspInit(void) {
    __HAL_RCC_AFIO_CLK_ENABLE(); __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_AFIO_REMAP_SWJ_NOJTAG();
}
void HAL_I2C_MspInit(I2C_HandleTypeDef *h) {
    (void)h; __HAL_RCC_GPIOB_CLK_ENABLE(); __HAL_RCC_I2C1_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0}; gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_OD; gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    /* F1 AF_OD has no internal pull-up. Use common bus pull-ups to 3.3 V. */
    HAL_GPIO_Init(GPIOB, &gpio);
}
void HAL_I2C_MspDeInit(I2C_HandleTypeDef *h) {
    (void)h; __HAL_RCC_I2C1_FORCE_RESET(); __HAL_RCC_I2C1_RELEASE_RESET();
    __HAL_RCC_I2C1_CLK_DISABLE();
}
static bool BusInit(void) {
    bus.Instance = I2C1; bus.Init.ClockSpeed = 10000;
    bus.Init.DutyCycle = I2C_DUTYCYCLE_2; bus.Init.OwnAddress1 = 0x10U << 1;
    bus.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    bus.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    bus.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    bus.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    return HAL_I2C_Init(&bus) == HAL_OK && HAL_I2C_EnableListen_IT(&bus) == HAL_OK;
}
void HAL_I2C_AddrCallback(I2C_HandleTypeDef *h, uint8_t dir, uint16_t addr) {
    (void)addr; transferStarted = HAL_GetTick(); transferring = true;
    if (dir == I2C_DIRECTION_RECEIVE) {
        unsigned length=12;
        if(maintenanceQuery){maintenanceQuery=0;length=28;memset(tx,0,sizeof(tx));tx[0]=0xB6;tx[1]=1;
            WireWrite32(tx+4,maintenanceCompletedToken);WireWrite32(tx+24,WireCrc32(tx,24));
        }else memcpy(tx,snapshot,12);
        if (HAL_I2C_Slave_Seq_Transmit_IT(h, tx, length, I2C_FIRST_AND_LAST_FRAME) != HAL_OK) recovery = true;
    } else if (HAL_I2C_Slave_Seq_Receive_IT(h, control, 20, I2C_FIRST_AND_LAST_FRAME) != HAL_OK) recovery = true;
}
void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef*h){
    if(h->Instance!=I2C1||control[0]!=0xA6||control[1]!=1||WireRead32(control+16)!=WireCrc32(control,16))return;
    uint32_t token=WireRead32(control+4);
    if(control[2]==2)maintenanceQuery=1;
    else if(control[2]==3&&maintenanceRebootToken==token){maintenanceExpiredToken=token;maintenanceRebootToken=0;}
    else if(control[2]==1&&token&&token!=maintenanceCompletedToken&&token!=maintenanceExpiredToken
        &&(!maintenanceRebootToken||maintenanceRebootToken==token)){
        if(!maintenanceRebootToken)maintenanceRebootAt=HAL_GetTick();maintenanceRebootToken=token;
    }
}
void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *h) {
    transferring = false;
    if (HAL_I2C_EnableListen_IT(h) != HAL_OK) recovery = true;
}
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *h) { (void)h; recovery = true; }
void I2C1_EV_IRQHandler(void) { HAL_I2C_EV_IRQHandler(&bus); }
void I2C1_ER_IRQHandler(void) { HAL_I2C_ER_IRQHandler(&bus); }
void SysTick_Handler(void) { HAL_IncTick(); }
void Error_Handler(void) { __disable_irq(); for (;;) {} }
int main(void) {
    HAL_Init(); /* HSI 8 MHz: no dependence on fitted crystal or LSE pins. */
    __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0}; gpio.Mode = GPIO_MODE_INPUT; gpio.Pull = GPIO_PULLUP;
    gpio.Pin = 0x7fU; HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15; HAL_GPIO_Init(GPIOC, &gpio);
    HAL_NVIC_SetPriority(I2C1_EV_IRQn, 1, 0); HAL_NVIC_SetPriority(I2C1_ER_IRQn, 1, 0);
    MaintenanceBootInit();
    recovery = !BusInit(); HAL_NVIC_EnableIRQ(I2C1_EV_IRQn); HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
    IWDG->KR = 0x5555U; IWDG->PR = 6U; IWDG->RLR = 625U; IWDG->KR = 0xCCCCU;
    for (;;) {
        MaintenanceRebootService();
        uint32_t now = HAL_GetTick();
        ButtonControlStep(&buttons, now, ReadButtons());
        uint8_t frame[12] = {0xB1U, 1U, (uint8_t)buttons.stable, (uint8_t)(buttons.stable >> 8)};
        WireWrite32(frame + 4, buttons.sequence); WireWrite32(frame + 8, WireCrc32(frame, 8));
        uint32_t mask = __get_PRIMASK(); __disable_irq();
        for (unsigned i = 0; i < sizeof(frame); ++i) snapshot[i] = frame[i];
        __set_PRIMASK(mask);
        if (transferring && (uint32_t)(now - transferStarted) >= 100U) recovery = true;
        if (recovery && (int32_t)(now - nextRecovery) >= 0) {
            HAL_NVIC_DisableIRQ(I2C1_EV_IRQn); HAL_NVIC_DisableIRQ(I2C1_ER_IRQn);
            HAL_I2C_DeInit(&bus); transferring = false; recovery = !BusInit();
            HAL_NVIC_ClearPendingIRQ(I2C1_EV_IRQn); HAL_NVIC_ClearPendingIRQ(I2C1_ER_IRQn);
            HAL_NVIC_EnableIRQ(I2C1_EV_IRQn); HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
            nextRecovery = HAL_GetTick() + 100U;
        } else if (!recovery && HAL_I2C_GetState(&bus) == HAL_I2C_STATE_READY) {
            recovery = HAL_I2C_EnableListen_IT(&bus) != HAL_OK;
        }
        IWDG->KR = 0xAAAAU;
    }
}
