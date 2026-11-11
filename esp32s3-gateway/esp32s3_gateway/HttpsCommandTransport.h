#pragma once

#include <Arduino.h>

enum class HttpsCommandState : uint8_t {
  Unknown,
  Reserved,
  Queued,
  InFlight,
  Confirmed,
  Failed,
  Ambiguous
};

enum class HttpsCommandKind : uint8_t {
  Payment,
  Transient,
  Cancellation,
  DurableFinal
};

struct HttpsCommandHandle {
  uint8_t slot = UINT8_MAX;
  uint32_t generation = 0;

  bool valid() const { return slot != UINT8_MAX && generation != 0; }
};

// Diagnostic snapshot only; never changes admission, transport or journal state.
struct HttpsWorkerHealth {
  uint32_t loops=0, lastLoopMs=0, phaseSinceMs=0;
  uint8_t phase=0, used=0, queued=0, inFlight=0;
  int lastHttpCode=0;
  uint32_t lastCompletedMs=0;
  uint8_t lastOutcome=0,lastKind=0;
};

class HttpsCommandTransport {
 public:
  bool begin();
  static bool setEndpoint(const String& url);
  static String endpoint();
  static bool idle();
  static HttpsWorkerHealth health();
  void notifyWifiReady(bool ready);
  HttpsCommandHandle enqueue(const String& payload,
                             HttpsCommandKind kind = HttpsCommandKind::Transient,
                             uint32_t lifetimeMs = 60000U);
  HttpsCommandHandle reserve(HttpsCommandKind kind,
                             uint32_t lifetimeMs = 60000U);
  bool commit(const HttpsCommandHandle& handle, const String& payload);
  HttpsCommandState stateFor(const HttpsCommandHandle& handle) const;
  void cancel(const HttpsCommandHandle& handle);
  void suspendForReboot();
  // Atomically cancel a payment and report whether its POST was committed to an attempt.
  bool cancelPaymentAndWasAttempted(const HttpsCommandHandle& handle);
  void release(const HttpsCommandHandle& handle);
  bool configured() const;
  bool available() const;
  bool tlsClockReady() const;

 private:
  static void workerEntry(void* context);
  void worker();
};
