/*
 * lcd.c
 *
 *  Created on: Dec 29, 2025
 *      Author: maksim
 */


#include "lcd.h"
#include "cmsis_os.h"
#include "configuration.h"

extern osMutexId_t i2cMutexHandle;

extern I2C_HandleTypeDef DISPLAY_I2C_HANDLE;

#define LCD_I2C_TX_TIMEOUT_MS 100U
#define I2C_RECOVERY_PULSES   9U
#define I2C_SCL_PORT          GPIOB
#define I2C_SCL_PIN           GPIO_PIN_10
#define I2C_SDA_PORT          GPIOB
#define I2C_SDA_PIN           GPIO_PIN_3

static volatile uint8_t lcd_reinit_request = 0;

static HAL_StatusTypeDef LCD_TransmitRaw(uint8_t *pData, uint16_t size) {
	HAL_StatusTypeDef status = HAL_I2C_Master_Transmit(&DISPLAY_I2C_HANDLE,
			LCD_ADDR, pData, size, LCD_I2C_TX_TIMEOUT_MS);

	if (status != HAL_OK) {
		lcd_reinit_request = 1;
	}

	return status;
}

static void I2C_RecoveryDelay(void) {
	for (volatile uint32_t i = 0; i < 500; i++) {
		__NOP();
	}
}

static void I2C_RecoverBus(void) {
	GPIO_InitTypeDef GPIO_InitStruct = { 0 };
	I2C_HandleTypeDef *hi2c = &DISPLAY_I2C_HANDLE;

	HAL_I2C_DeInit(hi2c);

	__HAL_RCC_GPIOB_CLK_ENABLE();

	HAL_GPIO_WritePin(I2C_SCL_PORT, I2C_SCL_PIN, GPIO_PIN_SET);
	HAL_GPIO_WritePin(I2C_SDA_PORT, I2C_SDA_PIN, GPIO_PIN_SET);

	GPIO_InitStruct.Pin = I2C_SCL_PIN | I2C_SDA_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

	I2C_RecoveryDelay();

	for (uint8_t i = 0; i < I2C_RECOVERY_PULSES; i++) {
		if (HAL_GPIO_ReadPin(I2C_SDA_PORT, I2C_SDA_PIN) == GPIO_PIN_SET) {
			break;
		}

		HAL_GPIO_WritePin(I2C_SCL_PORT, I2C_SCL_PIN, GPIO_PIN_RESET);
		I2C_RecoveryDelay();
		HAL_GPIO_WritePin(I2C_SCL_PORT, I2C_SCL_PIN, GPIO_PIN_SET);
		I2C_RecoveryDelay();
	}

	HAL_GPIO_WritePin(I2C_SDA_PORT, I2C_SDA_PIN, GPIO_PIN_RESET);
	I2C_RecoveryDelay();
	HAL_GPIO_WritePin(I2C_SCL_PORT, I2C_SCL_PIN, GPIO_PIN_SET);
	I2C_RecoveryDelay();
	HAL_GPIO_WritePin(I2C_SDA_PORT, I2C_SDA_PIN, GPIO_PIN_SET);
	I2C_RecoveryDelay();

	__HAL_RCC_I2C2_FORCE_RESET();
	I2C_RecoveryDelay();
	__HAL_RCC_I2C2_RELEASE_RESET();

	hi2c->State = HAL_I2C_STATE_RESET;
	HAL_I2C_Init(hi2c);
}

HAL_StatusTypeDef I2C_MasterTransmitSafe(uint16_t devAddr, uint8_t *pData,
		uint16_t size, uint32_t timeout) {
	HAL_StatusTypeDef status;
	I2C_HandleTypeDef *hi2c = &DISPLAY_I2C_HANDLE;

	status = HAL_I2C_Master_Transmit(hi2c, devAddr, pData, size, timeout);
	if (status == HAL_OK) {
		return status;
	}

	I2C_RecoverBus();
	status = HAL_I2C_Master_Transmit(hi2c, devAddr, pData, size, timeout);

	if (status != HAL_OK) {
		lcd_reinit_request = 1;
	}

	return status;
}

HAL_StatusTypeDef I2C_MasterReceiveSafe(uint16_t devAddr, uint8_t *pData,
		uint16_t size, uint32_t timeout) {
	HAL_StatusTypeDef status;
	I2C_HandleTypeDef *hi2c = &DISPLAY_I2C_HANDLE;

	status = HAL_I2C_Master_Receive(hi2c, devAddr, pData, size, timeout);
	if (status == HAL_OK) {
		return status;
	}

	I2C_RecoverBus();
	status = HAL_I2C_Master_Receive(hi2c, devAddr, pData, size, timeout);

	if (status != HAL_OK) {
		lcd_reinit_request = 1;
	}

	return status;
}

uint8_t LCD_ReinitRequested(void) {
	return lcd_reinit_request;
}

void LCD_ClearReinitRequest(void) {
	lcd_reinit_request = 0;
}

// --- Custom Characters (Битовые маски 5x8) ---
// Мы можем создать максимум 8 символов (адреса 0-7)

// --- Custom Characters (Битовые маски 5x8) ---
// Память дисплея (CGRAM) вмещает только 8 символов (адреса 0-7)

const uint8_t CC_B[8]  = {0x1F, 0x10, 0x10, 0x1F, 0x11, 0x11, 0x1F, 0x00}; // 0: Б
const uint8_t CC_TS[8] = {0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x1F, 0x01}; // 1: Ц (с хвостиком)
const uint8_t CC_D[8]  = {0x0F, 0x05, 0x05, 0x09, 0x11, 0x1F, 0x11, 0x00}; // 2: Д
const uint8_t CC_I[8]  = {0x11, 0x11, 0x11, 0x13, 0x15, 0x19, 0x11, 0x00}; // 3: И
const uint8_t CC_L[8]  = {0x0F, 0x05, 0x05, 0x05, 0x05, 0x09, 0x11, 0x00}; // 4: Л
const uint8_t CC_P[8]  = {0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x00}; // 5: П
const uint8_t CC_Y[8]  = {0x11, 0x11, 0x11, 0x1D, 0x15, 0x15, 0x1D, 0x00}; // 6: Ы
const uint8_t CC_SH[8] = {0x15, 0x15, 0x15, 0x15, 0x15, 0x15, 0x1F, 0x00}; // 7: Ш
// Примечание: Для Ц, Щ, Й мест не хватило, используем замены в таблице ниже.

const uint8_t Font_Cyrillic[] = {
    // А, Б, В, Г, Д, Е, Ж, З, И, Й, К, Л, М, Н, О, П
    'A', 0x00, 'B', 'r', 0x02, 'E', 0xA6, '3',
    0x03, 0x03, 'K', 0x04, 'M', 'H', 'O', 0x05,

    // Р, С, Т, У, Ф, Х, Ц, Ч, Ш, Щ, Ъ, Ы, Ь, Э, Ю, Я
    'P', 'C', 'T', 'Y', 0xE4, 'X', 0x01, '4',
    0x07, 0x07, 'b', 0x06, 'b', 0x05, 0xB1, 0x52,

    // строчные (дублируем): а, б, в, г...
    'a', 0x00, 'B', 'r', 0x02, 'e', 0xB6, 'z',
    0x03, 0x03, 'k', 0x04, 'm', 'H', 'o', 0x05,

    // р, с, т, у, ф, х, ц, ч...
    'p', 'c', 'T', 'y', 0xE4, 'x', 0x01, '4',
    0x07, 0x07, 'b', 0x06, 'b', 0x05, 0xB1, 0x52
};

void LCD_LoadCustomChar(uint8_t loc, const uint8_t *pattern) {
    if (loc > 7) return;
    LCD_SendCommand(0x40 + (loc * 8)); // Адрес CGRAM
    for(int i=0; i<8; i++) {
        LCD_SendData(pattern[i]);
    }
}

void LCD_SendInternal(uint8_t data, uint8_t flags) {
	uint8_t up = data & 0xF0;
	uint8_t lo = (data << 4) & 0xF0;

	uint8_t data_arr[4];
	data_arr[0] = up | flags | LCD_BL | LCD_EN;
	data_arr[1] = up | flags | LCD_BL;          // Enable pulsing low
	data_arr[2] = lo | flags | LCD_BL | LCD_EN;
	data_arr[3] = lo | flags | LCD_BL;

	LCD_TransmitRaw(data_arr, 4);
}

void LCD_SendCommand(uint8_t cmd) {
	LCD_SendInternal(cmd, 0);
}

void LCD_SendData(uint8_t data) {
	LCD_SendInternal(data, LCD_RS);
}

void LCD_WriteNibble(uint8_t nibble, uint8_t rs) {
    uint8_t data = (nibble << 4) & 0xF0; // Сдвигаем данные в старшие 4 бита

    uint8_t data_arr[2];
    // Формируем пакет: Подсветка + RS + Данные + Enable Pulse
    // Бит Enable (LCD_EN) должен "дернуться" 1 -> 0

    // Шаг 1: Устанавливаем данные и Enable = 1
    data_arr[0] = data | rs | LCD_BL | LCD_EN;

    // Шаг 2: Enable = 0 (защелкиваем данные)
    data_arr[1] = data | rs | LCD_BL;

	LCD_TransmitRaw(data_arr, 2);
}
#if 0
void LCD_Init() {
	// Инициализация по даташиту HD44780 для 4-битного режима
	osDelay(50);
	LCD_SendCommand(0x30);
	osDelay(5);
	LCD_SendCommand(0x30);
	osDelay(1);
	LCD_SendCommand(0x30);
	osDelay(10);
	LCD_SendCommand(0x20); // 4-bit mode
	osDelay(10);

	// Настройка дисплея
	LCD_SendCommand(0x28); // Function set: DL=0 (4bit), N=1 (2 lines), F=0 (5x8)
	LCD_SendCommand(0x08); // Display off
	LCD_SendCommand(0x01); // Clear display
	osDelay(2);
	LCD_SendCommand(0x06); // Entry mode set
	LCD_SendCommand(0x0C); // Display on, cursor off, blink off
}
# endif

void LCD_Init() {
    // 1. Ждем стабилизации питания (>15ms по даташиту)
    osDelay(150);

    // 2. Магическая последовательность сброса (ОТПРАВЛЯЕМ ТОЛЬКО ПОЛУБАЙТЫ!)
    // Внимание: мы шлем 0x03, а не 0x30, так как функция WriteNibble сама делает сдвиг << 4

    LCD_WriteNibble(0x03, 0); // 1-й Reset
    osDelay(5);               // Ждем >4.1ms

    LCD_WriteNibble(0x03, 0); // 2-й Reset
    osDelay(5);               // Ждем >100us

    LCD_WriteNibble(0x03, 0); // 3-й Reset
    osDelay(5);

    LCD_WriteNibble(0x02, 0); // 4. Переход в 4-битный режим!
    osDelay(5);

    // --- С этого момента дисплей в 4-битном режиме, можно использовать SendCommand ---

    // Настройка дисплея
    LCD_SendCommand(0x28); // 4-bit, 2 lines, 5x8 font
    //LCD_SendCommand(0x20);
    osDelay(5);

    LCD_SendCommand(0x08); // Display off
    osDelay(5);

    LCD_SendCommand(0x01); // Clear display (долгая команда!)
    osDelay(5);            // Обязательно >1.5ms

    LCD_SendCommand(0x06); // Entry mode set (cursor increments)
    osDelay(5);

    LCD_SendCommand(0x0C); // Display on, cursor off
    osDelay(5);

    LCD_LoadCustomChar(0, CC_B);  // Б
    osDelay(5);
    LCD_LoadCustomChar(1, CC_TS); // 1: Ц  // Г
        osDelay(5);

        LCD_LoadCustomChar(2, CC_D);  // Д
        osDelay(5);
        LCD_LoadCustomChar(3, CC_I);  // И
        osDelay(5);
        LCD_LoadCustomChar(4, CC_L);  // Л
        osDelay(5);
        LCD_LoadCustomChar(5, CC_P);  // П
        osDelay(5);
        LCD_LoadCustomChar(6, CC_Y);  // Ы
        osDelay(5);
        LCD_LoadCustomChar(7, CC_SH); // Ш
        osDelay(5);

        LCD_SendCommand(0x80);
    osDelay(5);
}



void LCD_Clear() {
	LCD_SendCommand(0x01);
	osDelay(2);
}

void LCD_SetCursor(uint8_t row, uint8_t col) {
	uint8_t address = (row == 0) ? 0x80 : 0xC0;
	address += col;
	LCD_SendCommand(address);
}

#if 0
void LCD_Print(char *str) {
	while (*str) LCD_SendData((uint8_t)(*str++));
}
#endif

#if 0
void LCD_Print(char *str) {
    uint8_t i = 0;
    uint8_t buff_char;

    while(str[i] != 0) {
        // Проверка на кириллицу UTF-8 (первый байт >= 0xC0)
        if ((uint8_t)str[i] >= 0xC0) {
            switch ((uint8_t)str[i]) {
                case 0xD0: { // Основной диапазон А-Я, а-п
                    i++; // Берем следующий байт
                    if ((uint8_t)str[i] >= 0x90 && (uint8_t)str[i] <= 0xBF) {
                        buff_char = str[i] + 0x30; // Сдвиг индекса для А-Я, а-п
                    }
                    else if ((uint8_t)str[i] == 0x81) { // Ё
                        buff_char = 0xA8; // Внутренний код для Ё (в таблице Font_Cyrillic индекс 64, но логика либы хитрее)
                        // Упрощение: используем прямой доступ к массиву
                        LCD_SendData(Font_Cyrillic[64]);
                        i++; continue;
                    }
                    break;
                }
                case 0xD1: { // Диапазон р-я, ё
                    i++;
                    if ((uint8_t)str[i] >= 0x80 && (uint8_t)str[i] <= 0x8F) {
                        buff_char = str[i] + 0x70; // Сдвиг для р-я
                    }
                    else if ((uint8_t)str[i] == 0x91) { // ё
                        LCD_SendData(Font_Cyrillic[65]);
                        i++; continue;
                    }
                    break;
                }
            }

            // Вывод символа из таблицы Font_Cyrillic
            // Базовая логика смещения из библиотеки:
            // Символы А..Я (192..223) -> Индексы 0..31
            // Символы а..п (224..239) -> Индексы 32..47
            // Символы р..я (240..255) -> Индексы 48..63

            if (buff_char >= 192) {
                LCD_SendData(Font_Cyrillic[buff_char - 192]);
            } else {
                // Если декодирование не удалось, шлем как есть
                LCD_SendData('?');
            }
        }
        else {
            // Обычная латиница
            LCD_SendData((uint8_t)str[i]);
        }
        i++;
    }
}
#endif

void LCD_Print(char *str) {
    uint8_t i = 0;
    uint8_t buff_char = 0; // <--- ВАЖНО: Обязательная инициализация нулем

    while(str[i] != 0) {
        // Принудительно приводим к беззнаковому типу для корректных сравнений
        uint8_t current_byte = (uint8_t)str[i];

        // Проверка на кириллицу UTF-8 (первый байт >= 0xC0)
        if (current_byte >= 0xC0) {
            uint8_t is_cyrillic = 0;

            switch (current_byte) {
                case 0xD0: { // Основной диапазон А-Я, а-п
                    i++; // Берем следующий байт
                    uint8_t next_byte = (uint8_t)str[i];

                    if (next_byte >= 0x90 && next_byte <= 0xBF) {
                        // Используем (uint8_t) для корректной арифметики без знака
                        buff_char = next_byte + 0x30;
                        is_cyrillic = 1;
                    }
                    else if (next_byte == 0x81) { // Ё (D0 81)
                        LCD_SendData(0xA2); // Символ Ё из таблицы LCD (или Font_Cyrillic[64])
                        i++; // Пропускаем байт, так как мы его уже обработали
                        continue; // Идем к следующей итерации while
                    }
                    break;
                }
                case 0xD1: { // Диапазон р-я, ё
                    i++;
                    uint8_t next_byte = (uint8_t)str[i];

                    if (next_byte >= 0x80 && next_byte <= 0x8F) {
                        buff_char = next_byte + 0x70;
                        is_cyrillic = 1;
                    }
                    else if (next_byte == 0x91) { // ё (D1 91)
                        LCD_SendData(0xB5); // Символ ё из таблицы LCD
                        i++;
                        continue;
                    }
                    break;
                }
                default:
                    // Если сюда попали, значит это байт >= 0xC0, но не D0/D1.
                    // Возможно, файл сохранен в CP1251, а не UTF-8.
                    // В таком случае можно попробовать вывести его "как есть" или заменить на '?'
                    break;
            }

            if (is_cyrillic) {
                // Защита от выхода за границы массива
                // 192 (0xC0) - начало таблицы CP1251
                if (buff_char >= 192) {
                    uint8_t index = buff_char - 192;
                    // В массиве Font_Cyrillic около 72 элементов (проверьте размер массива)
                    if (index < sizeof(Font_Cyrillic)) {
                         LCD_SendData(Font_Cyrillic[index]);
                    } else {
                         LCD_SendData('?'); // Ошибка индекса
                    }
                }
            }
            // Если это не UTF-8 последовательность кириллицы, но байт > 0xC0,
            // просто игнорируем или выводим '?' чтобы не ломать дисплей
        }
        else {
            // Обычная латиница
            LCD_SendData(current_byte);
        }
        i++;
    }
}
