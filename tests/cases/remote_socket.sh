#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"
for token_mode in plain token; do
fyai_test_setup
mock_start remote_socket.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" "$FYAI_TIMEOUT_SCALE" "$token_mode" <<'PY'
import json, os, select, socket, subprocess, sys, time
binary, cwd, scale, mode = sys.argv[1:]
scale = int(scale)
path = cwd + ('/.fyai/unix/remote.sock' if mode == 'plain' else '/remote.sock')
os.mkdir(cwd + '/nested')
log = open(cwd + '/server.err', 'wb')
args = [binary, 'serve']
if mode == 'token':
    args += ['--socket', path]
fds = ()
if mode == 'token':
    r, w = os.pipe()
    args += ['--token-fd', str(w)]
    fds = (w,)
server = subprocess.Popen(args, cwd=cwd, pass_fds=fds,
                          stdout=subprocess.PIPE, stderr=log)
if mode == 'token':
    os.close(w)
clients = []
def receive(stream, timeout=20):
    data = bytearray()
    deadline = time.monotonic() + timeout * scale
    fd = stream.fileno()
    while time.monotonic() < deadline:
        assert select.select([fd], [], [], max(0, deadline-time.monotonic()))[0], 'frame timeout'
        c = os.read(fd, 1)
        assert c, 'unexpected EOF: ' + open(cwd + '/server.err').read()
        if c == b'\n':
            return json.loads(data)
        data.extend(c)
    raise AssertionError('frame timeout')
def send(stream, value):
    os.write(stream.fileno(), (json.dumps(value)+'\n').encode())
def start_client(branch):
    args = [binary]
    if branch:
        args += ['-b', branch]
    args += ['remote', 'connect', '--protocol', 'sdk']
    if mode == 'token':
        args += [path]
    fds = ()
    if mode == 'token':
        tr, tw = os.pipe()
        os.write(tw, token)
        os.close(tw)
        args += ['--token-fd', str(tr)]
        fds = (tr,)
    p = subprocess.Popen(args, cwd=cwd + '/nested', pass_fds=fds, stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if fds:
        os.close(tr)
    return p
try:
    if mode == 'token':
        assert select.select([r], [], [], 20 * scale)[0], 'token timeout'
        token = os.read(r, 65)
        assert len(token) == 65 and token[-1:] == b'\n', open(cwd + '/server.err').read()
        os.close(r)
        for request in [
            {'subtype':'fyai_authenticate','token':'0'*64},
            {'subtype':'initialize'},
        ]:
            bad = socket.socket(socket.AF_UNIX)
            bad.connect(path)
            send(bad, {'type':'control_request','request_id':'auth','request':request})
            assert select.select([bad], [], [], 5 * scale)[0] and bad.recv(1) == b'', 'unauthenticated client accepted'
            bad.close()
    else:
        deadline = time.monotonic() + 20 * scale
        while True:
            probe = socket.socket(socket.AF_UNIX)
            try:
                probe.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                assert server.poll() is None and time.monotonic() < deadline, 'socket startup failed'
            finally:
                probe.close()
    for i in range(2):
        p = start_client(f'remote/{i}')
        clients.append(p)
        send(p.stdin, {'type':'control_request','request_id':'init','request':{'subtype':'initialize'}})
    for p in clients:
        f = receive(p.stdout)
        assert f['type'] == 'control_response' and f['response']['subtype'] == 'success', f
    for p in clients:
        send(p.stdin, {'type':'user','message':{'role':'user','content':f'Hello from {clients.index(p)}.'}})
    ids = []
    for p in reversed(clients):
        while True:
            f = receive(p.stdout)
            if f['type'] == 'result':
                assert not f.get('is_error'), f
                ids.append(f['session_id'])
                if p is clients[1]:
                    open(cwd + '/release-zero', 'w').close()
                break
    assert set(ids) == {'remote/0', 'remote/1'}, ids
    first = clients.pop(0)
    first.terminate()
    first.communicate(timeout=10)
    reconnected = start_client('remote/0')
    clients.append(reconnected)
    send(reconnected.stdin, {'type':'control_request','request_id':'again','request':{'subtype':'initialize'}})
    f = receive(reconnected.stdout)
    assert f['response']['request_id'] == 'again' and f['response']['subtype'] == 'success', f
    send(reconnected.stdin, {'type':'user','message':{'role':'user','content':'Continue branch zero.'}})
    while True:
        f = receive(reconnected.stdout)
        if f['type'] == 'result':
            assert not f.get('is_error') and f['session_id'] == 'remote/0', f
            break
    # A client that ends its input still gets the result of its message.
    p = start_client('remote/eof')
    send(p.stdin, {'type':'control_request','request_id':'init','request':{'subtype':'initialize'}})
    send(p.stdin, {'type':'user','message':{'role':'user','content':'End of input.'}})
    p.stdin.close()
    out = p.stdout.read()
    err = p.stderr.read()
    p.wait(timeout=20 * scale)
    frames = [json.loads(l) for l in out.splitlines() if l.strip()]
    assert p.returncode == 0, err
    assert any(f['type'] == 'result' and not f.get('is_error') for f in frames), frames
    # A session that no branch names ends with its client.
    def workers():
        r = subprocess.run(['pgrep', '-P', str(server.pid)],
                           capture_output=True, text=True)
        return set(int(x) for x in r.stdout.split())
    def gone(pid):
        # Wait for the exit of a child of the server, which is not ours.
        if hasattr(os, 'pidfd_open'):
            try:
                pidfd = os.pidfd_open(pid)
            except ProcessLookupError:
                return True
            try:
                return bool(select.select([pidfd], [], [], 20 * scale)[0])
            finally:
                os.close(pidfd)
        kq = select.kqueue()
        try:
            ev = select.kevent(pid, filter=select.KQ_FILTER_PROC,
                               flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT,
                               fflags=select.KQ_NOTE_EXIT)
            return bool(kq.control([ev], 1, 20 * scale))
        except ProcessLookupError:
            return True
        finally:
            kq.close()
    before = workers()
    p = start_client(None)
    clients.append(p)
    send(p.stdin, {'type':'control_request','request_id':'init','request':{'subtype':'initialize'}})
    send(p.stdin, {'type':'user','message':{'role':'user','content':'Anonymous hello.'}})
    while receive(p.stdout)['type'] != 'result':
        pass
    started = workers() - before
    assert len(started) == 1, started
    worker = started.pop()
    clients.remove(p)
    p.terminate()
    p.communicate(timeout=10)
    assert gone(worker), 'the worker of a closed anonymous session stayed'
    # A second server must preserve the socket owned by the first server.
    tr, tw = os.pipe()
    collision_args = [binary, 'serve']
    if mode == 'token':
        collision_args += ['--socket', path]
    collision_args += ['--token-fd', str(tw)]
    collision = subprocess.run(collision_args,
                               cwd=cwd, pass_fds=(tw,), capture_output=True, timeout=10)
    os.close(tr); os.close(tw)
    assert collision.returncode != 0 and os.path.exists(path), collision.stderr
finally:
    for p in clients:
        p.terminate()
        p.communicate(timeout=10)
    server.terminate()
    server.communicate(timeout=10)
    log.close()
assert server.returncode == 0, open(cwd + '/server.err').read()
assert not os.path.exists(path), 'server left its socket behind'
# A server that was killed leaves its socket; the next server replaces it.
log2 = open(cwd + '/stale.err', 'wb')
def serve():
    r, w = os.pipe()
    args = [binary, 'serve', '--token-fd', str(w)]
    if mode == 'token':
        args += ['--socket', path]
    s = subprocess.Popen(args, cwd=cwd, pass_fds=(w,),
                         stdout=subprocess.DEVNULL, stderr=log2)
    os.close(w)
    # The token is written after the socket listens.
    assert select.select([r], [], [], 20 * scale)[0], 'serve did not listen'
    assert len(os.read(r, 65)) == 65, open(cwd + '/stale.err').read()
    os.close(r)
    return s
killed = serve()
killed.kill()
killed.wait()
assert os.path.exists(path), 'the killed server took its socket'
again = serve()
again.terminate()
again.wait(timeout=10)
log2.close()
assert again.returncode == 0, open(cwd + '/stale.err').read()
assert not os.path.exists(path), 'the server left its socket behind'
PY
mock_stop 5
assert_request 2 'any("Hello from 0." in str(m.get("content", "")) for m in r["body"]["messages"])'
assert_request 2 'not any("Hello from 1." in str(m.get("content", "")) for m in r["body"]["messages"])'
fyai_test_cleanup
done
echo "PASS: remote_socket.sh (plain and token)"
