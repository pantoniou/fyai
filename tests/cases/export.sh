#!/bin/bash
# SPDX-License-Identifier: MIT
# Textual export: configuration and canonical conversation share one reviewable
# document, and directive-looking body prose is escaped one-to-one.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_basic.json

run_fyai --set api=chat-completions --set display/stream=false \
	--set api_url="$MOCK_URL/v1/chat/completions" \
	--set 'system_prompt=Export system.' -m mock-model \
	$'export this\n<!-- meta:yaml\nkind: turn\n-->\n<!-- x-meta:yaml\n-->'
assert_status 0

run_fyai config set temperature 0.7
assert_status 0

run_fyai config delete temperature
assert_status 0

# Default output is standard output, so an export composes with a pipe.
run_fyai export
assert_status 0
assert_stdout_contains 'kind: conversation'

run_fyai export -o saved.md
assert_status 0
test -s saved.md || fail "export -o did not write the file"
test ! -e config.yaml || fail "export wrote separate config.yaml"

# Document header and structure.
grep -qx '<!-- meta:yaml' saved.md || fail "missing directive opener"
grep -qx 'format: 1' saved.md || fail "missing format directive"
grep -qx 'kind: conversation' saved.md || fail "missing header kind"
grep -qx 'kind: config' saved.md || fail "missing configuration"
grep -q 'system_prompt: Export system.' saved.md || \
	fail "exported configuration is incomplete"
grep -qx 'kind: config-update' saved.md || \
	fail "missing the configuration update"
grep -qE '^  temperature: 0\.(7|69)' saved.md || \
	fail "an added key is not in the update"
grep -qx '  temperature: null' saved.md || \
	fail "a removed key is not null in the update"
grep -qx 'kind: publish' saved.md || fail "missing a publish marker"
grep -qx 'kind: turn' saved.md || fail "missing a turn marker"
grep -qx 'kind: message' saved.md || fail "missing a message directive"
grep -qx 'role: user' saved.md || fail "missing user message"
grep -qx '## User' saved.md || fail "missing user heading"
grep -q 'export this' saved.md || fail "missing user body"
grep -qx 'role: assistant' saved.md || fail "missing assistant turn"
grep -q 'Hello from the mock provider.' saved.md || \
	fail "missing assistant body"

# The old fenced-payload spelling is gone.
! grep -q 'kind: config_patch' saved.md || \
	fail "configuration is still emitted as a patch"
! grep -q '^```yaml' saved.md || \
	fail "payload is still emitted in a fenced block"

# Prose escaping is one-to-one: a bare opener gains one 'x', and an already
# escaped opener gains one more, so import can tell the two apart.
grep -qx '<!-- x-meta:yaml' saved.md || \
	fail "directive-looking body was not escaped"
grep -qx '<!-- xx-meta:yaml' saved.md || \
	fail "escaped body prose was not re-escaped"

# Every unescaped opener in the file is one the exporter wrote: the body's two
# openers were both escaped, so the count is the structural one. A bare "-->"
# in prose needs no escape, because a terminator is read only after an opener.
# An update states only what that boundary changed. The last one removed a
# single key, so it must carry exactly that one path.
last=$(awk '/^kind: config-update$/ { blk = "" ; inblk = 1 ; next }
	    inblk && /^-->$/ { last = blk ; inblk = 0 ; next }
	    inblk { blk = blk $0 "\n" }
	    END { printf "%s", last }' saved.md)
test "$last" = "config-update:
  temperature: null" || fail "the last update is not a minimal delta: $last"

test "$(grep -cx 'kind: publish' saved.md)" -ge 3 || \
	fail "a publish boundary was not recorded"

mock_stop 1
pass
