#ifndef INC_MDB_H_
#define INC_MDB_H_

#include <stdbool.h>
#include <stdint.h>

#define MDB_POLL_QUEUE_CAPACITY 6U

#define MDB_PAYMENT_RESULT_APPROVED 1U
#define MDB_PAYMENT_RESULT_DENIED   2U
#define MDB_PAYMENT_RESULT_TIMEOUT  3U

typedef enum {
	MDB_POLL_NONE = 0,
	MDB_POLL_JUST_RESET,
	MDB_POLL_BEGIN_SESSION,
	MDB_POLL_VEND_APPROVED,
	MDB_POLL_VEND_DENIED,
	MDB_POLL_END_SESSION,
	MDB_POLL_COMMAND_OOS
} MdbPendingPollResponse_t;

typedef enum {
	MDB_SESSION_INACTIVE = 0,
	MDB_SESSION_DISABLED,
	MDB_SESSION_ENABLED,
	MDB_SESSION_IDLE,
	MDB_SESSION_WAIT_USB_RESULT,
	MDB_SESSION_APPROVAL_PENDING,
	MDB_SESSION_DENIED_PENDING,
	MDB_SESSION_APPROVED
} MdbSessionState_t;

typedef enum {
	MDB_FSM_ACCEPTED = 0,
	MDB_FSM_DUPLICATE,
	MDB_FSM_COMPAT_CANCEL,
	MDB_FSM_STALE,
	MDB_FSM_OUT_OF_SEQUENCE,
	MDB_FSM_ABORTED_OUT_OF_SEQUENCE,
	MDB_FSM_QUEUE_FULL
} MdbFsmAction_t;

typedef struct {
	MdbPendingPollResponse_t type;
	uint16_t amount;
	uint32_t transactionId;
} MdbPollReply_t;

typedef struct {
	MdbSessionState_t sessionState;
	bool readerEnabled;
	bool setupSeen;
	bool resetRequired;
	uint16_t currentSelectionId;
	uint16_t currentPriceMinor;
	uint32_t currentTransactionId;
	uint32_t nextTransactionId;
	uint8_t lastVendResultSubCommand;
	MdbPendingPollResponse_t pollQueue[MDB_POLL_QUEUE_CAPACITY];
	uint8_t pollQueueHead;
	uint8_t pollQueueCount;
	uint32_t deadlineAt;
	bool deadlineActive;
} MdbCashlessFsm_t;

void MdbFsmInit(MdbCashlessFsm_t *fsm);
void MdbFsmReset(MdbCashlessFsm_t *fsm);
MdbFsmAction_t MdbFsmHandleSetupConfig(MdbCashlessFsm_t *fsm);
MdbFsmAction_t MdbFsmHandleReaderEnable(MdbCashlessFsm_t *fsm);
MdbFsmAction_t MdbFsmHandleReaderDisable(MdbCashlessFsm_t *fsm);
MdbFsmAction_t MdbFsmHandleReaderCancel(MdbCashlessFsm_t *fsm);
MdbFsmAction_t MdbFsmHandleVendRequest(MdbCashlessFsm_t *fsm,
		uint16_t selectionId, uint16_t priceMinor);
MdbFsmAction_t MdbFsmApplyPaymentResult(MdbCashlessFsm_t *fsm,
		uint32_t transactionId, uint8_t resultCode);
MdbFsmAction_t MdbFsmHandleVendCancel(MdbCashlessFsm_t *fsm);
MdbFsmAction_t MdbFsmHandleVendResult(MdbCashlessFsm_t *fsm,
		uint8_t vendSubCommand);
MdbFsmAction_t MdbFsmHandleSessionComplete(MdbCashlessFsm_t *fsm);
MdbFsmAction_t MdbFsmQueueCommandOos(MdbCashlessFsm_t *fsm);
bool MdbFsmNextPollReply(MdbCashlessFsm_t *fsm, MdbPollReply_t *reply);
bool MdbFsmPeekPollReply(MdbCashlessFsm_t *fsm, MdbPollReply_t *reply);
bool MdbFsmAcknowledgePollReply(MdbCashlessFsm_t *fsm, const MdbPollReply_t *reply);
void MdbFsmSetDeadline(MdbCashlessFsm_t *fsm, uint32_t now, uint32_t durationMs);
MdbFsmAction_t MdbFsmCheckDeadline(MdbCashlessFsm_t *fsm, uint32_t now);
uint8_t MdbFsmProtocolStatus(const MdbCashlessFsm_t *fsm);
bool MdbFsmHasPendingResponse(const MdbCashlessFsm_t *fsm,
		MdbPendingPollResponse_t response);

#endif /* INC_MDB_H_ */
