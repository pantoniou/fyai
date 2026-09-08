#!/bin/bash
# SPDX-License-Identifier: MIT
# A persona selects a model of another provider. The sub-agent leg must use
# the endpoint, model and key of that provider. A second delegation to the
# same name has no persona. It revives the branch and must keep the stored
# provider.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_persona_provider.json

cat > catalog.yaml <<EOF
models:
- name: parent-model
  capabilities: []
- name: child-model
  capabilities: []
providers:
- name: openaiprov
  root_url: $MOCK_URL/openai
  endpoints:
  - protocol: responses
    endpoint: /v1/responses
  models:
  - canonical_id: parent-model
- name: otherprov
  root_url: $MOCK_URL/other
  endpoints:
  - protocol: responses
    endpoint: /v1/responses
  models:
  - canonical_id: child-model
EOF
run_fyai catalog import catalog.yaml
assert_status 0

export OPENAIPROV_API_KEY=parent-key
export OTHERPROV_API_KEY=child-key
set +e
"$FYAI_BIN" --color off \
	 --set tools=true --set display/stream=false --set api=responses \
	 --set api_url="$MOCK_URL/openai/v1/responses" -m openaiprov/parent-model \
	 --set 'agent/personas/other/description=Other provider.' \
	 --set 'agent/personas/other/system_prompt=You are OTHER. Report.' \
	 --set 'agent/personas/other/model=otherprov/child-model' \
	 "delegate twice to the scout agent" \
	 >"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null
FYAI_STATUS=$?
set -e
assert_status 0
assert_stdout_contains "Delegated and done."

# The parent leg uses its own endpoint, model and key.
assert_request 0 'r["path"] == "/openai/v1/responses"'
assert_request 0 'r["body"]["model"] == "parent-model"'
assert_request 0 'r["auth"] == "Bearer parent-key"'
# The persona leg uses the other provider.
assert_request 1 'r["path"] == "/other/v1/responses"'
assert_request 1 'r["body"]["model"] == "otherprov/child-model"'
assert_request 1 'r["auth"] == "Bearer child-key"'
# The revived agent keeps the persona provider.
assert_request 3 'r["path"] == "/other/v1/responses"'
assert_request 3 'r["body"]["model"] == "child-model"'
assert_request 3 'r["auth"] == "Bearer child-key"'

mock_stop 5
pass
