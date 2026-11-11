# Project-local HTTPClient 3.3.7

This is a source copy of the ESP32 Arduino core 3.3.7 HTTPClient library. It
retains the upstream LGPL-2.1-or-later license headers and is selected only by
`scripts/build-esp.sh --library` for this gateway.

Upstream source: `~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.7/libraries/HTTPClient`.
The copied reference files had these SHA-256 values:

- `src/HTTPClient.cpp`: `66e6eaa952a3de03c01aaf3f4b2b097394c4857f1f4bd20334228d95641adab2`
- `src/HTTPClient.h`: `f33abcb03f2a79eef2f4fae7877ae1a8cc56a02eaf5e875f9f924a7c7f6c5601`

`VMC_LIMITS.patch` records the complete delta. It adds an 8000 ms absolute
application-I/O deadline after connect/TLS, a 5000 ms / 4096-byte response
header reader, and a 2000 ms / 256-byte JSON body reader. Fixed-length,
unknown-length JSON, and chunked bodies remain supported. The active transport
does not call upstream `getString()`.
