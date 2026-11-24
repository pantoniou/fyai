#!/bin/bash
# SPDX-License-Identifier: MIT
# After /compact the transcript shows the compaction: the card of the
# command and the summary, and no empty user card. The next exchange follows
# it. The session runs as a client of its worker, which sends the turns of
# the new chain.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start compact.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
	--set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
	--set 'api_key={type: env, value: MOCK_KEY}' \
	--set display/screen=fullscreen --set display/markdown=true \
	>/dev/null 2>&1 || fail "configure the project"

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -i <<'PTY'
steps:
  - type: "remember the magic word\r"
  - wait-screen: "Seeded reply."
    timeout: session
  - type: "/compact\r"
  - wait-screen: "/compact"
    timeout: session
  - wait-screen: "SUMMARY: seeded exchange, all good."
  - type: "go on\r"
  - wait-screen: "Post-compact reply."
    timeout: session
  - checkpoint: compacted.screen
  - type: "/quit\r"
PTY

"$PYTHON" - "$TEST_DIR/compacted.screen" <<'PYEOF' || fail "the compacted transcript"
import sys
rows = [l[2:] for l in open(sys.argv[1]) if l.startswith("R|")]
text = [r.strip() for r in rows]
card = next(i for i, r in enumerate(text) if r == "▌ /compact")
summary = next(i for i, r in enumerate(text) if "SUMMARY: seeded exchange" in r)
user = next(i for i, r in enumerate(text) if r == "▌ go on")
assert card < summary < user, (card, summary, user, rows)
# No user card without its words stands in the transcript.
assert not any(r == "▌" for r in text[:user]), rows
PYEOF
mock_stop 3
pass
