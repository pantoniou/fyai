#!/bin/bash
# SPDX-License-Identifier: MIT
# Delegation after /model keeps the new provider's endpoint and key.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_tool.json

cat > catalog.yaml <<EOF
models:
- name: parent-model
  capabilities: []
- name: child-model
  capabilities: []
providers:
- name: parentprov
  root_url: $MOCK_URL/parent
  endpoints:
  - protocol: responses
    endpoint: /v1/responses
  models:
  - canonical_id: parent-model
- name: childprov
  root_url: $MOCK_URL/child
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: child-model
EOF
run_fyai catalog import catalog.yaml
assert_status 0
run_fyai config set model parentprov/parent-model
assert_status 0

export PARENTPROV_API_KEY=parent-key
export CHILDPROV_API_KEY=child-key
set +e
"$FYAI_BIN" --color off --set display/stream=false -i \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" <<'EOF'
/model child-model
delegate a greeting to a sub-agent
/quit
EOF
FYAI_STATUS=$?
set -e
assert_status 0
assert_stdout_contains "Delegated and done."

for request in 0 1 2 3; do
	assert_request "$request" 'r["path"] == "/child/v1/chat/completions"'
	assert_request "$request" 'r["body"]["model"] == "child-model"'
	assert_request "$request" 'r["auth"] == "Bearer child-key"'
done
mock_stop 4
pass
