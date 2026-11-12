#define MDB_HARDWARE_USART2 1
#include "configuration.h"
#include "config_record.h"
#include "../../../common/wire_codec.h"
#include "ftasks.h"
#include "keypad.h"
#include "mdb.h"
#include "f411_recovery.h"
#include "mdb_rx_trace.h"
#include "lcd.h"
#include "fatfs.h"
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "cmsis_os.h"
#include "main.h"
#include "main_io.h"
#include "esp_link.h"

#define KEYPAD_POLL_RATE_MS      (20U)
#define DEBOUNCE_THRESHOLD       (3U)
#define PAYMENT_WAIT_TIMEOUT_MS  (60000U)
#define PAYMENT_RECOVERY_TIMEOUT_MS (PAYMENT_WAIT_TIMEOUT_MS + 5000U)
#define VENDING_TIMEOUT_MS       (13000U)
#define MACHINE_RESULT_TIMEOUT_MS (60000U)
#define SELECTION_RETRY_BLOCK_MS (5000U)
#define ESP_UART_LINE_BUFFER_SIZE (128U)
#define USB_TX_OUTBOX_CAPACITY   (32U)
#define USB_TX_OUTBOX_RETRY_MS   (20U)
#define MANAGER_EVENT_FALLBACK_CAPACITY (8U)
#define DISPLAY_CRITICAL_HOLD_MS (1800U)
#define DISPLAY_PAYMENT_HOLD_MS  (1800U)
#define DISPLAY_MIN_UPDATE_MS    (1000U)
#define MDB_DISPLAY_QUIET_MS     (30U)
#define MANAGER_SERVICE_INTERVAL_MS (20U)
#define CONTROLLER_HEALTH_INTERVAL_MS (60000U)
#define MOTOR_RESULT_POLL_INTERVAL_MS (250U)

extern osMessageQueueId_t eventQueueHandle;
extern osMessageQueueId_t logQueueHandle;
extern osMessageQueueId_t mdbCommandQueueHandle;
extern osMessageQueueId_t displayQueueHandle;
extern osMutexId_t i2cMutexHandle;
extern UART_HandleTypeDef huart1;

extern volatile uint8_t sd_debug_status; // Подключаем переменную
volatile uint8_t sd_available = 0U;

// --- Локальные переменные потока ---
static VendingConfig_t loadedConfig;
static volatile bool configLoadComplete;
static VendingConfig_t currentConfig;     // Основная база цен (RAM)
static uint8_t editItemIndex = 0;         // Индекс редактируемого товара
extern osTimerId_t paymentTimerHandle;
extern osTimerId_t vendingTimerHandle;

typedef enum {
	LOCAL_VEND_UNKNOWN,
	LOCAL_VEND_SUCCESS,
	LOCAL_VEND_FAILURE,
	LOCAL_VEND_SKIPPED
} LocalVendStatus_t;

typedef enum {
	MACHINE_VEND_UNKNOWN,
	MACHINE_VEND_SUCCESS,
	MACHINE_VEND_FAILURE,
	MACHINE_VEND_SESSION_COMPLETE,
	MACHINE_VEND_CANCELLED
} MachineVendStatus_t;

// Состояние конечного автомата
static enum {
	STATE_INIT,
	STATE_IDLE,
	STATE_PAYMENT,
	STATE_VENDING,
	STATE_WAIT_MACHINE,
	STATE_DIRECT_VENDING
} currentState = STATE_INIT;

static uint16_t activeSelectionId = 0;
static uint16_t activePriceMinor = 0;
static uint32_t activeMdbTransactionId = 0;
static int8_t activeMotorIndex = -1;
static LocalVendStatus_t localVendStatus = LOCAL_VEND_UNKNOWN;
static MachineVendStatus_t machineVendStatus = MACHINE_VEND_UNKNOWN;
static bool transactionActive = false;
static uint32_t maintenanceToken, maintenanceSince, maintenanceNext;
static uint8_t maintenancePhase;
static bool maintenanceResetPending;
static uint32_t maintenanceResetDeadline;
static volatile bool maintenanceForced;

static bool paymentCancelledByMachine = false;
static bool directDeliveryActive = false;
static uint8_t directDeliveryLine = 0;
static uint32_t directDeliveryOperationId = 0;
static uint16_t blockedSelectionId = 0;
static uint16_t blockedPriceMinor = 0;
static uint32_t blockedSelectionUntilTick = 0;
static volatile uint32_t mdbDisplayQuietUntilTick = 0;
static volatile uint32_t machineResultTransactionId;
static volatile uint8_t machineResultCode;
static volatile uint32_t wirePaymentTransactionId = 0U;
static volatile uint32_t wirePaymentDeadlineTick = 0U;
/* Upper half of private transaction IDs belongs to local buttons. */
static bool snackButtonTransaction, snackPaymentAvailable;
static uint32_t nextSnackTransactionId = 0x80000000U;
static uint32_t paymentDeadlineTick = 0;
static uint32_t vendingDeadlineTick = 0;
static uint32_t machineDeadlineTick = 0;

typedef enum {
	USB_TX_STATUS_OK,
	USB_TX_STATUS_BAD_LINE,
	USB_TX_STATUS_NOT_CONFIGURED,
	USB_TX_STATUS_NO_CDC,
	USB_TX_STATUS_BUSY,
	USB_TX_STATUS_ERROR
} UsbTxStatus_t;

static UsbTxStatus_t lastUsbTxStatus = USB_TX_STATUS_OK;
static char usbTxOutbox[USB_TX_OUTBOX_CAPACITY][ESP_UART_LINE_BUFFER_SIZE];
static uint8_t usbTxOutboxHead = 0;
static uint8_t usbTxOutboxTail = 0;
static uint8_t usbTxOutboxCount = 0;
static char usbTxCriticalDeliveryResult[ESP_UART_LINE_BUFFER_SIZE];
static bool usbTxCriticalDeliveryResultPending = false;
static char vendReportOutbox[ESP_UART_LINE_BUFFER_SIZE];
static uint32_t vendReportTransactionId;
static bool vendReportPending;
static uint32_t vendReportRetryTick;
static uint32_t usbTxOutboxDropCount = 0;
static event_t managerEventFallback[MANAGER_EVENT_FALLBACK_CAPACITY];
static volatile uint8_t managerEventFallbackHead = 0;
static volatile uint8_t managerEventFallbackTail = 0;
static volatile uint8_t managerEventFallbackCount = 0;
static volatile uint32_t managerEventFallbackUseCount = 0;
static volatile uint32_t managerEventDropCount = 0;
static event_t managerEventLastFallback;
static event_t managerEventLastDropped;

typedef struct {
	uint8_t resultCode;
	uint16_t selectionId;
	uint32_t transactionId;
	uint32_t sequence;
} MdbPaymentMailbox_t;

static volatile MdbPaymentMailbox_t mdbPaymentMailbox;
static uint32_t mdbPaymentMailboxConsumedSequence = 0U;

static void RestoreInterruptState(uint32_t primask);

/* MDB owns the wire snapshot; manager owns formatting/IO/reset. No heap or flash. */
typedef struct {uint8_t state;bool enabled,setup,resetRequired,cachePending,seenSelection;uint32_t lastCommandMs;} F411WireHealth;
static volatile F411WireHealth f411WireHealth;
static uint32_t f411Progress, f411FirstRejectMs, f411LastRejectMs, f411LastReasonTx, f411LastStateMs;
static uint8_t f411RejectBurst;
static char f411LastReason[25]="none";
static uint32_t f411RecoveryCount;
static F411RecoveryTimer f411RecoveryTimer, f411TxRecoveryTimer;

static uint32_t f411LastWorkProgressMs;
static bool f411WorkTracked;
static bool f411PaymentBudget;
static void F411RecoveryProgress(void) {
 uint32_t primask=__get_PRIMASK();__disable_irq();
 ++f411Progress;f411RejectBurst=0;
 if(f411WorkTracked)f411LastWorkProgressMs=HAL_GetTick();
 f411PaymentBudget=false;
 RestoreInterruptState(primask);
}
/* New admitted work gets a fresh budget; it must not clear repeated-reject history. */
static void F411RecoveryAdmitted(bool payment) {
 const uint32_t primask=__get_PRIMASK();__disable_irq();
 f411WorkTracked=true;f411LastWorkProgressMs=HAL_GetTick();f411PaymentBudget=payment;
 RestoreInterruptState(primask);
}
static void F411RecordReason(const char*reason,uint32_t tx,bool rejected) {
 uint32_t now=HAL_GetTick(),primask=__get_PRIMASK();__disable_irq();
 unsigned i=0;for(;reason&&reason[i]&&i<24;i++){
  char c=reason[i];f411LastReason[i]=((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')?c:'_';
 }
 f411LastReason[i]=0;if(!i)memcpy(f411LastReason,"none",5);
 f411LastReasonTx=tx;
 if(rejected){if(now-f411LastRejectMs>10000U)f411RejectBurst=0;if(!f411RejectBurst)f411FirstRejectMs=now;if(f411RejectBurst<255)++f411RejectBurst;f411LastRejectMs=now;}
 RestoreInterruptState(primask);
}
static void F411ObserveWire(const MdbCashlessFsm_t*fsm,bool cachePending,bool commandSeen) {
 const uint32_t primask=__get_PRIMASK();__disable_irq();
 f411WireHealth.state=(uint8_t)fsm->sessionState;f411WireHealth.enabled=fsm->readerEnabled;
 f411WireHealth.setup=fsm->setupSeen;f411WireHealth.resetRequired=fsm->resetRequired;
 f411WireHealth.cachePending=cachePending;
 if(commandSeen)f411WireHealth.lastCommandMs=HAL_GetTick();
 if(fsm->currentTransactionId)f411WireHealth.seenSelection=true;
 RestoreInterruptState(primask);
}
/* MDB owns the session flag; logger alone owns each in-flight SD operation.
 * Completion never clears a payment block. Already-started I/O may finish. */
volatile bool g_mdb_session_active = false;
volatile uint32_t g_payment_request_tick = 0U;
volatile uint32_t g_payment_result_tick = 0U;
static volatile bool sdOperationBusy = false;

static bool SdTryBeginOperation(void) {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    const bool permitted = !g_mdb_session_active && !sdOperationBusy;
    if (permitted) sdOperationBusy = true;
    RestoreInterruptState(primask);
    return permitted;
}

static void SdCompleteOperation(void) {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    sdOperationBusy = false;
    RestoreInterruptState(primask);
}

static void SdObserveMdbSession(const MdbCashlessFsm_t *fsm) {
    /* OOS explicitly aborts the vend; denied/approved replies do not.
     * Keep the block through END_SESSION ACK, including compatibility cancel. */
    if (fsm->currentTransactionId == 0U || fsm->resetRequired ||
        MdbFsmHasPendingResponse(fsm, MDB_POLL_COMMAND_OOS))
        g_mdb_session_active = false;
}

static void SendF411State(void);
static void ServiceF411Recovery(void);

static void SendStatusUpdate(const char *line1, const char *line2);
static void SendControllerStorageDiagnostic(void);
static void ServiceMdbTraceDiagnostics(void);
static void ServiceMdbRxDiagnostics(void);
void UpdateDisplay(DisplayCommand_t *cmd, bool blinkVisible);

static void RestoreInterruptState(uint32_t primask) {
	if (primask == 0U) {
		__enable_irq();
	}
}

bool QueueManagerEvent(const event_t *event) {
	uint32_t primask;

	if (event == NULL) {
		return false;
	}

	primask = __get_PRIMASK();
	__disable_irq();
	/* Once overflow starts, new events stay behind that older segment. */
	if (managerEventFallbackCount == 0U && eventQueueHandle != NULL
			&& osMessageQueuePut(eventQueueHandle, event, 0, 0) == osOK) {
		RestoreInterruptState(primask);
		return true;
	}

	if (managerEventFallbackCount < MANAGER_EVENT_FALLBACK_CAPACITY) {
		managerEventFallback[managerEventFallbackTail] = *event;
		managerEventFallbackTail = (uint8_t)((managerEventFallbackTail + 1U)
				% MANAGER_EVENT_FALLBACK_CAPACITY);
		managerEventFallbackCount++;
		managerEventFallbackUseCount++;
		managerEventLastFallback = *event;
		RestoreInterruptState(primask);
		return true;
	}

	managerEventDropCount++;
	managerEventLastDropped = *event;
	RestoreInterruptState(primask);
	return false;
}

static bool TryGetManagerEvent(event_t *event) {
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	if (osMessageQueueGet(eventQueueHandle, event, NULL, 0U) == osOK) {
		RestoreInterruptState(primask);
		return true;
	}
	if (managerEventFallbackCount == 0U) {
		RestoreInterruptState(primask);
		return false;
	}
	*event = managerEventFallback[managerEventFallbackHead];
	managerEventFallbackHead = (uint8_t)((managerEventFallbackHead + 1U)
			% MANAGER_EVENT_FALLBACK_CAPACITY);
	managerEventFallbackCount--;
	RestoreInterruptState(primask);
	return true;
}

static bool TakeManagerQueueDiagnostic(bool dropped, uint32_t *count,
		event_t *event) {
	volatile uint32_t *counter = dropped ? &managerEventDropCount
			: &managerEventFallbackUseCount;
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	if (*counter == 0U) {
		RestoreInterruptState(primask);
		return false;
	}

	*count = *counter;
	*counter = 0U;
	*event = dropped ? managerEventLastDropped : managerEventLastFallback;
	RestoreInterruptState(primask);
	return true;
}

void SetDefaultConfig(VendingConfig_t *cfg) {
	for (int i = 0; i < 10; i++) {
		cfg->items[i].price_x = 0; // 0
		cfg->items[i].price_y = 5; // 5
		cfg->items[i].price_z = 0; // .0
		cfg->items[i].price_p = 0; // 0  -> Итого 5.00 руб
		/* Full-hardware profile: MDB IDs 101..110 belong to F103 motor lines
		 * 1..10. Every other selection remains on the coffee-machine path. */
		cfg->items[i].selection_id = (uint16_t)(101 + i);
	}
}

bool WaitForConfigLoad() {
	uint32_t started = HAL_GetTick();
	while (!configLoadComplete && HAL_GetTick() - started < 6000U) osDelay(10U);
	/* Logger publishes once. A late SD completion never changes an active map. */
	if (configLoadComplete) {
		__DMB();
		currentConfig = loadedConfig;
		return true;
	}
	SetDefaultConfig(&currentConfig);
	return false;
}

bool SaveConfigToSD() {
	StorageCommand_t cmd;
	cmd.type = CMD_CONFIG_SAVE;
	// Копируем текущий конфиг в сообщение (безопасная передача)
	cmd.payload.configData = currentConfig;

	// Отправляем в очередь логгера
	return osMessageQueuePut(logQueueHandle, &cmd, 0, 100) == osOK;
}

void WriteLog(uint8_t id, uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4) {
	StorageCommand_t cmd;
	cmd.type = CMD_LOG_WRITE;
	cmd.payload.logData.eventId = id;
	cmd.payload.logData.payload[0] = d1;
	cmd.payload.logData.payload[1] = d2;
	cmd.payload.logData.payload[2] = d3;
	cmd.payload.logData.payload[3] = d4;
	cmd.payload.logData.timestamp = HAL_GetTick();

	osMessageQueuePut(logQueueHandle, &cmd, 0, 0);
}

static void CopyLcdStatusLine(char dest[17], const char *src) {
	memset(dest, ' ', 16);
	dest[16] = '\0';

	if (src == NULL) {
		return;
	}

	for (uint8_t i = 0; i < 16 && src[i] != '\0'; i++) {
		dest[i] = src[i];
	}
}

static bool QueueDisplayCommand(const DisplayCommand_t *cmd) {
	DisplayCommand_t dropped;

	if (displayQueueHandle == NULL || cmd == NULL) {
		return false;
	}

	if (osMessageQueuePut(displayQueueHandle, cmd, 0, 0) == osOK) {
		return true;
	}

	if (osMessageQueueGet(displayQueueHandle, &dropped, NULL, 0) != osOK) {
		return false;
	}

	return osMessageQueuePut(displayQueueHandle, cmd, 0, 0) == osOK;
}

static void DrainDisplayQueueToLatest(DisplayCommand_t *cmd) {
	DisplayCommand_t latest;

	if (displayQueueHandle == NULL || cmd == NULL) {
		return;
	}

	while (osMessageQueueGet(displayQueueHandle, &latest, NULL, 0) == osOK) {
		*cmd = latest;
	}
}

static bool IsOperationalStatusLine(const char *line1) {
	if (line1 == NULL) {
		return false;
	}

	return strncmp(line1, "MDB VEND", 8) == 0
			|| strncmp(line1, "USB ", 4) == 0
			|| strncmp(line1, "UART ", 5) == 0;
}

static void SendStatusUpdateInternal(const char *line1, const char *line2,
		bool force) {
	static char lastLine1[17] = { 0 };
	static char lastLine2[17] = { 0 };
	DisplayCommand_t cmd = { 0 };

	if (!IsOperationalStatusLine(line1)) {
		return;
	}

	cmd.type = SCENE_STATUS;
	CopyLcdStatusLine(cmd.data.status.line1, line1);
	CopyLcdStatusLine(cmd.data.status.line2, line2);

	if (!force
			&& memcmp(lastLine1, cmd.data.status.line1,
					sizeof(lastLine1)) == 0
			&& memcmp(lastLine2, cmd.data.status.line2,
					sizeof(lastLine2)) == 0) {
		return;
	}

	memcpy(lastLine1, cmd.data.status.line1, sizeof(lastLine1));
	memcpy(lastLine2, cmd.data.status.line2, sizeof(lastLine2));
	QueueDisplayCommand(&cmd);
}

static void SendStatusUpdate(const char *line1, const char *line2) {
	SendStatusUpdateInternal(line1, line2, false);
}

static void SendStatusUpdateForce(const char *line1, const char *line2) {
	SendStatusUpdateInternal(line1, line2, true);
}

void SendDisplayUpdate(SceneType_t scene, ItemConfig_t *data, uint8_t editIdx) {
	// Статические переменные хранят состояние последнего отправленного пакета
	static SceneType_t lastScene = -1;
	static uint8_t lastEditIdx = 0xFF;
	static ItemConfig_t lastData = { 0 };

	if (scene != SCENE_PAYMENT) {
		return;
	}

	// 1. Проверяем, изменилось ли что-то
	bool isChanged = false;

	if (scene != lastScene)
		isChanged = true;
	if (editIdx != lastEditIdx)
		isChanged = true;

	if (data != NULL) {
		if (data->price_x != lastData.price_x
				|| data->price_y != lastData.price_y
				|| data->price_z != lastData.price_z
				|| data->price_p != lastData.price_p
				|| data->selection_id != lastData.selection_id) {
			isChanged = true;
		}
	}

	// 2. Если ничего не изменилось - выходим, не забиваем очередь
	if (!isChanged) {
		return;
	}

	// 3. Обновляем "кэш"
	lastScene = scene;
	lastEditIdx = editIdx;
	if (data != NULL) {
		lastData = *data;
	}

	// 4. Отправляем команду
	DisplayCommand_t cmd = {0};
	cmd.type = scene;
	cmd.data.settings.editIndex = editIdx;
	if (data != NULL) {
		cmd.data.settings.x = data->price_x;
		cmd.data.settings.y = data->price_y;
		cmd.data.settings.z = data->price_z;
		cmd.data.settings.p = data->price_p;
		cmd.data.settings.q = data->selection_id;
	}
	QueueDisplayCommand(&cmd);
}

#define MOTOR_CONTROLLER_I2C_ADDR (0x0F << 1)
#define MOTOR_I2C_RETRIES        5U
#define MOTOR_STATUS_READ_RETRIES 3U
#define MOTOR_STATUS_RETRY_DELAY_MS 5U
#define MOTOR_START_LISTEN_SETTLE_MS 10U
#define MOTOR_START_CONFIRM_TIMEOUT_MS 750U
#define MOTOR_START_CONFIRM_POLL_MS 20U
#define MOTOR_START_CONFIRM_READ_TIMEOUT_MS 100U
#define MOTOR_STATUS_IDLE        0x00U
#define MOTOR_STATUS_BUSY        0x01U
#define MOTOR_STATUS_SUCCESS     0x02U
#define MOTOR_STATUS_FAILED      0x03U
#define MOTOR_STATUS_UNAVAILABLE 0xFFU
#define MOTOR_STATUS_PROTOCOL_VERSION 3U
#define MOTOR_STATUS_FRAME_SIZE  28U
#define MOTOR_STATUS_FLAG_OUTPUT_ACTIVE  (1U << 1)
#define MOTOR_STATUS_FLAG_STORAGE_ERROR (1U << 4)

typedef enum {
	MOTOR_START_CONFIRMED,
	MOTOR_START_I2C_LOCK_TIMEOUT,
	MOTOR_START_STATUS_READ_FAILED,
	MOTOR_START_PROTOCOL_MISMATCH,
	MOTOR_START_ALREADY_BUSY,
	MOTOR_START_SENSOR_BLOCKED,
	MOTOR_START_COMMAND_WRITE_FAILED,
	MOTOR_START_NO_BUSY_CONFIRMATION,
	MOTOR_START_COMMAND_NOT_BOUND,
	MOTOR_START_OUTPUT_NOT_ACTIVE
} MotorStartResultCode_t;

typedef struct {
	uint8_t status;
	uint8_t protocolVersion;
	uint8_t commandCounter;
	uint8_t deliveryLine;
	uint8_t motorPhase;
	uint8_t flags;
	uint8_t resetFlags;
	uint16_t i2cErrorCount;
	uint16_t i2cLastError;
	uint8_t turns;
	uint32_t bootIdentity, operationId, directOperationId;
} MotorControllerSnapshot_t;

typedef struct {
	MotorStartResultCode_t code;
	uint8_t f103Status;
	MotorControllerSnapshot_t snapshot;
	HAL_StatusTypeDef writeStatus;
	HAL_StatusTypeDef lastReadStatus;
	uint32_t writeError;
	uint32_t lastReadError;
	uint32_t expectedOperationId;
	uint8_t expectedCommandCounter;
	uint8_t confirmationReads;
	uint8_t lastReadWireComplete;
} MotorStartResult_t;

typedef struct {
	uint32_t transactionId;
	uint32_t bootIdentity;
	bool valid;
} MotorPaymentClaim_t;

static uint32_t nextMotorOperationId;
static uint32_t activeMotorBootIdentity,activeMotorOperationId;
static bool activeMotorResultTracked = false;
static uint8_t activeMotorCommandCounter = 0U;
static uint8_t activeMotorDeliveryLine = 0U;
static uint32_t nextMotorResultPollTick = 0U;

static bool WirePaymentPermitted(uint32_t transactionId) {
    if (transactionId & 0x80000000U) {
        return F411_GetBootIdentity() != 0U && snackButtonTransaction && snackPaymentAvailable &&
            activeMdbTransactionId == transactionId && (int32_t)(HAL_GetTick() - paymentDeadlineTick) < 0;
    }
	return F411_GetBootIdentity() != 0U && transactionId != 0U
			&& wirePaymentTransactionId == transactionId
			&& (int32_t)(HAL_GetTick() - wirePaymentDeadlineTick) < 0;
}

static bool ClaimMotorPayment(uint32_t transactionId,
		MotorPaymentClaim_t *claim) {
	uint32_t primask;
	bool permitted;

	if (claim == NULL) {
		return false;
	}
	*claim = (MotorPaymentClaim_t) { 0 };
	primask = __get_PRIMASK();
	__disable_irq();
	permitted = WirePaymentPermitted(transactionId);
	if (permitted) {
		if (transactionId & 0x80000000U) snackPaymentAvailable = false;
		else wirePaymentTransactionId = 0U;
		claim->transactionId = transactionId;
		claim->bootIdentity = F411_GetBootIdentity();
		claim->valid = true;
	}
	if (primask == 0U) {
		__enable_irq();
	}
	return permitted;
}

static bool ConsumeMotorPaymentClaim(MotorPaymentClaim_t *claim) {
	if (claim == NULL || !claim->valid || claim->transactionId == 0U
			|| claim->bootIdentity == 0U
			|| claim->bootIdentity != F411_GetBootIdentity()) {
		return false;
	}
	claim->valid = false;
	return true;
}

static void ResetActiveMotorResultTracking(void) {
    MainDropDisarm();
    activeMotorResultTracked = false;
    activeMotorBootIdentity = 0;
    activeMotorOperationId = 0;
    activeMotorCommandCounter = 0U;
    activeMotorDeliveryLine = 0U;
    nextMotorResultPollTick = 0U;
}

static void TrackActiveMotorResult(const MotorControllerSnapshot_t *snapshot) {
    if (snapshot == NULL || snapshot->protocolVersion != MOTOR_STATUS_PROTOCOL_VERSION ||
        (snapshot->status != MOTOR_STATUS_BUSY && snapshot->status != MOTOR_STATUS_SUCCESS &&
         snapshot->status != MOTOR_STATUS_FAILED)) {
        ResetActiveMotorResultTracking();
        return;
    }

    activeMotorResultTracked = true;
    activeMotorBootIdentity = snapshot->bootIdentity;
    activeMotorOperationId = snapshot->operationId;
    activeMotorCommandCounter = snapshot->commandCounter;
    activeMotorDeliveryLine = snapshot->deliveryLine;
    nextMotorResultPollTick = HAL_GetTick();
}

static HAL_StatusTypeDef DecodeMotorControllerSnapshot(
        const uint8_t frame[MOTOR_STATUS_FRAME_SIZE], MotorControllerSnapshot_t *snapshot) {
    if (snapshot == NULL || frame[1] != MOTOR_STATUS_PROTOCOL_VERSION ||
        WireRead32(frame + 24) != WireCrc32(frame, 24)) {
        return HAL_ERROR;
    }

    snapshot->bootIdentity = WireRead32(frame + 12);
    snapshot->operationId = WireRead32(frame + 16);
    snapshot->directOperationId = WireRead32(frame + 20);
    snapshot->status = frame[0];
    snapshot->protocolVersion = frame[1];
    snapshot->commandCounter = frame[2];
    snapshot->deliveryLine = frame[3];
    snapshot->motorPhase = frame[4];
    snapshot->flags = frame[5];
    snapshot->resetFlags = frame[6];
    snapshot->i2cErrorCount = (uint16_t)frame[7] | ((uint16_t)frame[8] << 8U);
    snapshot->i2cLastError = (uint16_t)frame[9] | ((uint16_t)frame[10] << 8U);
    snapshot->turns = frame[11];
    return HAL_OK;
}

static HAL_StatusTypeDef ReadMotorControllerSnapshotOnce(
        MotorControllerSnapshot_t *snapshot, uint32_t timeoutMs,
        uint32_t *halError, uint8_t *wireComplete) {
    uint8_t frame[MOTOR_STATUS_FRAME_SIZE] = {0};
    HAL_StatusTypeDef readStatus;

    if (halError != NULL) {
        *halError = HAL_I2C_ERROR_NONE;
    }
    if (wireComplete != NULL) {
        *wireComplete = 0U;
    }
    if (snapshot == NULL) {
        return HAL_ERROR;
    }
    readStatus = HAL_I2C_Master_Receive(&DISPLAY_I2C_HANDLE, MOTOR_CONTROLLER_I2C_ADDR,
                                       frame, MOTOR_STATUS_FRAME_SIZE, timeoutMs);
    if (halError != NULL) {
        *halError = HAL_I2C_GetError(&DISPLAY_I2C_HANDLE);
    }
    if (readStatus != HAL_OK) {
        return readStatus;
    }
    if (wireComplete != NULL) {
        *wireComplete = 1U;
    }
    return DecodeMotorControllerSnapshot(frame, snapshot);
}

static HAL_StatusTypeDef ReadMotorControllerSnapshot(MotorControllerSnapshot_t *snapshot) {
    HAL_StatusTypeDef readStatus = HAL_ERROR;
    uint8_t frame[MOTOR_STATUS_FRAME_SIZE] = {0};

    if (snapshot == NULL) {
        return HAL_ERROR;
    }

    for (uint8_t attempt = 0; attempt < MOTOR_STATUS_READ_RETRIES; attempt++) {
        readStatus =
            I2C_MasterReceiveSafe(MOTOR_CONTROLLER_I2C_ADDR, frame, MOTOR_STATUS_FRAME_SIZE, 100U);
        if (readStatus == HAL_OK) {
            readStatus = DecodeMotorControllerSnapshot(frame, snapshot);
            if (readStatus == HAL_OK) {
                return HAL_OK;
            }
        }
        if ((attempt + 1U) < MOTOR_STATUS_READ_RETRIES) {
            osDelay(MOTOR_STATUS_RETRY_DELAY_MS);
        }
    }

    return readStatus;
}

static MotorStartResult_t StartMotorConfirmed(uint8_t itemIndex, uint8_t turns,
											  MotorPaymentClaim_t *paymentClaim,
                                              uint32_t directOperationId) {
    MotorStartResult_t result = {.code = MOTOR_START_I2C_LOCK_TIMEOUT,
                                 .f103Status = MOTOR_STATUS_UNAVAILABLE,
                                 .snapshot = {.status = MOTOR_STATUS_UNAVAILABLE},
                                 .writeStatus = HAL_ERROR,
                                 .lastReadStatus = HAL_ERROR};
    uint8_t command[20] = {0xA5U, MOTOR_STATUS_PROTOCOL_VERSION, itemIndex, 1U};
    uint32_t operationId = 0;
    uint32_t confirmationStartedTick = 0U;
    MotorControllerSnapshot_t before = {.status = MOTOR_STATUS_UNAVAILABLE};
    uint8_t expectedCommandCounter = 0U;

    (void)turns;
    if (F411_GetBootIdentity() == 0U) {
        result.code = MOTOR_START_PROTOCOL_MISMATCH;
        return result;
    }
	if (directOperationId == 0U) {
		if (!ConsumeMotorPaymentClaim(paymentClaim)) {
			result.code = MOTOR_START_COMMAND_NOT_BOUND;
			return result;
		}
	} else if (paymentClaim != NULL && paymentClaim->valid) {
		result.code = MOTOR_START_PROTOCOL_MISMATCH;
		return result;
	}
    if (i2cMutexHandle == NULL || osMutexAcquire(i2cMutexHandle, 3000U) != osOK) {
        return result;
    }

    if (ReadMotorControllerSnapshot(&before) != HAL_OK) {
        result.code = MOTOR_START_STATUS_READ_FAILED;
        goto done;
    }

    result.snapshot = before;
    result.f103Status = before.status;
    if (before.protocolVersion != MOTOR_STATUS_PROTOCOL_VERSION) {
        result.code = MOTOR_START_PROTOCOL_MISMATCH;
        goto done;
    }
    if (before.flags & MOTOR_STATUS_FLAG_STORAGE_ERROR) {
        result.code = MOTOR_START_STATUS_READ_FAILED;
        goto done;
    }
    if (directOperationId != 0U && before.directOperationId == directOperationId &&
        before.deliveryLine == itemIndex && before.bootIdentity != 0U && before.operationId != 0U &&
        (before.status == MOTOR_STATUS_BUSY || before.status == MOTOR_STATUS_SUCCESS ||
         before.status == MOTOR_STATUS_FAILED)) {
        result.code = MOTOR_START_CONFIRMED;
        goto done; // Adopt or return the stored operation, never issue START again.
    }
    if (before.status == MOTOR_STATUS_BUSY) {
        result.code = MOTOR_START_ALREADY_BUSY;
        goto done;
    }
    if (MainDropBlocked()) {
        result.code = MOTOR_START_SENSOR_BLOCKED;
        goto done;
    }
    if (nextMotorOperationId == UINT32_MAX) {
        result.code = MOTOR_START_COMMAND_NOT_BOUND;
        goto done;
    }
    operationId = ++nextMotorOperationId;
    result.expectedOperationId = operationId;
    WireWrite32(command + 4, F411_GetBootIdentity());
    WireWrite32(command + 8, operationId);
    WireWrite32(command + 12, directOperationId);
    WireWrite32(command + 16, WireCrc32(command, 16));
    expectedCommandCounter = (uint8_t)(before.commandCounter + 1U);
    result.expectedCommandCounter = expectedCommandCounter;

    /* An I2C error may mean the slave already accepted START. Never resend it.
     * Reconcile the read-only snapshot even after an uncertain write. */
    /* A completed slave read can briefly leave F103 between LISTEN cycles. Keep
     * START single-shot and give the slave time to re-arm before that write. */
    osDelay(MOTOR_START_LISTEN_SETTLE_MS);
    result.code = MOTOR_START_NO_BUSY_CONFIRMATION;
    if (MainDropBlocked()) { result.code = MOTOR_START_SENSOR_BLOCKED; goto done; }
    MainDropArm(F411_GetBootIdentity(), operationId);
    result.writeStatus = HAL_I2C_Master_Transmit(&DISPLAY_I2C_HANDLE,
                                                 MOTOR_CONTROLLER_I2C_ADDR, command,
                                                 sizeof(command), 100U);
    result.writeError = HAL_I2C_GetError(&DISPLAY_I2C_HANDLE);

    confirmationStartedTick = HAL_GetTick();
    for (;;) {
        MotorControllerSnapshot_t after = {.status = MOTOR_STATUS_UNAVAILABLE};
        uint32_t elapsed = HAL_GetTick() - confirmationStartedTick;
        uint32_t remaining;

        if (elapsed >= MOTOR_START_CONFIRM_TIMEOUT_MS) {
            break;
        }
        remaining = MOTOR_START_CONFIRM_TIMEOUT_MS - elapsed;
        /* At 10 kHz a complete 28-byte snapshot needs at least 26.1 ms on the
         * wire. Never start a final read with an artificially shortened HAL
         * timeout: it can abort an otherwise healthy slave transaction. */
        if (remaining < MOTOR_START_CONFIRM_READ_TIMEOUT_MS) {
            osDelay(remaining);
            break;
        }

        result.confirmationReads++;
        result.lastReadStatus = ReadMotorControllerSnapshotOnce(
                &after, MOTOR_START_CONFIRM_READ_TIMEOUT_MS,
                &result.lastReadError, &result.lastReadWireComplete);
        if (result.lastReadStatus == HAL_OK) {
            result.snapshot = after;
            result.f103Status = after.status;
            if (after.protocolVersion != MOTOR_STATUS_PROTOCOL_VERSION) {
                result.code = MOTOR_START_PROTOCOL_MISMATCH;
                break;
            }
            if (after.bootIdentity != F411_GetBootIdentity() || after.operationId != operationId ||
                after.commandCounter != expectedCommandCounter || after.deliveryLine != itemIndex) {
                result.code = MOTOR_START_COMMAND_NOT_BOUND;
            } else if ((after.status == MOTOR_STATUS_SUCCESS ||
                        after.status == MOTOR_STATUS_FAILED) &&
                       after.motorPhase == 0U &&
                       (after.flags & MOTOR_STATUS_FLAG_OUTPUT_ACTIVE) == 0U) {
                /* The accepted command may finish before the first BUSY read. */
                result.code = MOTOR_START_CONFIRMED;
                break;
            } else if (after.status == MOTOR_STATUS_BUSY && after.motorPhase == 1U &&
                       (after.flags & MOTOR_STATUS_FLAG_OUTPUT_ACTIVE) != 0U) {
                result.code = MOTOR_START_CONFIRMED;
                break;
            } else if (after.status == MOTOR_STATUS_BUSY) {
                result.code = MOTOR_START_OUTPUT_NOT_ACTIVE;
            }
        }
        elapsed = HAL_GetTick() - confirmationStartedTick;
        if (elapsed >= MOTOR_START_CONFIRM_TIMEOUT_MS) {
            break;
        }
        remaining = MOTOR_START_CONFIRM_TIMEOUT_MS - elapsed;
        osDelay(remaining < MOTOR_START_CONFIRM_POLL_MS ? remaining
                                                       : MOTOR_START_CONFIRM_POLL_MS);
        if ((HAL_GetTick() - confirmationStartedTick) >= MOTOR_START_CONFIRM_TIMEOUT_MS) {
            break;
        }
    }

done:
    if (result.code != MOTOR_START_CONFIRMED) MainDropDisarm();
    osMutexRelease(i2cMutexHandle);
    return result;
}

static bool ReadMotorControllerSnapshotLocked(
		MotorControllerSnapshot_t *snapshot, uint32_t lockTimeoutMs) {
	bool read = false;

	if (snapshot == NULL || i2cMutexHandle == NULL
			|| osMutexAcquire(i2cMutexHandle, lockTimeoutMs) != osOK) {
		return false;
	}
	read = ReadMotorControllerSnapshot(snapshot) == HAL_OK;
	osMutexRelease(i2cMutexHandle);
	return read;
}

static void FillItemFromMinor(ItemConfig_t *item, uint16_t priceMinor) {
	uint16_t rubles = priceMinor / 100U;
	uint8_t kopecks = (uint8_t)(priceMinor % 100U);

	memset(item, 0, sizeof(*item));
	item->price_x = (uint8_t)(rubles / 10U);
	item->price_y = (uint8_t)(rubles % 10U);
	item->price_z = (uint8_t)(kopecks / 10U);
	item->price_p = (uint8_t)(kopecks % 10U);
}

static int8_t FindMotorBySelectionId(uint16_t selectionId) {
	for (uint8_t i = 0; i < 10; i++) {
		if (currentConfig.items[i].selection_id != 0xFFFFU
				&& currentConfig.items[i].selection_id == selectionId) {
			return (int8_t)i;
		}
	}

	return -1;
}

static const char* LocalVendStatusToString(LocalVendStatus_t status) {
	switch (status) {
	case LOCAL_VEND_SUCCESS:
		return "success";
	case LOCAL_VEND_FAILURE:
		return "fail";
	case LOCAL_VEND_SKIPPED:
		return "skip";
	default:
		return "pending";
	}
}

static const char* MachineVendStatusToString(MachineVendStatus_t status) {
	switch (status) {
	case MACHINE_VEND_SUCCESS:
		return "success";
	case MACHINE_VEND_FAILURE:
		return "failure";
	case MACHINE_VEND_SESSION_COMPLETE:
		return "session_complete";
	case MACHINE_VEND_CANCELLED:
		return "cancelled";
	default:
		return "pending";
	}
}

static const char* UsbTxStatusToDisplayLine(UsbTxStatus_t status) {
	switch (status) {
	case USB_TX_STATUS_OK:
		return "UART TX QUEUED";
	case USB_TX_STATUS_BAD_LINE:
		return "UART BAD LINE";
	case USB_TX_STATUS_NOT_CONFIGURED:
		return "UART NOT RDY";
	case USB_TX_STATUS_NO_CDC:
		return "UART NO LINK";
	case USB_TX_STATUS_BUSY:
		return "UART BUSY";
	default:
		return "UART TX ERR";
	}
}

/* Accepted means queued for transport, not acknowledged by ESP. */
static bool UsbTrySendLine(const char *line) {
    bool queued = EspLinkSendLine(line);
    lastUsbTxStatus = queued ? USB_TX_STATUS_OK : USB_TX_STATUS_BUSY;
    return queued;
}

static bool QueueUsbTxLine(const char *line) {
	size_t length;

	if (line == NULL) {
		return false;
	}

	length = strlen(line);
	if (length == 0U || length >= ESP_UART_LINE_BUFFER_SIZE) {
		lastUsbTxStatus = USB_TX_STATUS_BAD_LINE;
		return false;
	}

	if (usbTxOutboxCount >= USB_TX_OUTBOX_CAPACITY) {
		if ((strncmp(line, "DELIVERY_RESULT,", 16U) == 0
				|| strncmp(line, "VEND_REPORT,", 12U) == 0)
				&& !usbTxCriticalDeliveryResultPending) {
			memcpy(usbTxCriticalDeliveryResult, line, length + 1U);
			usbTxCriticalDeliveryResultPending = true;
			return true;
		}
		usbTxOutboxDropCount++;
		return false;
	}

	memcpy(usbTxOutbox[usbTxOutboxTail], line, length + 1U);
	usbTxOutboxTail = (uint8_t)((usbTxOutboxTail + 1U)
			% USB_TX_OUTBOX_CAPACITY);
	usbTxOutboxCount++;
	return true;
}

static bool UsbSendLine(const char *line) {
	if (usbTxOutboxCount == 0U && UsbTrySendLine(line)) {
		return true;
	}

	return QueueUsbTxLine(line);
}

static void ServiceUsbTxOutbox(void) {
	char diagnosticLine[96];
	if (vendReportPending && (int32_t)(HAL_GetTick() - vendReportRetryTick) >= 0) {
		(void)UsbTrySendLine(vendReportOutbox);
		vendReportRetryTick = HAL_GetTick() + 500U;
	}

	if (usbTxOutboxCount == 0U) {
		if (usbTxCriticalDeliveryResultPending) {
			QueueUsbTxLine(usbTxCriticalDeliveryResult);
			usbTxCriticalDeliveryResultPending = false;
		}
		if (usbTxOutboxDropCount == 0U) {
			return;
		}
	}

	if (usbTxOutboxCount > 0U
			&& UsbTrySendLine(usbTxOutbox[usbTxOutboxHead])) {
		usbTxOutboxHead = (uint8_t)((usbTxOutboxHead + 1U)
				% USB_TX_OUTBOX_CAPACITY);
		usbTxOutboxCount--;
	}

	if (usbTxCriticalDeliveryResultPending
			&& usbTxOutboxCount < USB_TX_OUTBOX_CAPACITY) {
		QueueUsbTxLine(usbTxCriticalDeliveryResult);
		usbTxCriticalDeliveryResultPending = false;
	}

	if (usbTxOutboxDropCount > 0U
			&& usbTxOutboxCount < USB_TX_OUTBOX_CAPACITY) {
		snprintf(diagnosticLine, sizeof(diagnosticLine),
				"CONTROLLER_DIAGNOSTIC,source=uartTx,event=outboxDrop,line=0,count=%lu,status=dropped",
				(unsigned long)usbTxOutboxDropCount);
		usbTxOutboxDropCount = 0U;
		QueueUsbTxLine(diagnosticLine);
	}
}

static bool UsbTxOutboxPending(void) {
	return usbTxOutboxCount > 0U || usbTxCriticalDeliveryResultPending;
}

/* Only manager parses private protocol and changes payment/motor state. */
static void ServiceEspInputLines(void) {
    char line[ESP_LINK_LINE_SIZE];
    for (unsigned budget = 0U; budget < 8U && EspLinkReceiveLine(line); ++budget)
        UsbCdcProcessLine(line);
}

static void FormatFiveDigitValue(char *dest, uint16_t value) {
	for (int8_t i = 4; i >= 0; i--) {
		dest[i] = (char)('0' + (value % 10U));
		value /= 10U;
	}
}

static void SendUsbTransactionStatus(const char *line1, uint16_t selectionId,
		uint16_t priceMinor, int8_t motorIndex) {
	char line2[17];
	uint8_t motorNumber = 0;

	memset(line2, ' ', 16);
	line2[16] = '\0';
	line2[0] = 'I';
	FormatFiveDigitValue(&line2[1], selectionId);
	line2[6] = 'P';
	FormatFiveDigitValue(&line2[7], priceMinor);
	line2[12] = 'M';

	if (motorIndex >= 0) {
		motorNumber = (uint8_t)(motorIndex + 1);
		line2[13] = (char)('0' + (motorNumber / 10U));
		line2[14] = (char)('0' + (motorNumber % 10U));
	} else {
		line2[13] = '-';
		line2[14] = '-';
	}

	SendStatusUpdateForce(line1, line2);
}

static void SendUsbMapStatus(const char *line1, uint8_t motorIndex,
		uint16_t selectionId) {
	char line2[17];

	snprintf(line2, sizeof(line2), "M%02u ID%u", (unsigned)motorIndex,
			(unsigned)selectionId);
	SendStatusUpdate(line1, line2);
}

static void SendDirectDeliveryStatus(const char *line1, uint8_t motorIndex) {
	char line2[17];

	snprintf(line2, sizeof(line2), "M%02u DIRECT", (unsigned)motorIndex);
	SendStatusUpdateForce(line1, line2);
}

static const char* MotorControllerStatusToString(uint8_t status) {
	switch (status) {
	case MOTOR_STATUS_IDLE:
		return "idle";
	case MOTOR_STATUS_BUSY:
		return "busy";
	case MOTOR_STATUS_SUCCESS:
		return "success";
	case MOTOR_STATUS_FAILED:
		return "failed";
	default:
		return "unavailable";
	}
}

static const char* MotorStartResultReasonToString(MotorStartResultCode_t code) {
	switch (code) {
	case MOTOR_START_CONFIRMED:
		return "none";
	case MOTOR_START_I2C_LOCK_TIMEOUT:
		return "i2cLockTimeout";
	case MOTOR_START_STATUS_READ_FAILED:
		return "statusReadFailed";
	case MOTOR_START_PROTOCOL_MISMATCH:
		return "protocolMismatch";
	case MOTOR_START_ALREADY_BUSY:
		return "motorBoardBusy";
	case MOTOR_START_SENSOR_BLOCKED:
		return "sensorBlocked";
	case MOTOR_START_COMMAND_WRITE_FAILED:
		return "commandWriteFailed";
	case MOTOR_START_NO_BUSY_CONFIRMATION:
		return "noBusyConfirmation";
	case MOTOR_START_COMMAND_NOT_BOUND:
		return "commandNotBound";
	case MOTOR_START_OUTPUT_NOT_ACTIVE:
		return "outputNotActive";
	default:
		return "noBusyConfirmation";
	}
}

#define VEND_FLOW_TRACE_CAPACITY 32U
typedef struct {
	char line[128];
} VendFlowTraceRecord_t;
static VendFlowTraceRecord_t vendFlowTrace[VEND_FLOW_TRACE_CAPACITY];
static uint8_t vendFlowTraceHead;
static uint8_t vendFlowTraceTail;
static uint8_t vendFlowTraceCount;
static uint32_t vendFlowTraceDropCount;

/* Decision paths only append to bounded RAM. UART transmission is serviced
 * later, so diagnostics cannot delay a payment decision or a motor START. */
static void SendVendFlowDiagnostic(const char *eventName,
		uint16_t selectionId, uint16_t priceMinor, int8_t motorIndex,
		uint32_t transactionId, const char *reason, uint8_t managerState) {
	VendFlowTraceRecord_t *record;
	uint8_t deliveryLine = motorIndex >= 0 ? (uint8_t)motorIndex + 1U : 0U;
	(void)managerState;
 const bool rejection=eventName&&(strcmp(eventName,"managerRejected")==0||strcmp(eventName,"paymentRejected")==0||strcmp(eventName,"mdbRejected")==0||strcmp(eventName,"motorRejected")==0);
 if(rejection||(eventName&&strcmp(eventName,"paymentIgnored")==0))
  F411RecordReason(reason,transactionId,rejection&&reason&&strcmp(reason,"machineCancelled")!=0&&strcmp(reason,"motorNotReady")!=0);
	if (vendFlowTraceCount >= VEND_FLOW_TRACE_CAPACITY) {
		vendFlowTraceDropCount++;
		return;
	}
	record = &vendFlowTrace[vendFlowTraceTail];

	snprintf(record->line, sizeof(record->line),
			"CONTROLLER_DIAGNOSTIC,source=vendFlow,event=%s,line=%u,count=%u,status=r%s-t%lu-p%u",
			eventName != NULL ? eventName : "decision",
			(unsigned)deliveryLine, (unsigned)selectionId,
			reason != NULL ? reason : "unknown",
			(unsigned long)transactionId, (unsigned)priceMinor);
	vendFlowTraceTail = (uint8_t)((vendFlowTraceTail + 1U)
			% VEND_FLOW_TRACE_CAPACITY);
	vendFlowTraceCount++;
}

static void ServiceVendFlowDiagnostics(void) {
	char overflow[128];

	if (vendFlowTraceCount != 0U) {
		if (QueueUsbTxLine(vendFlowTrace[vendFlowTraceHead].line)) {
			vendFlowTraceHead = (uint8_t)((vendFlowTraceHead + 1U)
					% VEND_FLOW_TRACE_CAPACITY);
			vendFlowTraceCount--;
		}
		return;
	}
	if (vendFlowTraceDropCount != 0U) {
		snprintf(overflow, sizeof(overflow),
				"CONTROLLER_DIAGNOSTIC,source=vendFlow,event=logOverflow,line=0,count=%lu,status=rdiagnosticQueueFull",
				(unsigned long)vendFlowTraceDropCount);
		if (QueueUsbTxLine(overflow)) {
			vendFlowTraceDropCount = 0U;
		}
	}
}

static void SendMotorSnapshotDiagnostic(const char *eventName,
		uint8_t deliveryLine, const MotorControllerSnapshot_t *snapshot) {
	char usbLine[192];
	MotorControllerSnapshot_t unavailable = {
			.status = MOTOR_STATUS_UNAVAILABLE
	};
	const MotorControllerSnapshot_t *data = snapshot != NULL
			? snapshot : &unavailable;

	snprintf(usbLine, sizeof(usbLine),
			"CONTROLLER_DIAGNOSTIC,source=f103,event=%s,line=%u,count=%u,status=v%u-s%u-c%u-l%u-p%u-f%02X-r%02X-e%u-x%04X-t%u",
			eventName, (unsigned)deliveryLine,
			(unsigned)data->commandCounter,
			(unsigned)data->protocolVersion,
			(unsigned)data->status,
			(unsigned)data->commandCounter,
			(unsigned)data->deliveryLine,
			(unsigned)data->motorPhase,
			(unsigned)data->flags,
			(unsigned)data->resetFlags,
			(unsigned)data->i2cErrorCount,
			(unsigned)data->i2cLastError,
			(unsigned)data->turns);
	UsbSendLine(usbLine);
}

static void SendMotorStartFailureIoDiagnostic(const MotorStartResult_t *result,
		uint8_t deliveryLine) {
	char usbLine[128];

	if (result == NULL || result->code == MOTOR_START_CONFIRMED) {
		return;
	}

	snprintf(usbLine, sizeof(usbLine),
			"CONTROLLER_DIAGNOSTIC,source=f103,event=mIo,line=%u,count=%u,status=c%uw%u.%lXr%u.%lXa%uz%ue%lXo%lX",
			(unsigned)deliveryLine, (unsigned)result->expectedCommandCounter,
			(unsigned)result->code, (unsigned)result->writeStatus,
			(unsigned long)result->writeError,
			(unsigned)result->lastReadStatus,
			(unsigned long)result->lastReadError,
			(unsigned)result->confirmationReads,
			(unsigned)result->lastReadWireComplete,
			(unsigned long)result->expectedOperationId,
			(unsigned long)result->snapshot.operationId);
	UsbSendLine(usbLine);
}

/* Idempotent keyed completion is safe to retry; START is still single-shot. */
static void SendDetectedDropCompletion(void) {
    bool detected = MainDropDetected(activeMotorBootIdentity, activeMotorOperationId);
    if ((!detected && !MainDropBlocked()) || osMutexAcquire(i2cMutexHandle, 0U) != osOK) return;
    uint8_t command[20] = {0xA6U, MOTOR_STATUS_PROTOCOL_VERSION, activeMotorDeliveryLine,
        detected ? MOTOR_STATUS_SUCCESS : MOTOR_STATUS_FAILED};
    WireWrite32(command + 4, activeMotorBootIdentity);
    WireWrite32(command + 8, activeMotorOperationId);
    WireWrite32(command + 16, WireCrc32(command, 16));
    (void)HAL_I2C_Master_Transmit(&DISPLAY_I2C_HANDLE, MOTOR_CONTROLLER_I2C_ADDR,
                                command, sizeof(command), 50U);
    osMutexRelease(i2cMutexHandle);
}

static bool PollActiveMotorControllerResult(event_t *event) {
	MotorControllerSnapshot_t snapshot = {
			.status = MOTOR_STATUS_UNAVAILABLE
	};
	uint32_t now = HAL_GetTick();

	if (event == NULL || !activeMotorResultTracked
			|| (currentState != STATE_VENDING
					&& currentState != STATE_DIRECT_VENDING)
			|| (int32_t)(now - nextMotorResultPollTick) < 0) {
		return false;
	}

	SendDetectedDropCompletion();
	if (!ReadMotorControllerSnapshotLocked(&snapshot, 50U)
			|| snapshot.protocolVersion != MOTOR_STATUS_PROTOCOL_VERSION
			|| snapshot.bootIdentity != activeMotorBootIdentity || snapshot.operationId != activeMotorOperationId
			|| snapshot.commandCounter != activeMotorCommandCounter
			|| snapshot.deliveryLine != activeMotorDeliveryLine) {
		nextMotorResultPollTick = HAL_GetTick()
				+ MOTOR_RESULT_POLL_INTERVAL_MS;
		return false;
	}
	nextMotorResultPollTick = HAL_GetTick() + MOTOR_RESULT_POLL_INTERVAL_MS;

	if (snapshot.status != MOTOR_STATUS_SUCCESS
			&& snapshot.status != MOTOR_STATUS_FAILED) {
		return false;
	}
	if (snapshot.motorPhase != 0U
			|| (snapshot.flags & MOTOR_STATUS_FLAG_OUTPUT_ACTIVE) != 0U) {
		return false;
	}

	SendMotorSnapshotDiagnostic(
			snapshot.status == MOTOR_STATUS_SUCCESS
					? "sensorSuccess" : "motorFailed",
			activeMotorDeliveryLine, &snapshot);
	memset(event, 0, sizeof(*event));
	event->source = SRC_MOTOR;
	event->payload.motor_status = snapshot.status;
	ResetActiveMotorResultTracking();
	return true;
}

static void SendDirectDeliveryStartResult(uint32_t operationId, uint8_t motorIndex, bool started,
                                          uint8_t f103Status, const char *reason) {
    char usbLine[128];

    snprintf(usbLine, sizeof(usbLine),
             "DELIVERY_START_RESULT,line=%u,status=%s,f103Status=%s,reason=%s,op=%lu",
             (unsigned)motorIndex, started ? "started" : "failed",
             MotorControllerStatusToString(f103Status), reason, (unsigned long)operationId);
    UsbSendLine(usbLine);
}

static void SendDirectDeliveryResult(uint32_t operationId, uint8_t motorIndex, bool success) {
    char usbLine[64];
    MotorControllerSnapshot_t snapshot = {.status = MOTOR_STATUS_UNAVAILABLE};

    (void)ReadMotorControllerSnapshotLocked(&snapshot, 250U);
    SendMotorSnapshotDiagnostic(success ? "resultOk" : "resultFail", motorIndex, &snapshot);
    snprintf(usbLine, sizeof(usbLine), "DELIVERY_RESULT,line=%u,status=%s,op=%lu",
             (unsigned)motorIndex, success ? "success" : "failed", (unsigned long)operationId);
    UsbSendLine(usbLine);
    SendDirectDeliveryStatus(success ? "UART DELIV OK" : "UART DELIV FAIL", motorIndex);
}

static void ResetTransactionState(void) {
    snackButtonTransaction = false; snackPaymentAvailable = false;
	ResetActiveMotorResultTracking();
	transactionActive = false;
	activeSelectionId = 0;
	activePriceMinor = 0;
	activeMdbTransactionId = 0U;
	activeMotorIndex = -1;
	localVendStatus = LOCAL_VEND_UNKNOWN;
	machineVendStatus = MACHINE_VEND_UNKNOWN;
	paymentCancelledByMachine = false;
	paymentDeadlineTick = 0;
	vendingDeadlineTick = 0;
	machineDeadlineTick = 0;
}

static void ResetDirectDeliveryState(void) {
	ResetActiveMotorResultTracking();
	directDeliveryActive = false;
	directDeliveryLine = 0;
    directDeliveryOperationId = 0;
}

static void BlockSelectionRetry(uint16_t selectionId, uint16_t priceMinor) {
	blockedSelectionId = selectionId;
	blockedPriceMinor = priceMinor;
	blockedSelectionUntilTick = HAL_GetTick() + SELECTION_RETRY_BLOCK_MS;
}

static bool IsSelectionRetryBlocked(uint16_t selectionId, uint16_t priceMinor) {
	if (blockedSelectionId != selectionId || blockedPriceMinor != priceMinor) {
		return false;
	}

	if ((int32_t)(blockedSelectionUntilTick - HAL_GetTick()) <= 0) {
		blockedSelectionId = 0;
		blockedPriceMinor = 0;
		blockedSelectionUntilTick = 0;
		return false;
	}

	return true;
}

static bool CanStartDirectDelivery(void) {
	return maintenancePhase < 2U && F411_GetBootIdentity() != 0U && (currentState == STATE_IDLE || currentState == STATE_INIT)
			&& !usbTxCriticalDeliveryResultPending && !vendReportPending;
}

static bool IsDeliveryLineReady(uint8_t motorIndex,
		MotorControllerSnapshot_t *snapshot) {
	if (motorIndex == 0 || motorIndex > 10) {
		return false;
	}

	if (!CanStartDirectDelivery()) {
		return false;
	}

	if (!ReadMotorControllerSnapshotLocked(snapshot, 250U)) {
		return false;
	}
	return !MainDropBlocked() && snapshot->protocolVersion == MOTOR_STATUS_PROTOCOL_VERSION
			&& snapshot->status != MOTOR_STATUS_BUSY
			&& (snapshot->flags & MOTOR_STATUS_FLAG_STORAGE_ERROR) == 0U;
}

static bool IsSelectionReadyForPayment(uint16_t selectionId) {
	int8_t motorIndex = FindMotorBySelectionId(selectionId);
	MotorControllerSnapshot_t snapshot = {
			.status = MOTOR_STATUS_UNAVAILABLE
	};

	/* Coffee selections do not use the snack motor board. */
	return motorIndex < 0
			|| IsDeliveryLineReady((uint8_t)motorIndex + 1U, &snapshot);
}

static void SendDeliveryLineCheckResult(uint8_t motorIndex, bool ready) {
	char usbLine[80];

	snprintf(usbLine, sizeof(usbLine),
			"DELIVERY_LINE_CHECK_RESULT,line=%u,status=%s",
			(unsigned)motorIndex,
			ready ? "ready" : "invalidDeliveryLine");
	UsbSendLine(usbLine);
	SendDirectDeliveryStatus(ready ? "UART LINE READY" : "UART LINE BAD",
			motorIndex);
}

static void HandleDeliveryLineCheck(uint8_t motorIndex) {
	MotorControllerSnapshot_t snapshot = {
			.status = MOTOR_STATUS_UNAVAILABLE
	};
	bool ready = IsDeliveryLineReady(motorIndex, &snapshot);

	SendMotorSnapshotDiagnostic("lineCheck", motorIndex, &snapshot);
	SendDeliveryLineCheckResult(motorIndex, ready);
}

static const char* ManagerEventSourceToString(EventSource_t source) {
	switch (source) {
	case SRC_USB:
		return "usb";
	case SRC_MOTOR:
		return "motor";
	case SRC_TIMER:
		return "timer";
	case SRC_MDB:
		return "mdb";
	case SRC_KEYPAD:
		return "keypad";
	case SRC_SD:
		return "sd";
	default:
		return "system";
	}
}

static uint8_t ManagerEventCode(const event_t *event) {
	if (event->source == SRC_USB) {
		return event->payload.usb.code;
	}
	if (event->source == SRC_MDB) {
		return event->payload.mdb.code;
	}
	if (event->source == SRC_TIMER) {
		return event->payload.timer_id;
	}
	return 0U;
}

static uint8_t ManagerEventLine(const event_t *event) {
	if (event->source == SRC_USB
			&& (event->payload.usb.code == USB_EVT_DELIVERY_REQUEST
					|| event->payload.usb.code == USB_EVT_DELIVERY_LINE_CHECK)) {
		return event->payload.usb.motor_index;
	}
	return 0U;
}

static void SendManagerQueueDiagnostic(bool dropped, uint32_t count,
		const event_t *event) {
	char usbLine[128];
	uint8_t line = ManagerEventLine(event);

	snprintf(usbLine, sizeof(usbLine),
			"CONTROLLER_DIAGNOSTIC,source=eventQueue,event=%u,line=%u,count=%lu,status=%s",
			(unsigned)ManagerEventCode(event), (unsigned)line,
			(unsigned long)count, dropped ? "dropped" : "fallback");
	UsbSendLine(usbLine);
	SendStatusUpdate(dropped ? "EVENT Q DROP" : "EVENT Q FALLBACK",
			ManagerEventSourceToString(event->source));
	if (dropped && event->source == SRC_MDB
			&& event->payload.mdb.code == MDB_EVT_SELECTION_REQUEST) {
		SendVendFlowDiagnostic("managerRejected",
				event->payload.mdb.item_id,
				event->payload.mdb.price_minor,
				FindMotorBySelectionId(event->payload.mdb.item_id),
				event->payload.mdb.transaction_id, "managerQueueFull",
				(uint8_t)currentState);
	}

	if (!dropped || event->source != SRC_USB) {
		return;
	}

	if (event->payload.usb.code == USB_EVT_DELIVERY_REQUEST) {
		SendDirectDeliveryStartResult(event->payload.usb.transaction_id, line, false,
				MOTOR_STATUS_UNAVAILABLE, "controllerQueueFull");
		SendDirectDeliveryResult(event->payload.usb.transaction_id, line, false);
	} else if (event->payload.usb.code == USB_EVT_DELIVERY_LINE_CHECK) {
		SendDeliveryLineCheckResult(line, false);
	}
}

static void ServiceManagerQueueDiagnostics(void) {
	event_t event;
	uint32_t count;

	if (TakeManagerQueueDiagnostic(false, &count, &event)) {
		SendManagerQueueDiagnostic(false, count, &event);
	}
	if (TakeManagerQueueDiagnostic(true, &count, &event)) {
		SendManagerQueueDiagnostic(true, count, &event);
	}
}

static void SendControllerBootDiagnostic(void) {
	char usbLine[128];

	snprintf(usbLine, sizeof(usbLine),
			"CONTROLLER_DIAGNOSTIC,source=f411,event=boot,line=0,count=%u,status=%s-r%02X",
			(unsigned)F411_GetResetFlags(), F411_GetResetReason(),
			(unsigned)F411_GetResetFlags());
	UsbSendLine(usbLine);
	UsbSendLine("CONTROLLER_DIAGNOSTIC,source=f411,event=build,line=0,count=1,status=esp-async-v1");
}

static void SendControllerStorageDiagnostic(void) {
	char usbLine[128];

	snprintf(usbLine, sizeof(usbLine),
			"CONTROLLER_DIAGNOSTIC,source=f411,event=sd,line=0,count=%u,status=%s",
			(unsigned)sd_debug_status,
			sd_available ? "available" : "unavailable");
	UsbSendLine(usbLine);
}

static void ServiceControllerHealthDiagnostic(void) {
	static uint32_t lastHealthTick = 0U;
	uint32_t now = HAL_GetTick();
	char usbLine[128];

	if ((now - lastHealthTick) < CONTROLLER_HEALTH_INTERVAL_MS) {
		return;
	}
	lastHealthTick = now;
	snprintf(usbLine, sizeof(usbLine),
			"CONTROLLER_DIAGNOSTIC,source=f411,event=health,line=%u,count=%lu,status=%s-r%02X",
			(unsigned)currentState, (unsigned long)(now / 1000U),
			F411_GetResetReason(), (unsigned)F411_GetResetFlags());
	UsbSendLine(usbLine);
}

static void EnterWaitMachineState(void) {
	currentState = STATE_WAIT_MACHINE;
	machineDeadlineTick = HAL_GetTick() + MACHINE_RESULT_TIMEOUT_MS;
}

static uint32_t GetManagerWaitTimeoutMs(void) {
	uint32_t now = HAL_GetTick();
	uint32_t deadline = 0;

	if (currentState == STATE_PAYMENT) {
		deadline = paymentDeadlineTick;
	} else if (currentState == STATE_VENDING
			|| currentState == STATE_DIRECT_VENDING) {
		deadline = vendingDeadlineTick;
	} else if (currentState == STATE_WAIT_MACHINE) {
		deadline = machineDeadlineTick;
	} else {
		return MANAGER_SERVICE_INTERVAL_MS;
	}

	if (deadline == 0U) {
		return osWaitForever;
	}

	if ((int32_t)(deadline - now) <= 0) {
		return 0U;
	}
	return (deadline - now) > MANAGER_SERVICE_INTERVAL_MS
			? MANAGER_SERVICE_INTERVAL_MS : (deadline - now);
}

static uint32_t ClampManagerWaitForMotorPoll(uint32_t waitTimeoutMs) {
	uint32_t now;
	uint32_t pollWaitMs;

	if (!activeMotorResultTracked
			|| (currentState != STATE_VENDING
					&& currentState != STATE_DIRECT_VENDING)) {
		return waitTimeoutMs;
	}

	now = HAL_GetTick();
	if ((int32_t)(nextMotorResultPollTick - now) <= 0) {
		return 0U;
	}
	pollWaitMs = nextMotorResultPollTick - now;
	return waitTimeoutMs == osWaitForever || pollWaitMs < waitTimeoutMs
			? pollWaitMs : waitTimeoutMs;
}

static bool BuildManagerTimeoutEvent(event_t *event) {
	if (event == NULL) {
		return false;
	}

	memset(event, 0, sizeof(*event));
	event->source = SRC_TIMER;

	if (currentState == STATE_PAYMENT) {
		event->payload.timer_id = TIMER_ID_PAYMENT;
		return true;
	}

	if (currentState == STATE_VENDING) {
		event->payload.timer_id = TIMER_ID_VENDING;
		return true;
	}

	if (currentState == STATE_DIRECT_VENDING) {
		event->payload.timer_id = TIMER_ID_VENDING;
		return true;
	}

	if (currentState == STATE_WAIT_MACHINE) {
		event->payload.timer_id = TIMER_ID_MACHINE;
		return true;
	}

	return false;
}

static void PublishMdbPaymentResult(uint8_t resultCode,
		uint32_t transactionId, uint16_t selectionId) {
    if (transactionId & 0x80000000U) return; /* No VMC transaction for a button. */
	uint32_t primask = __get_PRIMASK();
	uint32_t nextSequence;

	__disable_irq();
	nextSequence = mdbPaymentMailbox.sequence + 1U;
	if (nextSequence == 0U) {
		nextSequence = 1U;
	}
	mdbPaymentMailbox.resultCode = resultCode;
	mdbPaymentMailbox.selectionId = selectionId;
	mdbPaymentMailbox.transactionId = transactionId;
	__DMB();
	mdbPaymentMailbox.sequence = nextSequence;
	RestoreInterruptState(primask);
}

static void FinalizeVendReportIfReady(void) {
	char usbLine[96];

	if (!transactionActive) {
		return;
	}

	if (localVendStatus == LOCAL_VEND_UNKNOWN
			|| machineVendStatus == MACHINE_VEND_UNKNOWN) {
		return;
	}

	snprintf(usbLine, sizeof(usbLine), "VEND_REPORT,id=%u,machine=%s,local=%s,tx=%lu,boot=%lu",
			activeSelectionId, MachineVendStatusToString(machineVendStatus),
			LocalVendStatusToString(localVendStatus), (unsigned long)activeMdbTransactionId,
			(unsigned long)F411_GetBootIdentity());
	if (vendReportPending) return;
	strncpy(vendReportOutbox, usbLine, sizeof(vendReportOutbox) - 1U);
	vendReportTransactionId = activeMdbTransactionId;
	vendReportPending = true;
	vendReportRetryTick = HAL_GetTick();

	ResetTransactionState();
	currentState = STATE_IDLE;
	SendDisplayUpdate(SCENE_IDLE, NULL, 0);
}

static void MarkPaymentCancelledByMachine(void) {
	bool firstCancel = !paymentCancelledByMachine;
 if(firstCancel)F411RecordReason("machineCancelled",activeMdbTransactionId,false);

	paymentCancelledByMachine = true;
	if (activeMdbTransactionId != 0U && activeMdbTransactionId == machineResultTransactionId) {
		__DMB();
		machineVendStatus = machineResultCode == MDB_EVT_MACHINE_VEND_SUCCESS
				? MACHINE_VEND_SUCCESS : MACHINE_VEND_FAILURE;
	} else if (machineVendStatus == MACHINE_VEND_UNKNOWN) {
		machineVendStatus = MACHINE_VEND_CANCELLED;
	}

	if (localVendStatus == LOCAL_VEND_UNKNOWN) {
		localVendStatus =
				(activeMotorIndex >= 0) ? LOCAL_VEND_FAILURE : LOCAL_VEND_SKIPPED;
	}

	if (firstCancel) {
		char line[64];
		snprintf(line, sizeof(line), "ORDER_CANCELLED,tx=%lu,boot=%lu",
				(unsigned long)activeMdbTransactionId, (unsigned long)F411_GetBootIdentity());
		UsbSendLine(line);
	}
}

static void DropCancelledPaymentWait(const char *statusLine) {
	SendUsbTransactionStatus(statusLine, activeSelectionId, activePriceMinor,
			activeMotorIndex);
	FinalizeVendReportIfReady();
}

static void UpdateMachineVendStatus(MdbEventCode_t code) {
	if (code == MDB_EVT_MACHINE_VEND_SUCCESS) {
		machineVendStatus = MACHINE_VEND_SUCCESS;
	} else if (code == MDB_EVT_MACHINE_VEND_FAILURE) {
		machineVendStatus = MACHINE_VEND_FAILURE;
	} else if (code == MDB_EVT_MACHINE_CANCELLED) {
		machineVendStatus = MACHINE_VEND_CANCELLED;
	} else if (code == MDB_EVT_MACHINE_SESSION_COMPLETE
			&& machineVendStatus == MACHINE_VEND_UNKNOWN) {
		machineVendStatus = MACHINE_VEND_SESSION_COMPLETE;
	}
}

static bool IsMdbTransactionEvent(const event_t *event) {
	if (event == NULL || event->source != SRC_MDB) {
		return false;
	}

	return event->payload.mdb.code == MDB_EVT_MACHINE_VEND_SUCCESS
			|| event->payload.mdb.code == MDB_EVT_MACHINE_VEND_FAILURE
			|| event->payload.mdb.code == MDB_EVT_MACHINE_SESSION_COMPLETE
			|| event->payload.mdb.code == MDB_EVT_MACHINE_CANCELLED;
}

static bool MdbEventMatchesActiveTransaction(const event_t *event) {
	return transactionActive && activeMdbTransactionId != 0U
			&& event->payload.mdb.transaction_id == activeMdbTransactionId;
}

static void ReportStaleMdbManagerEvent(const event_t *event) {
	char usbLine[128];

	snprintf(usbLine, sizeof(usbLine),
			"CONTROLLER_DIAGNOSTIC,source=mdbManager,event=stale,line=%u,count=%lu,status=expected-%lu",
			(unsigned)event->payload.mdb.code,
			(unsigned long)event->payload.mdb.transaction_id,
			(unsigned long)activeMdbTransactionId);
	UsbSendLine(usbLine);
}

static void StartVendSession(uint16_t selectionId, uint16_t priceMinor,
		uint32_t transactionId) {
	char usbLine[96];
	ItemConfig_t paymentSceneItem;
	bool usbSent = false;

 F411RecoveryAdmitted(true);
	transactionActive = true;
	activeSelectionId = selectionId;
	activePriceMinor = priceMinor;
	activeMdbTransactionId = transactionId;
	activeMotorIndex = FindMotorBySelectionId(selectionId);
	localVendStatus =
			(activeMotorIndex >= 0) ? LOCAL_VEND_UNKNOWN : LOCAL_VEND_SKIPPED;
	machineVendStatus = MACHINE_VEND_UNKNOWN;

	FillItemFromMinor(&paymentSceneItem, priceMinor);
	paymentSceneItem.selection_id = selectionId;
	SendDisplayUpdate(SCENE_PAYMENT, &paymentSceneItem, 0);

	snprintf(usbLine, sizeof(usbLine), "PAYMENT_REQUEST,id=%u,price=%u,tx=%lu,line=%u,boot=%lu",
			selectionId, priceMinor, (unsigned long)transactionId,
			(unsigned)(activeMotorIndex < 0 ? 0 : activeMotorIndex + 1),
			(unsigned long)F411_GetBootIdentity());
	usbSent = UsbSendLine(usbLine);
	SendUsbTransactionStatus(usbSent ? "UART TX PAY QUEUED" :
			UsbTxStatusToDisplayLine(lastUsbTxStatus),
			selectionId, priceMinor, activeMotorIndex);
}

static bool StartSnackButtonPayment(uint8_t key) {
    if (key < 1U || key > 10U || !CanStartDirectDelivery() ||
        wirePaymentTransactionId != 0U || nextSnackTransactionId == UINT32_MAX) return false;
    const ItemConfig_t *item = &currentConfig.items[key - 1U];
    uint16_t price = item->price_x * 1000U + item->price_y * 100U + item->price_z * 10U + item->price_p;
    if (item->selection_id == 0xFFFFU || price == 0U ||
        FindMotorBySelectionId(item->selection_id) != key - 1 ||
        IsSelectionRetryBlocked(item->selection_id, price)) return false;
    if (!IsSelectionReadyForPayment(item->selection_id)) {
        SendVendFlowDiagnostic("managerRejected",item->selection_id,price,key-1,nextSnackTransactionId+1U,"motorNotReady",(uint8_t)currentState);
        return false;
    }
    snackButtonTransaction = true; snackPaymentAvailable = true;
    paymentDeadlineTick = HAL_GetTick() + PAYMENT_WAIT_TIMEOUT_MS;
    StartVendSession(item->selection_id, price, ++nextSnackTransactionId);
    currentState = STATE_PAYMENT;
    osTimerStart(paymentTimerHandle, PAYMENT_WAIT_TIMEOUT_MS);
    return true;
}

static void ServiceSnackButtonBoard(void) {
    static uint32_t nextPoll, sequence;
    static bool online, armed;
    uint32_t now = HAL_GetTick();
    bool ready = CanStartDirectDelivery() && wirePaymentTransactionId == 0U;
    if (!ready) armed = false;
    if ((int32_t)(now - nextPoll) < 0) return;
    nextPoll = now + 100U;
    if (osMutexAcquire(i2cMutexHandle, 0U) != osOK) return;
    uint8_t frame[12];
    HAL_StatusTypeDef status = HAL_I2C_Master_Receive(&DISPLAY_I2C_HANDLE, 0x10U << 1,
                                                    frame, sizeof(frame), 30U);
    osMutexRelease(i2cMutexHandle);
    if (status != HAL_OK || frame[0] != 0xB1U || frame[1] != 1U ||
        WireRead32(frame + 8) != WireCrc32(frame, 8)) {
        online = armed = false; nextPoll = HAL_GetTick() + 1000U; return;
    }
    uint16_t mask = (uint16_t)frame[2] | ((uint16_t)frame[3] << 8);
    uint32_t incoming = WireRead32(frame + 4);
    if (!online) { online = true; sequence = incoming; armed = false; }
    bool changed = incoming != sequence;
    sequence = incoming; /* Busy presses are consumed, never queued for later payment. */
    if (!mask) { armed = ready; return; }
    if (!armed || !changed || !ready || mask > 0x3ffU || (mask & (mask - 1U))) return;
    armed = false;
    uint8_t key = 1; while ((mask >>= 1U) != 0U) ++key;
    (void)StartSnackButtonPayment(key);
}

static void HandleUsbMapUpdate(uint8_t motorIndex, uint16_t selectionId) {
	char usbLine[96];
	bool queued;

	if (motorIndex == 0 || motorIndex > 10) {
		UsbSendLine("SET_MAP_RESULT,error");
		SendUsbMapStatus("UART RX MAP BAD", motorIndex, selectionId);
		return;
	}

	for (uint8_t i=0; i<10; i++) {
		if (i != motorIndex-1U && currentConfig.items[i].selection_id == selectionId)
			currentConfig.items[i].selection_id = 0xFFFFU;
	}
	currentConfig.items[motorIndex - 1U].selection_id = selectionId;
	queued = SaveConfigToSD();

	snprintf(usbLine, sizeof(usbLine),
			"SET_MAP_RESULT,ok,motor=%u,id=%u,storage=%s",
			motorIndex, selectionId,
			(sd_available && queued) ? "ready" : "unavailable");
	UsbSendLine(usbLine);
	SendUsbMapStatus("UART RX MAP OK", motorIndex, selectionId);
}

static void StartDirectDelivery(uint8_t motorIndex, uint32_t operationId) {
    MotorStartResult_t startResult;

    if (motorIndex == 0 || motorIndex > 10 || operationId == 0) {
        SendDirectDeliveryStartResult(operationId, motorIndex, false, MOTOR_STATUS_UNAVAILABLE,
                                      "invalidLine");
        SendDirectDeliveryResult(operationId, motorIndex, false);
        return;
    }

    F411RecoveryAdmitted(false);
    directDeliveryActive = true;
    directDeliveryLine = motorIndex;
    directDeliveryOperationId = operationId;
    SendDirectDeliveryStatus("UART DELIV REQ", motorIndex);
    SendDisplayUpdate(SCENE_VEND, NULL, 0);

    startResult = StartMotorConfirmed(motorIndex, 1U, NULL, operationId);
    SendMotorSnapshotDiagnostic(startResult.code == MOTOR_START_CONFIRMED ? "startOk" : "startFail",
                                motorIndex, &startResult.snapshot);
    SendMotorStartFailureIoDiagnostic(&startResult, motorIndex);
    if (startResult.code == MOTOR_START_CONFIRMED) {
        TrackActiveMotorResult(&startResult.snapshot);
        SendDirectDeliveryStartResult(operationId, motorIndex, true, startResult.f103Status,
                                      "none");
        vendingDeadlineTick = HAL_GetTick() + VENDING_TIMEOUT_MS;
        osTimerStart(vendingTimerHandle, VENDING_TIMEOUT_MS);
        currentState = STATE_DIRECT_VENDING;
        return;
    }

    WriteLog(0x02, 0, 0, 0, 0);
    SendDisplayUpdate(SCENE_ERROR, NULL, 1);
    SendDirectDeliveryStartResult(operationId, motorIndex, false, startResult.f103Status,
                                  MotorStartResultReasonToString(startResult.code));
    SendDirectDeliveryResult(operationId, motorIndex, false);
    ResetDirectDeliveryState();
    currentState = STATE_IDLE;
}

/* Read-only recovery. This path must never call StartMotorConfirmed or transmit START. */
static void HandleDirectDeliveryQuery(uint8_t line, uint32_t operationId) {
    MotorControllerSnapshot_t snapshot = {.status = MOTOR_STATUS_UNAVAILABLE};
    if (!operationId || line < 1U || line > 10U ||
        !ReadMotorControllerSnapshotLocked(&snapshot, 50U) ||
        snapshot.protocolVersion != MOTOR_STATUS_PROTOCOL_VERSION ||
        (snapshot.flags & MOTOR_STATUS_FLAG_STORAGE_ERROR))
        return;
    bool ownsActive = directDeliveryActive && directDeliveryLine == line &&
                      directDeliveryOperationId == operationId;
    if (snapshot.directOperationId != operationId || snapshot.deliveryLine != line ||
        !snapshot.bootIdentity || !snapshot.operationId) {
        // No matching durable acceptance exists; do not create an operation during recovery.
        if (!ownsActive)
            SendDirectDeliveryResult(operationId, line, false);
        return;
    }
    if ((snapshot.status == MOTOR_STATUS_SUCCESS || snapshot.status == MOTOR_STATUS_FAILED) &&
        snapshot.motorPhase == 0U && !(snapshot.flags & MOTOR_STATUS_FLAG_OUTPUT_ACTIVE)) {
        SendDirectDeliveryResult(operationId, line, snapshot.status == MOTOR_STATUS_SUCCESS);
        if (ownsActive) {
            osTimerStop(vendingTimerHandle);
            ResetDirectDeliveryState();
            currentState = STATE_IDLE;
        }
    } else if (snapshot.status == MOTOR_STATUS_BUSY) {
        SendDirectDeliveryStartResult(operationId, line, true, snapshot.status, "none");
        if (!directDeliveryActive && (currentState == STATE_IDLE || currentState == STATE_INIT)) {
            directDeliveryActive = true;
            directDeliveryLine = line;
            directDeliveryOperationId = operationId;
            TrackActiveMotorResult(&snapshot);
            vendingDeadlineTick = HAL_GetTick() + VENDING_TIMEOUT_MS;
            osTimerStart(vendingTimerHandle, VENDING_TIMEOUT_MS);
            currentState = STATE_DIRECT_VENDING;
        }
    }
}

static bool ParseUint32Decimal(const char *text, size_t length, uint32_t *value) {
    uint32_t n = 0;
    if (!text || !value || length == 0)
        return false;
    for (size_t i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9')
            return false;
        uint32_t digit = (uint32_t)(text[i] - '0');
        if (n > (UINT32_MAX - digit) / 10U)
            return false;
        n = n * 10U + digit;
    }
    *value = n;
    return true;
}

static bool ParseInternalKey(const char *text, uint32_t *tx, uint32_t *boot) {
    const char *separator = strstr(text, ",boot=");
    return separator && ParseUint32Decimal(text, (size_t)(separator - text), tx) &&
           ParseUint32Decimal(separator + 6, strlen(separator + 6), boot) && *tx != 0U &&
           *boot != 0U;
}

static bool ParseUint8Value(const char *text, uint8_t *value) {
    uint32_t n;
    if (!ParseUint32Decimal(text, strlen(text), &n) || n > UINT8_MAX)
        return false;
    *value = (uint8_t)n;
    return true;
}

static bool ParseUint16Value(const char *text, uint16_t *value) {
    uint32_t n;
    if (!ParseUint32Decimal(text, strlen(text), &n) || n > UINT16_MAX)
        return false;
    *value = (uint16_t)n;
    return true;
}

/* Manager-only maintenance: no MDB bit timing, ISR work or motor START. */
static void MaintenanceMessage(const char *kind, const char *reason) {
    char line[112];
    if (reason) snprintf(line,sizeof(line),"%s,token=%lu,reason=%s",kind,(unsigned long)maintenanceToken,reason);
    else if(strcmp(kind,"REBOOT_FORCE_CONTROLLER_READY")==0)
        snprintf(line,sizeof(line),"%s,token=%lu,boot=%lu",kind,(unsigned long)maintenanceToken,(unsigned long)F411_GetBootIdentity());
    else snprintf(line,sizeof(line),"%s,token=%lu",kind,(unsigned long)maintenanceToken);
    (void)UsbTrySendLine(line);
}
static bool MaintenanceTokenParse(const char *line,const char *prefix,uint32_t *token) {
    size_t n=strlen(prefix);
    return strncmp(line,prefix,n)==0 && ParseUint32Decimal(line+n,strlen(line+n),token) && *token!=0U;
}
/* Explicit two-board profile: no I2C/F103 maintenance requests. */
static void MaintenanceService(void) {
    if (!maintenancePhase) return;
    uint32_t now=HAL_GetTick();
    if (maintenanceResetPending) {
        if (EspLinkTxIdle() || (int32_t)(now-maintenanceResetDeadline)>=0) {
            __DSB();NVIC_SystemReset();
        }
        return;
    }
    if ((uint32_t)(now-maintenanceSince)>=30000U) {
        MaintenanceMessage("REBOOT_FAILED","controllerAckTimeout");
        maintenancePhase=0U;__DSB();NVIC_SystemReset();return;
    }
    if ((int32_t)(now-maintenanceNext)<0) return;
    maintenanceNext=now+500U;
    MaintenanceMessage("REBOOT_FORCE_CONTROLLER_READY",NULL);
}

static bool MaintenanceHandleLine(const char *line) {
 uint32_t rebootToken;
 if(MaintenanceTokenParse(line,"REBOOT_FORCE_CONTROLLER_BEGIN,token=",&rebootToken)) {
   if (!maintenancePhase) {
     maintenanceForced=true;MainIoStopForMaintenance();
     osTimerStop(paymentTimerHandle);osTimerStop(vendingTimerHandle);
     maintenanceToken=rebootToken;maintenancePhase=3U;maintenanceSince=HAL_GetTick();maintenanceNext=maintenanceSince;
   }
   else if(maintenanceToken==rebootToken && maintenancePhase==3U) MaintenanceMessage("REBOOT_FORCE_CONTROLLER_READY",NULL);
   return true;
 }
 if(MaintenanceTokenParse(line,"REBOOT_FORCE_CONTROLLER_ACK,token=",&rebootToken)) {
   if(maintenancePhase==3U && rebootToken==maintenanceToken && !maintenanceResetPending) {
      MaintenanceMessage("REBOOT_BLACK_READY",NULL);
      maintenanceResetPending=true;maintenanceResetDeadline=HAL_GetTick()+1000U;
   }
   return true;
 }
 return false;
}

void UsbCdcProcessLine(const char *line) {
	event_t event = { 0 };
	char buffer[64];
	char *token1 = NULL;
	char *token2 = NULL;

	if (eventQueueHandle == NULL || line == NULL || *line == '\0') {
		return;
	}

 if(MaintenanceHandleLine(line)) return;
 if(strcmp(line,"F411_QUERY")==0){SendF411State();return;}
	if (strncmp(line, "VEND_ACK,tx=", 12U) == 0) {
        uint32_t tx, boot;
        if (ParseInternalKey(line + 12, &tx, &boot) && boot == F411_GetBootIdentity()
                && vendReportPending && tx == vendReportTransactionId) vendReportPending = false;
        return;
    }

	/* The public terminal JSON is unchanged. Only the private UART carries tx. */
	const char *replyTx = strstr(line, ",tx=");
	if (replyTx != NULL && strncmp(line, "PAYMENT_", 8U) == 0) {
        uint32_t tx, boot;
		if (!ParseInternalKey(replyTx + 4, &tx, &boot) || boot != F411_GetBootIdentity()) return;
		if ((size_t)(replyTx - line) == 10U && strncmp(line, "PAYMENT_OK", 10U) == 0)
			event.payload.usb.code = USB_EVT_PAYMENT_APPROVED;
		else if ((size_t)(replyTx - line) == 12U && strncmp(line, "PAYMENT_FAIL", 12U) == 0)
			event.payload.usb.code = USB_EVT_PAYMENT_DENIED;
		else if ((size_t)(replyTx - line) == 15U && strncmp(line, "PAYMENT_TIMEOUT", 15U) == 0)
			event.payload.usb.code = USB_EVT_PAYMENT_TIMEOUT;
		else return;
		event.source = SRC_USB;
		event.payload.usb.transaction_id = (uint32_t)tx;
		if (QueueManagerEvent(&event)) {
			char ack[64];
			snprintf(ack, sizeof(ack), "PAYMENT_ACK,tx=%lu,boot=%lu", (unsigned long)tx, (unsigned long)boot);
			UsbSendLine(ack);
		}
		return;
	}

    bool directRequest = strncmp(line, "DELIVERY_REQUEST,line=", 22U) == 0;
    bool directQuery = strncmp(line, "DELIVERY_QUERY,line=", 20U) == 0;
    if (directRequest || directQuery) {
        const char *value = line + (directRequest ? 22U : 20U);
        const char *op = strstr(value, ",op=");
        uint32_t motor, operationId;
        if (!op || !ParseUint32Decimal(value, (size_t)(op-value), &motor)
                || motor < 1U || motor > 10U
                || !ParseUint32Decimal(op+4, strlen(op+4), &operationId) || operationId == 0U) return;
        event.source = SRC_USB;
        event.payload.usb.code = directRequest ? USB_EVT_DELIVERY_REQUEST : USB_EVT_DELIVERY_QUERY;
        event.payload.usb.motor_index = (uint8_t)motor;
        event.payload.usb.transaction_id = operationId;
        QueueManagerEvent(&event);
        return;
    }

	if (strncmp(line, "DELIVERY_LINE_CHECK,", 20) == 0) {
		strncpy(buffer, line + 20, sizeof(buffer) - 1U);
		buffer[sizeof(buffer) - 1U] = '\0';

		token1 = strtok(buffer, ",");
		if (token1 == NULL) {
			SendStatusUpdate("UART LINE BAD", line);
			return;
		}

		if (strncmp(token1, "line=", 5) == 0) {
			token1 += 5;
		} else if (strncmp(token1, "motor=", 6) == 0) {
			token1 += 6;
		}

		event.source = SRC_USB;
		event.payload.usb.code = USB_EVT_DELIVERY_LINE_CHECK;
		if (!ParseUint8Value(token1, &event.payload.usb.motor_index)) {
			SendStatusUpdate("UART LINE BAD", line);
			return;
		}

		QueueManagerEvent(&event);
		return;
	}

	if (strncmp(line, "SET_MAP,", 8) != 0) {
		SendStatusUpdate("UART RX UNKNOWN", line);
		return;
	}

	strncpy(buffer, line + 8, sizeof(buffer) - 1U);
	buffer[sizeof(buffer) - 1U] = '\0';

	token1 = strtok(buffer, ",");
	token2 = strtok(NULL, ",");
	if (token1 == NULL || token2 == NULL) {
		SendStatusUpdate("UART RX MAP BAD", line);
		return;
	}

	if (strncmp(token1, "motor=", 6) == 0) {
		token1 += 6;
	}
	if (strncmp(token2, "id=", 3) == 0) {
		token2 += 3;
	}

	event.source = SRC_USB;
	event.payload.usb.code = USB_EVT_SET_MAP;
	if (!ParseUint8Value(token1, &event.payload.usb.motor_index)
			|| !ParseUint16Value(token2, &event.payload.usb.item_id)) {
		SendStatusUpdate("UART RX MAP BAD", line);
		return;
	}

	QueueManagerEvent(&event);
}


static void SendF411State(void) {
 char line[128],reason[25];F411WireHealth wire;uint32_t reasonTx;
 uint32_t primask=__get_PRIMASK();__disable_irq();
 wire=f411WireHealth;memcpy(reason,f411LastReason,sizeof(reason));reasonTx=f411LastReasonTx;
 RestoreInterruptState(primask);
 snprintf(line,sizeof(line),"F411_STATE,b=%lu,s=%u,m=%u,t=%lu,p=%u,r=%s,x=%lu,c=%lu",
  (unsigned long)F411_GetBootIdentity(),(unsigned)currentState,(unsigned)wire.state,
  (unsigned long)activeMdbTransactionId,(unsigned)vendReportPending,reason,
  (unsigned long)reasonTx,(unsigned long)f411RecoveryCount);
 UsbSendLine(line);
}
static void ServiceF411Recovery(void) {
 uint32_t now=HAL_GetTick();F411WireHealth wire;uint32_t progress;uint8_t rejected;
 static int observedState=-1;static uint32_t stateSince,observedTx;
 if(observedState!=(int)currentState||observedTx!=activeMdbTransactionId){observedState=(int)currentState;observedTx=activeMdbTransactionId;stateSince=now;}
 const uint32_t stateAge=now-stateSince;
 const bool activeState=currentState==STATE_PAYMENT||currentState==STATE_VENDING||currentState==STATE_DIRECT_VENDING||currentState==STATE_WAIT_MACHINE;
 const bool workPending=activeState||vendReportPending||usbTxCriticalDeliveryResultPending;
 /* PAYMENT timeout -> pending report must not restart the same no-progress budget. */
 uint32_t primask=__get_PRIMASK();__disable_irq();
 wire=f411WireHealth;progress=f411Progress;rejected=f411RejectBurst;
 const uint32_t txProgress=EspLinkGetStats().txCompleted,workProgressMs=f411LastWorkProgressMs;
 const bool workTracked=f411WorkTracked,paymentBudget=f411PaymentBudget;
 now=HAL_GetTick(); /* Snapshot timestamps cannot be newer than this sample. */
 const bool recentReject=(uint32_t)(now-f411LastRejectMs)<=10000U;
 const uint32_t firstRejectMs=f411FirstRejectMs;
 RestoreInterruptState(primask);
 /* Preserve the payment admission budget through timeout -> pending report.
  * Successful paid/vend progress restores ordinary 55-second recovery. */
 const uint32_t workLimit=paymentBudget?PAYMENT_RECOVERY_TIMEOUT_MS:F411_RECOVERY_DELAY_MS;
 const bool protectedPayment=workTracked&&workPending&&paymentBudget&&(int32_t)(now-workProgressMs)<(int32_t)workLimit;
 const bool workStuck=workTracked&&workPending&&(int32_t)(now-workProgressMs)>=(int32_t)workLimit;
 const bool activeStuck=workStuck||(!workTracked&&activeState&&stateAge>=F411_RECOVERY_DELAY_MS);
 if(now-f411LastStateMs>=2000U){f411LastStateMs=now;SendF411State();}
 const bool wireLive=wire.setup&&wire.enabled&&(uint32_t)(now-wire.lastCommandMs)<=5000U;
 const bool cacheStuck=wireLive&&wire.seenSelection&&wire.cachePending;
 const bool rejectionLoop=wire.setup&&wire.seenSelection&&recentReject&&rejected>=3U&&(uint32_t)(now-wire.lastCommandMs)<=5000U;
 const bool blocked=vendReportPending||usbTxCriticalDeliveryResultPending||
  (wireLive&&wire.resetRequired)||cacheStuck||rejectionLoop;
 const bool wasArmed=f411RecoveryTimer.armed;
 bool due=F411RecoveryDue(&f411RecoveryTimer,now,blocked,progress);
 if(rejectionLoop&&!wasArmed&&f411RecoveryTimer.armed){f411RecoveryTimer.since=firstRejectMs;due=(int32_t)(now-firstRejectMs)>=(int32_t)F411_RECOVERY_DELAY_MS;}
 const bool txStuck=F411RecoveryDue(&f411TxRecoveryTimer,now,
  !EspLinkTxIdle()||usbTxOutboxCount!=0U||usbTxCriticalDeliveryResultPending||vendReportPending,txProgress);
 if(!protectedPayment&&(due||activeStuck||txStuck)) {
  char line[128];const char*reason=txStuck?"uartTxStuck":(activeStuck?"managerStateStuck":(vendReportPending?"vendReportPending":
   (usbTxCriticalDeliveryResultPending?"criticalResultPending":
   (wire.resetRequired?"mdbResetRequired":(cacheStuck?"mdbCacheStuck":"rejectedLoop")))));
  ++f411RecoveryCount;
  snprintf(line,sizeof(line),"F411_RECOVER,b=%lu,r=%s,t=%lu",(unsigned long)F411_GetBootIdentity(),reason,(unsigned long)activeMdbTransactionId);
  /* Best effort enqueue; immediate recovery reset can discard diagnostics.
   * Final outcomes remain the responsibility of the existing persistent journals. */
  (void)UsbTrySendLine(line);
  NVIC_SystemReset();
 }
}

void managerTask(void *argument) {
	event_t event;
	osStatus_t status;
	bool configEventReceived = false;
	uint32_t waitTimeoutMs = osWaitForever;

	osDelay(5000);
	SendStatusUpdate("MANAGER START", "UART INIT");
	osDelay(100);
	SendControllerBootDiagnostic();
	SendStatusUpdate("UART READY", "CFG WAIT");
	osDelay(100);
	configEventReceived = WaitForConfigLoad();
	SendControllerStorageDiagnostic();
	SendStatusUpdate(configEventReceived ? "CFG EVENT" : "CFG DEFAULT",
			"MDB WAIT");
	currentState = STATE_INIT;
	ResetTransactionState();

	for (;;) {
		F411_WatchdogServiceFromManager();
		ServiceEspInputLines();
  MaintenanceService();
  if(maintenanceForced){osDelay(1U);continue;}
  ServiceF411Recovery();
        ServiceSnackButtonBoard();
		if (!snackButtonTransaction && transactionActive && activeMdbTransactionId == machineResultTransactionId) {
			__DMB();
			UpdateMachineVendStatus((MdbEventCode_t)machineResultCode);
		}
		ServiceUsbTxOutbox();
		ServiceVendFlowDiagnostics();
		FinalizeVendReportIfReady();
		ServiceManagerQueueDiagnostics();
		ServiceControllerHealthDiagnostic();
		ServiceMdbTraceDiagnostics();
		ServiceMdbRxDiagnostics();
		if (GetManagerWaitTimeoutMs() == 0U && BuildManagerTimeoutEvent(&event)) {
			status = osOK;
		} else if (PollActiveMotorControllerResult(&event)) {
			status = osOK;
		} else {
			waitTimeoutMs = GetManagerWaitTimeoutMs();
			waitTimeoutMs = ClampManagerWaitForMotorPoll(waitTimeoutMs);
   if(waitTimeoutMs==osWaitForever||waitTimeoutMs>MANAGER_SERVICE_INTERVAL_MS)waitTimeoutMs=MANAGER_SERVICE_INTERVAL_MS;
			if (UsbTxOutboxPending()
					&& (waitTimeoutMs == osWaitForever
							|| waitTimeoutMs > USB_TX_OUTBOX_RETRY_MS)) {
				waitTimeoutMs = USB_TX_OUTBOX_RETRY_MS;
			}

			if (TryGetManagerEvent(&event)) {
				status = osOK;
			} else {
				status = osMessageQueueGet(eventQueueHandle, &event, NULL,
						waitTimeoutMs);
			}
		}

		if (status == osErrorTimeout) {
			if (GetManagerWaitTimeoutMs() != 0U) {
				continue;
			}
			if (!BuildManagerTimeoutEvent(&event)) {
				continue;
			}
		} else if (status != osOK) {
			if (GetManagerWaitTimeoutMs() != 0U
					|| !BuildManagerTimeoutEvent(&event)) {
				continue;
			}
		}

		if (event.source == SRC_USB &&
				(event.payload.usb.code == USB_EVT_PAYMENT_APPROVED ||
				 event.payload.usb.code == USB_EVT_PAYMENT_DENIED ||
				 event.payload.usb.code == USB_EVT_PAYMENT_TIMEOUT) &&
				(!transactionActive || event.payload.usb.transaction_id != activeMdbTransactionId)) {
			SendVendFlowDiagnostic("paymentIgnored", activeSelectionId,
					activePriceMinor, activeMotorIndex,
					event.payload.usb.transaction_id,
					transactionActive ? "transactionMismatch" : "noActiveTransaction",
					(uint8_t)currentState);
			continue;
		}

		/* Timer callbacks are wakeups only; state and absolute deadline own expiry. */
		if (event.source == SRC_TIMER) {
			if (GetManagerWaitTimeoutMs() != 0U || !BuildManagerTimeoutEvent(&event)) continue;
		}

			if (event.source == SRC_USB
					&& event.payload.usb.code == USB_EVT_SET_MAP) {
				HandleUsbMapUpdate(event.payload.usb.motor_index,
						event.payload.usb.item_id);
				continue;
			}

			if (event.source == SRC_USB
					&& event.payload.usb.code == USB_EVT_DELIVERY_LINE_CHECK) {
				HandleDeliveryLineCheck(event.payload.usb.motor_index);
				continue;
			}

            if (event.source == SRC_USB && event.payload.usb.code == USB_EVT_DELIVERY_QUERY) {
                HandleDirectDeliveryQuery(event.payload.usb.motor_index, event.payload.usb.transaction_id);
                continue;
            }
            if (event.source == SRC_USB && event.payload.usb.code == USB_EVT_DELIVERY_REQUEST) {
                if (directDeliveryActive && directDeliveryOperationId == event.payload.usb.transaction_id
                        && directDeliveryLine == event.payload.usb.motor_index) {
                    HandleDirectDeliveryQuery(event.payload.usb.motor_index, event.payload.usb.transaction_id);
                } else if (CanStartDirectDelivery()) {
                    StartDirectDelivery(event.payload.usb.motor_index, event.payload.usb.transaction_id);
                } else {
                    SendDirectDeliveryStartResult(event.payload.usb.transaction_id, event.payload.usb.motor_index,
                            false, MOTOR_STATUS_UNAVAILABLE, "controllerBusy");
                    SendDirectDeliveryResult(event.payload.usb.transaction_id, event.payload.usb.motor_index, false);
                }
                continue;
            }

                if (maintenancePhase>=2U && event.source==SRC_MDB && event.payload.mdb.code==MDB_EVT_SELECTION_REQUEST) {
                    PublishMdbPaymentResult(MDB_PAYMENT_RESULT_DENIED,event.payload.mdb.transaction_id,event.payload.mdb.item_id);
                    continue;
                }
				if (event.source == SRC_MDB
						&& event.payload.mdb.code == MDB_EVT_SELECTION_REQUEST
						&& currentState != STATE_IDLE) {
					SendVendFlowDiagnostic("managerRejected",
							event.payload.mdb.item_id,
							event.payload.mdb.price_minor,
							FindMotorBySelectionId(event.payload.mdb.item_id),
							event.payload.mdb.transaction_id,
							"controllerBusy", (uint8_t)currentState);
					PublishMdbPaymentResult(MDB_PAYMENT_RESULT_DENIED,
							event.payload.mdb.transaction_id,
							event.payload.mdb.item_id);
					BlockSelectionRetry(event.payload.mdb.item_id,
							event.payload.mdb.price_minor);
					SendUsbTransactionStatus("MDB BUSY DENY",
							event.payload.mdb.item_id,
							event.payload.mdb.price_minor,
							FindMotorBySelectionId(event.payload.mdb.item_id));
					continue;
				}

				if (event.source == SRC_MDB
						&& event.payload.mdb.code == MDB_EVT_RESET) {
                    if (snackButtonTransaction) continue;
					osTimerStop(paymentTimerHandle);
					if (currentState == STATE_DIRECT_VENDING) continue;
					if (transactionActive) {
						MarkPaymentCancelledByMachine();
						if (currentState == STATE_VENDING) {
							localVendStatus = LOCAL_VEND_UNKNOWN;
							continue;
						}
						FinalizeVendReportIfReady();
					}
					if (!transactionActive) currentState = STATE_IDLE;
					continue;
				}

				if (snackButtonTransaction && IsMdbTransactionEvent(&event)) continue;
				if (transactionActive && IsMdbTransactionEvent(&event)
						&& !MdbEventMatchesActiveTransaction(&event)) {
					ReportStaleMdbManagerEvent(&event);
					continue;
				}

		switch (currentState) {
		case STATE_INIT:
			if (event.source == SRC_MDB
					&& event.payload.mdb.code == MDB_EVT_READY) {
				currentState = STATE_IDLE;
				SendDisplayUpdate(SCENE_IDLE, NULL, 0);
			}
			break;

			case STATE_IDLE:
				if (event.source == SRC_MDB
						&& event.payload.mdb.code == MDB_EVT_SELECTION_REQUEST) {
					if (vendReportPending || usbTxCriticalDeliveryResultPending || !WirePaymentPermitted(event.payload.mdb.transaction_id)
							|| IsSelectionRetryBlocked(event.payload.mdb.item_id,
								event.payload.mdb.price_minor)) {
						const char *rejectReason = vendReportPending
								? "vendReportPending"
								: (usbTxCriticalDeliveryResultPending
										? "criticalResultPending"
										: (!WirePaymentPermitted(event.payload.mdb.transaction_id)
												? "paymentWindowExpired" : "duplicateWindow"));
						SendVendFlowDiagnostic("managerRejected",
								event.payload.mdb.item_id,
								event.payload.mdb.price_minor,
								FindMotorBySelectionId(event.payload.mdb.item_id),
								event.payload.mdb.transaction_id,
								rejectReason, (uint8_t)currentState);
						PublishMdbPaymentResult(MDB_PAYMENT_RESULT_DENIED,
								event.payload.mdb.transaction_id,
								event.payload.mdb.item_id);
						SendUsbTransactionStatus("MDB DUP DENY",
								event.payload.mdb.item_id,
								event.payload.mdb.price_minor,
								FindMotorBySelectionId(event.payload.mdb.item_id));
					} else if (!IsSelectionReadyForPayment(
							event.payload.mdb.item_id)) {
						SendVendFlowDiagnostic("managerRejected",
								event.payload.mdb.item_id,
								event.payload.mdb.price_minor,
								FindMotorBySelectionId(event.payload.mdb.item_id),
								event.payload.mdb.transaction_id,
								"motorNotReady", (uint8_t)currentState);
						PublishMdbPaymentResult(MDB_PAYMENT_RESULT_DENIED,
								event.payload.mdb.transaction_id,
								event.payload.mdb.item_id);
						BlockSelectionRetry(event.payload.mdb.item_id,
								event.payload.mdb.price_minor);
						SendUsbTransactionStatus("MDB SENSOR DENY",
								event.payload.mdb.item_id,
								event.payload.mdb.price_minor,
								FindMotorBySelectionId(event.payload.mdb.item_id));
					} else {
						StartVendSession(event.payload.mdb.item_id,
								event.payload.mdb.price_minor,
								event.payload.mdb.transaction_id);
						currentState = STATE_PAYMENT;
						paymentDeadlineTick = wirePaymentDeadlineTick;
						osTimerStart(paymentTimerHandle, PAYMENT_WAIT_TIMEOUT_MS);
					}
				}
				break;

			case STATE_PAYMENT:
				if (event.source == SRC_USB
						&& event.payload.usb.code == USB_EVT_PAYMENT_APPROVED) {
					osTimerStop(paymentTimerHandle);
					if (paymentCancelledByMachine || !WirePaymentPermitted(activeMdbTransactionId)) {
						SendVendFlowDiagnostic("paymentRejected", activeSelectionId,
								activePriceMinor, activeMotorIndex,
								activeMdbTransactionId,
								paymentCancelledByMachine ? "machineCancelled"
										: "paymentWindowExpired",
								(uint8_t)currentState);
						MarkPaymentCancelledByMachine();
						SendDisplayUpdate(SCENE_ERROR, NULL, 3);
						DropCancelledPaymentWait("UART CANCL PAID");
						break;
					}

     F411RecoveryProgress();
					SendDisplayUpdate(SCENE_VEND, NULL, 0);
					SendUsbTransactionStatus("UART RX PAY OK", activeSelectionId,
							activePriceMinor, activeMotorIndex);

					if (activeMotorIndex >= 0) {
						MotorPaymentClaim_t paymentClaim = { 0 };
						if (!ClaimMotorPayment(activeMdbTransactionId,
								&paymentClaim)) {
							SendVendFlowDiagnostic("paymentRejected",
									activeSelectionId, activePriceMinor,
									activeMotorIndex, activeMdbTransactionId,
									"claimLost", (uint8_t)currentState);
							MarkPaymentCancelledByMachine();
							SendDisplayUpdate(SCENE_ERROR, NULL, 3);
							DropCancelledPaymentWait("MDB CLAIM LOST");
							break;
						}
						/* Commit the paid local vend before telling the VMC not to
						 * dispense. SESSION COMPLETE caused by that VEND_DENIED must
						 * close the MDB side without revoking this one-shot claim. */
						PublishMdbPaymentResult(MDB_PAYMENT_RESULT_DENIED,
								activeMdbTransactionId, activeSelectionId);
						BlockSelectionRetry(activeSelectionId, activePriceMinor);
						machineVendStatus = MACHINE_VEND_CANCELLED;
						MotorStartResult_t normalStart = StartMotorConfirmed(
								(uint8_t)activeMotorIndex + 1U, 1U, &paymentClaim, 0U);
						SendMotorSnapshotDiagnostic(
								normalStart.code == MOTOR_START_CONFIRMED
										? "vendStartOk" : "vendStartFail",
								(uint8_t)activeMotorIndex + 1U,
								&normalStart.snapshot);
						SendMotorStartFailureIoDiagnostic(
								&normalStart, (uint8_t)activeMotorIndex + 1U);
						if (normalStart.code != MOTOR_START_CONFIRMED) {
							SendVendFlowDiagnostic("motorRejected",
									activeSelectionId, activePriceMinor,
									activeMotorIndex, activeMdbTransactionId,
									MotorStartResultReasonToString(normalStart.code),
									(uint8_t)currentState);
						}
						if (normalStart.code == MOTOR_START_CONFIRMED) {
							TrackActiveMotorResult(&normalStart.snapshot);
							vendingDeadlineTick = HAL_GetTick()
									+ VENDING_TIMEOUT_MS;
							osTimerStart(vendingTimerHandle, VENDING_TIMEOUT_MS);
							currentState = STATE_VENDING;
						} else {
							localVendStatus = LOCAL_VEND_FAILURE;
							WriteLog(0x02, activeSelectionId, 0, 0, 0);
							SendDisplayUpdate(SCENE_ERROR, NULL, 1);
							EnterWaitMachineState();
							FinalizeVendReportIfReady();
						}
					} else {
						PublishMdbPaymentResult(MDB_PAYMENT_RESULT_APPROVED,
								activeMdbTransactionId, activeSelectionId);
						EnterWaitMachineState();
					}
				} else if (event.source == SRC_USB
						&& (event.payload.usb.code == USB_EVT_PAYMENT_DENIED
								|| event.payload.usb.code == USB_EVT_PAYMENT_TIMEOUT)) {
					osTimerStop(paymentTimerHandle);
					SendVendFlowDiagnostic("paymentRejected", activeSelectionId,
							activePriceMinor, activeMotorIndex,
							activeMdbTransactionId,
							event.payload.usb.code == USB_EVT_PAYMENT_TIMEOUT
									? "terminalTimeout" : "terminalDenied",
							(uint8_t)currentState);
					if (paymentCancelledByMachine) {
						DropCancelledPaymentWait(
								(event.payload.usb.code == USB_EVT_PAYMENT_TIMEOUT) ?
										"UART CANCL TMO" : "UART CANCL FAIL");
						break;
					}

					BlockSelectionRetry(activeSelectionId, activePriceMinor);
					PublishMdbPaymentResult(
							(event.payload.usb.code == USB_EVT_PAYMENT_TIMEOUT) ?
									MDB_PAYMENT_RESULT_TIMEOUT :
									MDB_PAYMENT_RESULT_DENIED,
							activeMdbTransactionId, activeSelectionId);
					SendDisplayUpdate(SCENE_ERROR, NULL, 3);
					SendUsbTransactionStatus(
							(event.payload.usb.code == USB_EVT_PAYMENT_TIMEOUT) ?
									"UART RX TIMEOUT" : "UART RX PAY FAIL",
							activeSelectionId, activePriceMinor, activeMotorIndex);
					ResetTransactionState();
					currentState = STATE_IDLE;
					SendDisplayUpdate(SCENE_IDLE, NULL, 0);
				} else if (event.source == SRC_TIMER
						&& event.payload.timer_id == TIMER_ID_PAYMENT) {
					osTimerStop(paymentTimerHandle);
					SendVendFlowDiagnostic("paymentRejected", activeSelectionId,
							activePriceMinor, activeMotorIndex,
							activeMdbTransactionId, "managerTimeout",
							(uint8_t)currentState);
					if (paymentCancelledByMachine) {
						DropCancelledPaymentWait("UART CANCL TMO");
						break;
					}

					BlockSelectionRetry(activeSelectionId, activePriceMinor);
					PublishMdbPaymentResult(MDB_PAYMENT_RESULT_TIMEOUT,
							activeMdbTransactionId, activeSelectionId);
					SendDisplayUpdate(SCENE_ERROR, NULL, 3);
					SendUsbTransactionStatus("UART WAIT TMO", activeSelectionId,
							activePriceMinor, activeMotorIndex);
					ResetTransactionState();
					currentState = STATE_IDLE;
					SendDisplayUpdate(SCENE_IDLE, NULL, 0);
				} else if (event.source == SRC_MDB
						&& (event.payload.mdb.code == MDB_EVT_MACHINE_CANCELLED
								|| event.payload.mdb.code
										== MDB_EVT_MACHINE_SESSION_COMPLETE)) {
					MarkPaymentCancelledByMachine();
					SendUsbTransactionStatus("MDB CANCEL WAIT", activeSelectionId,
							activePriceMinor, activeMotorIndex);
				}
				break;

			case STATE_VENDING:
				if (event.source == SRC_MOTOR
						&& event.payload.motor_status == MOTOR_STATUS_SUCCESS) {
					osTimerStop(vendingTimerHandle);
					WriteLog(0x01, activeSelectionId, 0, 0, 0);
					localVendStatus = LOCAL_VEND_SUCCESS;
					SendDisplayUpdate(SCENE_ERROR, NULL, 5);
					EnterWaitMachineState();
					FinalizeVendReportIfReady();
				} else if (event.source == SRC_MOTOR
						&& event.payload.motor_status == MOTOR_STATUS_FAILED) {
					osTimerStop(vendingTimerHandle);
					WriteLog(0x02, activeSelectionId, 0, 0, 0);
					localVendStatus = LOCAL_VEND_FAILURE;
					SendDisplayUpdate(SCENE_ERROR, NULL, 1);
					EnterWaitMachineState();
					FinalizeVendReportIfReady();
				} else if (event.source == SRC_TIMER
						&& event.payload.timer_id == TIMER_ID_VENDING) {
					ResetActiveMotorResultTracking();
					WriteLog(0x02, activeSelectionId, 0, 0, 0);
					localVendStatus = LOCAL_VEND_FAILURE;
					SendDisplayUpdate(SCENE_ERROR, NULL, 1);
					EnterWaitMachineState();
					FinalizeVendReportIfReady();
				} else if (event.source == SRC_MDB) {
					UpdateMachineVendStatus(event.payload.mdb.code);
					FinalizeVendReportIfReady();
				}
				break;

			case STATE_WAIT_MACHINE:
				if (event.source == SRC_MDB) {
					UpdateMachineVendStatus(event.payload.mdb.code);
					FinalizeVendReportIfReady();
				} else if (event.source == SRC_TIMER
						&& event.payload.timer_id == TIMER_ID_MACHINE) {
					if (machineVendStatus == MACHINE_VEND_UNKNOWN) {
						machineVendStatus = MACHINE_VEND_FAILURE;
					}
					if (localVendStatus == LOCAL_VEND_UNKNOWN) {
						localVendStatus = LOCAL_VEND_FAILURE;
					}
					FinalizeVendReportIfReady();
					if (currentState == STATE_WAIT_MACHINE)
						machineDeadlineTick = HAL_GetTick() + USB_TX_OUTBOX_RETRY_MS;
				}
				break;

			case STATE_DIRECT_VENDING:
				if (event.source == SRC_MOTOR
						&& event.payload.motor_status == MOTOR_STATUS_SUCCESS) {
					osTimerStop(vendingTimerHandle);
					WriteLog(0x01, 0, 0, 0, 0);
					SendDisplayUpdate(SCENE_ERROR, NULL, 5);
					SendDirectDeliveryResult(directDeliveryOperationId, directDeliveryLine, true);
					ResetDirectDeliveryState();
					currentState = STATE_IDLE;
					SendDisplayUpdate(SCENE_IDLE, NULL, 0);
				} else if (event.source == SRC_MOTOR
						&& event.payload.motor_status == MOTOR_STATUS_FAILED) {
					osTimerStop(vendingTimerHandle);
					WriteLog(0x02, 0, 0, 0, 0);
					SendDisplayUpdate(SCENE_ERROR, NULL, 1);
					SendDirectDeliveryResult(directDeliveryOperationId, directDeliveryLine, false);
					ResetDirectDeliveryState();
					currentState = STATE_IDLE;
					SendDisplayUpdate(SCENE_IDLE, NULL, 0);
				} else if (event.source == SRC_TIMER
						&& event.payload.timer_id == TIMER_ID_VENDING) {
					ResetActiveMotorResultTracking();
					WriteLog(0x02, 0, 0, 0, 0);
					SendDisplayUpdate(SCENE_ERROR, NULL, 1);
					SendDirectDeliveryResult(directDeliveryOperationId, directDeliveryLine, false);
					ResetDirectDeliveryState();
					currentState = STATE_IDLE;
					SendDisplayUpdate(SCENE_IDLE, NULL, 0);
				}
				break;

			default:
				currentState = STATE_IDLE;
				SendDisplayUpdate(SCENE_IDLE, NULL, 0);
				break;
		}
	}
}

void keypadTask(void *argument) {
	for (;;) {
		/* Lock is serviced independently by MainIoTick. */
		osDelay(250);
	}
}

// --- Константы протокола MDB Cashless Device ---
#define MDB_ADDRESS_MASK             0xF8
#define MDB_COMMAND_MASK             0x07
#define MDB_ADDR_CASHLESS_1          0x10

#define MDB_CMD_RESET                0x00
#define MDB_CMD_SETUP                0x01
#define MDB_CMD_POLL                 0x02
#define MDB_CMD_VEND                 0x03
#define MDB_CMD_READER               0x04
#define MDB_CMD_REVALUE              0x05
#define MDB_CMD_EXPANSION            0x07

#define MDB_ACK                      0x00
#define MDB_RET                      0xAA
#define MDB_NAK                      0xFF

#define MDB_RESP_JUST_RESET          0x00
#define MDB_RESP_READER_CONFIG       0x01
#define MDB_RESP_BEGIN_SESSION       0x03
#define MDB_RESP_VEND_APPROVED       0x05
#define MDB_RESP_VEND_DENIED         0x06
#define MDB_RESP_END_SESSION         0x07
#define MDB_RESP_CANCELLED           0x08
#define MDB_RESP_PERIPHERAL_ID       0x09
#define MDB_RESP_COMMAND_OOS         0x0B
#define MDB_RESP_REVALUE_DENIED      0x0E
#define MDB_RESP_REVALUE_LIMIT       0x0F

#define MDB_READER_LEVEL             0x03
#define MDB_SCALE_FACTOR             0x01
#define MDB_DECIMAL_PLACES           0x02
#define MDB_REVALUE_LIMIT_AMOUNT     0x0000U
#define MDB_SESSION_FUNDS_AVAILABLE  3000U
#define MDB_SESSION_PAYMENT_MEDIA_ID 0x00000001UL
#define MDB_PAYMENT_TYPE_DEFAULT     0x00
#define MDB_PAYMENT_DATA_DEFAULT     0x0000U
#define MDB_APP_MAX_RESPONSE_TIME    100
#define MDB_MISC_OPTIONS             0x00
#define MDB_ADVERTISED_FEATURES      0x00

extern UART_HandleTypeDef huart2;

typedef struct {
	uint8_t bytes[40];
	uint8_t len;
} MdbFrame_t;

typedef enum {
	MDB_TRACE_RESET = 1,
	MDB_TRACE_SETUP,
	MDB_TRACE_READER,
	MDB_TRACE_VEND_REQUEST,
	MDB_TRACE_PAYMENT_RESULT,
	MDB_TRACE_POLL_RESPONSE,
	MDB_TRACE_VEND_CANCEL,
	MDB_TRACE_VEND_RESULT,
	MDB_TRACE_SESSION_COMPLETE,
	MDB_TRACE_OOS,
	MDB_TRACE_RESPONSE_RETRY,
	MDB_TRACE_ACK_MISSING,
	MDB_TRACE_STALE_RESULT,
	MDB_TRACE_QUEUE_FULL,
	MDB_TRACE_BAD_FRAME,
	MDB_TRACE_VEND_FLOW
} MdbTraceEvent_t;

typedef struct {
	uint32_t tick;
	uint32_t transactionId;
	uint16_t detail;
	uint8_t event;
	uint8_t sessionState;
	uint8_t command;
	uint8_t subCommand;
} MdbTraceRecord_t;

#define MDB_TRACE_RING_CAPACITY 64U
#define MDB_TRACE_DRAIN_PER_MANAGER_LOOP 4U

static MdbTraceRecord_t mdbTraceRing[MDB_TRACE_RING_CAPACITY];
static volatile uint8_t mdbTraceHead = 0U;
static volatile uint8_t mdbTraceTail = 0U;
static volatile uint32_t mdbTraceDropCount = 0U;

typedef struct {
	uint8_t bytes[40];
	uint8_t len;
	uint8_t retryCount;
	bool valid;
	bool awaitingAck;
	bool replayRequired;
	bool pollReplyPending;
	MdbPollReply_t pollReply;
} MdbResponseCache_t;

static MdbResponseCache_t mdbResponseCache;
static bool mdbPrefetchedWordValid = false;
static uint16_t mdbPrefetchedWord = 0U;
static uint8_t mdbNegotiatedLevel = 1U;
static uint32_t mdbBusActiveSinceTick;
static bool mdbBusActiveTracking;

#define MDB_DEBUG_NO_RX_PERIOD_MS 1000U
#define MDB_DEBUG_NO_RX_IDLE_MS   7000U
#define MDB_DEBUG_EVENT_PERIOD_MS 500U
#define MDB_POLL_STATUS_PERIOD_MS 1000U
#define MDB_FRAME_ISSUE_PERIOD_MS 1000U
#define MDB_ACK_WAIT_MS 5U
#define MDB_DATA_WAIT_MS 5U
#define MDB_VEND_RESULT_SUBCOMMAND_WAIT_MS 10U
#define MDB_COMMAND_RX_TIMEOUT_MS 600U
#define MDB_TASK_YIELD_DELAY_MS   1U
#define MDB_RX_TIMEOUT             0
#define MDB_RX_INVALID            (-1)
#define MDB_RX_ACK                (-2)
#define MDB_RX_NAK                (-3)
#define MDB_RX_RET                (-4)
#define MDB_RX_BUS_RESET          (-5)
#define MDB_BUS_RESET_ACTIVE_MS   100U
#define MDB_SOFT_UART_BAUD        9600U
#define MDB_SOFT_UART_DATA_BITS   9U
#define MDB_SOFT_RX_GPIO_PORT     GPIOA
#define MDB_SOFT_RX_GPIO_PIN      GPIO_PIN_3
#define MDB_SOFT_TX_GPIO_PORT     GPIOA
#define MDB_SOFT_TX_GPIO_PIN      GPIO_PIN_2

static bool mdbDebugEnabled = true;
static uint16_t mdbDebugLastWord = 0;
static uint8_t mdbDebugFrameErrorCount = 0;
static uint8_t mdbDebugNoiseErrorCount = 0;
static uint8_t mdbDebugOverrunErrorCount = 0;
static uint32_t mdbDebugLastNoRxTick = 0;
static uint32_t mdbDebugLastEventTick = 0;
static uint32_t mdbDebugLastFrameIssueTick = 0;
static uint32_t mdbDebugLastRxTick = 0;
static uint32_t mdbSoftBitCycles = 0;
static bool mdbSoftRxLastPhysicalHigh = false;
static bool mdbBusResetDetected = false;
static bool mdbBusResetLatched = false;

/* Diagnostic candidate: fixed RAM only in the MDB task. Formatting/USART
 * enqueue belongs exclusively to managerTask, never the bit sampler. */
enum { MDB_DIAG_COMMAND = 1, MDB_DIAG_ACK, MDB_DIAG_DATA };
enum { MDB_DIAG_ADDRESS = 1, MDB_DIAG_WORD, MDB_DIAG_FRAME,
    MDB_DIAG_INVALID, MDB_DIAG_ACK_WORD, MDB_DIAG_ACK_TIMEOUT,
    MDB_DIAG_NOISE, MDB_DIAG_STOP, MDB_DIAG_NOTIFY, MDB_DIAG_ORPHAN,
    MDB_DIAG_IDLE_TIMEOUT };
static MdbRxTrace mdbRxTrace;
static uint8_t mdbRxPhase = MDB_DIAG_COMMAND, mdbRxLength;
static uint32_t mdbRxTransaction;
static bool mdbRxDetail;

static void MdbRxRecord(uint8_t event, uint16_t word, uint8_t length) {
    MdbRxTraceRecord record = {0};
    record.tick = HAL_GetTick();
    record.cycles = DWT->CYCCNT;
    record.transactionId = mdbRxTransaction;
    record.word = word;
    record.event = event;
    record.phase = mdbRxPhase;
    record.length = length;
    record.cacheFlags = (mdbResponseCache.valid ? 1U : 0U)
            | (mdbResponseCache.awaitingAck ? 2U : 0U)
            | (mdbResponseCache.replayRequired ? 4U : 0U)
            | (mdbResponseCache.pollReplyPending ? 8U : 0U);
    MdbRxTracePush(&mdbRxTrace, record);
}
#define MDB_DIAG_RECORD(event, word, length) MdbRxRecord(event, word, length)
#define MDB_DIAG_CONTEXT(phase, tx) do { \
    mdbRxPhase = (phase); mdbRxTransaction = (tx); \
    if ((phase) == MDB_DIAG_COMMAND || (phase) == MDB_DIAG_ACK) mdbRxLength = 0U; \
} while (0)
#define MDB_DIAG_BEGIN(word) do { \
    mdbRxLength = 1U; \
    mdbRxDetail = ((word) & 0x100U) && MdbIsCashlessAddress((uint8_t)(word)) \
            && MdbCommandCode((uint8_t)(word)) != MDB_CMD_POLL; \
    if (mdbRxDetail) MDB_DIAG_RECORD(MDB_DIAG_ADDRESS, word, 1U); \
} while (0)
#define MDB_DIAG_DATA_WORD(word, length) do { \
    mdbRxLength = (length); \
    if (mdbRxDetail) MDB_DIAG_RECORD(MDB_DIAG_WORD, word, length); \
} while (0)
#define MDB_DIAG_END(result, length, word) do { \
    if (mdbRxDetail) MDB_DIAG_RECORD((result) > 0 ? MDB_DIAG_FRAME : MDB_DIAG_INVALID, word, length); \
    mdbRxDetail = false; \
} while (0)
#define MDB_DIAG_ERROR(event, word) do { \
    MDB_DIAG_RECORD(event, word, mdbRxLength); \
} while (0)
#define MDB_DIAG_POLL() MdbRxTracePoll(&mdbRxTrace)

static void ServiceMdbRxDiagnostics(void) {
    static uint32_t lastDrainTick, lastCountersTick;
    static bool pending;
    static MdbRxTraceRecord record;
    char line[128];
    uint32_t now = HAL_GetTick();
    /* At most one enqueue attempt per 50 ms; retain record on UART backpressure.
     * Raw UART capture is required: ESP coalesces controller diagnostics. */
    if (now - lastDrainTick < 50U) return;
    lastDrainTick = now;
    if (now - lastCountersTick >= 5000U) {
        snprintf(line, sizeof(line),
                "CONTROLLER_DIAGNOSTIC,source=mdbRx,event=counters,line=0,count=%lu,status=drop%lu",
                (unsigned long)atomic_load_explicit(&mdbRxTrace.polls, memory_order_relaxed),
                (unsigned long)atomic_load_explicit(&mdbRxTrace.dropped, memory_order_relaxed));
        if (UsbSendLine(line)) lastCountersTick = now;
        return;
    }
    if (!pending) pending = MdbRxTraceTake(&mdbRxTrace, &record);
    if (!pending) return;
    snprintf(line, sizeof(line),
            "CONTROLLER_DIAGNOSTIC,source=mdbRx,event=rx,line=%u,count=%lu,status=%lX-%lX-%lX-e%u-w%03X-n%u-c%X",
            (unsigned)record.phase, (unsigned long)record.sequence,
            (unsigned long)record.tick, (unsigned long)record.cycles,
            (unsigned long)record.transactionId, (unsigned)record.event,
            (unsigned)record.word, (unsigned)record.length, (unsigned)record.cacheFlags);
    if (UsbSendLine(line)) pending = false;
}

static void MdbTraceRecordEvent(MdbTraceEvent_t event,
		const MdbCashlessFsm_t *fsm, uint8_t command, uint8_t subCommand,
		uint16_t detail) {
	uint8_t head = mdbTraceHead;
	uint8_t next = (uint8_t)((head + 1U) % MDB_TRACE_RING_CAPACITY);
	MdbTraceRecord_t *record;

	if (next == mdbTraceTail) {
		mdbTraceDropCount++;
		return;
	}

	record = &mdbTraceRing[head];
	record->tick = HAL_GetTick();
	record->transactionId = (fsm != NULL) ? fsm->currentTransactionId : 0U;
	record->detail = detail;
	record->event = (uint8_t)event;
	record->sessionState = (fsm != NULL) ? (uint8_t)fsm->sessionState : 0U;
	record->command = command;
	record->subCommand = subCommand;
	__DMB();
	mdbTraceHead = next;
}

static bool MdbTraceTake(MdbTraceRecord_t *record) {
	uint8_t tail = mdbTraceTail;

	if (record == NULL || tail == mdbTraceHead) {
		return false;
	}

	__DMB();
	*record = mdbTraceRing[tail];
	mdbTraceTail = (uint8_t)((tail + 1U) % MDB_TRACE_RING_CAPACITY);
	return true;
}

static const char *MdbTraceEventName(uint8_t event) {
	switch ((MdbTraceEvent_t)event) {
	case MDB_TRACE_RESET:
		return "reset";
	case MDB_TRACE_SETUP:
		return "setup";
	case MDB_TRACE_READER:
		return "reader";
	case MDB_TRACE_VEND_REQUEST:
		return "vendRequest";
	case MDB_TRACE_PAYMENT_RESULT:
		return "paymentResult";
	case MDB_TRACE_POLL_RESPONSE:
		return "pollResponse";
	case MDB_TRACE_VEND_CANCEL:
		return "vendCancel";
	case MDB_TRACE_VEND_RESULT:
		return "vendResult";
	case MDB_TRACE_SESSION_COMPLETE:
		return "sessionComplete";
	case MDB_TRACE_OOS:
		return "oos";
	case MDB_TRACE_RESPONSE_RETRY:
		return "responseRetry";
	case MDB_TRACE_ACK_MISSING:
		return "ackMissing";
	case MDB_TRACE_STALE_RESULT:
		return "staleResult";
	case MDB_TRACE_QUEUE_FULL:
		return "queueFull";
	case MDB_TRACE_BAD_FRAME:
		return "badFrame";
	case MDB_TRACE_VEND_FLOW:
		return "vendFlow";
	default:
		return "unknown";
	}
}

static const char *MdbFsmActionReason(uint8_t action) {
	if (action == 0xFEU) {
		return "badFrame";
	}
	switch ((MdbFsmAction_t)action) {
	case MDB_FSM_ACCEPTED:
		return "accepted";
	case MDB_FSM_DUPLICATE:
		return "duplicate";
	case MDB_FSM_COMPAT_CANCEL:
		return "compatCancel";
	case MDB_FSM_STALE:
		return "stale";
	case MDB_FSM_OUT_OF_SEQUENCE:
		return "outOfSequence";
	case MDB_FSM_ABORTED_OUT_OF_SEQUENCE:
		return "abortedOutOfSequence";
	case MDB_FSM_QUEUE_FULL:
		return "queueFull";
	default:
		return "unknown";
	}
}

static void ServiceMdbTraceDiagnostics(void) {
	MdbTraceRecord_t record;
	char usbLine[128];
	uint32_t dropped;
	uint32_t primask;

	for (uint8_t i = 0U; i < MDB_TRACE_DRAIN_PER_MANAGER_LOOP; i++) {
		if (!MdbTraceTake(&record)) {
			break;
		}
		if (record.event == MDB_TRACE_VEND_FLOW) {
			SendVendFlowDiagnostic(
					record.command == MDB_FSM_ACCEPTED
							? "mdbAccepted" : "mdbRejected",
					record.detail, 0U,
					record.subCommand == 0U
							? -1 : (int8_t)record.subCommand - 1,
					record.transactionId,
					MdbFsmActionReason(record.command), record.sessionState);
			continue;
		}
		snprintf(usbLine, sizeof(usbLine),
				"CONTROLLER_DIAGNOSTIC,source=mdb,event=%s,line=%u,count=%lu,status=c%02X-u%02X-d%04X-t%lu",
				MdbTraceEventName(record.event),
				(unsigned)record.sessionState,
				(unsigned long)record.transactionId,
				(unsigned)record.command,
				(unsigned)record.subCommand,
				(unsigned)record.detail,
				(unsigned long)record.tick);
		UsbSendLine(usbLine);
	}

	primask = __get_PRIMASK();
	__disable_irq();
	dropped = mdbTraceDropCount;
	mdbTraceDropCount = 0U;
	RestoreInterruptState(primask);
	if (dropped != 0U) {
		snprintf(usbLine, sizeof(usbLine),
				"CONTROLLER_DIAGNOSTIC,source=mdb,event=traceOverflow,line=0,count=%lu,status=dropped",
				(unsigned long)dropped);
		UsbSendLine(usbLine);
	}
}

static void MdbDebugSetEnabled(bool enabled) {
	mdbDebugEnabled = enabled;
	if (enabled) {
		mdbDebugLastNoRxTick = 0;
		mdbDebugLastEventTick = 0;
	}
}

static uint8_t MdbDebugDisplayCount(uint8_t value) {
	return (uint8_t)(value % 100U);
}

static char MdbDebugHexDigit(uint8_t value) {
	value &= 0x0FU;
	return (value < 10U) ? (char)('0' + value) : (char)('A' + value - 10U);
}

static char MdbDebugDecDigit(uint8_t value) {
	return (char)('0' + (value % 10U));
}

static void MdbDebugCountSoftFrameError(void) {
	mdbDebugFrameErrorCount++;
}

static void MdbDebugCountSoftNoiseError(void) {
	mdbDebugNoiseErrorCount++;
}

static void MdbDebugReport(const char *line1, const char *line2,
		bool throttle) {
	uint32_t now = HAL_GetTick();

	if (!mdbDebugEnabled) {
		return;
	}

	if (throttle
			&& (now - mdbDebugLastEventTick) < MDB_DEBUG_EVENT_PERIOD_MS) {
		return;
	}

	mdbDebugLastEventTick = now;
	SendStatusUpdate(line1, line2);
}

static void MdbDebugFormatWordLine(char line[17], uint16_t word) {
	uint16_t raw = word & 0x01FFU;

	memset(line, ' ', 16);
	line[16] = '\0';
	line[0] = 'W';
	line[1] = MdbDebugHexDigit((uint8_t)(raw >> 8));
	line[2] = MdbDebugHexDigit((uint8_t)(raw >> 4));
	line[3] = MdbDebugHexDigit((uint8_t)raw);
	line[5] = 'F';
	line[6] = MdbDebugDecDigit(mdbDebugFrameErrorCount);
	line[7] = 'N';
	line[8] = MdbDebugDecDigit(mdbDebugNoiseErrorCount);
	line[9] = 'O';
	line[10] = MdbDebugDecDigit(mdbDebugOverrunErrorCount);
}

static void MdbDebugReportWord(const char *line1, uint16_t word) {
	char line2[17];

	MdbDebugFormatWordLine(line2, word);
	MdbDebugReport(line1, line2, true);
}

static void MdbDebugReportNoRx(void) {
	char line2[17];
	uint32_t now = HAL_GetTick();

	if (!mdbDebugEnabled
			|| (now - mdbDebugLastNoRxTick) < MDB_DEBUG_NO_RX_PERIOD_MS) {
		return;
	}

	if (mdbDebugLastRxTick != 0U
			&& (now - mdbDebugLastRxTick) < MDB_DEBUG_NO_RX_IDLE_MS) {
		return;
	}

	mdbDebugLastNoRxTick = now;
	snprintf(line2, sizeof(line2), "RX0 F%02uN%02uO%02u",
			(unsigned)MdbDebugDisplayCount(mdbDebugFrameErrorCount),
			(unsigned)MdbDebugDisplayCount(mdbDebugNoiseErrorCount),
			(unsigned)MdbDebugDisplayCount(mdbDebugOverrunErrorCount));
	SendStatusUpdate("MDB NO RX", line2);
}

static void MdbDebugReportFrameIssue(const char *line1, uint8_t len,
		uint16_t word) {
	char line2[17];

	snprintf(line2, sizeof(line2), "L%02u W%03X", (unsigned)len,
			(unsigned)(word & 0x01FFU));
	MdbDebugReport(line1, line2, true);
}

static void MdbReportFrameIssueAlways(const char *line1, uint8_t len,
		uint16_t word) {
	char line2[17];
	uint32_t now = HAL_GetTick();

	if ((now - mdbDebugLastFrameIssueTick) < MDB_FRAME_ISSUE_PERIOD_MS) {
		return;
	}

	mdbDebugLastFrameIssueTick = now;
	snprintf(line2, sizeof(line2), "L%02u W%03X", (unsigned)len,
			(unsigned)(word & 0x01FFU));
	SendStatusUpdate(line1, line2);
}

static void MdbReportPollStatus(uint32_t pollCounter, bool readerEnabled,
		MdbSessionState_t sessionState,
		MdbPendingPollResponse_t pollResponse) {
	char line1[17];
	char line2[17] = "R0S0P0E00";
	uint8_t errorCount = MdbDebugDisplayCount(mdbDebugFrameErrorCount);

	snprintf(line1, sizeof(line1), "MDB POLL %05lu",
			(unsigned long)(pollCounter % 100000UL));
	line2[1] = readerEnabled ? '1' : '0';
	line2[3] = MdbDebugDecDigit((uint8_t)sessionState);
	line2[5] = MdbDebugDecDigit((uint8_t)pollResponse);
	line2[7] = MdbDebugDecDigit((uint8_t)(errorCount / 10U));
	line2[8] = MdbDebugDecDigit((uint8_t)errorCount);
	SendStatusUpdate(line1, line2);
}

#if MDB_HARDWARE_USART2
#include "mdb_uart2_transport.h"
#endif

static void MdbSoftUartConfigureRxPin(void) {
	GPIO_InitTypeDef GPIO_InitStruct = { 0 };

	__HAL_RCC_GPIOA_CLK_ENABLE();

	GPIO_InitStruct.Pin = MDB_SOFT_RX_GPIO_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	HAL_GPIO_Init(MDB_SOFT_RX_GPIO_PORT, &GPIO_InitStruct);

	CLEAR_BIT(huart2.Instance->CR1, USART_CR1_RE);
}

static void MdbSoftUartConfigureTxPin(void) {
	GPIO_InitTypeDef GPIO_InitStruct = { 0 };

	__HAL_RCC_GPIOA_CLK_ENABLE();

	HAL_GPIO_WritePin(MDB_SOFT_TX_GPIO_PORT, MDB_SOFT_TX_GPIO_PIN,
			GPIO_PIN_RESET);

	GPIO_InitStruct.Pin = MDB_SOFT_TX_GPIO_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(MDB_SOFT_TX_GPIO_PORT, &GPIO_InitStruct);

	CLEAR_BIT(huart2.Instance->CR1, USART_CR1_TE);
}

static void MdbSoftUartInit(void) {
#if MDB_HARDWARE_USART2
    MdbUart2Init();
#else
	MdbSoftUartConfigureRxPin();
	MdbSoftUartConfigureTxPin();

	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

	mdbSoftBitCycles = (SystemCoreClock + (MDB_SOFT_UART_BAUD / 2U))
			/ MDB_SOFT_UART_BAUD;
	if (mdbSoftBitCycles == 0U) {
		mdbSoftBitCycles = 1U;
	}
	mdbSoftRxLastPhysicalHigh =
			(MDB_SOFT_RX_GPIO_PORT->IDR & MDB_SOFT_RX_GPIO_PIN) != 0U;
#endif
}
static bool MdbSoftRxPhysicalHigh(void) {
	return (MDB_SOFT_RX_GPIO_PORT->IDR & MDB_SOFT_RX_GPIO_PIN) != 0U;
}

static uint8_t MdbSoftRxLogicalBit(void) {
	return MdbSoftRxPhysicalHigh() ? 0U : 1U;
}

static void MdbSoftWaitCycles(uint32_t targetCycle) {
	while ((int32_t)(DWT->CYCCNT - targetCycle) < 0) {
	}
}

static void MdbSoftTxPhysicalHigh(bool high) {
	if (high) {
		MDB_SOFT_TX_GPIO_PORT->BSRR = MDB_SOFT_TX_GPIO_PIN;
	} else {
		MDB_SOFT_TX_GPIO_PORT->BSRR = (uint32_t) MDB_SOFT_TX_GPIO_PIN << 16U;
	}
}

static void MdbSoftTxLogicalBit(uint8_t bit) {
	MdbSoftTxPhysicalHigh(bit == 0U);
}

static void MdbSoftRestoreIrq(uint32_t primask) {
	if ((primask & 1U) == 0U) {
		__enable_irq();
	}
}

static uint8_t calculate_mdb_chk(const uint8_t *buffer, uint16_t length) {
	uint8_t chk_sum = 0;

	for (uint16_t i = 0; i < length; i++) {
		chk_sum += buffer[i];
	}

	return chk_sum;
}

static bool MdbIsCashlessAddress(uint8_t addressByte) {
	uint8_t address = addressByte & MDB_ADDRESS_MASK;

	return address == MDB_ADDR_CASHLESS_1;
}

static uint8_t MdbCommandCode(uint8_t addressByte) {
	return addressByte & MDB_COMMAND_MASK;
}

static uint8_t MdbExpectedBytesAfterSubCommand(uint8_t command,
		uint8_t subCommand) {
	switch (command) {
	case MDB_CMD_SETUP:
		if (subCommand == 0x00 || subCommand == 0x01) {
			return 5U;
		}
		break;

	case MDB_CMD_VEND:
		if (subCommand == 0x00) {
			return 5U;
		}
		if (subCommand == 0x02) {
			return 3U;
		}
		if (subCommand == 0x05 || subCommand == 0x06) {
			return 5U;
		}
		if (subCommand == 0x01 || subCommand == 0x03
				|| subCommand == 0x04) {
			return 1U;
		}
		break;

	case MDB_CMD_READER:
		if (subCommand == 0x00 || subCommand == 0x01
				|| subCommand == 0x02) {
			return 1U;
		}
		break;

	case MDB_CMD_REVALUE:
		if (subCommand == 0x01) {
			return 1U;
		}
		if (subCommand == 0x00) {
			return 3U;
		}
		break;

	case MDB_CMD_EXPANSION:
		if (subCommand == 0x00) {
			return 30U;
		}
		if (subCommand == 0x04) {
			return 5U;
		}
		break;

	default:
		break;
	}

	return 0U;
}

static bool MdbNotifyManager(MdbEventCode_t code, uint16_t itemId,
		uint16_t priceMinor, uint32_t transactionId) {
	event_t event = { 0 };

	if (code == MDB_EVT_SELECTION_REQUEST) {
		wirePaymentDeadlineTick = HAL_GetTick() + PAYMENT_WAIT_TIMEOUT_MS;
		wirePaymentTransactionId = transactionId;
	} else if (code == MDB_EVT_RESET ||
			((code == MDB_EVT_MACHINE_CANCELLED || code == MDB_EVT_MACHINE_SESSION_COMPLETE)
			 && wirePaymentTransactionId == transactionId)) {
		wirePaymentTransactionId = 0U;
	}

	/* Preserve the definitive VMC outcome even if both event queues are full.
	 * SESSION COMPLETE must never overwrite SUCCESS/FAILURE in this latch. */
	if (code == MDB_EVT_MACHINE_VEND_SUCCESS || code == MDB_EVT_MACHINE_VEND_FAILURE) {
		machineResultCode = (uint8_t)code;
		__DMB();
		machineResultTransactionId = transactionId;
	}
	MDB_DIAG_CONTEXT(mdbRxPhase, transactionId);
	MDB_DIAG_RECORD(MDB_DIAG_NOTIFY, (uint16_t)code, 0U);
	event.source = SRC_MDB;
	event.payload.mdb.code = code;
	event.payload.mdb.item_id = itemId;
	event.payload.mdb.price_minor = priceMinor;
	event.payload.mdb.transaction_id = transactionId;
	return QueueManagerEvent(&event);
}

/* MDB task only. Protect a started command, never the idle bus wait.
 * Keep TIM11/SysTick enabled. UART RX DMA keeps filling its 4096-byte ring.
 * Defer only external handlers that can execute nontrivial HAL/USB work.
 * All paths release the exact saved NVIC enables and one scheduler nesting. */
#define MDB_RX_FRAME_BUDGET_MS 25U
static bool mdbRxAddressHandoffRequested;
static bool mdbRxFrameActive;
static bool mdbRxFrameBudgetExceeded;
static uint32_t mdbRxFrameStartedCycle;
static uint32_t mdbRxFrameSavedIrqs;
static uint32_t mdbRxFrameBudgetCycles;
static const IRQn_Type mdbRxDeferredIrqs[] = {
    USART1_IRQn, DMA2_Stream2_IRQn, OTG_FS_IRQn, EXTI15_10_IRQn
};

static void MdbRxFrameBegin(void) {
#if MDB_HARDWARE_USART2
    return;
#else
    if (!mdbRxAddressHandoffRequested || mdbRxFrameActive) return;
    /* Caller already holds PRIMASK while validating a start/consuming prefetch. */
    mdbRxFrameBudgetExceeded = false;
    mdbRxFrameStartedCycle = DWT->CYCCNT;
    mdbRxFrameBudgetCycles = (SystemCoreClock / 1000U) * MDB_RX_FRAME_BUDGET_MS;
    mdbRxFrameSavedIrqs = 0U;
    for (uint32_t i = 0U; i < 4U; ++i) {
        if (NVIC_GetEnableIRQ(mdbRxDeferredIrqs[i]))
            mdbRxFrameSavedIrqs |= 1UL << i;
        NVIC_DisableIRQ(mdbRxDeferredIrqs[i]);
    }
    vTaskSuspendAll();
    mdbRxFrameActive = true;
#endif
}
static bool MdbRxFrameExpired(void) {
#if MDB_HARDWARE_USART2
    return false;
#else
    return mdbRxFrameActive && (mdbRxFrameBudgetExceeded
        || (uint32_t)(DWT->CYCCNT - mdbRxFrameStartedCycle)
            >= mdbRxFrameBudgetCycles);
#endif
}
static void MdbRxFrameEnd(void) {
#if MDB_HARDWARE_USART2
    return;
#else
    if (!mdbRxFrameActive) return;
    mdbRxFrameActive = false;
    for (uint32_t i = 0U; i < 4U; ++i) {
        if (mdbRxFrameSavedIrqs & (1UL << i))
            NVIC_EnableIRQ(mdbRxDeferredIrqs[i]);
    }
    (void)xTaskResumeAll();
#endif
}
static bool MdbReadWord(uint16_t *word, uint32_t timeoutMs);

static void MdbSoftSendWord(uint16_t word) {
#if MDB_HARDWARE_USART2
    MdbUart2Send(&word, 1U);
#else
	uint32_t startCycle = DWT->CYCCNT;
	uint32_t primask = __get_PRIMASK();

	__disable_irq();

	MdbSoftTxLogicalBit(0U);

	for (uint8_t bit = 0; bit < MDB_SOFT_UART_DATA_BITS; bit++) {
		MdbSoftWaitCycles(startCycle
				+ ((uint32_t)(bit + 1U) * mdbSoftBitCycles));
		MdbSoftTxLogicalBit((uint8_t)((word >> bit) & 0x01U));
	}

	MdbSoftWaitCycles(startCycle
			+ ((MDB_SOFT_UART_DATA_BITS + 1U) * mdbSoftBitCycles));
	MdbSoftTxLogicalBit(1U);

	MdbSoftWaitCycles(startCycle
			+ ((MDB_SOFT_UART_DATA_BITS + 2U) * mdbSoftBitCycles));
	MdbSoftRestoreIrq(primask);
#endif
}
static void MdbSendWords(const uint16_t *words, uint8_t len) {
#if MDB_HARDWARE_USART2
    MdbUart2Send(words, len);
#else
	for (uint8_t i = 0; i < len; i++) {
		MdbSoftSendWord(words[i]);
	}
#endif
}
static void MdbSendAck(void) {
	uint16_t ack = (uint16_t)(MDB_ACK | 0x0100U);
	MdbSendWords(&ack, 1);
}

static void MdbSendNak(void) {
	uint16_t nak = (uint16_t)(MDB_NAK | 0x0100U);
	MdbSendWords(&nak, 1);
}

static void MdbCommitPollReply(MdbCashlessFsm_t *fsm, const MdbPollReply_t *reply) {
	if (MdbFsmAcknowledgePollReply(fsm, reply)
			&& (reply->type == MDB_POLL_VEND_APPROVED
					|| reply->type == MDB_POLL_VEND_DENIED)) {
		MdbFsmSetDeadline(fsm, HAL_GetTick(), MACHINE_RESULT_TIMEOUT_MS);
	}
}

static void MdbAcknowledgeCachedResponse(MdbCashlessFsm_t *fsm) {
	if (mdbResponseCache.pollReplyPending)
		MdbCommitPollReply(fsm, &mdbResponseCache.pollReply);
	memset(&mdbResponseCache, 0, sizeof(mdbResponseCache));
}

static void MdbSendCachedResponseBlock(MdbCashlessFsm_t *fsm) {
	uint16_t txBuffer[40];
	uint16_t responseWord = 0U;
	uint8_t checksum;

	if (!mdbResponseCache.valid || mdbResponseCache.len == 0U) {
		return;
	}

	checksum = calculate_mdb_chk(mdbResponseCache.bytes,
			mdbResponseCache.len);
	for (uint8_t i = 0U; i < mdbResponseCache.len; i++) {
		txBuffer[i] = mdbResponseCache.bytes[i];
	}
	txBuffer[mdbResponseCache.len] = (uint16_t)(checksum | 0x0100U);

	for (;;) {
#if MDB_HARDWARE_USART2
        mdbUart2TxFailed = false;
#endif
		MdbSendWords(txBuffer, (uint8_t)(mdbResponseCache.len + 1U));
#if MDB_HARDWARE_USART2
        if (mdbUart2TxFailed) { mdbResponseCache.replayRequired=true; return; }
#endif
		mdbResponseCache.awaitingAck = true;
		mdbResponseCache.replayRequired = false;

		MDB_DIAG_CONTEXT(MDB_DIAG_ACK, fsm->currentTransactionId);
		if (!MdbReadWord(&responseWord, MDB_ACK_WAIT_MS)) {
			MDB_DIAG_RECORD(MDB_DIAG_ACK_TIMEOUT, mdbDebugLastWord, 0U);
			mdbResponseCache.replayRequired = true;
			MdbTraceRecordEvent(MDB_TRACE_ACK_MISSING, fsm, 0U, 0U,
					mdbResponseCache.retryCount);
			return;
		}

		MDB_DIAG_RECORD(MDB_DIAG_ACK_WORD, responseWord, 0U);
		if ((responseWord & 0x0100U) != 0U) {
			mdbPrefetchedWord = responseWord;
			mdbPrefetchedWordValid = true;
			mdbResponseCache.replayRequired = true;
			MdbTraceRecordEvent(MDB_TRACE_ACK_MISSING, fsm,
					(uint8_t)responseWord, 0U, 0x0100U);
			return;
		}

		if ((uint8_t)responseWord == MDB_ACK) {
			MdbAcknowledgeCachedResponse(fsm);
			return;
		}

		if ((uint8_t)responseWord == MDB_RET
				&& mdbResponseCache.retryCount < 3U) {
			mdbResponseCache.retryCount++;
			MdbTraceRecordEvent(MDB_TRACE_RESPONSE_RETRY, fsm, 0U,
					MDB_RET, mdbResponseCache.retryCount);
			continue;
		}

		mdbResponseCache.awaitingAck = false;
		mdbResponseCache.replayRequired = true;
		MdbTraceRecordEvent(MDB_TRACE_RESPONSE_RETRY, fsm, 0U,
				(uint8_t)responseWord, mdbResponseCache.retryCount);
		return;
	}
}

static void MdbSendResponseBlock(const uint8_t *payload, uint8_t payloadLen,
		MdbCashlessFsm_t *fsm) {
	if (payload == NULL || payloadLen == 0U
			|| payloadLen >= sizeof(mdbResponseCache.bytes)) {
		MdbSendNak();
		return;
	}

	memset(&mdbResponseCache, 0, sizeof(mdbResponseCache));
	memcpy(mdbResponseCache.bytes, payload, payloadLen);
	mdbResponseCache.len = payloadLen;
	mdbResponseCache.retryCount = 0U;
	mdbResponseCache.valid = true;
	mdbResponseCache.awaitingAck = false;
	mdbResponseCache.replayRequired = false;
	MdbSendCachedResponseBlock(fsm);
}

static bool MdbReadWord(uint16_t *word, uint32_t timeoutMs) {
#if MDB_HARDWARE_USART2
    if (mdbPrefetchedWordValid) { *word=mdbPrefetchedWord; mdbPrefetchedWordValid=false; return true; }
    return MdbUart2Read(word, timeoutMs, mdbRxPhase == MDB_DIAG_ACK);
#else
	/* A prefetched word must not sample the bus or alter the caller's IRQ mask. */
	if (mdbPrefetchedWordValid) {
        uint32_t prefetchMask = __get_PRIMASK();
        __disable_irq();
        MdbRxFrameBegin();
        MdbSoftRestoreIrq(prefetchMask);
		*word = mdbPrefetchedWord;
		mdbPrefetchedWordValid = false;
		return true;
	}
	uint32_t startTick = HAL_GetTick();
	uint32_t startCycle = DWT->CYCCNT;
	/* ACK and inter-word waits need a full physical five milliseconds.
	 * HAL's integer tick can expire almost one millisecond early depending
	 * on where the wait begins within the current tick. */
	bool preciseTimeout = timeoutMs <= 10U;
	uint32_t timeoutCycles = (SystemCoreClock / 1000U) * timeoutMs;
	/* Keep the sampled idle/start level across calls. A local re-sample here
	 * loses a start edge when the next VMC byte begins between two calls. */
	bool lastHigh = mdbSoftRxLastPhysicalHigh;
	bool firstSample = true;

	while (preciseTimeout
			? (uint32_t)(DWT->CYCCNT - startCycle) < timeoutCycles
			: (HAL_GetTick() - startTick) < timeoutMs) {
        if (MdbRxFrameExpired()) return false;
		uint32_t nowTick = HAL_GetTick();
		uint32_t primask = __get_PRIMASK();
		/* GPIO sample and bit-clock origin belong to one atomic observation.
		 * An ISR between them shifts ACK data into the ninth/address bit. */
		__disable_irq();
		bool nowHigh = MdbSoftRxPhysicalHigh();
		uint32_t sampleCycle = DWT->CYCCNT;
		uint32_t observedTick = firstSample ? startTick : nowTick;
		firstSample = false;

		/* Also measure a completed pulse on its falling edge. */
		if (mdbBusActiveTracking && !mdbBusResetLatched
				&& nowTick - mdbBusActiveSinceTick >= MDB_BUS_RESET_ACTIVE_MS) {
			mdbSoftRxLastPhysicalHigh = nowHigh;
			mdbBusResetDetected = true;
			mdbBusResetLatched = nowHigh;
			mdbBusActiveTracking = nowHigh;
			MdbSoftRestoreIrq(primask);
			return false;
		}

		if (!nowHigh) {
			mdbBusActiveTracking = false;
			mdbBusResetLatched = false;
		} else if (!mdbBusResetLatched) {
			if (!mdbBusActiveTracking) {
				mdbBusActiveSinceTick = observedTick;
				mdbBusActiveTracking = true;
			} else if ((nowTick - mdbBusActiveSinceTick)
					>= MDB_BUS_RESET_ACTIVE_MS) {
				mdbBusResetDetected = true;
				mdbBusResetLatched = true;
				MdbSoftRestoreIrq(primask);
				return false;
			}
		}

		if (!lastHigh && nowHigh) {
            if (mdbRxFrameActive
                    && (uint32_t)(sampleCycle - mdbRxFrameStartedCycle)
                        + 12U * mdbSoftBitCycles >= mdbRxFrameBudgetCycles) {
                mdbRxFrameBudgetExceeded = true;
                MdbSoftRestoreIrq(primask);
                return false;
            }
			uint32_t startCycle = sampleCycle;
			uint32_t halfBitCycles = mdbSoftBitCycles / 2U;
			uint16_t rxWord = 0;

			if (halfBitCycles == 0U) {
				halfBitCycles = 1U;
			}

			MdbSoftWaitCycles(startCycle + halfBitCycles);
			if (!MdbSoftRxPhysicalHigh()) {
				/* The sampled level is still the idle physical low. Store that
				 * fact before IRQs can delay us into the next start bit. */
				lastHigh = false;
				mdbSoftRxLastPhysicalHigh = false;
				MdbSoftRestoreIrq(primask);
				MDB_DIAG_ERROR(MDB_DIAG_NOISE, mdbDebugLastWord);
				MdbDebugCountSoftNoiseError();
				MdbDebugReportWord("MDB SOFT NOISE", mdbDebugLastWord);
				continue;
			}

            MdbRxFrameBegin();
			for (uint8_t bit = 0; bit < MDB_SOFT_UART_DATA_BITS; bit++) {
				uint32_t sampleCycle = startCycle
						+ ((uint32_t)(bit + 1U) * mdbSoftBitCycles)
						+ halfBitCycles;

				MdbSoftWaitCycles(sampleCycle);
				if (MdbSoftRxLogicalBit() != 0U) {
					rxWord |= (uint16_t)(1U << bit);
				}
			}
			MdbSoftWaitCycles(startCycle
					+ ((MDB_SOFT_UART_DATA_BITS + 1U) * mdbSoftBitCycles)
					+ halfBitCycles);

			if (MdbSoftRxLogicalBit() == 0U) {
				mdbDebugLastWord = (uint16_t)(rxWord & 0x01FFU);
				/* Stop was sampled physical high. Do not re-sample after
				 * restoring IRQs because that aliases a later transition. */
				lastHigh = true;
				mdbSoftRxLastPhysicalHigh = true;
				MdbSoftRestoreIrq(primask);
				MDB_DIAG_ERROR(MDB_DIAG_STOP, mdbDebugLastWord);
				MdbDebugCountSoftFrameError();
				MdbDebugReportWord("MDB SOFT ERR", mdbDebugLastWord);
				continue;
			}

			*word = (uint16_t)(rxWord & 0x01FFU);
			mdbDebugLastWord = *word;
			mdbDebugLastRxTick = HAL_GetTick();
			/* A valid stop bit is logical high / physical low. Preserve this
			 * known sample before restoring IRQs. Re-reading PA3 afterwards
			 * can observe the next start bit and make the next call miss it. */
			mdbSoftRxLastPhysicalHigh = false;

			MdbSoftRestoreIrq(primask);
			return true;
		}

		lastHigh = nowHigh;
		mdbSoftRxLastPhysicalHigh = nowHigh;
		MdbSoftRestoreIrq(primask);
	}

	return false;
#endif
}
static bool MdbReadDataBytes(MdbFrame_t *frame, uint8_t count,
		uint32_t timeoutMs) {
	uint16_t word = 0;

	for (uint8_t i = 0; i < count; i++) {
		if (!MdbReadWord(&word, timeoutMs)) {
			MdbDebugReportFrameIssue("MDB FRAME TO", frame->len,
					mdbDebugLastWord);
			return false;
		}

		if ((word & 0x0100U) != 0U) {
			mdbPrefetchedWord = word;
			mdbPrefetchedWordValid = true;
			MdbDebugReportFrameIssue("MDB DATA 9BIT", frame->len, word);
			return false;
		}

		frame->bytes[frame->len++] = (uint8_t) word;
		MDB_DIAG_DATA_WORD(word, frame->len);
	}

	return true;
}

static bool MdbValidateFrame(const MdbFrame_t *frame) {
	if (frame->len < 2U) {
		return false;
	}

	return calculate_mdb_chk(frame->bytes, frame->len - 1U)
			== frame->bytes[frame->len - 1U];
}

static bool MdbIsBusControlWord(uint16_t word) {
	uint8_t byte = (uint8_t) word;

	if ((word & 0x0100U) != 0U) {
		return false;
	}

	return byte == MDB_ACK || byte == MDB_NAK || byte == MDB_RET;
}

static void MdbStartDisplayQuietWindow(void) {
	mdbDisplayQuietUntilTick = HAL_GetTick() + MDB_DISPLAY_QUIET_MS;
}

static void MdbDrainIgnoredFrame(uint16_t firstWord) {
	(void)firstWord;
	/* Foreign peripherals have different grammars. Never consume their next
	 * address as payload and never interpret their ACK as ours. */
	for (uint8_t i = 0U; i < 40U; ++i) {
		uint16_t word;
		if (!MdbReadWord(&word, 5U)) return;
		if ((word & 0x0100U) != 0U) {
			mdbPrefetchedWord = word;
			mdbPrefetchedWordValid = true;
			return;
		}
	}
}

static int MdbReceiveCommand(MdbFrame_t *frame, uint32_t timeoutMs,
		bool vendResultExpected) {
	uint16_t firstWord = 0;
	uint8_t command = 0;
	uint8_t subCommand = 0;
	uint8_t remainingBytes = 0;
	uint32_t subCommandWaitMs = MDB_DATA_WAIT_MS;
#if !MDB_HARDWARE_USART2
	osPriority_t oldPriority = osPriorityNone;
	bool priorityRaised = false;
#endif
	int result = -1;

	frame->len = 0;

	mdbRxAddressHandoffRequested = true;
	const bool received = MdbReadWord(&firstWord, timeoutMs);
	mdbRxAddressHandoffRequested = false;
	if (!received) {
        MdbRxFrameEnd();
		if (mdbBusResetDetected) {
			mdbBusResetDetected = false;
			return MDB_RX_BUS_RESET;
		}
		MDB_DIAG_RECORD(MDB_DIAG_IDLE_TIMEOUT, mdbDebugLastWord, 0U);
		MdbDebugReportNoRx();
		return 0;
	}

	if ((firstWord & 0x0100U) == 0U) {
        MdbRxFrameEnd();
		if (MdbIsBusControlWord(firstWord)) {
			if ((uint8_t)firstWord == MDB_ACK) {
				return MDB_RX_ACK;
			}
			if ((uint8_t)firstWord == MDB_NAK) {
				return MDB_RX_NAK;
			}
			return MDB_RX_RET;
		}
		MDB_DIAG_RECORD(MDB_DIAG_ORPHAN, firstWord, 0U);
		MdbDebugReportWord("MDB RX NO 9B", firstWord);
		return MDB_RX_TIMEOUT;
	}

#if !MDB_HARDWARE_USART2
	oldPriority = osThreadGetPriority(osThreadGetId());
	if (oldPriority < osPriorityRealtime) {
		osThreadSetPriority(osThreadGetId(), osPriorityRealtime);
		priorityRaised = true;
	}
#endif

	/* Record only after the existing priority raise: diagnostics must not
	 * enlarge the task-preemption window before receiving the payload. */
	MDB_DIAG_BEGIN(firstWord);
	frame->bytes[frame->len++] = (uint8_t) firstWord;
	if (!MdbIsCashlessAddress(frame->bytes[0])) {
		MdbDrainIgnoredFrame(firstWord);
		result = MDB_RX_TIMEOUT;
		goto done;
	}

	command = MdbCommandCode(frame->bytes[0]);
	if (command == MDB_CMD_VEND) {
		MDB_DIAG_CONTEXT(MDB_DIAG_DATA, mdbRxTransaction);
	}

	switch (command) {
	case MDB_CMD_RESET:
	case MDB_CMD_POLL:
		result = MdbReadDataBytes(frame, 1, MDB_DATA_WAIT_MS) ? frame->len : -1;
		break;

	case MDB_CMD_SETUP:
	case MDB_CMD_VEND:
	case MDB_CMD_READER:
	case MDB_CMD_REVALUE:
	case MDB_CMD_EXPANSION:
		/* A few VMCs pause before the result subcommand. Only an already
		 * approved vend owns this bounded compatibility window. ACK timing
		 * and every subsequent data-byte wait remain unchanged. */
		if (command == MDB_CMD_VEND && vendResultExpected) {
			subCommandWaitMs = MDB_VEND_RESULT_SUBCOMMAND_WAIT_MS;
		}
		if (!MdbReadDataBytes(frame, 1, subCommandWaitMs)) {
			result = -1;
			break;
		}

		subCommand = frame->bytes[1];
        /* REQUEST_ID carries 32 words (~37 ms even without gaps). Only this
         * known long command gets a larger, still absolute 60-ms budget. */
        if (command == MDB_CMD_EXPANSION && subCommand == 0U)
            mdbRxFrameBudgetCycles = (SystemCoreClock / 1000U) * 60U;
		remainingBytes = MdbExpectedBytesAfterSubCommand(command, subCommand);

		/* Unknown subcommand: bounded drain to the next address or idle gap.
		 * Do not mistake its arbitrary payload for separate MDB commands. */
		if (remainingBytes == 0U) {
			uint16_t word;
			while (frame->len < sizeof(frame->bytes) && MdbReadWord(&word, 5U)) {
				if ((word & 0x100U) != 0U) {
					mdbPrefetchedWord = word;
					mdbPrefetchedWordValid = true;
					break;
				}
				frame->bytes[frame->len++] = (uint8_t)word;
			}
			result = frame->len >= 3U ? frame->len : -1;
			break;
		}

		result = MdbReadDataBytes(frame, remainingBytes, MDB_DATA_WAIT_MS) ? frame->len : -1;
		break;

	default:
		/* Reserved command codes have no defined data in the supported
		 * profile. Consume the checksum and let the dispatcher ACK + OOS. */
		result = MdbReadDataBytes(frame, 1U, MDB_DATA_WAIT_MS) ? frame->len : -1;
		break;
	}

done:
    if (MdbRxFrameExpired()) result = MDB_RX_TIMEOUT;
    MdbRxFrameEnd();
#if !MDB_HARDWARE_USART2
	if (priorityRaised) {
		osThreadSetPriority(osThreadGetId(), oldPriority);
	}
#endif

	if (mdbBusResetDetected) {
		mdbBusResetDetected = false;
		result = MDB_RX_BUS_RESET;
	}

	if (result == MDB_RX_INVALID) {
		MdbReportFrameIssueAlways("MDB FRAME BAD", frame->len,
				mdbDebugLastWord);
	}

	MDB_DIAG_END(result, frame->len, mdbDebugLastWord);
	return result;
}

static void MdbSendReaderConfig(MdbCashlessFsm_t *fsm) {
	const uint8_t response[] = {
			MDB_RESP_READER_CONFIG,
			MDB_READER_LEVEL,
			0x19,
			0x33,
			MDB_SCALE_FACTOR,
			MDB_DECIMAL_PLACES,
			MDB_APP_MAX_RESPONSE_TIME,
			MDB_MISC_OPTIONS };

	MdbSendResponseBlock(response, sizeof(response), fsm);
}

static void MdbSendPeripheralId(MdbCashlessFsm_t *fsm) {
	uint8_t response[34] = { 0 };

	response[0] = MDB_RESP_PERIPHERAL_ID;
	memcpy(&response[1], "VMC", 3);
	memcpy(&response[4], "000000000001", 12);
	memcpy(&response[16], "CTRL_CASHL01", 12);
	response[28] = 0x01;
	response[29] = 0x00;
	response[33] = MDB_ADVERTISED_FEATURES;

	MdbSendResponseBlock(response, mdbNegotiatedLevel >= 3U ? 34U : 30U, fsm);
}

static bool TakeMdbPaymentMailbox(MdbPaymentMailbox_t *payment) {
	uint32_t primask;

	if (payment == NULL) {
		return false;
	}

	primask = __get_PRIMASK();
	__disable_irq();
	if (mdbPaymentMailbox.sequence == 0U
			|| mdbPaymentMailbox.sequence == mdbPaymentMailboxConsumedSequence) {
		RestoreInterruptState(primask);
		return false;
	}

	payment->resultCode = mdbPaymentMailbox.resultCode;
	payment->selectionId = mdbPaymentMailbox.selectionId;
	payment->transactionId = mdbPaymentMailbox.transactionId;
	payment->sequence = mdbPaymentMailbox.sequence;
	mdbPaymentMailboxConsumedSequence = payment->sequence;
	RestoreInterruptState(primask);
	return true;
}

static void MdbTraceAction(MdbTraceEvent_t event,
		const MdbCashlessFsm_t *fsm, uint8_t command, uint8_t subCommand,
		MdbFsmAction_t action) {
 if(action==MDB_FSM_OUT_OF_SEQUENCE||action==MDB_FSM_ABORTED_OUT_OF_SEQUENCE||action==MDB_FSM_QUEUE_FULL)
  F411RecordReason(action==MDB_FSM_QUEUE_FULL?"mdbQueueFull":"mdbOos",fsm->currentTransactionId,true);
 else if(action==MDB_FSM_ACCEPTED&&(event==MDB_TRACE_VEND_RESULT))
  F411RecoveryProgress();
	if (action == MDB_FSM_ABORTED_OUT_OF_SEQUENCE
			&& fsm->currentTransactionId != 0U) {
		MdbNotifyManager(MDB_EVT_MACHINE_CANCELLED,
				fsm->currentSelectionId, fsm->currentPriceMinor,
				fsm->currentTransactionId);
	}

	MdbTraceRecordEvent((action == MDB_FSM_OUT_OF_SEQUENCE
			|| action == MDB_FSM_ABORTED_OUT_OF_SEQUENCE)
			? MDB_TRACE_OOS
			: ((action == MDB_FSM_QUEUE_FULL) ? MDB_TRACE_QUEUE_FULL : event),
			fsm, command, subCommand, (uint16_t)action);
}

static void MdbSendPollReply(MdbCashlessFsm_t *fsm,
		const MdbPollReply_t *reply) {
	uint8_t response[10] = { MDB_ACK, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	uint8_t responseLen = 1U;
	MdbCashlessFsm_t traceFsm = *fsm;

	traceFsm.currentTransactionId = reply->transactionId;
	switch (reply->type) {
	case MDB_POLL_JUST_RESET:
		response[0] = MDB_RESP_JUST_RESET;
		break;
	case MDB_POLL_BEGIN_SESSION:
		response[0] = MDB_RESP_BEGIN_SESSION;
		response[1] = (uint8_t)(MDB_SESSION_FUNDS_AVAILABLE >> 8);
		response[2] = (uint8_t)MDB_SESSION_FUNDS_AVAILABLE;
		response[3] = (uint8_t)(MDB_SESSION_PAYMENT_MEDIA_ID >> 24);
		response[4] = (uint8_t)(MDB_SESSION_PAYMENT_MEDIA_ID >> 16);
		response[5] = (uint8_t)(MDB_SESSION_PAYMENT_MEDIA_ID >> 8);
		response[6] = (uint8_t)MDB_SESSION_PAYMENT_MEDIA_ID;
		response[7] = MDB_PAYMENT_TYPE_DEFAULT;
		response[8] = (uint8_t)(MDB_PAYMENT_DATA_DEFAULT >> 8);
		response[9] = (uint8_t)MDB_PAYMENT_DATA_DEFAULT;
		responseLen = mdbNegotiatedLevel >= 2U ? 10U : 3U;
		break;
	case MDB_POLL_VEND_APPROVED:
		response[0] = MDB_RESP_VEND_APPROVED;
		response[1] = (uint8_t)(reply->amount >> 8);
		response[2] = (uint8_t)reply->amount;
		responseLen = 3U;
		break;
	case MDB_POLL_VEND_DENIED:
		response[0] = MDB_RESP_VEND_DENIED;
		break;
	case MDB_POLL_END_SESSION:
		response[0] = MDB_RESP_END_SESSION;
		break;
	case MDB_POLL_COMMAND_OOS:
		response[0] = MDB_RESP_COMMAND_OOS;
		response[1] = MdbFsmProtocolStatus(fsm);
		responseLen = mdbNegotiatedLevel >= 2U ? 2U : 1U;
		break;
	case MDB_POLL_NONE:
	default:
		MdbSendAck();
		return;
	}

	MdbTraceRecordEvent(MDB_TRACE_POLL_RESPONSE, &traceFsm, MDB_CMD_POLL,
			(uint8_t)reply->type, reply->amount);
	MdbSendResponseBlock(response, responseLen, fsm);
	if (mdbResponseCache.valid) {
		mdbResponseCache.pollReply = *reply;
		mdbResponseCache.pollReplyPending = true;
	} else {
		MdbCommitPollReply(fsm, reply);
	}
}

static void MdbClearResponseCache(void) {
	memset(&mdbResponseCache, 0, sizeof(mdbResponseCache));
}

static void MdbHandleLateBusControl(int rxResult, MdbCashlessFsm_t *fsm) {
	/* Only the synchronous 5 ms response window owns a bus ACK/NAK/RET.
	 * Unaddressed controls outside it may belong to any other peripheral. */
	(void)rxResult;
	(void)fsm;
}

void mdbTask(void *argument) {
	MdbFrame_t frame;
	MdbCashlessFsm_t fsm;
	MdbPaymentMailbox_t payment;
	bool readyNotified = false;
	bool pollSeen = false;
	uint32_t pollCounter = 0U;
	uint32_t lastPollStatusTick = 0U;
	char statusLine2[17];

	MdbFsmInit(&fsm);
	osDelay(3000);
	MdbSoftUartInit();
	MdbDebugSetEnabled(true);
	SendStatusUpdate("MDB USART2 9BIT", "PA3RX PA2TX");

	for (;;) {
		uint8_t command;
		uint8_t subCommand;
		int rxResult;

		SdObserveMdbSession(&fsm);
		F411_MdbHeartbeat();
  if(maintenanceForced){osDelay(1U);continue;}
  F411ObserveWire(&fsm,mdbResponseCache.valid&&mdbResponseCache.pollReplyPending,false);
		MdbFsmAction_t deadlineAction = MdbFsmCheckDeadline(&fsm, HAL_GetTick());
		if (deadlineAction != MDB_FSM_STALE) {
			MdbClearResponseCache();
			if (deadlineAction != MDB_FSM_ABORTED_OUT_OF_SEQUENCE) {
				MdbNotifyManager(MDB_EVT_MACHINE_CANCELLED,
						fsm.currentSelectionId, fsm.currentPriceMinor,
						fsm.currentTransactionId);
			}
			MdbTraceAction(MDB_TRACE_PAYMENT_RESULT, &fsm, 0U,
					MDB_PAYMENT_RESULT_TIMEOUT, deadlineAction);
			if (deadlineAction == MDB_FSM_ACCEPTED)
				MdbFsmSetDeadline(&fsm, HAL_GetTick(), MACHINE_RESULT_TIMEOUT_MS);
		}
		if (!readyNotified && fsm.setupSeen && fsm.readerEnabled && !fsm.resetRequired)
			readyNotified = MdbNotifyManager(MDB_EVT_READY, 0U, 0U, 0U);

		if (TakeMdbPaymentMailbox(&payment)) {
			MdbFsmAction_t paymentAction = MdbFsmApplyPaymentResult(&fsm,
					payment.transactionId, payment.resultCode);
            if (paymentAction == MDB_FSM_ACCEPTED)
                g_payment_result_tick = HAL_GetTick();
			MdbTraceRecordEvent(
					(paymentAction == MDB_FSM_STALE) ? MDB_TRACE_STALE_RESULT
							: ((paymentAction == MDB_FSM_QUEUE_FULL)
									? MDB_TRACE_QUEUE_FULL
									: MDB_TRACE_PAYMENT_RESULT),
					&fsm, 0U, payment.resultCode, payment.selectionId);
		}

		MDB_DIAG_CONTEXT(MDB_DIAG_COMMAND, fsm.currentTransactionId);
		rxResult = MdbReceiveCommand(&frame, MDB_COMMAND_RX_TIMEOUT_MS,
				fsm.sessionState == MDB_SESSION_APPROVED);
		if (rxResult == MDB_RX_BUS_RESET) {
			MdbClearResponseCache();
			MdbFsmReset(&fsm);
			MdbDebugSetEnabled(true);
			readyNotified = false;
			pollSeen = false;
			pollCounter = 0U;
			lastPollStatusTick = 0U;
			SendStatusUpdate("MDB BUS RESET", "ACTIVE >=100MS");
			MdbTraceRecordEvent(MDB_TRACE_RESET, &fsm, 0xFFU, 0U, 1U);
			MdbNotifyManager(MDB_EVT_RESET, 0U, 0U, 0U);
			continue;
		}
		if (rxResult == MDB_RX_ACK || rxResult == MDB_RX_NAK
				|| rxResult == MDB_RX_RET) {
			MdbHandleLateBusControl(rxResult, &fsm);
			continue;
		}
		if (rxResult == MDB_RX_INVALID) {
			MdbTraceRecordEvent(MDB_TRACE_BAD_FRAME, &fsm,
					(frame.len > 0U) ? MdbCommandCode(frame.bytes[0]) : 0xFFU,
					(frame.len > 1U) ? frame.bytes[1] : 0xFFU, frame.len);
			if (frame.len > 1U && MdbCommandCode(frame.bytes[0]) == MDB_CMD_VEND
					&& frame.bytes[1] == 0x00U) {
				uint16_t selectionId = frame.len > 5U
						? (uint16_t)(((uint16_t)frame.bytes[4] << 8) | frame.bytes[5])
						: 0U;
				MdbTraceRecordEvent(MDB_TRACE_VEND_FLOW, &fsm,
						0xFEU, 0U, selectionId);
			}
		}
		if (rxResult <= MDB_RX_TIMEOUT) {
			osDelay(MDB_TASK_YIELD_DELAY_MS);
			continue;
		}

		if (!MdbValidateFrame(&frame)) {
			snprintf(statusLine2, sizeof(statusLine2), "A%02X L%02u CHK",
					(unsigned)frame.bytes[0], (unsigned)frame.len);
			SendStatusUpdate("MDB BAD CHK", statusLine2);
			MdbTraceRecordEvent(MDB_TRACE_BAD_FRAME, &fsm,
					MdbCommandCode(frame.bytes[0]), 0U, frame.len);
			if (frame.len > 1U && MdbCommandCode(frame.bytes[0]) == MDB_CMD_VEND
					&& frame.bytes[1] == 0x00U) {
				uint16_t selectionId = frame.len > 5U
						? (uint16_t)(((uint16_t)frame.bytes[4] << 8) | frame.bytes[5])
						: 0U;
				MdbTraceRecordEvent(MDB_TRACE_VEND_FLOW, &fsm,
						0xFEU, 0U, selectionId);
			}
			MdbSendNak();
			MdbStartDisplayQuietWindow();
			continue;
		}

  F411ObserveWire(&fsm,mdbResponseCache.valid&&mdbResponseCache.pollReplyPending,true);
		command = MdbCommandCode(frame.bytes[0]);
		if (command == MDB_CMD_POLL) MDB_DIAG_POLL();
		subCommand = (frame.len > 2U) ? frame.bytes[1] : 0xFFU;

		if (mdbResponseCache.valid && command != MDB_CMD_RESET) {
			if (command == MDB_CMD_POLL) {
				pollCounter++;
				MdbSendCachedResponseBlock(&fsm);
				continue;
			}
			MdbPendingPollResponse_t pending = mdbResponseCache.pollReply.type;
			bool impliedAck = !mdbResponseCache.pollReplyPending
					|| (pending == MDB_POLL_JUST_RESET && command == MDB_CMD_SETUP)
					|| (pending == MDB_POLL_BEGIN_SESSION && command == MDB_CMD_VEND && subCommand == 0U)
					|| (pending == MDB_POLL_VEND_APPROVED && command == MDB_CMD_VEND
							&& (subCommand == 2U || subCommand == 3U))
					|| (pending == MDB_POLL_VEND_DENIED && command == MDB_CMD_VEND && subCommand == 4U)
					|| (pending == MDB_POLL_END_SESSION && command == MDB_CMD_READER && subCommand == 1U);
			if (impliedAck) {
				/* A legal dependent command proves the VMC received the response;
				 * its lost ACK must not cause us to discard this command. */
				MdbAcknowledgeCachedResponse(&fsm);
			} else if (command == MDB_CMD_VEND && subCommand == 1U
					&& pending == MDB_POLL_VEND_APPROVED) {
				MdbClearResponseCache(); /* Cancel an unacknowledged approval. */
			} else if (command != MDB_CMD_READER || subCommand > 1U) {
				MdbClearResponseCache();
				MdbSendAck();
				MdbTraceAction(MDB_TRACE_OOS, &fsm, command, subCommand,
						MdbFsmQueueCommandOos(&fsm));
				continue;
			}
		}

		switch (command) {
		case MDB_CMD_RESET:
			MdbSendAck();
			MdbClearResponseCache();
			MdbFsmReset(&fsm);
			MdbDebugSetEnabled(true);
			readyNotified = false;
			pollSeen = false;
			pollCounter = 0U;
			lastPollStatusTick = 0U;
			snprintf(statusLine2, sizeof(statusLine2), "ADDR %02X ACK",
					frame.bytes[0]);
			SendStatusUpdate("MDB RESET", statusLine2);
			MdbTraceRecordEvent(MDB_TRACE_RESET, &fsm, command, 0U, 0U);
			MdbNotifyManager(MDB_EVT_RESET, 0U, 0U, 0U);
			break;

		case MDB_CMD_POLL: {
			MdbPollReply_t reply = { 0 };
			uint32_t now;

			pollCounter++;
			if (!readyNotified && !pollSeen) {
				SendStatusUpdate("MDB POLL RX", "BUS OK");
				pollSeen = true;
			}
			if (MdbFsmPeekPollReply(&fsm, &reply)) {
				MdbSendPollReply(&fsm, &reply);
			} else {
				MdbSendAck();
			}

			now = HAL_GetTick();
			if ((now - lastPollStatusTick) >= MDB_POLL_STATUS_PERIOD_MS) {
				lastPollStatusTick = now;
				MdbReportPollStatus(pollCounter, fsm.readerEnabled,
						fsm.sessionState, reply.type);
			}
			break;
		}

		case MDB_CMD_SETUP:
			if (subCommand == 0x00U) {
				MdbFsmAction_t action = MdbFsmHandleSetupConfig(&fsm);
				MdbTraceAction(MDB_TRACE_SETUP, &fsm, command, subCommand,
						action);
				if (action == MDB_FSM_ACCEPTED) {
					mdbNegotiatedLevel = frame.bytes[2] < MDB_READER_LEVEL
							? frame.bytes[2] : MDB_READER_LEVEL;
					if (mdbNegotiatedLevel == 0U) mdbNegotiatedLevel = 1U;
					MdbSendReaderConfig(&fsm);
				} else {
					MdbSendAck();
				}
			} else if (subCommand == 0x01U && !fsm.resetRequired && fsm.setupSeen
					&& fsm.sessionState == MDB_SESSION_DISABLED) {
				MdbSendAck();
				MdbTraceRecordEvent(MDB_TRACE_SETUP, &fsm, command,
						subCommand, 0U);
			} else {
				MdbSendAck();
				MdbTraceAction(MDB_TRACE_SETUP, &fsm, command, subCommand,
						MdbFsmQueueCommandOos(&fsm));
			}
			break;

		case MDB_CMD_EXPANSION:
			if (fsm.resetRequired || !fsm.setupSeen
					|| fsm.sessionState != MDB_SESSION_DISABLED) {
				MdbSendAck();
				MdbTraceAction(MDB_TRACE_OOS, &fsm, command, subCommand,
						MdbFsmQueueCommandOos(&fsm));
			} else if (subCommand == 0x00U) {
				MdbSendPeripheralId(&fsm);
			} else if (subCommand == 0x04U && mdbNegotiatedLevel >= 3U
					&& (frame.bytes[2] | frame.bytes[3] | frame.bytes[4] | frame.bytes[5]) == 0U) {
				/* No optional feature bits are advertised or implemented. */
				MdbSendAck();
			} else {
				MdbSendAck();
				MdbTraceAction(MDB_TRACE_OOS, &fsm, command, subCommand,
						MdbFsmQueueCommandOos(&fsm));
			}
			break;

		case MDB_CMD_READER: {
			MdbFsmAction_t action;

			if (subCommand == 0x00U) {
				action = MdbFsmHandleReaderDisable(&fsm);
				if (mdbResponseCache.pollReplyPending
						&& !MdbFsmHasPendingResponse(&fsm, mdbResponseCache.pollReply.type))
					MdbClearResponseCache();
				MdbSendAck();
			} else if (subCommand == 0x01U) {
				action = MdbFsmHandleReaderEnable(&fsm);
				MdbSendAck();
				if (action == MDB_FSM_ACCEPTED && !readyNotified) {
					SendStatusUpdate("MDB READER EN", "READY EVENT");
					readyNotified = MdbNotifyManager(MDB_EVT_READY, 0U, 0U, 0U);
					MdbDebugSetEnabled(false);
				}
			} else if (subCommand == 0x02U) {
				action = MdbFsmHandleReaderCancel(&fsm);
				if (action == MDB_FSM_ACCEPTED) {
					MdbSendResponseBlock(
							(uint8_t[]) { MDB_RESP_CANCELLED }, 1U, &fsm);
				} else {
					MdbSendAck();
				}
			} else {
				action = MdbFsmQueueCommandOos(&fsm);
				MdbSendAck();
			}
			MdbTraceAction(MDB_TRACE_READER, &fsm, command, subCommand,
					action);
			break;
		}

		case MDB_CMD_REVALUE:
			if (subCommand == 0x01U && !fsm.resetRequired && fsm.setupSeen
					&& fsm.sessionState != MDB_SESSION_WAIT_USB_RESULT
					&& fsm.sessionState != MDB_SESSION_APPROVAL_PENDING
					&& fsm.sessionState != MDB_SESSION_DENIED_PENDING
					&& fsm.sessionState != MDB_SESSION_APPROVED) {
				const uint8_t response[] = {
						MDB_RESP_REVALUE_LIMIT,
						(uint8_t)(MDB_REVALUE_LIMIT_AMOUNT >> 8),
						(uint8_t)MDB_REVALUE_LIMIT_AMOUNT };
				MdbSendResponseBlock(response, sizeof(response), &fsm);
			} else if (subCommand == 0x00U && !fsm.resetRequired
					&& fsm.sessionState == MDB_SESSION_IDLE) {
				MdbSendResponseBlock(
						(uint8_t[]) { MDB_RESP_REVALUE_DENIED }, 1U, &fsm);
			} else {
				MdbSendAck();
				MdbTraceAction(MDB_TRACE_OOS, &fsm, command, subCommand,
						MdbFsmQueueCommandOos(&fsm));
			}
			break;

		case MDB_CMD_VEND:
			if (subCommand == 0x00U) {
				uint16_t selectionId =
						(uint16_t)(((uint16_t)frame.bytes[4] << 8)
								| frame.bytes[5]);
				uint16_t priceMinor =
						(uint16_t)(((uint16_t)frame.bytes[2] << 8)
								| frame.bytes[3]);
				MdbFsmAction_t action = MdbFsmHandleVendRequest(&fsm,
						selectionId, priceMinor);

				MdbSendAck();
				MdbTraceAction(MDB_TRACE_VEND_REQUEST, &fsm, command,
						subCommand, action);
				MdbTraceRecordEvent(MDB_TRACE_VEND_FLOW, &fsm,
						(uint8_t)action, 0U,
						selectionId);
				if (action == MDB_FSM_ACCEPTED) {
                    g_mdb_session_active = true;
                    g_payment_request_tick = HAL_GetTick();
                    g_payment_result_tick = 0U;
					SendUsbTransactionStatus("MDB VEND REQ", selectionId,
							priceMinor, FindMotorBySelectionId(selectionId));
					MdbFsmSetDeadline(&fsm, HAL_GetTick(), PAYMENT_WAIT_TIMEOUT_MS);
					if (!MdbNotifyManager(MDB_EVT_SELECTION_REQUEST, selectionId,
							priceMinor, fsm.currentTransactionId)) {
						(void)MdbFsmApplyPaymentResult(&fsm, fsm.currentTransactionId,
								MDB_PAYMENT_RESULT_DENIED);
					}
				} else if (action == MDB_FSM_OUT_OF_SEQUENCE) {
					SendUsbTransactionStatus("MDB VEND BLOCK", selectionId,
							priceMinor, FindMotorBySelectionId(selectionId));
				}
			} else if (subCommand == 0x01U) {
				MdbFsmAction_t action = MdbFsmHandleVendCancel(&fsm);

				MdbSendAck();
				MdbTraceAction(MDB_TRACE_VEND_CANCEL, &fsm, command,
						subCommand, action);
				if (action == MDB_FSM_ACCEPTED) {
					MdbNotifyManager(MDB_EVT_MACHINE_CANCELLED,
							fsm.currentSelectionId, fsm.currentPriceMinor,
							fsm.currentTransactionId);
				}
			} else if (subCommand == 0x02U || subCommand == 0x03U) {
				uint16_t itemId = (subCommand == 0x02U)
						? (uint16_t)(((uint16_t)frame.bytes[2] << 8)
								| frame.bytes[3])
						: fsm.currentSelectionId;
				uint32_t transactionId = fsm.currentTransactionId;
				MdbFsmAction_t action = MdbFsmHandleVendResult(&fsm,
						subCommand);

				MdbSendAck();
				MdbTraceAction(MDB_TRACE_VEND_RESULT, &fsm, command,
						subCommand, action);
				if (action == MDB_FSM_ACCEPTED) {
					MdbNotifyManager(
							(subCommand == 0x02U)
									? MDB_EVT_MACHINE_VEND_SUCCESS
									: MDB_EVT_MACHINE_VEND_FAILURE,
							itemId, 0U, transactionId);
				}
			} else if (subCommand == 0x04U) {
				uint16_t selectionId = fsm.currentSelectionId;
				uint32_t transactionId = fsm.currentTransactionId;
				MdbFsmAction_t action = MdbFsmHandleSessionComplete(&fsm);

				MdbSendAck();
				MdbTraceAction(MDB_TRACE_SESSION_COMPLETE, &fsm, command,
						subCommand, action);
				if (action == MDB_FSM_ACCEPTED
						|| action == MDB_FSM_COMPAT_CANCEL) {
					MdbNotifyManager(MDB_EVT_MACHINE_SESSION_COMPLETE,
							selectionId, 0U, transactionId);
				}
			} else if (subCommand == 0x05U) {
				MdbSendAck();
				MdbTraceAction(MDB_TRACE_OOS, &fsm, command, subCommand,
						MdbFsmQueueCommandOos(&fsm));
			} else {
				MdbSendAck();
				MdbTraceAction(MDB_TRACE_OOS, &fsm, command, subCommand,
						MdbFsmQueueCommandOos(&fsm));
			}
			break;

		default:
			MdbSendAck();
			MdbTraceAction(MDB_TRACE_OOS, &fsm, command, subCommand,
					MdbFsmQueueCommandOos(&fsm));
			break;
		}

		MdbStartDisplayQuietWindow();
		if (!mdbResponseCache.awaitingAck) {
			osDelay(MDB_TASK_YIELD_DELAY_MS);
		}
	}
}
// Вспомогательная функция для отрисовки цифры с учетом мигания
// val - цифра (0-9), index - номер этого поля, currentEdit - что сейчас редактируем, blink - фаза
void DrawBlinkingDigit(uint8_t val, uint8_t index, uint8_t currentEdit,
		bool show) {
	if (index == currentEdit && !show) {
		LCD_SendData(' '); // Рисуем пробел (скрываем цифру)
	} else {
		LCD_SendData(val + '0'); // Рисуем цифру (преобразуем число в ASCII)
	}
}

void UpdateDisplay(DisplayCommand_t *cmd, bool blinkVisible) {

	switch (cmd->type) {
	case SCENE_STATUS:
		LCD_SetCursor(0, 0);
		LCD_Print(cmd->data.status.line1);
		LCD_SetCursor(1, 0);
		LCD_Print(cmd->data.status.line2);
		break;

	case SCENE_IDLE:
		LCD_SetCursor(0, 0);
		LCD_Print("ВЫБОР ТОВАРА    "); // Пробелы чтобы затереть мусор
		LCD_SetCursor(1, 0);
		LCD_Print("                ");
		break;

	case SCENE_ERROR:
		LCD_SetCursor(0, 0);
		        // Проверяем код ошибки, который прислал менеджер
				if (cmd->data.settings.editIndex == 1) {
					LCD_Print("ОШИБКА ВЫДАЧИ   ");
					LCD_SetCursor(1, 0);
					LCD_Print("ВОЗВРАТ СРЕДСТВ ");
				} else if (cmd->data.settings.editIndex == 2) {
					LCD_Print("ОШИБКА ТЕРМИНАЛА");
					LCD_SetCursor(1, 0);
					LCD_Print("НЕТ СВЯЗИ (MDB) ");
				} else if (cmd->data.settings.editIndex == 3) {
					LCD_Print("ОТКАЗ БАНКА     ");
					LCD_SetCursor(1, 0);
					LCD_Print("                ");
				} else if (cmd->data.settings.editIndex == 4) {
					LCD_Print(" ЗАБЕРИТЕ ТОВАР ");
					LCD_SetCursor(1, 0);
					LCD_Print("                ");
				} else if (cmd->data.settings.editIndex == 5) {
					LCD_Print(" ЗАБЕРИТЕ ТОВАР ");
					LCD_SetCursor(1, 0);
					LCD_Print("                ");
				} else {
					LCD_Print("НЕИЗВЕСТ. ОШИБКА");
					LCD_SetCursor(1, 0);
					LCD_Print("                ");
				}
				break;

	case SCENE_PAYMENT: {
		char line1[17] = "ID00000 P00000 ";
		char line2[17] = "WAIT UART       ";
		uint16_t priceMinor =
				(uint16_t)(((uint16_t)cmd->data.settings.x * 10U
						+ cmd->data.settings.y) * 100U
						+ (uint16_t)cmd->data.settings.z * 10U
						+ cmd->data.settings.p);

		FormatFiveDigitValue(&line1[2], cmd->data.settings.q);
		FormatFiveDigitValue(&line1[9], priceMinor);

		LCD_SetCursor(0, 0);
		LCD_Print(line1);
		LCD_SetCursor(1, 0);
		LCD_Print(line2);
		break;
	}

	case SCENE_VEND:
		LCD_SetCursor(0, 0);
		LCD_Print("ВЫДАЧА ТОВАРА...");
		LCD_SetCursor(1, 0);
		LCD_Print("                ");
		break;

	case SCENE_SETTINGS:
		// Строка 1: Заголовок
		LCD_SetCursor(0, 0);
		LCD_Print("ЦЕНА МОТОР ");





				// editItemIndex идет от 0 до 9, прибавляем 1 для красивого вывода (1-10)
				uint8_t motorNum = editItemIndex + 1;

				if (motorNum == 10) {
					LCD_Print("10"); // Добиваем пробелами до 16 символов, чтобы стереть старый текст
				} else {
					LCD_SendData(motorNum + '0'); // Превращаем цифру в символ
					LCD_Print(" ");            // 4 пробела для очистки конца строки
				}
				// Строка 2: Значения "XX.YY R  QQ"
						// Предполагаем формат: X Y . Z P  (пробел) Q
						LCD_SetCursor(1, 0);

		// Рисуем X (Десятки рублей) - индекс 1
		DrawBlinkingDigit(cmd->data.settings.x, 1, cmd->data.settings.editIndex,
				blinkVisible);

		// Рисуем Y (Рубли) - индекс 2
		DrawBlinkingDigit(cmd->data.settings.y, 2, cmd->data.settings.editIndex,
				blinkVisible);

		LCD_SendData('.');

		// Рисуем Z (Десятки копеек) - индекс 3
		DrawBlinkingDigit(cmd->data.settings.z, 3, cmd->data.settings.editIndex,
				blinkVisible);

		// Рисуем P (Копейки) - индекс 4
		DrawBlinkingDigit(cmd->data.settings.p, 4, cmd->data.settings.editIndex,
				blinkVisible);

		LCD_Print(" P. "); // Разделитель

		break;
	}
}

static uint32_t GetDisplayHoldMs(const DisplayCommand_t *cmd) {
	const char *line1;

	if (cmd == NULL) {
		return 0U;
	}

	if (cmd->type == SCENE_PAYMENT) {
		return DISPLAY_PAYMENT_HOLD_MS;
	}

	if (cmd->type != SCENE_STATUS) {
		return 0U;
	}

	line1 = cmd->data.status.line1;
	if (strncmp(line1, "MDB VEND", 8) == 0
			|| strncmp(line1, "USB ", 4) == 0
			|| strncmp(line1, "UART ", 5) == 0) {
		return DISPLAY_CRITICAL_HOLD_MS;
	}

	return 0U;
}

static uint32_t GetMdbDisplayQuietRemainingMs(void) {
	int32_t remainingMs =
			(int32_t)(mdbDisplayQuietUntilTick - HAL_GetTick());

	return (remainingMs > 0) ? (uint32_t)remainingMs : 0U;
}

static void WaitForMdbDisplayQuietWindow(void) {
	uint32_t remainingMs;

	while ((remainingMs = GetMdbDisplayQuietRemainingMs()) != 0U) {
		osDelay((remainingMs > 5U) ? 5U : remainingMs);
	}
}

void displayTask(void *argument) {
	// 1. Инициализация дисплея
	osDelay(800);
	osMutexAcquire(i2cMutexHandle, osWaitForever);
	LCD_Init();
	LCD_Clear();
	LCD_SetCursor(0, 0);
	LCD_Print("System Boot...");
	osMutexRelease(i2cMutexHandle);

	DisplayCommand_t currentCmd = { 0 };
	DisplayCommand_t rxData;
	bool blinkState = true;
	osStatus_t status;
	uint32_t holdUntilTick = 0;
	uint32_t lastRenderTick = 0;

	SceneType_t lastScene = -1;

	// ---> ИСПРАВЛЕНИЕ: Ждем самую первую команду вечно! <---
	// Экран "System Boot..." будет висеть, пока не сработает эта строка
	if (osMessageQueueGet(displayQueueHandle, &rxData, NULL, osWaitForever)
			== osOK) {
		currentCmd = rxData;
		osMutexAcquire(i2cMutexHandle, osWaitForever);
		if (LCD_ReinitRequested()) {
			LCD_ClearReinitRequest();
			LCD_Init();
			LCD_Clear();
			lastScene = -1;
		}
		UpdateDisplay(&currentCmd, true);
		lastRenderTick = HAL_GetTick();
		holdUntilTick = HAL_GetTick() + GetDisplayHoldMs(&currentCmd);
		osMutexRelease(i2cMutexHandle);
	}

	for (;;) {
		if (holdUntilTick != 0U) {
			int32_t remainingMs =
					(int32_t)(holdUntilTick - HAL_GetTick());
			if (remainingMs > 0) {
				osDelay((remainingMs > 50) ? 50U : (uint32_t)remainingMs);
				continue;
			}
			holdUntilTick = 0U;
		}

		// Ждем сообщение из очереди МАКСИМУМ 250 мс
		status = osMessageQueueGet(displayQueueHandle, &rxData, NULL, 250);

		if (status == osOK) {
			// --- ПРИШЛИ НОВЫЕ ДАННЫЕ ---
			// Копируем их в текущее состояние
			currentCmd = rxData;
			DrainDisplayQueueToLatest(&currentCmd);

			if (lastRenderTick != 0U) {
				uint32_t elapsedMs = HAL_GetTick() - lastRenderTick;
				if (elapsedMs < DISPLAY_MIN_UPDATE_MS) {
					osDelay(DISPLAY_MIN_UPDATE_MS - elapsedMs);
					DrainDisplayQueueToLatest(&currentCmd);
				}
			}

				// Сбрасываем мигание в "видимое", чтобы при переключении меню текст не исчезал
				blinkState = true;
				WaitForMdbDisplayQuietWindow();
				osMutexAcquire(i2cMutexHandle, osWaitForever);
				if (LCD_ReinitRequested()) {
					LCD_ClearReinitRequest();
					LCD_Init();
					LCD_Clear();
					lastScene = -1;
				}
				if (currentCmd.type != lastScene) {
				lastScene = currentCmd.type;
			}
			// Отрисовываем немедленно (обновление данных)
			UpdateDisplay(&currentCmd, blinkState);
			lastRenderTick = HAL_GetTick();
			holdUntilTick = HAL_GetTick() + GetDisplayHoldMs(&currentCmd);
			osMutexRelease(i2cMutexHandle);
		} else {
			// --- ТАЙМАУТ (данных нет, прошло 250 мс) ---
			// Если мы в настройках - нужно мигать
				if (currentCmd.type == SCENE_SETTINGS) {
					blinkState = !blinkState; // Инвертируем (Вкл <-> Выкл)
					WaitForMdbDisplayQuietWindow();
					osMutexAcquire(i2cMutexHandle, osWaitForever);
					if (LCD_ReinitRequested()) {
						LCD_ClearReinitRequest();
						LCD_Init();
						LCD_Clear();
						lastScene = -1;
					}
					// Обновляем дисплей с новой фазой мигания
					UpdateDisplay(&currentCmd, blinkState);
				osMutexRelease(i2cMutexHandle);
			}
		}
	}
}

static FATFS fs;
static FIL fil;

// Максимальный размер лога (например, 1 МБ)
#define MAX_LOG_SIZE (1024 * 1024)

/* Two independently checksummed slots: overwrite only the older one. */
static const char *configSlots[2] = { "cfg0.bin", "cfg1.bin" };
static uint32_t configSequence;
static uint8_t configSlot;
static bool ReadConfigSlot(uint8_t slot, VendingConfig_t *config, uint32_t *sequence) {
	FIL file; UINT count=0; uint8_t record[CONFIG_RECORD_SIZE];
	if (f_open(&file, configSlots[slot], FA_READ) != FR_OK) return false;
	bool ok = f_size(&file) == CONFIG_RECORD_SIZE &&
		f_read(&file, record, sizeof(record), &count) == FR_OK && count == sizeof(record);
	if (f_close(&file) != FR_OK) ok = false;
	return ok && ConfigRecordDecode(record, config, sequence);
}
static bool StoreConfigRecord(const VendingConfig_t *config) {
	FIL file; UINT count=0; uint8_t record[CONFIG_RECORD_SIZE];
	uint8_t nextSlot = configSlot ^ 1U;
	if (!ConfigRecordEncode(record, config, configSequence + 1U) ||
		f_open(&file, configSlots[nextSlot], FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
	bool ok = f_write(&file, record, sizeof(record), &count) == FR_OK && count == sizeof(record);
	if (ok) ok = f_sync(&file) == FR_OK;
	if (f_close(&file) != FR_OK) ok = false;
	if (ok) { configSlot = nextSlot; configSequence++; }
	return ok;
}
static void LoadConfigRecords(void) {
	VendingConfig_t other; uint32_t sequence=0;
	bool first = ReadConfigSlot(0U, &loadedConfig, &configSequence);
	bool second = ReadConfigSlot(1U, &other, &sequence);
	configSlot = 0U;
	if (second && (!first || (int32_t)(sequence - configSequence) > 0)) {
		loadedConfig=other;configSequence=sequence;configSlot=1U;
	}
	if (first || second) return;
	/* One-time migration accepts only exact-size, semantically valid legacy data. */
	FIL file; UINT count=0; uint8_t record[CONFIG_RECORD_SIZE]; bool legacy=false;
	FILINFO info;
	bool damagedNewFormat = f_stat(configSlots[0], &info) == FR_OK || f_stat(configSlots[1], &info) == FR_OK;
	if (!damagedNewFormat && f_open(&file, FILE_CONFIG, FA_READ) == FR_OK) {
		legacy = f_size(&file) == sizeof(other) && f_read(&file, &other, sizeof(other), &count) == FR_OK &&
			count == sizeof(other) && ConfigRecordEncode(record, &other, 0U);
		f_close(&file);
	}
	if (legacy) loadedConfig = other; else SetDefaultConfig(&loadedConfig);
	if (damagedNewFormat) {
		for (unsigned i=0;i<10;i++) loadedConfig.items[i].selection_id=0xFFFFU;
		sd_available=0U;
		return;
	}
	configSequence=0U;
	if (!StoreConfigRecord(&loadedConfig)) sd_available=0U;
}

void loggerTask(void *argument) {
	StorageCommand_t rxCmd;
	UINT bytesWritten;
	FRESULT fres;
	bool sd_ok = false; // Флаг: жива ли карта

	// 1. Попытка инициализации (без вечного цикла!)
	osDelay(500);
	fres = f_mount(&fs, "", 1);
	sd_available = (fres == FR_OK) ? 1U : 0U;
	if (fres == FR_OK) {
		SendStatusUpdate("SD MOUNT OK", "CFG LOAD");
	} else {
		SendStatusUpdate("SD MOUNT FAIL", "USE DEFAULT");
	}
	osDelay(2000);
	if (fres == FR_OK) {
		sd_ok = true; // Карта смонтировалась

		LoadConfigRecords();

		__DMB();
		configLoadComplete = true;

		// Уведомляем менеджера, что мы закончили (если успели до таймаута)
		event_t readyEvent;
		readyEvent.source = SRC_SD;
		QueueManagerEvent(&readyEvent);

	} else {
		// ОШИБКА МОНТИРОВАНИЯ
		// Карта недоступна: работаем с дефолтным конфигом в RAM,
		// чтобы менеджер не остался с неинициализированными данными.
		SetDefaultConfig(&loadedConfig);
		sd_ok = false;
		__DMB();
		configLoadComplete = true;
		event_t failEvent;
		failEvent.source = SRC_SD; // Источник тот же
		QueueManagerEvent(&failEvent);
	}

	// 3. Главный цикл (работает всегда, даже если sd_ok == false)
	// Это нужно, чтобы вычитывать очередь и не забивать память, даже если писать некуда.
    bool commandPending = false;
	for (;;) {
        if (!commandPending)
            commandPending = osMessageQueueGet(logQueueHandle, &rxCmd, NULL,
                                               osWaitForever) == osOK;
        if (commandPending) {
            if (!sd_ok) { commandPending = false; continue; }
            if (!SdTryBeginOperation()) {
                /* Preserve FIFO order and retry even if no further event arrives. */
                osDelay(10U);
                continue;
            }
            commandPending = false;

			if (rxCmd.type == CMD_CONFIG_SAVE) {
				if (!StoreConfigRecord(&rxCmd.payload.configData)) {
					sd_available=0U;
					sd_ok=false;
				}

			} else if (rxCmd.type == CMD_LOG_WRITE) {
				// --- РОТАЦИЯ ЛОГОВ (Защита от переполнения) ---
				FILINFO fno;
				if (f_stat(FILE_LOG, &fno) == FR_OK) {
					if (fno.fsize > MAX_LOG_SIZE) {
						// Файл слишком большой. Удаляем его.
						// (Можно переименовать в .bak, но для простоты - удаляем)
						f_unlink(FILE_LOG);
					}
				}

				// Запись лога
				// Используем OPEN_APPEND. Если ошибка - просто игнорируем её.
				if (f_open(&fil, FILE_LOG, FA_WRITE | FA_OPEN_APPEND)
						== FR_OK) {
					f_write(&fil, &rxCmd.payload.logData, sizeof(LogEntry_t),
							&bytesWritten);
					f_close(&fil);
				} else {
					// Если запись не удалась (например, карту выдернули на ходу)
					// Можно попробовать сделать sd_ok = false;
					// Но FatFs может восстановиться при следующем вызове, поэтому просто игнорируем.
				}
			}
            SdCompleteOperation();
		}
	}
}
