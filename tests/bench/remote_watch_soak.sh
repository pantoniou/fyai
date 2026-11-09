#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Cost of watchers: N turns through one `fyai serve` worker with a driver and
# a number of watchers that read the session as `remote connect --watch` does.
# It prints, for each watcher count, the time of a turn, the CPU that the
# server and the worker spend for a turn, the bytes that a watcher reads, and
# the resident set of both.
#
#   tests/bench/remote_watch_soak.sh ./build/fyai [turns] [reply_kb] [counts]
#
# counts is a list such as "0 1 4 16" (default); each count runs on a branch
# of its own.
set -eu
FYAI_BIN="${1:?usage: remote_watch_soak.sh <fyai-binary> [turns] [reply_kb]}"
export FYAI_BIN
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/harness.sh"

TURNS="${2:-30}"
REPLY_KB="${3:-4}"
COUNTS="${4:-0 1 4 16}"
export COUNTS

fyai_test_setup
"$PYTHON" - "$TEST_DIR/soak.json" "$(( (TURNS + 1) * $(echo $COUNTS | wc -w) ))" "$REPLY_KB" <<'PY'
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
import json, os, select, socket, subprocess, sys, time
binary, cwd, turns, scale = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
counts = [int(c) for c in os.environ['COUNTS'].split()]
path = cwd + '/.fyai/unix/remote.sock'
log = open(cwd + '/server.err', 'wb')
server = subprocess.Popen([binary, 'serve'], cwd=cwd, stdout=subprocess.DEVNULL, stderr=log)

def stat(pid, what):
    a = 0
    try:
        for l in open('/proc/%d/status' % pid):
            if l.startswith(what):
                a = int(l.split()[1])
    except OSError:
        pass
    return a
def cpu(pid):
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
        out.append(int(k)); out.extend(descendants(int(k)))
    return out

def frames_of(buf):
    while True:
        nl = buf.find(b'\n')
        if nl < 0:
            return
        line = bytes(buf[:nl]); del buf[:nl + 1]
        yield line

class Watcher:
    def __init__(self, branch):
        self.s = socket.socket(socket.AF_UNIX); self.s.connect(path)
        self.buf = bytearray(); self.bytes = 0
        self.rpc({'subtype':'fyai_hello','version':'x','protocol':1,'features':['model']})
        self.rpc({'subtype':'fyai_session_attach','branch':branch,'watch':True})
        self.rpc({'subtype':'initialize','fyai':{'model':1}}, wait='init')
    def rpc(self, req, wait=None):
        rid = wait or req['subtype']
        self.s.sendall((json.dumps({'type':'control_request','request_id':rid,'request':req}) + '\n').encode())
        got = False; end = time.monotonic() + 30 * scale
        while not got:
            assert time.monotonic() < end, 'watcher timeout'
            select.select([self.s], [], [], 1)
            self.pump()
            got = self.seen == rid
    seen = None
    def pump(self):
        try:
            d = self.s.recv(1 << 20, socket.MSG_DONTWAIT)
        except BlockingIOError:
            return
        assert d, 'watcher EOF'
        self.bytes += len(d); self.buf.extend(d)
        for line in frames_of(self.buf):
            if b'control_response' in line[:40]:
                self.seen = json.loads(line)['response']['request_id']
            if b'"phase":"end"' in line:
                self.seen = 'init'

print('watchers turn_ms(med max) | cpu ms/turn: worker server | KB/turn per watcher | rss MB worker server')
for idx, n in enumerate(counts):
    branch = 'bench/w%d-%d' % (idx, n)
    client = subprocess.Popen([binary, '-b', branch, 'remote', 'connect', '--protocol', 'sdk'],
                              cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    pend = bytearray()
    def send(v):
        client.stdin.write((json.dumps(v) + '\n').encode()); client.stdin.flush()
    ws = []
    def turn(i):
        send({'type':'user','message':{'role':'user','content':'question %d' % i}})
        t0 = time.time()
        while True:
            fds = [client.stdout] + [w.s for w in ws]
            ready = select.select(fds, [], [], 60 * scale)[0]
            assert ready, 'timeout'
            for w in ws:
                if w.s in ready: w.pump()
            if client.stdout in ready:
                pend.extend(os.read(client.stdout.fileno(), 1 << 20))
                for line in frames_of(pend):
                    if b'result' in line and json.loads(line).get('type') == 'result':
                        return (time.time() - t0) * 1000
    try:
        send({'type':'control_request','request_id':'init','request':{'subtype':'initialize','fyai':{'model':1}}})
        while True:
            select.select([client.stdout], [], [], 30 * scale)
            pend.extend(os.read(client.stdout.fileno(), 1 << 20))
            if b'control_response' in pend:
                pend.clear(); break
        turn(0)
        for _ in range(n):
            ws.append(Watcher(branch))
        kids = descendants(server.pid); wpid = max(kids) if kids else 0
        c0 = (cpu(wpid), cpu(server.pid)); b0 = [w.bytes for w in ws]
        ms = []
        for i in range(1, turns + 1):
            ms.append(turn(i))
        # lag: a last turn, then the time until every watcher stops reading
        c1 = (cpu(wpid), cpu(server.pid))
        ms.sort()
        kb = (sum(w.bytes - b for w, b in zip(ws, b0)) / max(1, n) / 1024.0 / turns) if n else 0
        print('%8d %6.0f %6.0f | %6.1f %6.1f | %8.1f | %6.1f %6.1f' % (
            n, ms[len(ms) // 2], ms[-1], (c1[0] - c0[0]) / turns, (c1[1] - c0[1]) / turns, kb,
            stat(wpid, 'RssAnon:') / 1024.0, stat(server.pid, 'RssAnon:') / 1024.0))
        sys.stdout.flush()
    finally:
        for w in ws: w.s.close()
        client.terminate(); client.communicate(timeout=10)
server.terminate(); server.communicate(timeout=10)
PY
mock_stop_quiet 2>/dev/null || true
