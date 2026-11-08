#!/bin/bash
# SPDX-License-Identifier: MIT
# A client that asks for the UI model gets each store and text of the worker:
# the header values, the state of the turn, the source of the live document as
# deltas, and a question that it answers.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ask_user.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
	--set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
	--set 'api_key={type: env, value: MOCK_KEY}' --set tools=true \
	>/dev/null 2>&1
"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" "$FYAI_TIMEOUT_SCALE" <<'PY'
import json, os, select, subprocess, sys
binary, cwd, scale = sys.argv[1], sys.argv[2], int(sys.argv[3])
log = open(cwd + '/server.err', 'wb')
r, w = os.pipe()
server = subprocess.Popen([binary, 'serve', '--token-fd', str(w)], cwd=cwd,
                          pass_fds=(w,), stdout=subprocess.DEVNULL, stderr=log)
os.close(w)
client = None
def receive(stream):
    data = bytearray()
    fd = stream.fileno()
    while True:
        assert select.select([fd], [], [], 20 * scale)[0], 'frame timeout'
        c = os.read(fd, 1)
        assert c, 'unexpected EOF: ' + open(cwd + '/server.err').read()
        if c == b'\n':
            return json.loads(data)
        data.extend(c)
def send(value):
    client.stdin.write((json.dumps(value) + '\n').encode())
    client.stdin.flush()
try:
    # The token is written when the socket listens.
    assert select.select([r], [], [], 20 * scale)[0], 'serve did not listen'
    token = os.read(r, 65)
    os.close(r)
    tr, tw = os.pipe()
    os.write(tw, token)
    os.close(tw)
    client = subprocess.Popen([binary, '-b', 'remote/model', 'remote', 'connect',
                               '--protocol', 'sdk', '--token-fd', str(tr)],
                              cwd=cwd, pass_fds=(tr,), stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    os.close(tr)
    send({'type':'control_request','request_id':'init','request':{
        'subtype':'initialize','fyai':{'model':1}}})
    stores, texts, busy = {}, {}, []
    initialized = asked = False
    while True:
        f = receive(client.stdout)
        if f['type'] == 'control_response' and f['response']['request_id'] == 'init':
            info = f['response']['response']['fyai']
            assert info['model'] == 1 and info['epoch'], f
            epoch = info['epoch']
            initialized = True
            send({'type':'user','message':{'role':'user','content':'ask me something'}})
            continue
        if f['type'] == 'fyai' and f['subtype'] == 'store':
            stores[f['store']] = f['root']
            turn = f['root'].get('turn') if f['store'] == 'session' else None
            if turn is not None and (not busy or busy[-1] != turn['busy']):
                busy.append(turn['busy'])
            opened = stores.get('questions', {}).get('open', [])
            if opened and not asked:
                q = opened[0]
                assert q['questions'][0]['question'] == 'Proceed with the mock plan?', q
                send({'type':'control_request','request_id':'answer','request':{
                    'subtype':'fyai_answer','qid':q['qid'],
                    'replies':[{'selected':[0]}]}})
                asked = True
            continue
        if f['type'] == 'fyai' and f['subtype'] == 'text':
            if f.get('closed'):
                continue
            text = texts.setdefault(f['id'], bytearray())
            assert f['offset'] == len(text), (f, len(text))
            text.extend(f['data'].encode())
            continue
        if f['type'] == 'result':
            assert not f.get('is_error'), f
            break
    assert initialized and asked
    header = stores['session']['header']
    assert header['model'] == 'mock-model' and header['branch'] == 'remote/model', header
    assert busy[:1] == [True] and busy[-1] is False, busy
    assert any(b'User said yes' in t for t in texts.values()), texts
    assert stores['questions']['open'] == [], stores['questions']
    # The turns the worker stored, by negotiation: the client says which it holds.
    def sync(name, have):
        send({'type':'control_request','request_id':name,'request':{
            'subtype':'fyai_sync','have':have,'depth':64}})
        while True:
            f = receive(client.stdout)
            if f['type'] == 'control_response' and f['response']['request_id'] == name:
                assert f['response']['subtype'] == 'success', f
                return f['response']['response']
    full = sync('sync-full', [])
    assert full['epoch'] == epoch and full['base'] is None and not full['more'], full
    ids = [t['id'] for t in full['turns']]
    assert len(ids) >= 2 and full['head'] == ids[-1] and len(set(ids)) == len(ids), full
    assert any('User said yes' in json.dumps(t) for t in full['turns']), full
    same = sync('sync-same', [ids[-1]])
    assert same['turns'] == [] and same['base'] == ids[-1], same
    part = sync('sync-part', [ids[0], 'not-a-turn'])
    assert part['base'] == ids[0] and [t['id'] for t in part['turns']] == ids[1:], part
    other = sync('sync-other', ['0'])
    assert other['base'] is None and len(other['turns']) == len(ids), other
finally:
    if client:
        client.terminate()
        client.communicate(timeout=10)
    server.terminate()
    server.communicate(timeout=10)
    log.close()
assert server.returncode == 0, open(cwd + '/server.err').read()
PY
mock_stop 2
assert_request 1 '"yes" in json.dumps(r["body"]["messages"][-1])'
pass
