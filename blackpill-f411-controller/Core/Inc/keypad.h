/*
 * keypad.h
 *
 *  Created on: Nov 15, 2025
 *      Author: maksim
 */

#ifndef INC_KEYPAD_H_
#define INC_KEYPAD_H_

#define NO_KEY_PRESSED 32
#include "main.h"

uint8_t ScanKeypad(GPIO_TypeDef** out_ports, uint16_t* out_pins, GPIO_TypeDef** inp_ports, uint16_t* inp_pins);

#endif /* INC_KEYPAD_H_ */
