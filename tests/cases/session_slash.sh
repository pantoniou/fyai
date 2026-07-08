#!/bin/bash
# SPDX-License-Identifier: MIT
# Interactive slash commands: /help, /context, /model (mid-session switch to a
# second provider with its own env api-key), /clear (fresh chain), /effort
# (session setting reflected in the next request), unknown commands are not
# sent to the model, and the clear/context CLI verbs share the same backends.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start session_slash.json

# Two providers, both speaking chat_completions at the mock, distinguishable
# by endpoint path, wire id and api-key env var.
cat > catalog.yaml <<EOF
models:
- name: foo
  capabilities: [reasoning]
  context_window: 1000
- name: baz
  capabilities: [reasoning]
  context_window: 2000
providers:
- name: mockprov
  root_url: $MOCK_URL
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: foo
    provider_model_id: bar
- name: otherprov
  root_url: $MOCK_URL
  endpoints:
  - protocol: chat_completions
    endpoint: /v2/chat/completions
  models:
  - canonical_id: baz
    provider_model_id: qux
EOF
run_fyai catalog import catalog.yaml
assert_status 0

export MOCKPROV_API_KEY=mock-secret
export OTHERPROV_API_KEY=other-secret

# Drive the REPL over a pipe (non-tty stdin; -i forces the loop).
set +e
"$FYAI_BIN" --color off --no-markdown --no-stream -i -m foo \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" <<'EOF'
/help
/log all start
/log wire stop
/log
/config set display/tool_preview_lines 7
/config get display/tool_preview_lines
/config delete display/tool_preview_lines
/sandbox on
/sandbox
/context
/nope
hello one
/model baz
/logging wire start
/log conversation clear
hello two
/history last 2
/clear
hello three
/reasoning-effort high
/reasoning-summary concise
hello four
/list turns
/list exchanges
/exit
EOF
FYAI_STATUS=$?
set -e
assert_status 0

assert_stdout_contains "start a fresh conversation"
assert_stdout_contains "logging: wire off, stream on, conversation on"
assert_stdout_contains "logging: wire on, stream on, conversation on"
assert_stdout_contains "logging: cleared conversation"
assert_stdout_contains "7"
assert_stdout_contains "sandbox: on"
assert_stdout_contains "Metric       │ Value"
assert_stderr_contains "unknown or ambiguous command '/nope'"
assert_stdout_contains "Reply one."
assert_stdout_contains "model: qux (provider otherprov, api chat-completions)"
assert_stdout_contains "hello two"
assert_stdout_contains "conversation cleared"
assert_stdout_contains "Reply four."
assert_stdout_contains "reasoning-effort: high"
assert_stdout_contains "reasoning-summary: concise"
assert_stdout_contains "/reasoning-effort"
assert_stdout_contains "/effort"
assert_stdout_contains "Index │ Role"
assert_stdout_contains "Index │ Provider"

# Continuation after leaving the REPL keeps the switched provider/model.
set +e
"$FYAI_BIN" --color off --no-markdown --no-stream "hello five" \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null
FYAI_STATUS=$?
set -e
assert_status 0
assert_stdout_contains "Reply five."

# Exactly the five prompts hit the wire; slash lines never did.
mock_stop 5

# Request 0: original provider, wire id, env-derived key.
assert_request 0 'r["path"] == "/v1/chat/completions"'
assert_request 0 'r["auth"] == "Bearer mock-secret"'
assert_request 0 'r["body"]["model"] == "bar"'

# Request 1: after /model baz - other provider endpoint, wire id and key;
# the prior conversation replays across the switch.
assert_request 1 'r["path"] == "/v2/chat/completions"'
assert_request 1 'r["auth"] == "Bearer other-secret"'
assert_request 1 'r["body"]["model"] == "qux"'
assert_request 1 'any("hello one" in str(m.get("content","")) for m in r["body"]["messages"])'

# Request 2: after /clear - a fresh chain (system + the new user turn only).
assert_request 2 'len(r["body"]["messages"]) == 2'
assert_request 2 'not any("hello one" in str(m.get("content","")) for m in r["body"]["messages"])'

# Request 3: after /effort high - the Chat Completions reasoning field.
assert_request 3 'r["body"].get("reasoning_effort") == "high"'

# Request 4: a new process continues on the /model-selected provider.
assert_request 4 'r["path"] == "/v2/chat/completions"'
assert_request 4 'r["auth"] == "Bearer other-secret"'
assert_request 4 'r["body"]["model"] == "qux"'

# The /clear + later turns persisted: state carries the post-clear turns only.
assert_state_contains "hello four" dump state
"$FYAI_BIN" dump state >"$TEST_DIR/state.out" 2>&1
grep -qF "hello one" "$TEST_DIR/state.out" && fail "cleared turn still in state" || true

# CLI verb forms share the backends: context reports, clear resets durably.
run_fyai -m foo context
assert_status 0
assert_stdout_contains "Metric       │ Value"
assert_stdout_contains "Next request"

run_fyai config get display/tool_preview_lines
assert_status_nonzero
assert_stderr_contains "not set"

run_fyai clear
assert_status 0
assert_stdout_contains "conversation cleared"
"$FYAI_BIN" dump state >"$TEST_DIR/state.out" 2>&1
grep -qF "hello four" "$TEST_DIR/state.out" && fail "clear verb did not reset head" || true

pass
