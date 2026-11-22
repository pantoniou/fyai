#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify `fleet create` (CLI and fleet tool `create` action): creates nested
# projects and arenas, preserves existing projects, rejects escaping paths,
# inherits config without fleet/root, and starts a member task via the tool.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_create.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set tools=true
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
mkdir -p a/.git
(cd a && "$FYAI_BIN" init >/dev/null 2>&1 &&
	"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1) || fail "cannot make the project"

# CLI: nested path creates intermediate directories and initializes the arena.
"$FYAI_BIN" fleet create apps/web --output json >"$TEST_DIR/create.json" 2>&1 ||
	fail "fleet create failed: $(cat "$TEST_DIR/create.json")"
grep -q '"created": true' "$TEST_DIR/create.json" ||
	fail "the project is not new: $(cat "$TEST_DIR/create.json")"
[ -d apps/web/.fyai/arena ] || fail "the project has no arena"

# Existing projects are left intact and reported as kept.
"$FYAI_BIN" fleet create apps/web --output json >"$TEST_DIR/again.json" 2>&1 ||
	fail "fleet create of a project failed: $(cat "$TEST_DIR/again.json")"
grep -q '"kept": true' "$TEST_DIR/again.json" ||
	fail "the project was not kept: $(cat "$TEST_DIR/again.json")"

# Inherit root config (excluding fleet/root) unless --default-config is given;
# `init --root` sets fleet/root.
"$FYAI_BIN" config set title/max_chars 42 >/dev/null 2>&1 || fail "cannot set a key"
"$FYAI_BIN" fleet create apps/inherit >/dev/null 2>&1 || fail "fleet create failed"
"$FYAI_BIN" fleet create apps/sample --default-config >/dev/null 2>&1 ||
	fail "fleet create --default-config failed"
[ "$(cd apps/inherit && "$FYAI_BIN" config get title/max_chars)" = 42 ] ||
	fail "the project did not take the configuration of the root"
[ "$(cd apps/sample && "$FYAI_BIN" config get title/max_chars)" = 60 ] ||
	fail "--default-config did not store the sample configuration"
[ "$(cd apps/inherit && "$FYAI_BIN" config get fleet/root 2>&1)" != true ] ||
	fail "the project took fleet/root"
mkdir other && (cd other && "$FYAI_BIN" init --root >/dev/null 2>&1) ||
	fail "init --root failed"
[ "$(cd other && "$FYAI_BIN" config get fleet/root)" = true ] ||
	fail "init --root did not set fleet/root"

# Reject non-normalized, absolute, .fyai, and symlink-escaping paths.
outside="$(mktemp -d "${FYAI_TMPDIR_BASE:-/tmp}/fyai-outside.XXXXXX")"
fyai_test_on_exit "rm -rf '$outside'"
ln -s "$outside" link
for bad in ../x /abs apps//x apps/./x apps/.fyai/x link/x; do
	if "$FYAI_BIN" fleet create "$bad" >"$TEST_DIR/bad.out" 2>&1; then
		fail "fleet create took '$bad'"
	fi
done
[ -z "$(ls -A "$outside")" ] || fail "a project was made outside the root"

"$FYAI_BIN" fleet list --output json >"$TEST_DIR/list.json"
"$PYTHON" - "$TEST_DIR/list.json" <<'PY' || fail "the fleet does not list the project: $(cat "$TEST_DIR/list.json")"
import json, sys
projects = {r["project"] for r in json.load(open(sys.argv[1]))}
assert {"a", "apps/web"} <= projects, projects
PY

# Fleet tool `create` action (with approval off): creates project & runs task.
mock_watch
"$FYAI_BIN" config set fleet/approve never >/dev/null 2>&1 ||
	fail "cannot set fleet/approve"
"$FYAI_BIN" config set fleet/root true >/dev/null 2>&1 ||
	fail "cannot make the project a fleet root"
"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'

"$FYAI_BIN" -b main "Make a project" 8<&- >"$TEST_DIR/root.out" 2>&1 ||
	fail "the root run failed: $(cat "$TEST_DIR/root.out")"
grep -q "Made it." "$TEST_DIR/root.out" ||
	fail "the root run did not end: $(cat "$TEST_DIR/root.out")"
[ -d tools/docs/.fyai/arena ] || fail "the model made no project"
wait_requests 3
assert_request 1 'json.loads(r["body"]["messages"][-1]["content"])["created"] is True'
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the new project did not run its task"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
member = [r for r in reqs if any(m.get("role") == "user" and m.get("content") == "Write the readme"
                                 for m in r["body"]["messages"])]
assert len(member) == 1, len(member)
PY
"$FYAI_BIN" fleet tasks --output json >"$TEST_DIR/tasks.json"
"$PYTHON" - "$TEST_DIR/tasks.json" <<'PY' || fail "the root keeps no task: $(cat "$TEST_DIR/tasks.json")"
import json, sys
rows = json.load(open(sys.argv[1]))
assert [(r['session'], r['name']) for r in rows] == \
    [('main', 'tools/docs:fleet/write-the-readme')], rows
PY

# Region base is at offset 16 in the arena boot header (after magic/version/
# endianness). CLI init uses the default base; an active session whose arena
# occupies that base allocates an adjacent non-overlapping base.
"$PYTHON" - . <<'PY' || fail "the bases of the arenas are not as expected"
import struct, sys
def base(project):
    with open(f"{sys.argv[1]}/{project}/.fyai/arena/arena-0.bin", "rb") as f:
        return struct.unpack("<Q", f.read(24)[16:24])[0]
root, cli, tool = base("."), base("apps/web"), base("tools/docs")
assert cli == root, (hex(cli), hex(root))
assert tool != root, (hex(tool), hex(root))
PY

mock_stop 3
pass
