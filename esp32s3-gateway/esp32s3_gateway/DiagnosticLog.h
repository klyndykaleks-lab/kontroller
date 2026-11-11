#pragma once
#include <Arduino.h>
enum class DiagnosticEvent : uint8_t { Boot, TerminalRequest, TerminalResponse, BackendAttempt, BackendResponse, Flow, Network };
void diagnosticBegin(uint32_t boot, const char* version, const char* resetReason="startup");
void diagnosticRecord(DiagnosticEvent event, const char* context, const char* message, int code=0, bool important=false, const char* attributes=nullptr);
void diagnosticBusy(bool busy);
bool diagnosticSetEndpoint(const String& endpoint);
String diagnosticEndpoint();
String diagnosticStats();

// Called only by the existing transport worker when no business request is ready.
void diagnosticServiceIdle();
