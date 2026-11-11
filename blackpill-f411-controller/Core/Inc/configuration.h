/*
 * configuration.h
 *
 *  Created on: Nov 14, 2025
 *      Author: maksim
 */

#ifndef INC_CONFIGURATION_H_
#define INC_CONFIGURATION_H_

#include "stm32f4xx_hal.h"

#define MAX_ITEMS_COUNT     9      // Количество ячеек с товарами
#define KEY_CODE_ENTER      11      // Кнопка входа в настройки / переключения полей
#define KEY_CODE_NEXT       12      // Кнопка сохранения / следующего товара

extern I2C_HandleTypeDef hi2c2;


//display
#define DISPLAY_I2C_HANDLE hi2c2
#define DISPLAY_I2C_ADDR 0x4E

//keyboard
#define KEYPAD_OUT1_PORT          GPIOA
#define KEYPAD_OUT1_PIN           GPIO_PIN_4
#define KEYPAD_OUT2_PORT          GPIOA
#define KEYPAD_OUT2_PIN           GPIO_PIN_5
#define KEYPAD_OUT3_PORT          GPIOA
#define KEYPAD_OUT3_PIN           GPIO_PIN_6
#define KEYPAD_OUT4_PORT          GPIOA
#define KEYPAD_OUT4_PIN           GPIO_PIN_7

#define KEYPAD_INP1_PORT          GPIOB
#define KEYPAD_INP1_PIN           GPIO_PIN_2
#define KEYPAD_INP2_PORT          GPIOB
#define KEYPAD_INP2_PIN           GPIO_PIN_1
#define KEYPAD_INP3_PORT          GPIOB
#define KEYPAD_INP3_PIN           GPIO_PIN_0

//mdb
#define MDB_UART_HANDLE           huart2

//sensors
#define DHT11_1WIRE_PORT          GPIOB
#define DHT11_1WIRE_PIN           GPIO_PIN_14

//motors
#define MOTOR1_DETECT_PORT        GPIOB
#define MOTOR1_DETECT_PIN         GPIO_PIN_6
#define MOTOR2_DETECT_PORT        GPIOA
#define MOTOR2_DETECT_PIN         GPIO_PIN_15
#define MOTOR3_DETECT_PORT        GPIOA
#define MOTOR3_DETECT_PIN         GPIO_PIN_1
#define MOTOR4_DETECT_PORT        GPIOA
#define MOTOR4_DETECT_PIN         GPIO_PIN_0
#define MOTOR5_DETECT_PORT        GPIOC
#define MOTOR5_DETECT_PIN         GPIO_PIN_15
#define MOTOR6_DETECT_PORT        GPIOC
#define MOTOR6_DETECT_PIN         GPIO_PIN_14
#define MOTOR7_DETECT_PORT        GPIOC
#define MOTOR7_DETECT_PIN         GPIO_PIN_13
#define MOTOR8_DETECT_PORT        GPIOB
#define MOTOR8_DETECT_PIN         GPIO_PIN_9
#define MOTOR9_DETECT_PORT        GPIOB
#define MOTOR9_DETECT_PIN         GPIO_PIN_8
#define MOTOR10_DETECT_PORT       GPIOB
#define MOTOR10_DETECT_PIN        GPIO_PIN_7

#define EXPANDER1_I2C_HANDLE      hi2c2
#define EXPANDER1_I2C_ADDR        ()
#define EXPANDER2_I2C_HANDLE      hi2c2
#define EXPANDER2_I2C_ADDR        ()

//sd
#define SD_SPI_HANDLE             hspi3
#define SD_SPI_CS_PORT            GPIOB
#define SD_SPI_CS_PIN             GPIO_PIN_13

typedef struct {
    uint8_t price_x; // Десятки рублей
    uint8_t price_y; // Единицы рублей
    uint8_t price_z; // Десятки копеек
    uint8_t price_p; // Единицы копеек
    uint16_t selection_id; // ID товара кофемашины, который запускает мотор
} ItemConfig_t;

/*
 * Глобальный конфиг, который мы будем писать/читать с SD карты.
 * Это то, что хранит ManagerTask у себя.
 */
typedef struct {
    ItemConfig_t items[10];
} VendingConfig_t;

//lcd
#define LCD_ADDR (0x27 << 1)

typedef enum {
	SCENE_IDLE,         // Ожидание ("Выберите товар")
	SCENE_PAYMENT,      // Оплата ("Оплатите на терминале")
	SCENE_VEND,         // Выдача ("Заберите товар")
	SCENE_SETTINGS,      // Настройки
	SCENE_STATUS,        // Диагностика этапов загрузки
	SCENE_ERROR
} SceneType_t;

typedef struct {

	uint8_t x; // десятки рублей
	uint8_t y; // рубли
	uint8_t z; // десятки копеек
	uint8_t p; // копейки
	uint16_t q; // ID товара кофемашины
	uint8_t editIndex; // Какую цифру или поле мы сейчас редактируем (0-пусто, 1-цена, 2-обороты)
} SettingsData_t;

typedef struct {
	char line1[17];
	char line2[17];
} StatusData_t;

typedef struct {
	SceneType_t type;     // Какую сцену рисовать
	union {               // Данные, специфичные для сцены
		SettingsData_t settings;
		StatusData_t status;
	} data;
} DisplayCommand_t;

//event queue

typedef struct {
    uint8_t cmd;
    uint8_t arg1;
    uint8_t arg2;
    uint32_t value;
} mdbCommand_t;

	typedef enum {
		SRC_KEYPAD,
		SRC_MDB,
		SRC_USB,
		SRC_TIMER,   // Для таймаутов (5 сек выдача, 10 сек оплата)
		SRC_MOTOR,   // Терминальный результат F103, полученный по I2C
		SRC_SYSTEM,   // Например, конфиг загружен
		SRC_SD
	} EventSource_t;

	typedef enum {
		MDB_EVT_READY = 0x99,
		MDB_EVT_SELECTION_REQUEST = 1,
		MDB_EVT_RESET,
		MDB_EVT_MACHINE_VEND_SUCCESS,
		MDB_EVT_MACHINE_VEND_FAILURE,
		MDB_EVT_MACHINE_SESSION_COMPLETE,
		MDB_EVT_MACHINE_CANCELLED
	} MdbEventCode_t;

		typedef enum {
			USB_EVT_PAYMENT_APPROVED = 1,
			USB_EVT_PAYMENT_DENIED,
			USB_EVT_PAYMENT_TIMEOUT,
			USB_EVT_SET_MAP,
			USB_EVT_DELIVERY_REQUEST,
            USB_EVT_DELIVERY_QUERY,
			USB_EVT_DELIVERY_LINE_CHECK
		} UsbEventCode_t;

	typedef struct {
		uint8_t code;
		uint16_t item_id;
		uint16_t price_minor;
		uint32_t transaction_id;
	} MdbEventData_t;

	typedef struct {
		uint8_t code;
		uint8_t motor_index;
		uint16_t item_id;
		uint32_t transaction_id;
	} UsbEventData_t;

	typedef struct {
	    EventSource_t source;
	    union {
	        uint8_t key_code;      // 1-12 от Keypad
	        uint8_t motor_status;  // STATUS_SUCCESS/STATUS_FAIL из snapshot F103
	        uint8_t timer_id;      // Какой RTOS-таймер сработал
	        MdbEventData_t mdb;
	        UsbEventData_t usb;
	    } payload;
	} event_t;

// log
	typedef struct {
	    uint8_t eventId;      // Уникальный ID события
	    uint8_t payload[4];   // 4 байта данных (зависит от ID)
	    uint32_t timestamp;   // Если RTC не настроен, будем писать тики системы или 0
	} LogEntry_t;

	typedef enum {
	    CMD_LOG_WRITE,      // Записать лог
	    CMD_CONFIG_SAVE     // Сохранить конфиг
	} StorageCmdType_t;

	typedef struct {
	    StorageCmdType_t type;
	    union {
	        LogEntry_t logData;       // Данные для лога
	        VendingConfig_t configData; // Копия конфига для сохранения
	    } payload;
	} StorageCommand_t;

#define FILE_CONFIG "config.bin"
#define FILE_LOG    "log.bin"

#define TIMER_ID_PAYMENT 1
#define TIMER_ID_VENDING 2
#define TIMER_ID_MACHINE 3

#endif /* INC_CONFIGURATION_H_ */
