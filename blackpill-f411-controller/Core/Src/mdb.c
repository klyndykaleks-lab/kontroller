#include "mdb.h"

#include <string.h>

#define MDB_VEND_SUB_SUCCESS 0x02U
#define MDB_VEND_SUB_FAILURE 0x03U

/* Temporary machine-compatibility profile for the motorless MDB tester.
 * Define to 0 for the strict production policy that requires an MDB reset
 * after an out-of-sequence command aborts a live vend. */
#ifndef MDB_COMPAT_RECOVER_AFTER_OOS
#define MDB_COMPAT_RECOVER_AFTER_OOS 1U
#endif

static void MdbFsmClearTransaction(MdbCashlessFsm_t *fsm) {
	fsm->currentSelectionId = 0U;
	fsm->currentPriceMinor = 0U;
	fsm->currentTransactionId = 0U;
	fsm->lastVendResultSubCommand = 0U;
	fsm->deadlineActive = false;
}

static void MdbFsmClearPollQueue(MdbCashlessFsm_t *fsm) {
	memset(fsm->pollQueue, 0, sizeof(fsm->pollQueue));
	fsm->pollQueueHead = 0U;
	fsm->pollQueueCount = 0U;
}

bool MdbFsmHasPendingResponse(const MdbCashlessFsm_t *fsm,
		MdbPendingPollResponse_t response) {
	if (fsm == NULL) {
		return false;
	}

	for (uint8_t i = 0U; i < fsm->pollQueueCount; i++) {
		uint8_t index = (uint8_t)((fsm->pollQueueHead + i)
				% MDB_POLL_QUEUE_CAPACITY);
		if (fsm->pollQueue[index] == response) {
			return true;
		}
	}

	return false;
}

static bool MdbFsmQueuePollResponse(MdbCashlessFsm_t *fsm,
		MdbPendingPollResponse_t response) {
	uint8_t tail;

	if (response == MDB_POLL_NONE
			|| MdbFsmHasPendingResponse(fsm, response)) {
		return true;
	}
	if (fsm->pollQueueCount >= MDB_POLL_QUEUE_CAPACITY) {
		return false;
	}

	tail = (uint8_t)((fsm->pollQueueHead + fsm->pollQueueCount)
			% MDB_POLL_QUEUE_CAPACITY);
	fsm->pollQueue[tail] = response;
	fsm->pollQueueCount++;
	return true;
}

static void MdbFsmRemovePollResponse(MdbCashlessFsm_t *fsm,
		MdbPendingPollResponse_t response) {
	MdbPendingPollResponse_t kept[MDB_POLL_QUEUE_CAPACITY];
	uint8_t keptCount = 0U;

	for (uint8_t i = 0U; i < fsm->pollQueueCount; i++) {
		uint8_t index = (uint8_t)((fsm->pollQueueHead + i)
				% MDB_POLL_QUEUE_CAPACITY);
		if (fsm->pollQueue[index] != response) {
			kept[keptCount++] = fsm->pollQueue[index];
		}
	}

	MdbFsmClearPollQueue(fsm);
	for (uint8_t i = 0U; i < keptCount; i++) {
		(void)MdbFsmQueuePollResponse(fsm, kept[i]);
	}
}

void MdbFsmInit(MdbCashlessFsm_t *fsm) {
	if (fsm == NULL) {
		return;
	}

	memset(fsm, 0, sizeof(*fsm));
	MdbFsmReset(fsm);
}

void MdbFsmReset(MdbCashlessFsm_t *fsm) {
	uint32_t nextTransactionId;

	if (fsm == NULL) {
		return;
	}

	nextTransactionId = fsm->nextTransactionId;
	memset(fsm, 0, sizeof(*fsm));
	fsm->nextTransactionId = nextTransactionId;
	fsm->sessionState = MDB_SESSION_INACTIVE;
	(void)MdbFsmQueuePollResponse(fsm, MDB_POLL_JUST_RESET);
}

MdbFsmAction_t MdbFsmHandleSetupConfig(MdbCashlessFsm_t *fsm) {
	if (fsm == NULL) {
		return MDB_FSM_OUT_OF_SEQUENCE;
	}
	if (fsm->resetRequired || (fsm->sessionState != MDB_SESSION_INACTIVE
			&& fsm->sessionState != MDB_SESSION_DISABLED)) {
		return MdbFsmQueueCommandOos(fsm);
	}

	fsm->setupSeen = true;
	fsm->readerEnabled = false;
	fsm->sessionState = MDB_SESSION_DISABLED;
	return MDB_FSM_ACCEPTED;
}

MdbFsmAction_t MdbFsmHandleReaderEnable(MdbCashlessFsm_t *fsm) {
	if (fsm == NULL || fsm->resetRequired || !fsm->setupSeen
			|| fsm->sessionState == MDB_SESSION_INACTIVE) {
		return MdbFsmQueueCommandOos(fsm);
	}

	fsm->readerEnabled = true;
	if (fsm->sessionState != MDB_SESSION_DISABLED
			&& fsm->sessionState != MDB_SESSION_ENABLED) {
		return MDB_FSM_ACCEPTED;
	}
	fsm->sessionState = MDB_SESSION_ENABLED;
	if (!MdbFsmQueuePollResponse(fsm, MDB_POLL_BEGIN_SESSION)) {
		return MDB_FSM_QUEUE_FULL;
	}
	return MDB_FSM_ACCEPTED;
}

MdbFsmAction_t MdbFsmHandleReaderDisable(MdbCashlessFsm_t *fsm) {
	if (fsm == NULL || fsm->resetRequired || !fsm->setupSeen
			|| fsm->sessionState == MDB_SESSION_INACTIVE) {
		return MdbFsmQueueCommandOos(fsm);
	}

	fsm->readerEnabled = false;
	/* Disable prevents the NEXT session. An existing session/vend must finish. */
	if (fsm->sessionState == MDB_SESSION_DISABLED
			|| fsm->sessionState == MDB_SESSION_ENABLED) {
		fsm->sessionState = MDB_SESSION_DISABLED;
		MdbFsmRemovePollResponse(fsm, MDB_POLL_BEGIN_SESSION);
	}
	return MDB_FSM_ACCEPTED;
}

MdbFsmAction_t MdbFsmHandleReaderCancel(MdbCashlessFsm_t *fsm) {
	if (fsm == NULL || fsm->resetRequired
			|| fsm->sessionState != MDB_SESSION_ENABLED) {
		return MdbFsmQueueCommandOos(fsm);
	}
	return MDB_FSM_ACCEPTED;
}

MdbFsmAction_t MdbFsmHandleVendRequest(MdbCashlessFsm_t *fsm,
		uint16_t selectionId, uint16_t priceMinor) {
	if (fsm == NULL) {
		return MDB_FSM_OUT_OF_SEQUENCE;
	}
	if (fsm->resetRequired) {
		return MdbFsmQueueCommandOos(fsm);
	}

	if ((fsm->sessionState == MDB_SESSION_WAIT_USB_RESULT
			|| fsm->sessionState == MDB_SESSION_APPROVAL_PENDING
			|| fsm->sessionState == MDB_SESSION_DENIED_PENDING)
			&& fsm->currentSelectionId == selectionId
			&& fsm->currentPriceMinor == priceMinor) {
		return MDB_FSM_DUPLICATE;
	}

	if (!fsm->readerEnabled || fsm->sessionState != MDB_SESSION_IDLE
			|| MdbFsmHasPendingResponse(fsm, MDB_POLL_END_SESSION)) {
		return MdbFsmQueueCommandOos(fsm);
	}

	/* A new boot identity is required before transaction IDs can repeat. */
	if (fsm->nextTransactionId >= 0x7FFFFFFFU) return MdbFsmQueueCommandOos(fsm);
	fsm->nextTransactionId++;
	fsm->currentTransactionId = fsm->nextTransactionId;
	fsm->currentSelectionId = selectionId;
	fsm->currentPriceMinor = priceMinor;
	fsm->lastVendResultSubCommand = 0U;
	fsm->sessionState = MDB_SESSION_WAIT_USB_RESULT;
	return MDB_FSM_ACCEPTED;
}

MdbFsmAction_t MdbFsmApplyPaymentResult(MdbCashlessFsm_t *fsm,
		uint32_t transactionId, uint8_t resultCode) {
	MdbPendingPollResponse_t response;

	if (fsm == NULL || fsm->resetRequired || transactionId == 0U
			|| transactionId != fsm->currentTransactionId) {
		return MDB_FSM_STALE;
	}
	if (fsm->sessionState == MDB_SESSION_APPROVAL_PENDING
			|| fsm->sessionState == MDB_SESSION_DENIED_PENDING) {
		return MDB_FSM_DUPLICATE;
	}
	if (fsm->sessionState != MDB_SESSION_WAIT_USB_RESULT) {
		return MDB_FSM_STALE;
	}

	if (resultCode == MDB_PAYMENT_RESULT_APPROVED) {
		response = MDB_POLL_VEND_APPROVED;
		fsm->sessionState = MDB_SESSION_APPROVAL_PENDING;
	} else if (resultCode == MDB_PAYMENT_RESULT_DENIED
			|| resultCode == MDB_PAYMENT_RESULT_TIMEOUT) {
		response = MDB_POLL_VEND_DENIED;
		fsm->sessionState = MDB_SESSION_DENIED_PENDING;
	} else {
		return MDB_FSM_STALE;
	}

	if (!MdbFsmQueuePollResponse(fsm, response)) {
		fsm->sessionState = MDB_SESSION_WAIT_USB_RESULT;
		return MDB_FSM_QUEUE_FULL;
	}
	return MDB_FSM_ACCEPTED;
}

MdbFsmAction_t MdbFsmHandleVendCancel(MdbCashlessFsm_t *fsm) {
	if (fsm == NULL) {
		return MDB_FSM_OUT_OF_SEQUENCE;
	}
	if (fsm->resetRequired) {
		return MdbFsmQueueCommandOos(fsm);
	}
	if (fsm->sessionState == MDB_SESSION_DENIED_PENDING) {
		return MDB_FSM_DUPLICATE;
	}
	if (fsm->sessionState != MDB_SESSION_WAIT_USB_RESULT
			&& fsm->sessionState != MDB_SESSION_APPROVAL_PENDING) {
		return MdbFsmQueueCommandOos(fsm);
	}

	MdbFsmRemovePollResponse(fsm, MDB_POLL_VEND_APPROVED);
	if (!MdbFsmQueuePollResponse(fsm, MDB_POLL_VEND_DENIED)) {
		return MDB_FSM_QUEUE_FULL;
	}
	fsm->sessionState = MDB_SESSION_DENIED_PENDING;
	return MDB_FSM_ACCEPTED;
}

MdbFsmAction_t MdbFsmHandleVendResult(MdbCashlessFsm_t *fsm,
		uint8_t vendSubCommand) {
	if (fsm == NULL || (vendSubCommand != MDB_VEND_SUB_SUCCESS
			&& vendSubCommand != MDB_VEND_SUB_FAILURE)) {
		return MDB_FSM_OUT_OF_SEQUENCE;
	}
	if (fsm->resetRequired) {
		return MdbFsmQueueCommandOos(fsm);
	}

	if (fsm->sessionState == MDB_SESSION_IDLE
			&& fsm->lastVendResultSubCommand == vendSubCommand
			&& fsm->currentTransactionId != 0U) {
		return MDB_FSM_DUPLICATE;
	}
	if (fsm->sessionState != MDB_SESSION_APPROVED) {
		return MdbFsmQueueCommandOos(fsm);
	}

	fsm->lastVendResultSubCommand = vendSubCommand;
	fsm->sessionState = MDB_SESSION_IDLE;
	return MDB_FSM_ACCEPTED;
}

MdbFsmAction_t MdbFsmHandleSessionComplete(MdbCashlessFsm_t *fsm) {
	if (fsm == NULL) {
		return MDB_FSM_OUT_OF_SEQUENCE;
	}
	if (fsm->resetRequired) {
		return MdbFsmQueueCommandOos(fsm);
	}

	if (MdbFsmHasPendingResponse(fsm, MDB_POLL_END_SESSION)) {
		return MDB_FSM_DUPLICATE;
	}

	if (fsm->sessionState == MDB_SESSION_WAIT_USB_RESULT) {
		if (!MdbFsmQueuePollResponse(fsm, MDB_POLL_END_SESSION)) {
			return MDB_FSM_QUEUE_FULL;
		}
		fsm->sessionState = MDB_SESSION_IDLE;
		return MDB_FSM_COMPAT_CANCEL;
	}

	if (fsm->sessionState == MDB_SESSION_DENIED_PENDING) {
		if (!MdbFsmQueuePollResponse(fsm, MDB_POLL_END_SESSION)) {
			return MDB_FSM_QUEUE_FULL;
		}
		return MDB_FSM_ACCEPTED;
	}

	if (fsm->sessionState != MDB_SESSION_IDLE) {
		return MdbFsmQueueCommandOos(fsm);
	}

	if (!MdbFsmQueuePollResponse(fsm, MDB_POLL_END_SESSION)) {
		return MDB_FSM_QUEUE_FULL;
	}
	return MDB_FSM_ACCEPTED;
}

MdbFsmAction_t MdbFsmQueueCommandOos(MdbCashlessFsm_t *fsm) {
	bool vendWasActive;

	if (fsm == NULL) {
		return MDB_FSM_OUT_OF_SEQUENCE;
	}

	vendWasActive = fsm->currentTransactionId != 0U
			|| fsm->sessionState == MDB_SESSION_WAIT_USB_RESULT
			|| fsm->sessionState == MDB_SESSION_APPROVAL_PENDING
			|| fsm->sessionState == MDB_SESSION_DENIED_PENDING
			|| fsm->sessionState == MDB_SESSION_APPROVED;
	if (vendWasActive) {
		MdbFsmClearPollQueue(fsm);
		fsm->sessionState = MDB_SESSION_IDLE;
	}
	fsm->deadlineActive = false;
#if MDB_COMPAT_RECOVER_AFTER_OOS
	/* The tester must keep accepting selections from machines that issue
	 * vendor-specific or repeated commands. A live vend is cancelled above;
	 * its late payment result is stale because the state is now IDLE. */
	fsm->resetRequired = false;
#else
	fsm->resetRequired = vendWasActive;
#endif
	if (!MdbFsmQueuePollResponse(fsm, MDB_POLL_COMMAND_OOS)) {
		return MDB_FSM_QUEUE_FULL;
	}
	return vendWasActive ? MDB_FSM_ABORTED_OUT_OF_SEQUENCE
			: MDB_FSM_OUT_OF_SEQUENCE;
}

bool MdbFsmPeekPollReply(MdbCashlessFsm_t *fsm, MdbPollReply_t *reply) {
	if (fsm == NULL || reply == NULL) {
		return false;
	}

	if (fsm->pollQueueCount == 0U && !fsm->resetRequired && fsm->readerEnabled
			&& fsm->sessionState == MDB_SESSION_ENABLED) {
		(void)MdbFsmQueuePollResponse(fsm, MDB_POLL_BEGIN_SESSION);
	}

	if (fsm->pollQueueCount == 0U) {
		reply->type = MDB_POLL_NONE;
		reply->amount = 0U;
		reply->transactionId = fsm->currentTransactionId;
		return false;
	}

	reply->type = fsm->pollQueue[fsm->pollQueueHead];
	reply->amount = fsm->currentPriceMinor;
	reply->transactionId = fsm->currentTransactionId;
	return true;
}

/* Consuming a reply is a delivery commit, never merely preparing its bytes. */
bool MdbFsmNextPollReply(MdbCashlessFsm_t *fsm, MdbPollReply_t *reply) {
	if (!MdbFsmPeekPollReply(fsm, reply)) return false;
	MdbPendingPollResponse_t response = reply->type;
	fsm->pollQueue[fsm->pollQueueHead] = MDB_POLL_NONE;
	fsm->pollQueueHead = (uint8_t)((fsm->pollQueueHead + 1U)
			% MDB_POLL_QUEUE_CAPACITY);
	fsm->pollQueueCount--;

	reply->type = response;
	reply->amount = fsm->currentPriceMinor;
	reply->transactionId = fsm->currentTransactionId;

	switch (response) {
	case MDB_POLL_BEGIN_SESSION:
		fsm->sessionState = MDB_SESSION_IDLE;
		break;
	case MDB_POLL_VEND_APPROVED:
		fsm->sessionState = MDB_SESSION_APPROVED;
		break;
	case MDB_POLL_VEND_DENIED:
		fsm->sessionState = MDB_SESSION_IDLE;
		break;
	case MDB_POLL_END_SESSION:
		fsm->sessionState = fsm->readerEnabled ? MDB_SESSION_ENABLED
				: MDB_SESSION_DISABLED;
		MdbFsmClearTransaction(fsm);
		break;
	case MDB_POLL_JUST_RESET:
	case MDB_POLL_COMMAND_OOS:
	case MDB_POLL_NONE:
	default:
		break;
	}

	return true;
}

bool MdbFsmAcknowledgePollReply(MdbCashlessFsm_t *fsm, const MdbPollReply_t *reply) {
	MdbPollReply_t pending;
	if (reply == NULL || !MdbFsmPeekPollReply(fsm, &pending)
			|| pending.type != reply->type
			|| pending.transactionId != reply->transactionId) return false;
	return MdbFsmNextPollReply(fsm, &pending);
}

void MdbFsmSetDeadline(MdbCashlessFsm_t *fsm, uint32_t now, uint32_t durationMs) {
	fsm->deadlineAt = now + durationMs;
	fsm->deadlineActive = true;
}

MdbFsmAction_t MdbFsmCheckDeadline(MdbCashlessFsm_t *fsm, uint32_t now) {
	if (!fsm->deadlineActive || (int32_t)(now - fsm->deadlineAt) < 0)
		return MDB_FSM_STALE;
	fsm->deadlineActive = false;
	if (fsm->sessionState == MDB_SESSION_WAIT_USB_RESULT
			|| fsm->sessionState == MDB_SESSION_APPROVAL_PENDING) {
		MdbFsmRemovePollResponse(fsm, MDB_POLL_VEND_APPROVED);
		fsm->sessionState = MDB_SESSION_DENIED_PENDING;
		if (!MdbFsmQueuePollResponse(fsm, MDB_POLL_VEND_DENIED))
			return MdbFsmQueueCommandOos(fsm);
		return MDB_FSM_ACCEPTED;
	}
	/* An abandoned wire session cannot be silently reused after manager timeout. */
	return MdbFsmQueueCommandOos(fsm);
}

uint8_t MdbFsmProtocolStatus(const MdbCashlessFsm_t *fsm) {
	if (fsm == NULL) {
		return 0x01U;
	}

	switch (fsm->sessionState) {
	case MDB_SESSION_INACTIVE:
		return 0x01U;
	case MDB_SESSION_DISABLED:
		return 0x02U;
	case MDB_SESSION_ENABLED:
		return 0x03U;
	case MDB_SESSION_IDLE:
		return 0x04U;
	case MDB_SESSION_WAIT_USB_RESULT:
	case MDB_SESSION_APPROVAL_PENDING:
	case MDB_SESSION_DENIED_PENDING:
	case MDB_SESSION_APPROVED:
		return 0x05U;
	default:
		return 0x01U;
	}
}
