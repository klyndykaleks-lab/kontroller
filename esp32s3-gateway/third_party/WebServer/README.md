# Project-local WebServer 3.3.7

This directory is a source copy of the ESP32 Arduino core 3.3.7 WebServer library.
It retains the upstream LGPL-2.1-or-later license headers and is compiled only for
this repository through `scripts/build-esp.sh --library`.

Upstream source: `~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.7/libraries/WebServer`.
The reference files copied on 2026-09-19 had these SHA-256 values:

- `src/WebServer.cpp`: `5e5d1fc5e78b9b15b3eea4c098eb5feb684194022af3e5d56fa0755666fa1421`
- `src/WebServer.h`: `c8b8869cf85d6e4948df8e3dfcea1f61041d0c1cfea3b85f12f618ee41959b81`
- `src/Parsing.cpp`: `117ba2a370abf95b7367fff234f9ba2ee42efbb1842df752e7e488c26b04f54b`

`VMC_LIMITS.patch` records the complete delta: an absolute request receive
deadline, bounded request-line/header/body parsing before full-body allocation,
and an absolute response-write deadline that starts on the first response write.
Timer activity is stored separately, so a wrapped deadline value of zero remains
valid. Oversize/unsupported local request bodies use the API-wide `500 {}` rule.
VMC configures 2000 ms receive,
4096 total header bytes, 1024 bytes per line, 32 headers, 512 decoded body bytes,
and 1000 ms response write.
