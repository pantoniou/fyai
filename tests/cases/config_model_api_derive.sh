#!/bin/bash
# SPDX-License-Identifier: MIT
# A model change re-derives the grammar and the endpoint from the new provider,
# and does not carry a forced fallback over as if it had been a choice.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# rich: both grammars. poor: chat-completions only.
cat > catalog.yaml <<'EOF'
models:
- name: rich-model
  capabilities: []
- name: poor-model
  capabilities: []
providers:
- name: richprov
  root_url: https://rich.invalid
  endpoints:
  - protocol: responses
    endpoint: /v1/responses
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: rich-model
- name: poorprov
  root_url: https://poor.invalid
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: poor-model
EOF
run_fyai catalog import catalog.yaml
assert_status 0

run_fyai config set model rich-model
assert_status 0
run_fyai config get api
assert_status 0
assert_stdout_contains "responses"
run_fyai config get api_url
assert_status 0
assert_stdout_contains "https://rich.invalid/v1/responses"

# the poor provider offers one grammar, so its model is served by it ...
run_fyai config set model poor-model
assert_status 0
run_fyai config get api
assert_status 0
assert_stdout_contains "chat-completions"
run_fyai config get api_url
assert_status 0
assert_stdout_contains "https://poor.invalid/v1/chat/completions"

# ... and must not pin the rich provider to it on the way back
run_fyai config set model rich-model
assert_status 0
run_fyai config get api
assert_status 0
assert_stdout_contains "responses"
run_fyai config get api_url
assert_status 0
assert_stdout_contains "https://rich.invalid/v1/responses"

# an explicit grammar holds for as long as the model does
run_fyai config set api chat-completions
assert_status 0
run_fyai config set temperature 0.5
assert_status 0
run_fyai api
assert_status 0
assert_stdout_contains "api: chat-completions"

# a model change re-derives it, like the endpoint and max_tokens
run_fyai config set model poor-model
assert_status 0
run_fyai config set model rich-model
assert_status 0
run_fyai config get api
assert_status 0
assert_stdout_contains "responses"

pass
