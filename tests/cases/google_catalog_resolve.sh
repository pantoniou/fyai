#!/bin/bash
# SPDX-License-Identifier: MIT
# Google-shaped catalogue resolution: the provider lists its native
# generate_content protocol first — a protocol fyai does not speak — followed
# by the OpenAI-compatible chat_completions endpoint. Resolution must skip the
# unknown protocol, derive the chat_completions URL, authenticate with Bearer
# from the provider-derived GOOGLE_API_KEY, and send the wire model id.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_basic.json	# defines MOCK_URL / MOCK_PORT

# The google provider block as scrape-providers emits it, with root_url
# pointed at the mock. generate_content deliberately precedes chat_completions,
# mirroring data/catalog.yaml.
cat > catalog.yaml <<EOF
models:
- name: gemini-3.5-flash
  capabilities: []
providers:
- name: google
  root_url: $MOCK_URL
  endpoints:
  - protocol: generate_content
    endpoint: /v1beta/models/{model}:generateContent
  - protocol: chat_completions
    endpoint: /v1beta/openai/chat/completions
  models:
  - canonical_id: gemini-3.5-flash
    provider_model_id: gemini-3.5-flash
EOF
run_fyai catalog import catalog.yaml
assert_status 0

export GOOGLE_API_KEY=mock-secret

run_bare() {
	set +e
	# No -k: the key must come from GOOGLE_API_KEY (derived from provider).
	"$FYAI_BIN" --color off --set display/markdown=false --set display/stream=false "$@" \
		>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null
	FYAI_STATUS=$?
	set -e
}

# Bare model name -> google provider; the unsupported generate_content
# endpoint is skipped and the OpenAI-compatible surface is used.
run_bare -m gemini-3.5-flash "hello"
assert_status 0
assert_stdout_contains "Hello from the mock provider."
assert_request 0 'r["path"] == "/v1beta/openai/chat/completions"'
assert_request 0 'r["auth"] == "Bearer mock-secret"'
assert_request 0 'r["body"]["model"] == "gemini-3.5-flash"'
mock_stop 1

# The api verb reports the resolved provider/model without a request.
run_bare -m gemini-3.5-flash api
assert_status 0
assert_stdout_contains "model gemini-3.5-flash, provider google"

pass
