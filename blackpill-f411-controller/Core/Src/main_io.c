#include "main_io.h"
#include "main.h"
#include "lock_control.h"
static LockControl lock;
static volatile bool ready, dropArmed, dropDetected;
static volatile uint32_t dropBoot, dropOperation, dropStarted;
void MainIoInit(void) { ready = true; }
void MainIoStopForMaintenance(void) {
    uint32_t mask=__get_PRIMASK();__disable_irq();
    ready=false;dropArmed=false;dropDetected=false;
    HAL_GPIO_WritePin(GPIOA,GPIO_PIN_0,GPIO_PIN_RESET);
    __set_PRIMASK(mask);
}
void MainIoTick(void) {
    if (!ready) return;
    /* TIM11 owns this small GPIO-only service: I2C/SD cannot extend a lock pulse. */
    LockControlStep(&lock, HAL_GetTick(),
                    HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_4) == GPIO_PIN_RESET,
                    HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_1) == GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, lock.output ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
bool MainDropBlocked(void) { return HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_15) == GPIO_PIN_RESET; }
void MainDropArm(uint32_t boot, uint32_t operation) {
    uint32_t mask = __get_PRIMASK(); __disable_irq();
    dropArmed = false; dropDetected = false;
    __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_15);
    dropBoot = boot; dropOperation = operation; dropStarted = HAL_GetTick();
    dropArmed = true; __set_PRIMASK(mask);
}
void MainDropDisarm(void) { dropArmed = false; dropDetected = false; }
bool MainDropDetected(uint32_t boot, uint32_t operation) {
    return dropArmed && dropDetected && dropBoot == boot && dropOperation == operation;
}
void HAL_GPIO_EXTI_Callback(uint16_t pin) {
    if (pin == GPIO_PIN_15 && dropArmed && MainDropBlocked()) {
        uint32_t age = HAL_GetTick() - dropStarted;
        if (age > 15U && age < 11000U) dropDetected = true;
    }
}
