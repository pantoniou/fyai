#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Soak of a worker: N turns through one `fyai serve` worker, each answered by
# the mock with a reply of a given size. After each turn the script samples the
# resident set of the server and of the worker, and the time of the turn, and
# the time of a full and an incremental `fyai_sync`. A resident set that keeps
# growing after the first turns is a leak or an unbounded cache.
#
#   tests/bench/remote_soak.sh ./build/fyai [turns] [reply_kb]
set -eu
FYAI_BIN="${1:?usage: remote_soak.sh <fyai-binary> [turns] [reply_kb]}"
export FYAI_BIN
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/harness.sh"

TURNS="${2:-200}"
REPLY_KB="${3:-2}"

fyai_test_setup
"$PYTHON" - "$TEST_DIR/soak.json" "$TURNS" "$REPLY_KB" <<'PY'
import json, sys
path, n, kb = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
pad = ("lorem ipsum dolor sit amet " * 64)
pad = (pad * (kb * 1024 // len(pad) + 1))[:kb * 1024]
steps = []
for i in range(n):
    steps.append({"response": {
        "id": "chatcmpl-s%d" % i, "object": "chat.completion",
        "created": 1735689600 + i, "model": "mock-model",
        "choices": [{"index": 0, "message": {"role": "assistant",
                     "content": "answer %d: %s" % (i + 1, pad)},
                     "finish_reason": "stop"}],
        "usage": {"prompt_tokens": 10 * i, "completion_tokens": 50,
                  "total_tokens": 10 * i + 50}}})
json.dump({"steps": steps}, open(path, "w"))
PY
mock_start "$TEST_DIR/soak.json"
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
	--set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
	--set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1

MOCK_PID="$MOCK_PID" "$PYTHON" - "$FYAI_BIN" "$TEST_DIR" "$TURNS" "$FYAI_TIMEOUT_SCALE" <<'PY'
import json, os, select, subprocess, sys, time
binary, cwd, turns, scale = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
log = open(cwd + '/server.err', 'wb')
r, w = os.pipe()
server = subprocess.Popen(os.environ.get('SOAK_WRAP', '').split() + [binary, 'serve', '--token-fd', str(w)], cwd=cwd,
                          pass_fds=(w,), stdout=subprocess.DEVNULL, stderr=log)
os.close(w)

def rss(pid):
    # Anonymous and file-backed resident kilobytes. The arena is a file
    # mapping, so its growth is not a leak: the anonymous part is the heap.
    a = f = 0
    try:
        for l in open('/proc/%d/status' % pid):
            if l.startswith('RssAnon:'):
                a = int(l.split()[1])
            elif l.startswith('RssFile:'):
                f = int(l.split()[1])
    except OSError:
        pass
    return a, f

def cpu(pid):
    # Milliseconds of CPU time that the process has used.
    try:
        f = open('/proc/%d/stat' % pid).read().rsplit(')', 1)[1].split()
        return (int(f[11]) + int(f[12])) * 1000.0 / os.sysconf('SC_CLK_TCK')
    except (OSError, IndexError):
        return 0.0

def descendants(pid):
    out = []
    try:
        kids = open('/proc/%d/task/%d/children' % (pid, pid)).read().split()
    except OSError:
        return out
    for k in kids:
        out.append(int(k))
        out.extend(descendants(int(k)))
    return out

pending = bytearray()

def receive(stream):
    # Frames are lines; read what the pipe has in large blocks.
    fd = stream.fileno()
    while True:
        nl = pending.find(b'\n')
        if nl >= 0:
            line = bytes(pending[:nl])
            del pending[:nl + 1]
            return json.loads(line)
        assert select.select([fd], [], [], 60 * scale)[0], 'frame timeout'
        chunk = os.read(fd, 1 << 20)
        assert chunk, 'unexpected EOF'
        pending.extend(chunk)

client = None
def send(v):
    client.stdin.write((json.dumps(v) + '\n').encode()); client.stdin.flush()

def sync(name, have, depth=64):
    t0 = time.time()
    send({'type':'control_request','request_id':name,'request':{
        'subtype':'fyai_sync','have':have,'depth':depth}})
    while True:
        f = receive(client.stdout)
        if f['type'] == 'control_response' and f['response']['request_id'] == name:
            return f['response']['response'], (time.time() - t0) * 1000

try:
    assert select.select([r], [], [], 20 * scale)[0]
    token = os.read(r, 65); os.close(r)
    tr, tw = os.pipe(); os.write(tw, token); os.close(tw)
    client = subprocess.Popen([binary, '-b', 'remote/soak', 'remote', 'connect',
                               '--protocol', 'sdk', '--token-fd', str(tr)],
                              cwd=cwd, pass_fds=(tr,), stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    os.close(tr)
    send({'type':'control_request','request_id':'init','request':{
        'subtype':'initialize','fyai':{'model':1}}})
    while True:
        f = receive(client.stdout)
        if f['type'] == 'control_response': break
    probe = os.environ.get('SOAK_SYNC', '1') == '1'
    print('turn turn_ms | server anon file | worker anon file | cpu ms/turn: worker server mock | sync full inc ms')
    last = {}; lasti = 0; mock = int(os.environ.get('MOCK_PID', '0') or 0)
    step = max(1, turns // 20)
    step = max(step, 1)
    head = None
    for i in range(1, turns + 1):
        t0 = time.time(); frames = 0
        send({'type':'user','message':{'role':'user','content':'question %d' % i}})
        while True:
            f = receive(client.stdout); frames += 1
            if f['type'] == 'result':
                assert not f.get('is_error'), f
                break
        ms = (time.time() - t0) * 1000
        if i == 1 or i % step == 0 or i == turns:
            sf = si = 0.0
            if probe:
                full, sf = sync('f%d' % i, [], 64)
                inc, si = sync('i%d' % i, [head] if head else [], 64)
                head = full['head']
            kids = descendants(server.pid)
            wk = max([rss(k) for k in kids] or [(0, 0)])
            sa, sfl = rss(server.pid)
            dump = os.environ.get('SOAK_SMAPS')
            if dump and i in (turns // 4, turns):
                for tag, pid in (('server', server.pid), ('worker', (kids or [0])[0])):
                    try:
                        open('%s/%s.%d' % (dump, tag, i), 'w').write(
                            open('/proc/%d/smaps' % pid).read())
                    except OSError:
                        pass
            wpid = (kids or [0])[0]
            now = {'w': cpu(wpid), 's': cpu(server.pid), 'm': cpu(mock) if mock else 0.0}
            per = {k: (now[k] - last.get(k, 0.0)) / max(1, i - lasti) for k in now}
            last = now; lasti = i
            print('%4d %6.0f | %8d %6d | %8d %6d | %6.1f %6.1f %6.1f | %6.1f %6.1f' %
                  (i, ms, sa, sfl, wk[0], wk[1], per['w'], per['s'], per['m'], sf, si))
            sys.stdout.flush()
finally:
    if client:
        client.terminate(); client.communicate(timeout=10)
    server.terminate(); server.communicate(timeout=10)
    log.close()
PY
mock_stop_quiet 2>/dev/null || true
