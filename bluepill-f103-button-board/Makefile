# Standalone fourth target; share the repository-pinned HAL and startup only.
MOTOR := ../bluepill-f103-motor-board
HAL := $(MOTOR)/Drivers/STM32F1xx_HAL_Driver
CMSIS := $(MOTOR)/Drivers/CMSIS
CC := arm-none-eabi-gcc
CFLAGS := -mcpu=cortex-m3 -mthumb -mfloat-abi=soft -std=c11 -Os -g3 -Wall -Wextra -DUSE_HAL_DRIVER -DSTM32F103xB -ffunction-sections -fdata-sections
INCLUDES := -ICore/Inc -I$(HAL)/Inc -I$(HAL)/Inc/Legacy -I$(CMSIS)/Include -I$(CMSIS)/Device/ST/STM32F1xx/Include
SOURCES := Core/Src/main.c $(MOTOR)/Core/Src/system_stm32f1xx.c $(addprefix $(HAL)/Src/,stm32f1xx_hal.c stm32f1xx_hal_cortex.c stm32f1xx_hal_gpio.c stm32f1xx_hal_i2c.c stm32f1xx_hal_dma.c stm32f1xx_hal_rcc.c stm32f1xx_hal_rcc_ex.c stm32f1xx_hal_flash.c stm32f1xx_hal_flash_ex.c stm32f1xx_hal_pwr.c)
STARTUP := $(MOTOR)/Core/Startup/startup_stm32f103c8tx.s
.PHONY: all clean
all: build/Button_board.bin build/Button_board.hex
build/Button_board.elf: $(SOURCES) $(STARTUP) $(wildcard Core/Inc/*.h) ../common/wire_codec.h Makefile
	mkdir -p build
	$(CC) $(CFLAGS) $(INCLUDES) $(SOURCES) $(STARTUP) -T$(MOTOR)/STM32F103C8TX_FLASH.ld --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections,-Map=build/Button_board.map -o $@
	arm-none-eabi-size $@
build/Button_board.bin: build/Button_board.elf
	arm-none-eabi-objcopy -O binary $< $@
build/Button_board.hex: build/Button_board.elf
	arm-none-eabi-objcopy -O ihex $< $@
clean:
	rm -rf build
