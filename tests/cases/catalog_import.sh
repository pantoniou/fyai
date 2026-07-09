#!/bin/bash
# SPDX-License-Identifier: MIT
# Catalog: an imported scrape-providers document drives model->endpoint
# resolution, capability validation and max_tokens defaulting.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_basic_twice.json

# a catalogue whose only provider is the mock server
cat > cat.yaml << EOF
models:
- name: cat-model
  display_name: Catalog Model
  context_window: 32000
  max_output_tokens: 1234
  capabilities:
  - tools
providers:
- name: mock
  root_url: $MOCK_URL
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: cat-model
    provider_model_id: mock-model
    pricing:
      currency: USD
      unit: per_million_tokens
      input: 1.0
      output: 2.0
EOF

run_fyai catalog import cat.yaml
assert_status 0
assert_stdout_contains "1 models, 1 providers"

# the arena catalogue shows (not the embedded snapshot)
run_fyai catalog list models
assert_status 0
assert_stdout_contains "cat-model"
assert_stdout_not_contains "claude-fable-5"

# export round-trips the arena catalogue, to stdout or a file
run_fyai catalog export
assert_status 0
assert_stdout_contains "cat-model"
assert_stdout_contains "mock-model"

run_fyai catalog export cat-export.yaml
assert_status 0
grep -q "cat-model" cat-export.yaml
grep -q "mock-model" cat-export.yaml

run_fyai -m cat-model list --raw providers
assert_status 0
assert_stdout_contains "| Provider | Models |"
assert_stdout_contains "| **mock** | **cat-model** |"

run_fyai -m cat-model list --raw models
assert_status 0
assert_stdout_contains "| Model | Providers | Context | Max Output | Open |"
assert_stdout_contains "| **cat-model** | **mock** | 32000 | 1234 | no |"

# no -u / no api selection: the catalogue resolves endpoint, grammar and
# wire model id from the canonical name
run_fyai --set display/stream=false -m cat-model "hello"
assert_status 0
assert_stdout_contains "Hello from the mock"
assert_request 0 'r["path"] == "/v1/chat/completions"'
assert_request 0 'r["body"]["model"] == "mock-model"'
assert_request 0 '"temperature" in r["body"]'

cat > cat.yaml << EOF
models:
- name: cat-model
  display_name: Catalog Model
  context_window: 32000
  max_output_tokens: 1234
  capabilities:
  - tools
- name: reasoning-model
  display_name: Reasoning Model
  context_window: 32000
  max_output_tokens: 1234
  capabilities:
  - reasoning
  - tools
providers:
- name: mock
  root_url: $MOCK_URL
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: cat-model
    provider_model_id: mock-model
    pricing:
      currency: USD
      unit: per_million_tokens
      input: 1.0
      output: 2.0
  - canonical_id: reasoning-model
    provider_model_id: reasoning-wire-model
    pricing:
      currency: USD
      unit: per_million_tokens
      input: 1.0
      output: 2.0
EOF
run_fyai catalog import cat.yaml
assert_status 0

run_fyai --set display/stream=false -m reasoning-model "hello"
assert_status 0
assert_request 1 'r["body"]["model"] == "reasoning-wire-model"'
assert_request 1 '"temperature" not in r["body"]'

# max_tokens defaults from the model's max_output_tokens - a catalog
# derivation, visible on the api verb but never persisted into the config
run_fyai -m cat-model api
assert_status 0
assert_stdout_contains "max_tokens 1234"

# reasoning options on a non-reasoning model are rejected up front
run_fyai --set display/stream=false -m cat-model --set reasoning/effort=high "hello"
assert_status_nonzero
assert_stderr_contains "not reasoning-capable"

# unknown models skip catalogue validation entirely
run_fyai -m not-in-catalog config effective
assert_status 0

mock_stop 2
pass
