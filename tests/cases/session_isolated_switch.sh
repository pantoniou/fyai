#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a session in a view under the credential transport can switch to a
# provider with another key: the session holds the primary control connection, so
# it states the profile of the new provider, and the transport serves it.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
# The scratch directory of a view must lie outside the project.
export TMPDIR="$FYAI_TMPDIR_BASE"
mock_start session_isolated_switch.json

cat > catalog.yaml <<CATALOG
models:
- name: parent-model
  capabilities: []
- name: child-model
  capabilities: []
providers:
- name: parentprov
  root_url: $MOCK_URL/parent
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: parent-model
    provider_model_id: parent-model
- name: childprov
  root_url: $MOCK_URL/child
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: child-model
    provider_model_id: child-model
CATALOG
run_fyai catalog import catalog.yaml
assert_status 0
run_fyai config set model parentprov/parent-model
assert_status 0

export PARENTPROV_API_KEY=parent-key
export CHILDPROV_API_KEY=child-key
set +e
printf 'first question\n/model child-model\nsecond question\n/quit\n' |
	FYAI_TRANSPORT_ISOLATION=level-b "$FYAI_BIN" --color off --set display/stream=false \
		--set display/markdown=false --set view/isolate_session=true -i \
		>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr"
set -e
assert_stdout_contains "First answer."
assert_stdout_contains "Second answer."

assert_request 0 'r["path"] == "/parent/v1/chat/completions" and r["auth"] == "Bearer parent-key"'
assert_request 1 'r["path"] == "/child/v1/chat/completions" and r["auth"] == "Bearer child-key"'
mock_stop 2
pass
