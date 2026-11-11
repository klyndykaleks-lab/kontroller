#pragma once
#include <stdint.h>

// Fixed policy storage: one identity per MDB/direct result, not a result queue.
struct FinalResultBudget {
  uint32_t magic, first, second, remainingMs;
  uint64_t expiresUnixMs;
  uint8_t phase; // 1 pending; 2 confirmed; 3 explicitly abandoned after budget
  uint8_t reserved[7];
};
constexpr uint32_t FINAL_RESULT_BUDGET_MS=70000U;
constexpr uint32_t FINAL_RESULT_RETRY_MS=5000U;
void finalResultPolicyBegin();
bool finalResultTerminal(bool direct,uint32_t first,uint32_t second);
uint32_t finalResultRemaining(bool direct,uint32_t first,uint32_t second);
void finalResultComplete(bool direct,uint32_t first,uint32_t second,bool confirmed);
