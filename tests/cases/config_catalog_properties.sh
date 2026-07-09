#!/bin/bash
# SPDX-License-Identifier: MIT
# Config: the config document carries a read-only catalog: block mirroring
# the full catalogue models[] entry for the current model, plus
# canonical_provider - re-derived on every commit and removed when the
# model is not in the catalogue.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

cat > cat.yaml << EOF
models:
- name: cat-model
  display_name: Catalog Model
  context_window: 32000
  max_output_tokens: 1234
  capabilities:
  - tools
  open_source: true
providers:
- name: mock
  root_url: http://127.0.0.1:1
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: cat-model
    provider_model_id: mock-model
EOF
run_fyai catalog import cat.yaml
assert_status 0

# selecting a catalogued model adds the full models[] entry plus
# canonical_provider, read-only, derived from the catalogue.
run_fyai config set model cat-model
assert_status 0
run_fyai config effective
assert_status 0
assert_stdout_contains "catalog:"
assert_stdout_contains "display_name: Catalog Model"
assert_stdout_contains "context_window: 32000"
assert_stdout_contains "open_source: true"
assert_stdout_contains "canonical_provider: mock"

# switching to a model the catalogue does not know drops the block entirely.
run_fyai config set model not-in-catalog
assert_status 0
run_fyai config effective
assert_status 0
assert_stdout_not_contains "catalog:"

# a fresh catalogue import re-syncs the block for the currently configured
# model rather than leaving it stale.
run_fyai config set model cat-model
assert_status 0
cat > cat2.yaml << EOF
models:
- name: cat-model
  display_name: Catalog Model
  context_window: 32000
  max_output_tokens: 1234
  capabilities:
  - tools
  open_source: false
providers:
- name: mock2
  root_url: http://127.0.0.1:1
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: cat-model
    provider_model_id: mock-model
EOF
run_fyai catalog import cat2.yaml
assert_status 0
run_fyai config effective
assert_status 0
assert_stdout_contains "open_source: false"
assert_stdout_contains "canonical_provider: mock2"

pass
