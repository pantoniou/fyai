#!/bin/bash
# SPDX-License-Identifier: MIT
# A resumed child rechecks endpoint capabilities before constructing its request.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_revive_chat_no_search.json

cat > catalog.yaml <<EOF
models:
- name: no-search-model
  capabilities: []
providers:
- name: nosearch
  root_url: $MOCK_URL
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: no-search-model
    provider_model_id: no-search-model
EOF
run_fyai catalog import catalog.yaml
assert_status 0

run_fyai --set display/stream=false --set web_search=true \
	-m nosearch/no-search-model "ask the same sub-agent twice"
assert_status 0
assert_stdout_contains "Delegated twice."
[ "$(grep -c 'native web search is not supported' "$TEST_DIR/stderr" || true)" -eq 1 ] ||
	fail "endpoint notice was not emitted once by the user session"
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "unsupported search reached a child request"
import json
import sys

reqs = [json.loads(line) for line in open(sys.argv[1])]
if len(reqs) != 5:
    raise SystemExit("expected five requests, got %d" % len(reqs))
for req in reqs:
    if "web_search_options" in req["body"]:
        raise SystemExit("unsupported web_search_options in request")
PY
mock_stop 5
pass
