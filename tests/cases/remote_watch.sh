#!/bin/bash
# SPDX-License-Identifier: MIT
# A client that watches a session sees what the client that drives it sees, as
# it happens and from the start, and sends the session nothing: the server
# refuses a request of a watcher that changes the session.
set -eu
. "$(dirname "$0")/../remote-harness.sh"

remote_test_setup remote_watch.json
"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" "$FYAI_TIMEOUT_SCALE" <<'PY'
import json, os, select, socket, subprocess, sys, time
binary, cwd, scale = sys.argv[1], sys.argv[2], int(sys.argv[3])
path = cwd + '/.fyai/unix/remote.sock'
branch = 'watch/one'

def receive(stream, timeout=20):
    data = bytearray()
    end = time.monotonic() + timeout * scale
    fd = stream.fileno()
    while time.monotonic() < end:
        assert select.select([fd], [], [], max(0, end - time.monotonic()))[0], 'frame timeout'
        c = os.read(fd, 1)
        assert c, 'unexpected EOF'
        if c == b'\n':
            return json.loads(data)
        data.extend(c)
    raise AssertionError('frame timeout')

def send(stream, value):
    os.write(stream.fileno(), (json.dumps(value) + '\n').encode())

def answer(stream, text):
    while True:
        f = receive(stream)
        assert f.get('subtype') != 'snapshot', 'the driver got a snapshot of a watcher'
        if f['type'] == 'result':
            assert not f.get('is_error') and text in f['result'], f
            return

driver = subprocess.Popen([binary, '-b', branch, 'remote', 'connect',
                           '--protocol', 'sdk'], cwd=cwd, stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
try:
    send(driver.stdin, {'type':'control_request','request_id':'init','request':{'subtype':'initialize'}})
    assert receive(driver.stdout)['response']['subtype'] == 'success'
    send(driver.stdin, {'type':'user','message':{'role':'user','content':'First question.'}})
    answer(driver.stdout, 'First answer.')

    # A watcher of a branch that has no session starts none.
    def raw():
        s = socket.socket(socket.AF_UNIX)
        s.connect(path)
        send(s, {'type':'control_request','request_id':'hello','request':{
            'subtype':'fyai_hello','version':'x','protocol':1,'features':['model']}})
        assert receive(s)['response']['subtype'] == 'success'
        return s
    def request(s, rid, **fields):
        send(s, {'type':'control_request','request_id':rid,'request':fields})
        while True:
            f = receive(s)
            if f['type'] == 'control_response' and f['response']['request_id'] == rid:
                return f['response']
    s = raw()
    r = request(s, 'none', subtype='fyai_session_attach', branch='watch/none', watch=True)
    assert r['subtype'] == 'error' and 'no live session' in r['error'], r
    s.close()

    # --last names the last session of the directory, as resume --last does.
    s = raw()
    r = request(s, 'last', subtype='fyai_session_attach', watch=True,
                resume={'last': True})
    assert r['subtype'] == 'success' and r['response']['branch'] == branch, r
    s.close()

    # A watcher reads and changes nothing.
    s = raw()
    r = request(s, 'w', subtype='fyai_session_attach', branch=branch, watch=True)
    assert r['subtype'] == 'success', r
    for what in ({'subtype':'interrupt'}, {'subtype':'fyai_command','line':'/quit'},
                 {'subtype':'fyai_bang','command':'true'},
                 {'subtype':'fyai_session_attach','branch':'watch/two'}):
        r = request(s, 'no', **what)
        assert r['subtype'] == 'error' and 'only reads' in r['error'], (what, r)
    # It gets the model of the UI, and the driver does not get it a second time.
    r = request(s, 'init', subtype='initialize', fyai={'model':1})
    assert r['subtype'] == 'success', r
    # The snapshot comes to the watcher alone, whole, between two markers.
    seen = []
    while True:
        f = receive(s)
        if f.get('subtype') == 'snapshot':
            assert f['request_id'] == 'init', f
            seen.append(f['phase'])
            if f['phase'] == 'end':
                break
        else:
            assert seen == ['begin'], (seen, f)
    assert seen == ['begin', 'end'], seen
    s.close()

    # The full-screen watcher: it opens on the conversation so far, follows the
    # next turn of the driver, and its typed line goes nowhere.
    script = cwd + '/watch.yaml'
    open(script, 'w').write('''rows: 30
cols: 100
ready: watch/one
steps:
  - wait-screen: watching
  - wait-screen: First answer.
  - write: "typed by the watcher\\n"
  - touch: %s/typed
  - wait-screen: Second answer.
  - write: "\\x03"
''' % cwd)
    watcher = subprocess.Popen([binary, '__pty-driver', script, cwd + '/watch.out',
                                binary, '-b', branch, 'remote', 'connect',
                                '--watch'], cwd=cwd, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    # The reply waits for the line that the watcher typed.
    send(driver.stdin, {'type':'user','message':{'role':'user','content':'Second question.'}})
    answer(driver.stdout, 'Second answer.')
    out, err = watcher.communicate(timeout=30 * scale)
    assert watcher.returncode == 0, (out, err)
finally:
    driver.terminate()
    driver.communicate(timeout=10)
PY
remote_test_stop
mock_stop 2
assert_no_request 'any("typed by the watcher" in str(m.get("content", "")) for m in r["body"]["messages"])'
pass
