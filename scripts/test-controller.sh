#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_TMP=$(mktemp -d)
trap 'rm -rf "$TEST_TMP"' EXIT HUP INT TERM
cc -std=gnu11 -Wall -Wextra -Werror -Wno-misleading-indentation \
  -I"$ROOT/tests/esp_link_mock" -I"$ROOT/blackpill-f411-controller/Core/Inc" \
  "$ROOT/tests/test_esp_link.c" -o "$TEST_TMP/esp-link"
"$TEST_TMP/esp-link"
cc -std=c11 -Wall -Wextra -Werror -I"$ROOT/blackpill-f411-controller/Core/Inc" \
  "$ROOT/tests/test_mdb_lifecycle.c" "$ROOT/blackpill-f411-controller/Core/Src/mdb.c" \
  -o "$TEST_TMP/mdb-lifecycle"
"$TEST_TMP/mdb-lifecycle"
python3 "$ROOT/tests/test_sd_payment_guard.py"
