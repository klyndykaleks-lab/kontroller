# Project-local Network 3.3.7

This is a source copy of the ESP32 Arduino core 3.3.7 Network library. It
retains the upstream LGPL-2.1-or-later license headers and is selected only by
`scripts/build-esp.sh --library` for this gateway.

Upstream source: `~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.7/libraries/Network`.
The copied reference files had these SHA-256 values:

- `src/NetworkClient.cpp`: `5dbf304963810625c38a41408135ad090ca2f6469b507fb2c019dc3620cabd58`
- `src/NetworkClient.h`: `c4fa91ef6c964cec72b041d0709a3adc2e0fa06a45efd5135e3cade644ef4aab`

`VMC_LIMITS.patch` records the complete delta. A project-local `setTimeout`
synchronizes the Stream and socket timeout fields, and `NetworkClient::write`
uses that value as an absolute per-call deadline, including zero-progress,
partial-progress and `millis()` wrap cases. WebServer supplies the remaining
aggregate response budget before each call.

2026-11-04: NetworkServer now closes unowned accepted fds on option errors,
closes failed listener fds (bind/listen/fcntl), and closes cached accepted fd on end.
Socket lifecycle fault injection: tests/test_network_socket_health_host.py.
Delta from pre-audit source is SOCKET_LIFECYCLE.patch.
