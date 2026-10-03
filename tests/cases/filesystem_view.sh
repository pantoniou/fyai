#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
mkdir project
printf 'baseline\n' > project/file
printf '#!/bin/sh\nprintf executable\n' > project/executable
chmod 755 project/executable
printf readonly > project/readonly
chmod 444 project/readonly
mkdir project/.fyai
printf 'reserved\n' > project/.fyai/private
ln -s file project/link

run_fyai view create demo "$TEST_DIR/project" --verify --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PY'
import json, sys
v = json.load(open(sys.argv[1]))
assert v['baseline'] == v['root']
assert v['state'] == 'ready'
assert v['materialization'] in ('copy', 'metacopy')
PY
[ -d project/.fyai/objects/blake3 ] || fail 'missing project CAS store'
[ -n "$(find project/.fyai/objects/blake3 -type f -print -quit)" ] || fail 'missing project CAS blob'
[ -n "$(find project/.fyai/views -mindepth 2 -maxdepth 2 -name upper -type d -print -quit)" ] || fail 'missing project overlay upper'
[ ! -e "$HOME/.fyai/objects" ] || fail 'CAS store created under HOME'
[ ! -e "$HOME/.fyai/views" ] || fail 'overlay created under HOME'

run_fyai view create demo "$TEST_DIR/project"
assert_status 1

run_fyai view enter --verify demo true
assert_status 0
run_fyai view show demo --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PYVERIFY'
import json, sys
v = json.load(open(sys.argv[1]))
assert v['baseline'] == v['root']
PYVERIFY

sha256sum project/.fyai/objects/blake3/* > cas-before.sha256
run_fyai view enter --verify demo sh -c './executable && test "$(cat readonly)" = readonly && ! sh -c "printf denied > readonly" && chmod u+w readonly && printf changed > readonly'
assert_status 0
assert_stdout_contains executable
sha256sum -c cas-before.sha256 >/dev/null || fail 'copy-up changed a CAS blob'
[ "$(cat project/readonly)" = readonly ] || fail 'copy-up changed the host read-only file'

printf 'host changed\n' > project/file
run_fyai view enter demo sh -c 'test "$(cat file)" = baseline && test "$(cat link)" = baseline && test ! -e .fyai/private && ! touch .fyai/new && printf scratch > /tmp/scratch && test "$(cat /tmp/scratch)" = scratch && printf agent > file && mkdir child && printf nested > child/new'
assert_status 0
[ "$(cat project/file)" = 'host changed' ] || fail 'view modified host'
[ ! -e project/child ] || fail 'view created host directory'
[ "$(cat project/.fyai/private)" = reserved ] || fail 'reserved host file changed'

run_fyai view enter --verify demo sh -c 'test "$(cat file)" = agent && test "$(cat child/new)" = nested && rm link && mv child renamed'
assert_status 0
run_fyai view show demo --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PY'
import json, sys
v = json.load(open(sys.argv[1]))
assert v['baseline'] != v['root']
assert v['state'] == 'ready'
assert v['materialization'] in ('copy', 'metacopy')
PY
run_fyai view list --output json
assert_status 0
assert_stdout_contains 'demo'

run_fyai view enter --verify demo sh -c 'mkdir replacement && printf old > replacement/old'
assert_status 0
run_fyai view enter --verify demo sh -c 'rm -rf replacement && mkdir replacement && printf new > replacement/new && chmod 700 renamed && ln -s renamed/new new-link'
assert_status 0
run_fyai view enter --verify demo sh -c 'test ! -e replacement/old && test "$(cat replacement/new)" = new && test -L new-link'
assert_status 0

run_fyai view enter demo printf '%s\n' 'two words' '--help' '--output=json'
assert_status 0
printf 'two words\n--help\n--output=json\n' > expected
cmp expected "$TEST_DIR/stdout" || fail 'command argument or output changed'
run_fyai view enter demo sh -c 'exit 7'
assert_status 7
printf 'stream input\n' | "$FYAI_BIN" view enter demo cat > stream-output
printf 'stream input\n' > expected
cmp expected stream-output || fail 'stdin or stdout changed'
run_fyai view enter demo id -u
assert_status 0
[ "$(cat "$TEST_DIR/stdout")" = "$(id -u)" ] || fail 'view UID changed'

shell_prompt='$ '
[ "$(id -u)" != 0 ] || shell_prompt='# '
SHELL=/bin/sh FYAI_TERM_WAIT="$shell_prompt" \
FYAI_TERM_SEND='printf shell > interactive; printf "shell-ok\\n"; exit\n' \
FYAI_TERM_WAIT2='shell-ok' \
"$PYTHON" "$TESTS_DIR/term_driver.py" "$TEST_DIR/shell.out" \
    "$FYAI_BIN" view enter demo || fail 'interactive shell failed'
run_fyai view enter --verify demo cat interactive
assert_status 0
[ "$(cat "$TEST_DIR/stdout")" = shell ] || fail 'interactive write missing'
[ ! -e project/interactive ] || fail 'interactive shell wrote host file'

"$PYTHON" - "$FYAI_BIN" <<'PTY'
import os, select, time, sys
pid, fd = os.forkpty()
if pid == 0:
    os.execv(sys.argv[1], [sys.argv[1], 'view', 'enter', 'demo', 'sh', '-c',
                         'printf interrupt-ready; sleep 60'])
deadline = time.monotonic() + 15
output = b''
status = None
try:
    while b'interrupt-ready' not in output and time.monotonic() < deadline:
        if select.select([fd], [], [], 0.1)[0]:
            output += os.read(fd, 65536)
    assert b'interrupt-ready' in output, output
    time.sleep(0.1)
    os.write(fd, b'\x03')
    while time.monotonic() < deadline:
        done, current = os.waitpid(pid, os.WNOHANG)
        if done:
            status = current
            break
        time.sleep(0.02)
    else:
        raise AssertionError('command ignored Ctrl-C')
    assert os.waitstatus_to_exitcode(status) == 130, status
finally:
    if status is None or not os.WIFEXITED(status):
        try:
            os.kill(pid, 9)
            os.waitpid(pid, 0)
        except ProcessLookupError:
            pass
    os.close(fd)
PTY

"$PYTHON" - "$FYAI_BIN" <<'CANCEL'
import subprocess, signal, sys
p = subprocess.Popen([sys.argv[1], 'view', 'enter', 'demo', 'sh', '-c',
                      'printf "ready\\n"; sleep 60'], stdin=subprocess.DEVNULL,
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
assert p.stdout.readline() == b'ready\n'
p.send_signal(signal.SIGTERM)
output, error = p.communicate(timeout=10)
assert p.returncode == 143, (p.returncode, output, error)
CANCEL

run_fyai view update demo --verify --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PY'
import json, sys
v = json.load(open(sys.argv[1]))
assert v['baseline'] == v['root']
assert v['state'] == 'ready'
assert v['materialization'] in ('copy', 'metacopy')
PY
run_fyai view enter demo sh -c 'test "$(cat file)" = "host changed" && test ! -e renamed && test ! -e interactive && test -L link'
assert_status 0

"$FYAI_BIN" view enter demo sh -c 'printf holding; sleep 2' > holding &
writer=$!
"$PYTHON" - holding <<'PY'
import pathlib, sys, time
p = pathlib.Path(sys.argv[1])
for _ in range(200):
    if p.read_bytes() == b'holding':
        break
    time.sleep(0.02)
else:
    raise SystemExit('writer did not start')
PY
run_fyai view update demo
assert_status 1
wait "$writer"

run_fyai view create comparison "$TEST_DIR/project"
assert_status 0
run_fyai view enter comparison sh -c 'printf comparison > file'
assert_status 0
unshare -Urnm sh -eu -c '
    bin=$1
    root=$2
    "$bin" view mount demo "$root/inspect-a"
    "$bin" view mount comparison "$root/inspect-b"
    test "$(cat "$root/inspect-a/file")" = "host changed"
    test "$(cat "$root/inspect-b/file")" = comparison
    if touch "$root/inspect-a/new" 2>/dev/null; then exit 1; fi
    test ! -e "$root/inspect-a/.fyai/private"
    if "$bin" view update demo; then exit 1; fi
    if "$bin" view enter demo true; then exit 1; fi
    if unshare -m "$bin" view unmount demo; then exit 1; fi
    "$bin" view unmount comparison
    "$bin" view unmount demo
' sh "$FYAI_BIN" "$TEST_DIR"

mkfifo project/fifo
run_fyai view update demo
assert_status 1
assert_stderr_contains 'Operation not supported'
run_fyai view enter demo cat file
assert_status 0
assert_stdout_contains 'host changed'
run_fyai view create unsupported "$TEST_DIR/project"
assert_status 1
assert_stderr_contains 'Operation not supported'
mkdir -p git-project/.git/objects
printf 'git object' > git-project/.git/objects/object
chmod 444 git-project/.git/objects/object
run_fyai view create git-borrowed "$TEST_DIR/git-project" --verify
assert_status 0
run_fyai view create git-copied "$TEST_DIR/git-project" --copy-git-objects --verify
assert_status 0
run_fyai view enter --verify git-borrowed sh -c 'test "$(cat .git/objects/object)" = "git object"'
assert_status 0
chmod 644 git-project/.git/objects/object
printf 'bad object' > git-project/.git/objects/object
chmod 444 git-project/.git/objects/object
run_fyai view enter git-borrowed true
assert_status 1
assert_stderr_contains '--copy-git-objects'
run_fyai view update git-borrowed --copy-git-objects --verify
assert_status 0
run_fyai view enter --verify git-borrowed sh -c 'test "$(cat .git/objects/object)" = "bad object"'
assert_status 0
run_fyai view enter --verify git-copied sh -c 'test "$(cat .git/objects/object)" = "git object"'
assert_status 0
pass
