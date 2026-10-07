#!/bin/bash
# SPDX-License-Identifier: MIT
# A provider that reports no cost is priced from the catalogue, at the model
# of each call. The stored conversation keeps the estimate, and a change of
# model says what the lost prompt cache costs.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_basic.json

cat > catalog.yaml <<CAT
models:
- name: cheap-model
  capabilities: []
- name: dear-model
  capabilities: []
providers:
- name: priced
  root_url: $MOCK_URL
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: cheap-model
    provider_model_id: cheap-model
    pricing:
      input: 1000
      output: 4000
      extra: {cache_read: 100}
  - canonical_id: dear-model
    provider_model_id: dear-model
    pricing:
      input: 10000
      output: 40000
      extra: {cache_read: 1000}
CAT
run_fyai catalog import catalog.yaml
assert_status 0

# chat_basic reports 12 prompt and 7 completion tokens, and no cost.
run_fyai --set display/stream=false -m cheap-model "hello"
assert_status 0
mock_stop 1

run_fyai stats --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PY' || fail "the call was not priced from the catalogue"
import json, sys

stats = json.load(open(sys.argv[1]))
want = (12 * 1000 + 7 * 4000) / 1e6
if abs(stats["cost"] - want) > 1e-12 or abs(stats["cost_est"] - want) > 1e-12:
    sys.exit("cost %r, estimated %r, want %r" %
             (stats["cost"], stats.get("cost_est"), want))
PY

# The prompt of the conversation goes to the other model with no cache.
run_fyai config set model cheap-model
assert_status 0
run_fyai model dear-model
assert_status 0
assert_stdout_contains "cache miss: "
assert_stdout_contains " tokens go to dear-model with no cache: ~\$"
assert_stdout_contains " read from the cache of cheap-model (+~\$"

# A session on the stored conversation shows what it cost, marked as an
# estimate, before it makes a call of its own.
run_fyai config set model cheap-model
assert_status 0
FYAI_PTY_INPUT="/model" FYAI_PTY_NEEDLE="model: cheap-model" \
FYAI_PTY_TIMEOUT=30 FYAI_PTY_AFTER="wait-screen:~\$0.0400" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark --set display/markdown=true \
    --set display/screen=fullscreen -b main -i ||
    fail "the session did not show the estimated cost of the stored conversation"

pass
