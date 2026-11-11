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
#include "main_io.h"
#include "boot_identity.h"
#include "cmsis_os.h"
#include "fatfs.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "configuration.h" // <--- ОБЯЗАТЕЛЬНО
#include "ftasks.h"

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
I2C_HandleTypeDef hi2c2;

SPI_HandleTypeDef hspi3;

UART_HandleTypeDef huart1;

UART_HandleTypeDef huart2;

/* Definitions for manager */
osThreadId_t managerHandle;
const osThreadAttr_t manager_attributes = {
  .name = "manager",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};
/* Definitions for keypad */
osThreadId_t keypadHandle;
const osThreadAttr_t keypad_attributes = {
  .name = "keypad",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for mdb */
osThreadId_t mdbHandle;
const osThreadAttr_t mdb_attributes = {
  .name = "mdb",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};
/* Definitions for display */
osThreadId_t displayHandle;
const osThreadAttr_t display_attributes = {
  .name = "display",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityBelowNormal,
};
/* Definitions for logger */
osThreadId_t loggerHandle;
const osThreadAttr_t logger_attributes = {
  .name = "logger",
  .stack_size = 1024 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
/* Definitions for eventQueue */
osMessageQueueId_t eventQueueHandle;
const osMessageQueueAttr_t eventQueue_attributes = {
  .name = "eventQueue"
};
/* Definitions for logQueue */
osMessageQueueId_t logQueueHandle;
const osMessageQueueAttr_t logQueue_attributes = {
  .name = "logQueue"
};
/* Definitions for mdbCommandQueue */
osMessageQueueId_t mdbCommandQueueHandle;
const osMessageQueueAttr_t mdbCommandQueue_attributes = {
  .name = "mdbCommandQueue"
};
/* Definitions for displayQueue */
osMessageQueueId_t displayQueueHandle;
const osMessageQueueAttr_t displayQueue_attributes = {
  .name = "displayQueue"
};
/* Definitions for paymentTimer */
osTimerId_t paymentTimerHandle;
const osTimerAttr_t paymentTimer_attributes = {
  .name = "paymentTimer"
};
/* Definitions for vendingTimer */
osTimerId_t vendingTimerHandle;
const osTimerAttr_t vendingTimer_attributes = {
  .name = "vendingTimer"
};
/* Definitions for i2cMutex */
osMutexId_t i2cMutexHandle;
const osMutexAttr_t i2cMutex_attributes = {
  .name = "i2cMutex"
};
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C2_Init(void);
static void MX_SPI3_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
void managerTask(void *argument);
void keypadTask(void *argument);
void mdbTask(void *argument);
void displayTask(void *argument);
void loggerTask(void *argument);
void CallbackPaymentTimer(void *argument);
void CallbackVendingTimer(void *argument);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#define F411_RESET_PIN       (1U << 0)
#define F411_RESET_POWER     (1U << 1)
#define F411_RESET_SOFTWARE  (1U << 2)
#define F411_RESET_IWDG      (1U << 3)
#define F411_RESET_WWDG      (1U << 4)
#define F411_RESET_LOW_POWER (1U << 5)
#define F411_RESET_BROWNOUT  (1U << 6)
#define F411_IWDG_RELOAD 3250U
#define F411_MDB_HEARTBEAT_MAX_AGE_MS 5000U

static uint8_t f411_reset_flags = 0U;
static volatile uint32_t f411_mdb_last_heartbeat_tick = 0U;

static void F411_CaptureResetFlags(void)
{
  uint32_t csr = RCC->CSR;

  if ((csr & RCC_CSR_PINRSTF) != 0U) f411_reset_flags |= F411_RESET_PIN;
  if ((csr & RCC_CSR_PORRSTF) != 0U) f411_reset_flags |= F411_RESET_POWER;
  if ((csr & RCC_CSR_SFTRSTF) != 0U) f411_reset_flags |= F411_RESET_SOFTWARE;
  if ((csr & RCC_CSR_IWDGRSTF) != 0U) f411_reset_flags |= F411_RESET_IWDG;
  if ((csr & RCC_CSR_WWDGRSTF) != 0U) f411_reset_flags |= F411_RESET_WWDG;
  if ((csr & RCC_CSR_LPWRRSTF) != 0U) f411_reset_flags |= F411_RESET_LOW_POWER;
  if ((csr & RCC_CSR_BORRSTF) != 0U) f411_reset_flags |= F411_RESET_BROWNOUT;
  RCC->CSR |= RCC_CSR_RMVF;
}

/* STM32F411CE 512-KiB flash: reserve sectors 6 and 7 for boot generations.
 * Programming happens before RTOS/MDB starts, never in the transaction task. */
static uint32_t f411_boot_identity;
static uint32_t BootStorageAddress(unsigned page, uint32_t word) {
    return 0x08040000U + page * 0x20000U + word * 4U;
}
static uint32_t BootStorageRead(unsigned page, uint32_t word) {
    return *(volatile const uint32_t *)BootStorageAddress(page, word);
}
static bool BootStorageProgram(unsigned page, uint32_t word, uint32_t value) {
    HAL_StatusTypeDef status =
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, BootStorageAddress(page, word), value);
    FLASH_FlushCaches();
    return status == HAL_OK;
}
static bool BootStorageErase(unsigned page) {
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t error = 0;
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    erase.Sector = FLASH_SECTOR_6 + page;
    erase.NbSectors = 1U;
    return HAL_FLASHEx_Erase(&erase, &error) == HAL_OK;
}
static void F411_InitBootIdentity(void) {
    const BootIdentityStorage storage = {16384U, BootStorageRead, BootStorageProgram,
                                         BootStorageErase};
    if (HAL_FLASH_Unlock() == HAL_OK) {
        f411_boot_identity = BootIdentityReserve(&storage);
    }
    HAL_FLASH_Lock();
}
uint32_t F411_GetBootIdentity(void) {
    return f411_boot_identity;
}

static void F411_WatchdogInit(void) {
    IWDG->KR = 0xCCCCU;
    IWDG->KR = 0x5555U;
    IWDG->PR = 6U; /* LSI / 256. */
    IWDG->RLR = F411_IWDG_RELOAD;
    while (IWDG->SR != 0U) {
    }
    IWDG->KR = 0xAAAAU;
}

void F411_MdbHeartbeat(void)
{
  f411_mdb_last_heartbeat_tick = HAL_GetTick();
}

void F411_WatchdogServiceFromManager(void)
{
  uint32_t now = HAL_GetTick();

  if (f411_mdb_last_heartbeat_tick != 0U
      && (now - f411_mdb_last_heartbeat_tick)
          <= F411_MDB_HEARTBEAT_MAX_AGE_MS) {
    IWDG->KR = 0xAAAAU;
  }
}

uint8_t F411_GetResetFlags(void)
{
  return f411_reset_flags;
}

const char *F411_GetResetReason(void)
{
  if ((f411_reset_flags & F411_RESET_IWDG) != 0U) return "watchdog";
  if ((f411_reset_flags & F411_RESET_WWDG) != 0U) return "windowWatchdog";
  if ((f411_reset_flags & F411_RESET_POWER) != 0U) return "powerOn";
  if ((f411_reset_flags & F411_RESET_BROWNOUT) != 0U) return "brownout";
  if ((f411_reset_flags & F411_RESET_SOFTWARE) != 0U) return "software";
  if ((f411_reset_flags & F411_RESET_PIN) != 0U) return "resetPin";
  if ((f411_reset_flags & F411_RESET_LOW_POWER) != 0U) return "lowPower";
  return "unknown";
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

  F411_CaptureResetFlags();

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MainIoInit();
  MX_I2C2_Init();
  MX_SPI3_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_FATFS_Init();
  /* USER CODE BEGIN 2 */
  F411_InitBootIdentity();
  F411_WatchdogInit();
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();
  /* Create the mutex(es) */
  /* creation of i2cMutex */
  i2cMutexHandle = osMutexNew(&i2cMutex_attributes);

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* Create the timer(s) */
  /* creation of paymentTimer */
  paymentTimerHandle = osTimerNew(CallbackPaymentTimer, osTimerOnce, NULL, &paymentTimer_attributes);

  /* creation of vendingTimer */
  vendingTimerHandle = osTimerNew(CallbackVendingTimer, osTimerOnce, NULL, &vendingTimer_attributes);

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* creation of eventQueue */
  eventQueueHandle = osMessageQueueNew (32, sizeof(event_t), &eventQueue_attributes);

  /* creation of logQueue */
  logQueueHandle = osMessageQueueNew (4, sizeof(StorageCommand_t), &logQueue_attributes);

  /* creation of mdbCommandQueue */
  mdbCommandQueueHandle = osMessageQueueNew (16, sizeof(mdbCommand_t), &mdbCommandQueue_attributes);

  /* creation of displayQueue */
  displayQueueHandle = osMessageQueueNew (16, sizeof(DisplayCommand_t), &displayQueue_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of manager */
  managerHandle = osThreadNew(managerTask, NULL, &manager_attributes);

  /* creation of keypad */
  keypadHandle = osThreadNew(keypadTask, NULL, &keypad_attributes);

  /* creation of mdb */
  mdbHandle = osThreadNew(mdbTask, NULL, &mdb_attributes);

  /* creation of display */
  displayHandle = osThreadNew(displayTask, NULL, &display_attributes);

  /* creation of logger */
  loggerHandle = osThreadNew(loggerTask, NULL, &logger_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief I2C2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C2_Init(void)
{

  /* USER CODE BEGIN I2C2_Init 0 */

  /* USER CODE END I2C2_Init 0 */

  /* USER CODE BEGIN I2C2_Init 1 */

  /* USER CODE END I2C2_Init 1 */
  hi2c2.Instance = I2C2;
  hi2c2.Init.ClockSpeed = 10000;
  hi2c2.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c2.Init.OwnAddress1 = 0;
  hi2c2.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c2.Init.OwnAddress2 = 0;
  hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c2.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C2_Init 2 */

  /* USER CODE END I2C2_Init 2 */

}

/**
  * @brief SPI3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI3_Init(void)
{

  /* USER CODE BEGIN SPI3_Init 0 */

  /* USER CODE END SPI3_Init 0 */

  /* USER CODE BEGIN SPI3_Init 1 */

  /* USER CODE END SPI3_Init 1 */
  /* SPI3 parameter configuration*/
  hspi3.Instance = SPI3;
  hspi3.Init.Mode = SPI_MODE_MASTER;
  hspi3.Init.Direction = SPI_DIRECTION_2LINES;
  hspi3.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi3.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi3.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi3.Init.NSS = SPI_NSS_SOFT;
  hspi3.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256;
  hspi3.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi3.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi3.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi3.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI3_Init 2 */

  /* USER CODE END SPI3_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 9600;
  huart2.Init.WordLength = UART_WORDLENGTH_9B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef gpio = {0};
  __HAL_RCC_GPIOC_CLK_ENABLE(); __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE();
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, cs_Pin, GPIO_PIN_SET);
  gpio.Pin = GPIO_PIN_0; gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL; gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &gpio);
  gpio.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
  gpio.Mode = GPIO_MODE_INPUT; gpio.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &gpio);
  gpio.Pin = cs_Pin; gpio.Mode = GPIO_MODE_OUTPUT_PP; gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH; HAL_GPIO_Init(GPIOB, &gpio);
  gpio.Pin = GPIO_PIN_15; gpio.Mode = GPIO_MODE_IT_FALLING; gpio.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &gpio);
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 6, 0); HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/* USER CODE BEGIN Header_managerTask */
/**
  * @brief  Function implementing the manager thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_managerTask */
__weak void managerTask(void *argument)
{
  /* init code for USB_DEVICE */
  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN 5 */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END 5 */
}

/* USER CODE BEGIN Header_keypadTask */
/**
* @brief Function implementing the keypad thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_keypadTask */
__weak void keypadTask(void *argument)
{
  /* USER CODE BEGIN keypadTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END keypadTask */
}

/* USER CODE BEGIN Header_mdbTask */
/**
* @brief Function implementing the mdb thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_mdbTask */
__weak void mdbTask(void *argument)
{
  /* USER CODE BEGIN mdbTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END mdbTask */
}

/* USER CODE BEGIN Header_displayTask */
/**
* @brief Function implementing the display thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_displayTask */
__weak void displayTask(void *argument)
{
  /* USER CODE BEGIN displayTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END displayTask */
}

/* USER CODE BEGIN Header_loggerTask */
/**
* @brief Function implementing the logger thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_loggerTask */
__weak void loggerTask(void *argument)
{
  /* USER CODE BEGIN loggerTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END loggerTask */
}

/* CallbackPaymentTimer function */
void CallbackPaymentTimer(void *argument)
{
  /* USER CODE BEGIN CallbackPaymentTimer */
	event_t event = { 0 };
		  event.source = SRC_TIMER;
		  event.payload.timer_id = TIMER_ID_PAYMENT;
		  QueueManagerEvent(&event);
  /* USER CODE END CallbackPaymentTimer */
}

/* CallbackVendingTimer function */
void CallbackVendingTimer(void *argument)
{
  /* USER CODE BEGIN CallbackVendingTimer */
	event_t event = { 0 };
			  event.source = SRC_TIMER;
			  event.payload.timer_id = TIMER_ID_VENDING;
			  QueueManagerEvent(&event);


  /* USER CODE END CallbackVendingTimer */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM11 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM11)
  {
    HAL_IncTick();
    MainIoTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
	  //HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
	        for(volatile int i = 0; i < 1000000; i++) {}
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
