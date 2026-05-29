#!/bin/bash
# SPDX-License-Identifier: MIT
# Single-model resolution against the catalogue: a bare model name resolves its
# canonical provider (endpoint URL, wire model id, chat_completions grammar) and
# the provider-derived api-key env var; an explicit provider/ prefix pins the
# same routing. No provider presets, no -P.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_basic.json	# defines MOCK_URL / MOCK_PORT

# A catalogue whose single provider points at the mock, exposing canonical
# model "foo" under the provider wire id "bar".
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
  models:
  - canonical_id: foo
    provider_model_id: bar
EOF
run_fyai catalog import catalog.yaml
assert_status 0

export MOCKPROV_API_KEY=mock-secret

run_bare() {
	set +e
	# No -k: the key must come from MOCKPROV_API_KEY (derived from provider).
	"$FYAI_BIN" --color off --no-markdown --no-stream "$@" \
		>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null
	FYAI_STATUS=$?
	set -e
}

# Bare canonical name -> canonical provider, wire id, chat_completions endpoint
# (the only grammar the provider speaks), provider-derived api key.
run_bare -m foo "hello"
assert_status 0
assert_stdout_contains "Hello from the mock provider."
assert_request 0 'r["path"] == "/v1/chat/completions"'
assert_request 0 'r["auth"] == "Bearer mock-secret"'
assert_request 0 'r["body"]["model"] == "bar"'
mock_stop 1

# An explicit provider/ prefix pins the same provider and wire id (no request).
run_bare -m mockprov/foo config effective
assert_status 0
assert_stdout_contains "provider: mockprov"
assert_stdout_contains "model: bar"

pass
