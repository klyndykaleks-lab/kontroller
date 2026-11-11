/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define m_1_Pin GPIO_PIN_5
#define m_1_GPIO_Port GPIOA
#define m_2_Pin GPIO_PIN_6
#define m_2_GPIO_Port GPIOA
#define m_3_Pin GPIO_PIN_3
#define m_3_GPIO_Port GPIOA
#define m_4_Pin GPIO_PIN_4
#define m_4_GPIO_Port GPIOA
#define m_5_Pin GPIO_PIN_1
#define m_5_GPIO_Port GPIOA
#define m_6_Pin GPIO_PIN_2
#define m_6_GPIO_Port GPIOA
#define m_7_Pin GPIO_PIN_15
#define m_7_GPIO_Port GPIOC
#define m_8_Pin GPIO_PIN_0
#define m_8_GPIO_Port GPIOA
#define m_9_Pin GPIO_PIN_13
#define m_9_GPIO_Port GPIOC
#define m_10_Pin GPIO_PIN_14
#define m_10_GPIO_Port GPIOC
#define adr_2_Pin GPIO_PIN_0
#define adr_2_GPIO_Port GPIOB
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
