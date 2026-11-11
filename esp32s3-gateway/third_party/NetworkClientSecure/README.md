# Project-local NetworkClientSecure 3.3.7

This is a source copy of the ESP32 Arduino core 3.3.7 NetworkClientSecure
library. It retains the upstream Apache-2.0/LGPL license headers and is selected
only by `scripts/build-esp.sh --library` for this gateway. CA bundle and
hostname verification behavior are unchanged.

Upstream source:
`~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.7/libraries/NetworkClientSecure`.
The copied reference files had these SHA-256 values:

- `src/NetworkClientSecure.cpp`: `8c97b525e485270c59b78a4d4e05eba0672dff1be5a9f4b1b3bdbfe3d9b5e8c8`
- `src/NetworkClientSecure.h`: `15cbe72bd698aa6382fa39ffa42b0069cec695ece1957587d3414281c0b92b48`
- `src/ssl_client.cpp`: `9df8e4e8950619647659fedbc3a00b84b0a7e4a59c5941b5d2858d530ceecdf7`
- `src/ssl_client.h`: `b8ec7c711bdb795481332e4a6c234ee2c5afe6cdaae74a8002d545df09a6d3bb`

`VMC_LIMITS.patch` records the complete delta. `setTimeout(remaining)` now
updates the TLS context used by application reads/writes, and `send_ssl_data`
uses one absolute per-call deadline rather than resetting it after progress.
HTTPClient supplies the remaining 8-second aggregate application-I/O budget;
TCP connect and TLS handshake retain their separate limits.
