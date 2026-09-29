#!/bin/bash
# SPDX-License-Identifier: MIT
# A verb returns its result for the caller to present: --output json writes
# one JSON document of it, and --output yaml one YAML document.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# check_json EXPR VERB...: the output parses as JSON and EXPR holds for it.
check_json() {
	local expr="$1"
	shift
	run_fyai "$@" --output json
	assert_status 0
	"$PYTHON" - "$TEST_DIR/stdout" "$expr" <<'EOF' ||
import json, sys

doc = json.load(open(sys.argv[1]))
if not eval(sys.argv[2], {"d": doc}):
    raise SystemExit("unexpected: %r" % (doc,))
EOF
		fail "'$*' --output json: $(cat "$TEST_DIR/stdout")"
}

check_json 'any(b["branch"] == "main" for b in d)' branch
check_json 'isinstance(d, list) and all("active" in p for p in d)' \
	list providers
check_json '"total" in d' stats
check_json '"output_max" in d' context
check_json '"status" in d' auth
check_json 'd["root"] and d["head"] == "main"' root show
check_json 'set(d) == {"wire", "stream", "conversation", "mcp", "transport"}' log
check_json 'd == {"valid": True}' config validate
check_json '"model" in d and "api" in d' api
check_json 'isinstance(d, list) and d and "name" in d[0]' catalog list
check_json '"models" in d and "providers" in d' catalog show

run_fyai branch show main --output yaml
assert_status 0
assert_stdout_contains "branch: main"

pass
