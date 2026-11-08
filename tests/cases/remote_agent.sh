#!/bin/bash
# SPDX-License-Identifier: MIT
# A sub-agent of a worker with no display has a terminal while a client reads
# the model: the work store names its screen, and the screen carries what the
# sub-agent drew.
set -eu
. "$(dirname "$0")/../remote-harness.sh"

remote_test_setup remote_agent.json
# The case reads the bytes of the screen: an inline screen writes the text
# of the sub-agent as one run.
"$FYAI_BIN" --set tools=true --set display/screen=inline >/dev/null 2>&1
"$PYTHON" - "$FYAI_BIN" "$FYAI_TIMEOUT_SCALE" <<'PY'
import json, os, select, subprocess, sys
binary, scale = sys.argv[1], int(sys.argv[2])
p = subprocess.Popen([binary, '-b', 'remote/agent', 'remote', 'connect',
                      '--protocol', 'sdk'],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE)
def send(value):
    p.stdin.write((json.dumps(value) + '\n').encode())
    p.stdin.flush()
buf = b''
def receive():
    global buf
    while b'\n' not in buf:
        assert select.select([p.stdout], [], [], 20 * scale)[0], 'frame timeout'
        data = os.read(p.stdout.fileno(), 65536)
        assert data, 'the client closed'
        buf += data
    line, buf = buf.split(b'\n', 1)
    return json.loads(line)
try:
    send({'type':'control_request','request_id':'init','request':{
        'subtype':'initialize','fyai':{'model':1}}})
    send({'type':'user','message':{'role':'user','content':'Delegate it.'}})
    screen, texts = None, {}
    while True:
        f = receive()
        if f['type'] == 'fyai' and f['subtype'] == 'store':
            if f['store'] == 'questions':
                for q in f['root'].get('open', []):
                    send({'type':'control_request','request_id':'a','request':{
                        'subtype':'fyai_answer','qid':q['qid'],
                        'replies':[{'selected':[0]}]}})
            if f['store'] == 'work':
                for item in f['root'].get('items', []):
                    if item['kind'] == 'agent' and 'terminal' in item:
                        screen = item['terminal']['source']
        elif f['type'] == 'fyai' and f['subtype'] == 'text' and not f.get('closed'):
            texts.setdefault(f['id'], bytearray()).extend(f['data'].encode())
        elif f['type'] == 'result':
            assert not f.get('is_error'), f
            break
    assert screen, 'the sub-agent had no terminal in the work store'
    assert b'Remote agent greeting.' in texts.get(screen, b''), texts.get(screen)
finally:
    p.terminate()
    p.communicate(timeout=10)
PY
remote_test_stop
mock_stop 3
pass
