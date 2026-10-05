#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"
FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77

"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" <<'PY'
import json, os, pathlib, pty, re, select, subprocess, sys, time
from term_reply import answer_da1
binary, scratch = sys.argv[1:]
root = pathlib.Path(scratch)
project = root / 'diff-project'
project.mkdir()
(project / 'text').write_text('before\n')
(project / 'binary').write_bytes(bytes(range(256)))
(project / 'deleted').write_text('remove me\n')
(project / 'metadata').write_text('unchanged\n')
(project / 'kind').write_text('regular\n')
(project / 'exec').write_text('#!/bin/sh\n')
(project / 'link').symlink_to('text')
(project / '.git').mkdir()
(project / '.git/private').write_text('git metadata\n')

def run(*args, cwd=root, ok=True):
    p = subprocess.run(args, cwd=cwd, capture_output=True)
    if ok:
        assert p.returncode == 0, (args, p.stdout, p.stderr)
    return p

run('git', 'init', str(project))
run(binary, 'view', 'create', 'before', str(project))
run(binary, 'view', 'create', 'after', str(project))
assert not run(binary, 'view', 'diff', 'after').stdout
run(binary, 'view', 'enter', 'after', 'sh', '-c',
    "printf 'after\\n' > text; printf '\\000binary changed\\377' > binary; "
    "rm deleted link kind; ln -s binary link; mkdir kind; printf child > kind/child; "
    "printf addition > added; printf 'one-tab\\ttwo-tabs\\tthree-tabs\\n' > tabs; chmod 755 exec; touch -m -d @123 metadata; "
    "printf changed > .git/private")
run(binary, 'view', 'enter', '--verify', 'after', 'true')
result = json.loads(run(binary, 'view', 'diff', '--stat', 'after', '--output', 'json').stdout)
changes = {c['path']: c for c in result['changes']}
assert changes['text']['status'] == 'modified'
assert changes['binary']['before']['digest'] != changes['binary']['after']['digest']
assert changes['deleted']['status'] == 'deleted' and changes['deleted']['after'] is None
assert changes['added']['status'] == 'added' and changes['added']['before'] is None
assert changes['metadata']['before']['digest'] == changes['metadata']['after']['digest']
assert changes['metadata']['after']['mtime_sec'] == 123
assert changes['kind']['before']['kind'] == 'file' and changes['kind']['after']['kind'] == 'directory'
assert changes['exec']['after']['mode'] & 0o111
assert changes['link']['before']['target_hex'] != changes['link']['after']['target_hex']
assert not any(p == '.git' or p.startswith('.git/') for p in changes)
os.environ['GIT_CONFIG_COUNT'] = '1'
os.environ['GIT_CONFIG_KEY_0'] = 'color.ui'
os.environ['GIT_CONFIG_VALUE_0'] = 'always'
patch = run(binary, 'view', 'diff', 'after').stdout
assert b'\x1b[' not in patch
assert json.loads(run(binary, 'view', 'diff', 'after', '--output', 'json').stdout)['patch'].encode().rstrip(b'\n') == patch.rstrip(b'\n')
assert b'-before' in patch and b'+after' in patch
assert b'Binary files a/binary and b/binary differ' in patch
assert b'old mode 100644' in patch and b'new mode 100755' in patch
assert b'# metadata ' in patch
assert all(line.startswith(b'# metadata {') and line.endswith(b'}')
           for line in patch.splitlines() if line.startswith(b'# metadata '))
assert b'\n  path:' not in patch
assert b'.git/private' not in patch
assert patch == run(binary, 'view', 'diff', 'before', 'after').stdout
assert patch == run(binary, 'view', 'diff', '-u', 'after').stdout
for unified in (False, True):
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(root)
        os.environ['TERM'] = 'xterm-256color'
        os.environ['COLORTERM'] = 'truecolor'
        args = [binary, '--color', 'on', '--theme', 'ember:dark',
                '--set', 'display/markdown=true', 'view', 'diff', 'after']
        os.execv(binary, args + (['-u'] if unified else []))
    data = b''
    deadline = time.monotonic() + 20 * float(os.environ.get('FYAI_TIMEOUT_SCALE', '1'))
    while time.monotonic() < deadline:
        if not select.select([fd], [], [], 0.2)[0]:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        answer_da1(fd, chunk)
        if b'\x1b]11;?' in chunk:
            os.write(fd, b'\x1b]11;rgb:1010/1010/1010\x1b\\')
        data += chunk
    else:
        os.kill(pid, 9)
        raise AssertionError('view diff terminal timed out')
    os.close(fd)
    assert os.waitpid(pid, 0)[1] == 0, data[-500:]
    rows = [line for line in data.splitlines()
            if b'+after' in re.sub(rb'\x1b\[[0-9;]*m', b'', line)]
    assert len(rows) == 1 and b'\x1b[' in rows[0], data[-500:]
    plain = re.sub(rb'\x1b\[[0-9;]*m', b'', data)
    # The view expands a tab, as it cannot size a row that holds one; the unified rows keep the patch.
    if unified:
        assert b'+one-tab\ttwo-tabs\tthree-tabs' in plain, data[-500:]
    else:
        assert b'\t' not in data, data[-500:]
        assert b'+one-tab two-tabs        three-tabs' in plain, data[-500:]
    assert (b'[48;2;' in rows[0]) != unified, rows[0]

stat = run(binary, 'view', 'diff', '--stat', 'after').stdout
assert b'modified text' in stat and b'deleted deleted' in stat and b'added added' in stat
assert b'Binary files' not in stat
patch_file = root / 'changes.patch'
# Git cannot apply a binary change that carries no data: leave that file out.
text_patch = re.sub(rb'diff --git a/binary b/binary\n(?:(?!diff --git).*\n)*', b'', patch)
patch_file.write_bytes(text_patch)
run('git', 'apply', str(patch_file), cwd=project)
assert (project / 'text').read_bytes() == b'after\n'
assert (project / 'binary').read_bytes() != b'\x00binary changed\xff'
assert (project / 'kind/child').read_bytes() == b'child'
assert not (project / 'deleted').exists()
assert (project / 'link').readlink() == pathlib.Path('binary')
assert (project / '.git/private').read_text() == 'git metadata\n'
assert not list((project / '.fyai/views').glob('diff-*'))
assert run(binary, 'view', 'diff', 'missing', ok=False).returncode != 0
PY
pass
