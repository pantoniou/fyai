#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that with `durable`, killing a process mid-compaction leaves the
# branch unchanged and does not commit the summarization prompt as a turn.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start durable_compact.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set durable=true \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
"$FYAI_BIN" -b main --color off "remember the magic word: xyzzy" \
    >out.txt 2>err.txt || fail "the seed turn failed: $(cat err.txt)"
grep -qF "Seeded reply." out.txt || fail "no seed reply"

mock_watch
"$FYAI_BIN" -b main --color off compact >c.out 2>c.err 8<&- &
pid=$!
wait_requests 1
kill -9 "$pid"
wait "$pid" 2>/dev/null || true
"$FYAI_BIN" -b main export -o saved.md >/dev/null 2>&1 || fail "no export"
grep -qF "xyzzy" saved.md || fail "the conversation was lost"
grep -qF "Seeded reply." saved.md || fail "the reply was lost"
! grep -qF "Summarize this conversation" saved.md ||
	fail "the prompt of the summary is a turn of the branch"
touch release-compact
mock_stop
pass
