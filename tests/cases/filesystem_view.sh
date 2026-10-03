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

run_fyai view create demo "$TEST_DIR/project" --debug --verify --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PY'
import json, sys
v = json.load(open(sys.argv[1]))
assert v['baseline'] == v['root']
assert v['state'] == 'ready'
assert v['durability'] == 'lazy' and not v['synchronized']
assert v['materialization'] in ('copy', 'metacopy')
c = v['capture']
assert c['files'] == 3 and c['directories'] == 1 and c['symlinks'] == 1
assert c['logical_bytes'] == 9 + len('#!/bin/sh\nprintf executable\n') + 8
assert sum(x['files'] for x in c['storage'].values()) == c['files']
assert abs(sum(x['percent'] for x in c['storage'].values()) - 100) < 0.001
assert c['workers'] > 0 and c['elapsed_ms'] >= 0

PY
assert_stderr_contains 'view demo: scanning'
assert_stderr_contains 'hardlink'
[ -d project/.fyai/objects/blake3 ] || fail 'missing project CAS store'
[ -n "$(find project/.fyai/objects/blake3 -type f -print -quit)" ] || fail 'missing project CAS blob'
[ -n "$(find project/.fyai/views -mindepth 2 -maxdepth 2 -name upper -type d -print -quit)" ] || fail 'missing project overlay upper'
[ ! -e "$HOME/.fyai/objects" ] || fail 'CAS store created under HOME'
[ ! -e "$HOME/.fyai/views" ] || fail 'overlay created under HOME'

run_fyai view create demo "$TEST_DIR/project"
assert_status 1

run_fyai view sync demo --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PYSYNC'
import json, sys
v = json.load(open(sys.argv[1]))
assert v['durability'] == 'lazy' and v['synchronized']
PYSYNC
run_fyai view create durable "$TEST_DIR/project" --durability durable --quiet --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PYDURABLE'
import json, sys
v = json.load(open(sys.argv[1]))
assert v['durability'] == 'durable' and v['synchronized']
PYDURABLE
run_fyai view update durable --quiet --output json
assert_status 0
assert_stdout_contains '"durability": "durable"'
run_fyai view update durable --durability lazy --quiet --output json
assert_status 0
assert_stdout_contains '"durability": "lazy"'

"$PYTHON" - "$FYAI_BIN" "$TEST_DIR/project" <<'PYCONCURRENT'
import json, subprocess, sys
processes = [subprocess.Popen([sys.argv[1], 'view', 'create', name, sys.argv[2],
                              '--quiet', '--output', 'json'],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
             for name in ('concurrent-one', 'concurrent-two')]
roots = []
for p in processes:
    out, err = p.communicate(timeout=30)
    assert p.returncode == 0, (out, err)
    roots.append(json.loads(out)['root'])
assert roots[0] == roots[1]
for name in ('concurrent-one', 'concurrent-two'):
    p = subprocess.run([sys.argv[1], 'view', 'enter', '--verify', name, 'true'],
                       capture_output=True, timeout=30)
    assert p.returncode == 0, (p.stdout, p.stderr)
PYCONCURRENT

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
run_fyai view sync demo
assert_status 1
wait "$writer"

run_fyai view create comparison "$TEST_DIR/project" --quiet --output json
assert_status 0
[ ! -s "$TEST_DIR/stderr" ] || fail 'quiet create emitted progress'
assert_stdout_contains 'capture'
run_fyai view update comparison --quiet --output json
assert_status 0
[ ! -s "$TEST_DIR/stderr" ] || fail 'quiet update emitted progress'
assert_stdout_contains 'capture'
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
assert_status 0
run_fyai view enter --verify git-borrowed true
assert_status 1
assert_stderr_contains '--copy-git-objects'
run_fyai view update git-borrowed --copy-git-objects --verify
assert_status 0
run_fyai view enter --verify git-borrowed sh -c 'test "$(cat .git/objects/object)" = "bad object"'
assert_status 0
run_fyai view enter --verify git-copied sh -c 'test "$(cat .git/objects/object)" = "git object"'
assert_status 0

# Git worktree metadata is projected into an independent in-view repository.
"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" <<'PYTEST'
import json, os, pathlib, subprocess, sys
binary, scratch = sys.argv[1:]
root = pathlib.Path(scratch)
repo, work = root / 'git-main', root / 'git-work'
env = os.environ.copy()
env['GIT_CONFIG_NOSYSTEM'] = '1'
env['XDG_CONFIG_HOME'] = str(root / 'home/.config')
def run(args, cwd=root, ok=True):
    proc = subprocess.run(args, cwd=cwd, env=env, text=True, capture_output=True)
    if ok:
        assert proc.returncode == 0, (args, proc.stdout, proc.stderr)
    return proc
run(['git', 'init', str(repo)])
(repo / 'file').write_text('original\n')
(repo / 'bulk').mkdir()
for i in range(256):
    (repo / 'bulk' / f'file-{i}').write_text(f'unique content {i}\n')
(repo / 'bulk' / 'alias').write_text('unique content 7\n')
run(['git', 'add', '.'], repo)
run(['git', '-c', 'user.name=Test', '-c', 'user.email=test@example.test',
     '-c', 'core.hooksPath=/dev/null', 'commit', '-m', 'initial'], repo)
main_branch = run(['git', 'branch', '--show-current'], repo).stdout.strip()
run(['git', 'worktree', 'add', '-b', 'projected', str(work)], repo)
# Common configuration must not redirect the view to the host working tree.
run(['git', 'config', '--file', str(repo / '.git/config'), 'core.worktree', str(repo)])
run(['git', 'config', '--file', str(repo / '.git/config'), 'extensions.worktreeConfig', 'true'])
run(['git', 'config', '--worktree', 'core.worktree', str(work)], work)
(work / 'file').write_text('dirty worktree\n')
head = run(['git', 'rev-parse', 'HEAD'], work).stdout.strip()
status = run(['git', 'status', '--porcelain'], work).stdout.strip()
private = pathlib.Path((work / '.git').read_text().strip().removeprefix('gitdir: '))
(private / 'refs/worktree').mkdir(parents=True)
(private / 'refs/worktree/local').write_text(head + '\n')
(private / 'private-link').symlink_to('HEAD')
originals = {p: p.read_bytes() for p in [repo / '.git/config', private / 'config.worktree',
                                      private / 'HEAD', private / 'index', work / '.git']}
result = json.loads(run([binary, 'view', 'create', 'git-worktree', str(work),
                        '--verify', '--output', 'json']).stdout)
assert result['capture']['borrowed_files'] > 0
assert result['capture']['storage']['hardlink']['files'] > 0
script = ('test -d .git && test ! -e .git/gitdir && test ! -e .git/commondir && '
          'test ! -e .git/worktrees && test "$(git config core.worktree)" = .. && '
          'test -L .git/private-link && test "$(git rev-parse refs/worktree/local)" = ' + head +
          ' && git status --porcelain && git rev-parse HEAD && git reflog show ' + main_branch)
inside = run([binary, 'view', 'enter', '--verify', 'git-worktree', 'sh', '-c', script]).stdout
assert status in inside and head in inside and 'initial' in inside
run([binary, 'view', 'enter', '--verify', 'git-worktree', 'sh', '-c',
     'rm bulk/file-0; mv bulk/file-1 bulk/renamed; printf changed > bulk/file-7'])
assert (work / 'bulk/file-0').exists() and (work / 'bulk/file-1').exists()
assert (work / 'bulk/file-7').read_text() == 'unique content 7\n'
run([binary, 'view', 'enter', '--verify', 'git-worktree', 'sh', '-c',
     'git -c user.name=Test -c user.email=test@example.test -c core.hooksPath=/dev/null add file && '
     'git -c user.name=Test -c user.email=test@example.test -c core.hooksPath=/dev/null commit -m isolated'])
assert run(['git', 'rev-parse', 'HEAD'], work).stdout.strip() == head
assert all(p.read_bytes() == data for p, data in originals.items())
run([binary, 'view', 'update', 'git-worktree', '--verify'])
assert head in run([binary, 'view', 'enter', '--verify', 'git-worktree', 'git', 'rev-parse', 'HEAD']).stdout
run([binary, 'view', 'create', 'git-worktree-owned', str(work), '--copy-git-objects', '--verify'])
# A bare common repository still needs an in-view working tree.
bare, bare_work = root / 'git-bare', root / 'git-bare-work'
run(['git', 'clone', '--bare', str(repo), str(bare)])
run(['git', 'worktree', 'add', '-b', 'bare-projected', str(bare_work)], bare)
run([binary, 'view', 'create', 'git-bare-worktree', str(bare_work), '--verify'])
inside = run([binary, 'view', 'enter', '--verify', 'git-bare-worktree', 'git',
              'rev-parse', '--is-bare-repository']).stdout
assert inside.strip() == 'false', inside
# Unsupported external layouts must fail before publishing a view.
(private / 'outside-link').symlink_to('/outside/git')
failed = run([binary, 'view', 'create', 'git-external-link', str(work)], ok=False)
assert failed.returncode and 'Git metadata symlink leaves the project' in failed.stderr
(private / 'outside-link').unlink()
(repo / '.git/objects/info/alternates').write_text('/outside/objects\n')
failed = run([binary, 'view', 'create', 'git-alternates', str(work)], ok=False)
assert failed.returncode and 'external object alternates are unsupported' in failed.stderr
(repo / '.git/objects/info/alternates').unlink()
(work / 'nested').mkdir()
(work / 'nested/.git').write_text('gitdir: /outside/git\n')
failed = run([binary, 'view', 'create', 'git-nested', str(work)], ok=False)
assert failed.returncode and 'submodules are unsupported' in failed.stderr
PYTEST

# A source-directory change must restart capture with a private fresh baseline.
"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" <<'PY'
import json, os, pathlib, select, subprocess, sys, time
binary, scratch = sys.argv[1:]
project = pathlib.Path(scratch, 'capture-race')
project.mkdir()
for i in range(500):
    (project / ('file-%04d' % i)).write_bytes(i.to_bytes(8, 'little'))

def capture(name, continuous):
    proc = subprocess.Popen([binary, '--color', 'off', 'view', 'create', name,
                             str(project), '--debug', '--output', 'json'],
                            cwd=scratch,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    mutations = 0
    lines = []
    deadline = time.monotonic() + 30 * int(os.environ.get('FYAI_TIMEOUT_SCALE', '1'))
    try:
        while True:
            remaining = deadline - time.monotonic()
            assert remaining > 0, 'capture retry timed out'
            ready, _, _ = select.select([proc.stderr], [], [], remaining)
            assert ready, 'capture retry timed out'
            line = proc.stderr.readline()
            if not line:
                break
            lines.append(line)
            if b': capturing: 0/' in line and (continuous or not mutations):
                mutations += 1
                (project / ('%s-mutation-%d' % (name, mutations))).write_text('changed')
        out, _ = proc.communicate(timeout=max(1, deadline - time.monotonic()))
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    return proc.returncode, out, b''.join(lines), mutations

rc, out, err, mutations = capture('race-once', False)
assert rc == 0, err.decode()
assert mutations == 1
result = json.loads(out)
assert result['capture']['attempts'] == 2, result
assert result['capture']['files'] == 501, result
assert b'project changed during capture' in err
assert len(list((project / '.fyai/views').iterdir())) == 1

rc, out, err, mutations = capture('race-always', True)
assert rc != 0 and mutations == 3, (rc, err.decode(), mutations)
assert b'project changed during capture' in err and b'after 3 attempts' in err
assert len(list((project / '.fyai/views').iterdir())) == 1
PY

pass
