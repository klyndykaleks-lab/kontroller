/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2025 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include <stdint.h>
#include "motor_control.h"
#include "motor_journal.h"
#include "../../../common/wire_codec.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;

/* USER CODE BEGIN PV */
#define STATUS_IDLE    0x00
#define STATUS_BUSY    0x01
#define STATUS_SUCCESS 0x02
#define STATUS_FAIL    0x03
#define MOTOR_STATUS_PROTOCOL_VERSION 3U
#define MOTOR_STATUS_FRAME_SIZE 28U
#define MOTOR_STATUS_FLAG_OUTPUT_ACTIVE  (1U << 1)
#define MOTOR_STATUS_FLAG_I2C_RECOVERY   (1U << 2)
#define F103_RESET_PIN     (1U << 0)
#define F103_RESET_POWER   (1U << 1)
#define F103_RESET_SOFTWARE (1U << 2)
#define F103_RESET_IWDG    (1U << 3)
#define F103_RESET_WWDG    (1U << 4)
#define F103_RESET_LOW_POWER (1U << 5)
#define F103_IWDG_RELOAD 3750U
#define I2C1_RECOVERY_BACKOFF_MS 100U

static MotorControl motorControl;
static MotorJournal motorJournal;
/* I2C IRQ sees only an atomically published record, never a partial journal struct copy. */
static volatile MotorRecord reportedMotorRecord;
static volatile bool reportedMotorRecordValid;
static volatile bool motorStorageFailed;
static bool recoveredMotorResult;
static uint32_t motorJournalRetryTick;
static volatile uint32_t pendingMotorBoot, pendingMotorOperation, pendingDirectOperation;
static volatile uint8_t pendingMotorCommand;
static volatile uint8_t pendingMotorLine, pendingMotorTurns;
static volatile uint8_t pendingSensorEvent;
static volatile uint32_t pendingFinishBoot, pendingFinishOperation;
static volatile uint8_t pendingFinishLine, pendingFinishStatus;
static void AllMotorsOff(void);
static volatile uint32_t maintenanceRebootToken, maintenanceRebootAt, maintenanceExpiredToken;
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
    if (motorControl.status == MOTOR_BUSY || pendingMotorCommand) {__set_PRIMASK(mask); return;}
    AllMotorsOff();
    /* Lock is owned by F411 on Varka. */
    BKP->DR3 = 0U;
    BKP->DR1 = token & 0xFFFFU; BKP->DR2 = token >> 16;
    __DMB(); BKP->DR3 = 0xA610U; __DSB();
    NVIC_SystemReset();
}
uint8_t rxBuffer[20];
uint8_t tx_status_frame[MOTOR_STATUS_FRAME_SIZE];
volatile uint8_t reset_flags = 0;

// Массивы пинов
GPIO_TypeDef* motors_port[] = { m_1_GPIO_Port, m_2_GPIO_Port, m_3_GPIO_Port, m_4_GPIO_Port, m_5_GPIO_Port, m_6_GPIO_Port, m_7_GPIO_Port, m_8_GPIO_Port, m_9_GPIO_Port, m_10_GPIO_Port };
const uint16_t motors_pin[] = { m_1_Pin, m_2_Pin, m_3_Pin, m_4_Pin, m_5_Pin, m_6_Pin, m_7_Pin, m_8_Pin, m_9_Pin, m_10_Pin };
volatile uint8_t i2c_reinit_pending = 0;
volatile uint32_t i2c_error_count = 0;
volatile uint32_t i2c_last_error = 0;
/* HAL_I2C_MspInit normally enables the IRQs. Runtime recovery keeps them
 * isolated until the handle, peripheral and listen mode are all coherent. */
volatile uint8_t i2c1_runtime_recovery_guard = 0;
static uint32_t i2c1_next_recovery_tick = 0;
static uint32_t i2c1_recovery_count = 0;
/* Completion is real wire progress; peripheral reinitialisation alone is not. */
#define F103_TRANSPORT_STUCK_MS 2000U
#define F103_RECOVERY_LIMIT_MS 55000U
static volatile uint32_t i2c1_completed_transfers;
static uint32_t i2c1_observed_transfers, i2c1_fault_since, i2c1_busy_since;
static uint8_t i2c1_fault_armed, i2c1_busy_armed;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static void AllMotorsOff(void)
{
    for (uint8_t i = 0; i < 10; i++) {
        HAL_GPIO_WritePin(motors_port[i], motors_pin[i], GPIO_PIN_RESET);
    }
}

static uint32_t MotorStorageAddress(unsigned page, uint32_t word) {
    return 0x0800F800U + page * 1024U + word * 4U;
}
static uint32_t MotorStorageRead(unsigned page, uint32_t word) {
    return *(volatile const uint32_t *)MotorStorageAddress(page, word);
}
static bool MotorStorageProgram(unsigned page, uint32_t word, uint32_t value) {
    return HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, MotorStorageAddress(page, word), value) ==
           HAL_OK;
}
static bool MotorStorageErase(unsigned page) {
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t error = 0;
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = MotorStorageAddress(page, 0);
    erase.NbPages = 1;
    return HAL_FLASHEx_Erase(&erase, &error) == HAL_OK;
}
static void PublishMotorRecord(void) {
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    reportedMotorRecord = motorJournal.record;
    reportedMotorRecordValid = motorJournal.valid;
    __set_PRIMASK(mask);
}
static bool PersistMotorRecord(const MotorRecord *record) {
    bool ok = false;
    if (HAL_FLASH_Unlock() == HAL_OK) {
        ok = MotorJournalSave(&motorJournal, record);
    }
    HAL_FLASH_Lock();
    motorStorageFailed = !ok || motorJournal.corruptCommitted;
    if (ok)
        PublishMotorRecord();
    return ok;
}
static void RestoreMotorJournal(void) {
    const MotorJournalStorage storage = {32U, MotorStorageRead, MotorStorageProgram,
                                         MotorStorageErase};
    MotorJournalLoad(&motorJournal, &storage);
    PublishMotorRecord();
    motorStorageFailed = motorJournal.corruptCommitted;
    if (motorJournal.valid) {
        motorControl.line = motorJournal.record.line;
        motorControl.turns = motorJournal.record.turns;
        motorControl.counter = motorJournal.record.counter;
        recoveredMotorResult =
            motorJournal.record.recovered || motorJournal.record.status == MOTOR_BUSY;
        motorControl.status =
            motorJournal.record.status == MOTOR_BUSY ? MOTOR_FAILED : motorJournal.record.status;
        /* An interrupted operation is never resumed or started again. */
    }
}
static void CommitMotorOutputs(void) {
    for (uint8_t i = 0; i < 10; i++) {
        bool on = motorControl.outputActive && motorControl.line >= 1 && motorControl.line <= 10 &&
                  i == motorControl.line - 1U;
        HAL_GPIO_WritePin(motors_port[i], motors_pin[i], on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
}
static void MotorService(void) {
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    uint32_t now = HAL_GetTick();
    bool blocked = false; /* Sensor is owned by F411. */
    bool sensorEvent = pendingSensorEvent != 0U;
    bool finishMatches = pendingFinishBoot == motorJournal.record.bootIdentity &&
        pendingFinishOperation == motorJournal.record.operationId &&
        pendingFinishLine == motorControl.line && motorJournal.valid;
    pendingSensorEvent = 0;
    MotorControlStep(&motorControl, now, false, 0, false);
    if (sensorEvent && finishMatches && motorControl.status == MOTOR_BUSY) {
        motorControl.status = pendingFinishStatus;
        motorControl.phase = 0; motorControl.outputActive = false;
    }
    CommitMotorOutputs(); // terminal outputs are off before any flash operation
    uint8_t hasCommand = pendingMotorCommand, line = pendingMotorLine, turns = pendingMotorTurns;
    uint32_t boot = pendingMotorBoot, operation = pendingMotorOperation;
    uint32_t directOperation = pendingDirectOperation;
    pendingMotorCommand = 0;
    __set_PRIMASK(mask);

    if (motorJournal.valid && motorJournal.record.status != motorControl.status &&
        (int32_t)(now - motorJournalRetryTick) >= 0) {
        MotorRecord result = motorJournal.record;
        result.status = motorControl.status;
        result.recovered = recoveredMotorResult;
        (void)PersistMotorRecord(&result);
        motorJournalRetryTick = HAL_GetTick() + 250U;
    }
    if (!hasCommand || motorStorageFailed || motorControl.status == MOTOR_BUSY ||
        !MotorJournalIsNewOperation(&motorJournal, boot, operation) ||
        (directOperation != 0 && motorJournal.valid &&
         directOperation <= motorJournal.record.directOperationId)) {
        return;
    }

    MotorControl proposed = motorControl;
    if (MotorControlStart(&proposed, line, turns, HAL_GetTick(), blocked) !=
        MOTOR_COMMAND_ACCEPTED) {
        return;
    }
    MotorRecord accepted = {.bootIdentity = boot,
                            .operationId = operation,
                            .line = line,
                            .turns = turns,
                            .counter = proposed.counter,
                            .status = MOTOR_BUSY,
                            .recovered = false};
    accepted.directOperationId =
        directOperation ? directOperation
                        : (motorJournal.valid ? motorJournal.record.directOperationId : 0);
    accepted.isDirect = directOperation != 0;
    if (!PersistMotorRecord(&accepted)) {
        return; // no GPIO without durable acceptance
    }
    mask = __get_PRIMASK();
    __disable_irq();
    proposed.startedAt = HAL_GetTick();
    proposed.phaseStartedAt = proposed.startedAt;
    motorControl = proposed;
    recoveredMotorResult = false;
    CommitMotorOutputs();
    __set_PRIMASK(mask);
}

static HAL_StatusTypeDef I2C1_ReinitDeferred(void)
{
    HAL_StatusTypeDef status;

    i2c1_runtime_recovery_guard = 1U;
    HAL_NVIC_DisableIRQ(I2C1_EV_IRQn);
    HAL_NVIC_DisableIRQ(I2C1_ER_IRQn);
    HAL_NVIC_ClearPendingIRQ(I2C1_EV_IRQn);
    HAL_NVIC_ClearPendingIRQ(I2C1_ER_IRQn);
    __HAL_I2C_DISABLE_IT(&hi2c1, I2C_IT_EVT | I2C_IT_BUF | I2C_IT_ERR);

    status = HAL_I2C_DeInit(&hi2c1);
    if (status == HAL_OK) {
        hi2c1.State = HAL_I2C_STATE_RESET;
        status = HAL_I2C_Init(&hi2c1);
    }
    if (status == HAL_OK) {
        status = HAL_I2C_EnableListen_IT(&hi2c1);
    }

    HAL_NVIC_ClearPendingIRQ(I2C1_EV_IRQn);
    HAL_NVIC_ClearPendingIRQ(I2C1_ER_IRQn);
    i2c1_runtime_recovery_guard = 0U;
    if (status == HAL_OK) {
        HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
        HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
        i2c1_recovery_count++;
        i2c1_next_recovery_tick = 0U;
    }

    return status;
}

static void ServiceMotorRecovery(void)
{
    const uint32_t now = HAL_GetTick();
    const uint32_t completed = i2c1_completed_transfers;
    const HAL_I2C_StateTypeDef state = HAL_I2C_GetState(&hi2c1);
    if (completed != i2c1_observed_transfers) {
        i2c1_observed_transfers = completed;
        i2c1_fault_armed = 0U;
        i2c1_busy_armed = 0U;
    }
    if (state != HAL_I2C_STATE_READY && state != HAL_I2C_STATE_LISTEN) {
        if (!i2c1_busy_armed) {
            i2c1_busy_since = now;
            i2c1_busy_armed = 1U;
        }
        if ((uint32_t)(now - i2c1_busy_since) >= F103_TRANSPORT_STUCK_MS) {
            i2c_reinit_pending = 1U;
        }
    } else {
        i2c1_busy_armed = 0U;
    }
    if (i2c_reinit_pending && !i2c1_fault_armed) {
        i2c1_fault_since = now;
        i2c1_fault_armed = 1U;
    }
    if (i2c1_fault_armed &&
            (uint32_t)(now - i2c1_fault_since) >= F103_RECOVERY_LIMIT_MS) {
        /* Do not clear the operation journal: restore converts BUSY to FAILED,
         * retaining its identity, so retrying the old START cannot vend twice. */
        __disable_irq();
        AllMotorsOff();
        /* Lock belongs to F411 on this hardware; only motor outputs are local. */
        NVIC_SystemReset();
    }
}

static void EnsureI2C1Listening(void);

static void ServiceI2C1Transport(void)
{
    uint32_t now = HAL_GetTick();

    if (i2c_reinit_pending != 0U) {
        if ((int32_t)(now - i2c1_next_recovery_tick) < 0) {
            return;
        }

        /* ErrorCallback can leave HAL in LISTEN while the HAL disables EVT/ERR
         * after returning from that callback. Do not wait for READY here. */
        HAL_NVIC_DisableIRQ(I2C1_EV_IRQn);
        HAL_NVIC_DisableIRQ(I2C1_ER_IRQn);
        i2c_reinit_pending = 0U;
        if (I2C1_ReinitDeferred() != HAL_OK) {
            i2c_reinit_pending = 1U;
            i2c1_next_recovery_tick = HAL_GetTick() + I2C1_RECOVERY_BACKOFF_MS;
        }
        return;
    }

    EnsureI2C1Listening();
}

static void EnsureI2C1Listening(void)
{
    /* Never change HAL state from a transfer callback. The main loop repairs the
     * short READY gap immediately after HAL has completed the transaction. */
    if (HAL_I2C_GetState(&hi2c1) == HAL_I2C_STATE_READY &&
        HAL_I2C_EnableListen_IT(&hi2c1) != HAL_OK) {
        i2c_reinit_pending = 1U;
    }
}

static void CaptureResetFlags(void)
{
    uint32_t csr = RCC->CSR;

    if ((csr & RCC_CSR_PINRSTF) != 0U) reset_flags |= F103_RESET_PIN;
    if ((csr & RCC_CSR_PORRSTF) != 0U) reset_flags |= F103_RESET_POWER;
    if ((csr & RCC_CSR_SFTRSTF) != 0U) reset_flags |= F103_RESET_SOFTWARE;
    if ((csr & RCC_CSR_IWDGRSTF) != 0U) reset_flags |= F103_RESET_IWDG;
    if ((csr & RCC_CSR_WWDGRSTF) != 0U) reset_flags |= F103_RESET_WWDG;
    if ((csr & RCC_CSR_LPWRRSTF) != 0U) reset_flags |= F103_RESET_LOW_POWER;
    RCC->CSR |= RCC_CSR_RMVF;
}

static void MotorWatchdogInit(void)
{
    IWDG->KR = 0xCCCCU;
    IWDG->KR = 0x5555U;
    IWDG->PR = 4U; /* LSI / 64. */
    IWDG->RLR = F103_IWDG_RELOAD;
    while (IWDG->SR != 0U) {
    }
    IWDG->KR = 0xAAAAU;
}

static void MotorWatchdogRefresh(void)
{
    IWDG->KR = 0xAAAAU;
}

static void BuildMotorStatusFrame(void) {
    uint8_t flags = 0U;
    uint32_t error_count = i2c_error_count;
    uint32_t last_error = i2c_last_error;

    uint8_t current_motor_idx = motorControl.line >= 1U && motorControl.line <= 10U
                                    ? (uint8_t)(motorControl.line - 1U)
                                    : 10U;
    if (current_motor_idx < 10U &&
        HAL_GPIO_ReadPin(motors_port[current_motor_idx], motors_pin[current_motor_idx]) ==
            GPIO_PIN_SET) {
        flags |= MOTOR_STATUS_FLAG_OUTPUT_ACTIVE;
    }
    if (i2c_reinit_pending != 0U) {
        flags |= MOTOR_STATUS_FLAG_I2C_RECOVERY;
    }

    if (recoveredMotorResult)
        flags |= (1U << 3);
    if (motorStorageFailed)
        flags |= (1U << 4);
    /* Do not expose terminal success before it is committed to flash. */
    tx_status_frame[0] = reportedMotorRecordValid ? reportedMotorRecord.status : MOTOR_IDLE;
    tx_status_frame[1] = MOTOR_STATUS_PROTOCOL_VERSION;
    tx_status_frame[2] = motorControl.counter;
    tx_status_frame[3] = motorControl.line;
    tx_status_frame[4] = motorControl.phase;
    tx_status_frame[5] = flags;
    tx_status_frame[6] = reset_flags;
    tx_status_frame[7] = (uint8_t)(error_count & 0xFFU);
    tx_status_frame[8] = (uint8_t)((error_count >> 8U) & 0xFFU);
    tx_status_frame[9] = (uint8_t)(last_error & 0xFFU);
    tx_status_frame[10] = (uint8_t)((last_error >> 8U) & 0xFFU);
    tx_status_frame[11] = motorControl.turns;
    WireWrite32(tx_status_frame + 12,
                reportedMotorRecordValid ? reportedMotorRecord.bootIdentity : 0);
    WireWrite32(tx_status_frame + 16,
                reportedMotorRecordValid ? reportedMotorRecord.operationId : 0);
    WireWrite32(tx_status_frame + 20, reportedMotorRecordValid && reportedMotorRecord.isDirect
                                          ? reportedMotorRecord.directOperationId
                                          : 0);
    WireWrite32(tx_status_frame + 24, WireCrc32(tx_status_frame, 24));
}


/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  CaptureResetFlags();

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();
  /* USER CODE BEGIN 2 */

  MaintenanceBootInit();
  MotorControlInit(&motorControl);
  AllMotorsOff();
  RestoreMotorJournal();
  MotorWatchdogInit();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
	  MotorWatchdogRefresh();


      MotorService();
      MaintenanceRebootService();
      ServiceMotorRecovery();

	  ServiceI2C1Transport();

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	}
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL6;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 10000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  // --- ФИКСИРОВАННЫЙ АДРЕС I2C ---
  // Мы жестко задаем адрес 0x0F (десятичный 15).
  // HAL требует сдвига адреса влево на 1 бит.

  hi2c1.Init.OwnAddress1 = (0x0F << 1);

  // Переинициализируем I2C с новым адресом
  if (HAL_I2C_Init(&hi2c1) != HAL_OK) {
      Error_Handler();
  }

  // Включаем прослушивание шины (Listen Mode) - ОБЯЗАТЕЛЬНО для слейва
  if(HAL_I2C_EnableListen_IT(&hi2c1) != HAL_OK)
  {
      Error_Handler();
  }

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE(); __HAL_RCC_GPIOD_CLK_ENABLE();
    gpio.Mode = GPIO_MODE_OUTPUT_PP; gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    for (uint8_t i = 0; i < 10; ++i) {
        HAL_GPIO_WritePin(motors_port[i], motors_pin[i], GPIO_PIN_RESET);
        gpio.Pin = motors_pin[i]; HAL_GPIO_Init(motors_port[i], &gpio);
    }
}

// 2. I2C: Мастер обратился к нам по адресу
void HAL_I2C_AddrCallback(I2C_HandleTypeDef *hi2c, uint8_t TransferDirection, uint16_t AddrMatchCode)
{
    if (hi2c->Instance == I2C1)
    {
        if (TransferDirection == I2C_DIRECTION_TRANSMIT)
        {
            // Мастер хочет записать полную команду v2.
            if (HAL_I2C_Slave_Seq_Receive_IT(hi2c, rxBuffer, sizeof(rxBuffer),
                                             I2C_FIRST_AND_LAST_FRAME) != HAL_OK) {
                i2c_reinit_pending = 1U;
            }
        }
        else
        {
            // Мастер запрашивает атомарный диагностический snapshot.
            BuildMotorStatusFrame();
            if (maintenanceQuery) {
                maintenanceQuery = 0U;
                memset(tx_status_frame, 0, MOTOR_STATUS_FRAME_SIZE);
                tx_status_frame[0] = 0xB6U; tx_status_frame[1] = 1U;
                WireWrite32(tx_status_frame + 4, maintenanceCompletedToken);
                WireWrite32(tx_status_frame + 24, WireCrc32(tx_status_frame, 24));
            }
            if (HAL_I2C_Slave_Seq_Transmit_IT(hi2c, tx_status_frame,
                                              MOTOR_STATUS_FRAME_SIZE,
                                              I2C_FIRST_AND_LAST_FRAME) != HAL_OK) {
                i2c_reinit_pending = 1U;
            }
        }
    }
}

// 3. I2C: full v3 command received; validate integrity before publishing to main.
void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef *hi2c) {
    if(hi2c->Instance==I2C1)++i2c1_completed_transfers;
    if (hi2c->Instance == I2C1 && rxBuffer[0] == 0xA6U && rxBuffer[1] == 1U) {
        if (WireRead32(rxBuffer + 16) != WireCrc32(rxBuffer, 16)) return;
        uint32_t token = WireRead32(rxBuffer + 4);
        if (rxBuffer[2] == 2U) maintenanceQuery = 1U;
        else if (rxBuffer[2] == 3U && token && maintenanceRebootToken == token) {
            maintenanceExpiredToken = token; maintenanceRebootToken = 0U;
        }
        else if (rxBuffer[2] == 1U && token && token != maintenanceCompletedToken
                 && token != maintenanceExpiredToken
                 && (!maintenanceRebootToken || maintenanceRebootToken == token))
        {
            if (!maintenanceRebootToken) maintenanceRebootAt = HAL_GetTick();
            maintenanceRebootToken = token;
        }
        return;
    }
    if (maintenanceRebootToken) return;
    if (hi2c->Instance != I2C1 || rxBuffer[1] != MOTOR_STATUS_PROTOCOL_VERSION ||
        WireRead32(rxBuffer + 16) != WireCrc32(rxBuffer, 16)) return;
    if (rxBuffer[0] == 0xA6U && !pendingSensorEvent &&
        (rxBuffer[3] == MOTOR_SUCCESS || rxBuffer[3] == MOTOR_FAILED)) {
        /* Completion cannot start a motor. Main validates the entire active key. */
        pendingFinishBoot = WireRead32(rxBuffer + 4);
        pendingFinishOperation = WireRead32(rxBuffer + 8);
        pendingFinishLine = rxBuffer[2];
        pendingFinishStatus = rxBuffer[3];
        pendingSensorEvent = 1U;
    } else if (rxBuffer[0] == 0xA5U && !pendingMotorCommand && motorControl.status != MOTOR_BUSY) {
        pendingMotorLine = rxBuffer[2]; pendingMotorTurns = rxBuffer[3];
        pendingMotorBoot = WireRead32(rxBuffer + 4);
        pendingMotorOperation = WireRead32(rxBuffer + 8);
        pendingDirectOperation = WireRead32(rxBuffer + 12);
        pendingMotorCommand = 1U;
    }
}

void HAL_I2C_SlaveTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1) {
        i2c1_completed_transfers++;
    }
}

void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1)
    {
        if (HAL_I2C_EnableListen_IT(hi2c) != HAL_OK) {
            i2c_reinit_pending = 1;
        }
    }
}



// 5. Обработка ошибок I2C шины
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1)
    {
        i2c_last_error = HAL_I2C_GetError(hi2c);
        i2c_error_count++;
        i2c_reinit_pending = 1;
    }
}




/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
	/* User can add his own implementation to report the HAL error return state */
	__disable_irq();
	while (1) {
	}
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
