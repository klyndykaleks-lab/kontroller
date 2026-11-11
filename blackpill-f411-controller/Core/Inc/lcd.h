/*
 * lcd.h
 *
 *  Created on: Dec 29, 2025
 *      Author: maksim
 */

#ifndef INC_LCD_H_
#define INC_LCD_H_

#include "main.h"
#include "cmsis_os.h"
#include <stdio.h>
#include <string.h>
#include "configuration.h"

#define LCD_EN 0x04  // Enable bit
#define LCD_RW 0x02  // Read/Write bit
#define LCD_RS 0x01  // Register Select bit
#define LCD_BL 0x08  // Backlight bit

void LCD_WriteNibble(uint8_t nibble, uint8_t rs);
void LCD_SendInternal(uint8_t data, uint8_t flags);
void LCD_SendCommand(uint8_t cmd);
void LCD_SendData(uint8_t data);
void LCD_Init();
void LCD_Clear();
void LCD_SetCursor(uint8_t row, uint8_t col);
void LCD_Print(char *str);
HAL_StatusTypeDef I2C_MasterTransmitSafe(uint16_t devAddr, uint8_t *pData, uint16_t size, uint32_t timeout);
HAL_StatusTypeDef I2C_MasterReceiveSafe(uint16_t devAddr, uint8_t *pData, uint16_t size, uint32_t timeout);
uint8_t LCD_ReinitRequested(void);
void LCD_ClearReinitRequest(void);
#endif /* INC_LCD_H_ */
