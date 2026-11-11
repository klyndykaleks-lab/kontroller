/*
 * keypad.c
 *
 *  Created on: Nov 15, 2025
 *      Author: maksim
 */

#include "keypad.h"
#include <stdint.h>
#include "main.h"

#define KEYPAD_SETTLE_CYCLES 500U

const uint8_t g_keymap[4][3] = {
    {8, 4, 11},  // Ряд 1 (KEYPAD_OUT1)
    {6, 5, 12},  // Ряд 2 (KEYPAD_OUT2)
    {1, 2, 10},  // Ряд 3 (KEYPAD_OUT3)
    {3, 7, 9} // Ряд 4 (KEYPAD_OUT4)
};


uint8_t ScanKeypad(GPIO_TypeDef** out_ports, uint16_t* out_pins,
                   GPIO_TypeDef** inp_ports, uint16_t* inp_pins)
{
    for(int i = 0; i < 4; ++i) {
        HAL_GPIO_WritePin(out_ports[i], out_pins[i], GPIO_PIN_RESET);
    }

    for (int row = 0; row < 4; row++) {

        HAL_GPIO_WritePin(out_ports[row], out_pins[row], GPIO_PIN_SET);

        for(volatile uint32_t i=0; i<KEYPAD_SETTLE_CYCLES; i++) { __NOP(); }

        for (int col = 0; col < 3; col++) {
            if (HAL_GPIO_ReadPin(inp_ports[col], inp_pins[col]) == GPIO_PIN_SET) {

                HAL_GPIO_WritePin(out_ports[row], out_pins[row], GPIO_PIN_RESET);

                return g_keymap[row][col];
            }
        }

        HAL_GPIO_WritePin(out_ports[row], out_pins[row], GPIO_PIN_RESET);
    }

    return NO_KEY_PRESSED;
}
