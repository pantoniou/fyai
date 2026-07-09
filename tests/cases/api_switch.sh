#!/bin/bash
# SPDX-License-Identifier: MIT
# Live API-grammar switching: /api re-targets the same provider's endpoint for
# the new grammar mid-session (auth style included), rejects a grammar the
# provider does not offer, a continuation keeps the conversation's grammar,
# and the api verb persists the arena config default.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start api_switch.json

# One provider speaking two grammars at the mock, deepseek-style.
cat > catalog.yaml <<EOF
models:
- name: foo
  capabilities: []
providers:
- name: mockprov
  root_url: $MOCK_URL
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  - protocol: messages
    endpoint: /v1/messages
  models:
  - canonical_id: foo
    provider_model_id: bar
EOF
run_fyai catalog import catalog.yaml
assert_status 0

export MOCKPROV_API_KEY=mock-secret

# Drive the REPL over a pipe: chat call, switch to messages, messages call,
# then a grammar the provider does not offer.
set +e
"$FYAI_BIN" --color off --set display/markdown=false --set display/stream=false -i -m foo \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" <<'EOF'
first question
/api messages
second question
/api responses
EOF
FYAI_STATUS=$?
set -e
assert_status 0
assert_stdout_contains "Chat grammar answer."
assert_stdout_contains "api: messages (model bar, provider mockprov"
assert_stdout_contains "Messages grammar answer."
assert_stderr_contains "api: provider 'mockprov' does not offer responses"

# grammar per request: endpoint and auth style both switched
assert_request 0 'r["path"] == "/v1/chat/completions"'
assert_request 0 'r["auth"] == "Bearer mock-secret"'
assert_request 1 'r["path"] == "/v1/messages"'
assert_request 1 'r["x_api_key"] == "mock-secret"'
assert_request 1 'r["body"]["max_tokens"] > 0'

# a continuation keeps the conversation's model, provider and grammar
# (persisted arena config) - the switch sticks completely
run_fyai api
assert_status 0
assert_stdout_contains "api: messages (model bar, provider mockprov"

# the verb persists the project default; --new (a clear) keeps it too
run_fyai api chat-completions
assert_status 0
run_fyai config get api
assert_status 0
assert_stdout_contains "chat-completions"
run_fyai --new api
assert_status 0
assert_stdout_contains "api: chat-completions"

mock_stop 2
pass
