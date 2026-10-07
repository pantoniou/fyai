#!/bin/bash
# SPDX-License-Identifier: MIT
# The cost of a session includes what its sub-agents used. An agent forked
# from the conversation shares its first turns, which are counted once.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start cost_agent.json

cat > catalog.yaml <<CAT
models:
- name: cheap-model
  capabilities: []
providers:
- name: priced
  root_url: $MOCK_URL
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: cheap-model
    provider_model_id: cheap-model
    pricing: {input: 1000, output: 4000}
CAT
run_fyai catalog import catalog.yaml
assert_status 0

# The parent makes two calls (100/10 and 200/20 tokens), the agent one (50/5).
run_fyai --set display/stream=false --set tools=true -m cheap-model \
	 "delegate a greeting to a sub-agent"
assert_status 0
assert_stdout_contains "Delegated and done."
mock_stop 3

run_fyai stats --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PY' || fail "the agent was not counted in the cost"
import json, sys

stats = json.load(open(sys.argv[1]))
parent = (100 * 1000 + 10 * 4000 + 200 * 1000 + 20 * 4000) / 1e6
agent = (50 * 1000 + 5 * 4000) / 1e6
agents = stats.get("agents")
if not agents:
    sys.exit("no agents in %r" % stats)
if abs(stats["cost"] - parent) > 1e-9:
    sys.exit("conversation cost %r, want %r" % (stats["cost"], parent))
if agents["count"] != 1 or agents["calls"] != 1 or abs(agents["cost"] - agent) > 1e-9:
    sys.exit("agents %r, want one call costing %r" % (agents, agent))
if abs(stats["cost_all"] - parent - agent) > 1e-9:
    sys.exit("total %r, want %r" % (stats["cost_all"], parent + agent))
PY

# A session on the stored conversation shows the total with the agent.
run_fyai config set model cheap-model
assert_status 0
FYAI_PTY_INPUT="/model" FYAI_PTY_NEEDLE="model: cheap-model" \
FYAI_PTY_TIMEOUT=30 FYAI_PTY_AFTER="wait-screen:~\$0.4900" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark --set display/markdown=true \
    --set display/screen=fullscreen -b main -i ||
    fail "the session did not show the cost with the sub-agent"

pass
