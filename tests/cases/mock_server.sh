#!/bin/bash
# SPDX-License-Identifier: MIT
# The mock provider itself: a held reply does not block another connection,
# keep-alive, chunked request bodies, truncated streams, content matching.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mock_server.json

"$PYTHON" -I - "$MOCK_URL" "$TEST_DIR" "$FYAI_TIMEOUT_SCALE" <<'PYEOF' ||
import json
import select
import socket
import sys

url, run, scale = sys.argv[1], sys.argv[2], float(sys.argv[3])
port = int(url.rsplit(":", 1)[1])
LIMIT = 10.0 * scale


def connect():
    s = socket.create_connection(("127.0.0.1", port), timeout=LIMIT)
    return s


def post(path, body, extra=""):
    data = body.encode()
    return ("POST %s HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
            "%sContent-Length: %d\r\n\r\n" % (path, extra, len(data))
            ).encode() + data


def read_reply(s):
    """Return (status, headers, body); the body is what arrived before EOF."""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            raise SystemExit("closed before the headers: %r" % buf)
        buf += chunk
    head, _, body = buf.partition(b"\r\n\r\n")
    lines = head.decode().split("\r\n")
    status = int(lines[0].split()[1])
    headers = {}
    for line in lines[1:]:
        k, _, v = line.partition(":")
        headers[k.strip().lower()] = v.strip()
    want = int(headers.get("content-length", "0"))
    while len(body) < want:
        chunk = s.recv(4096)
        if not chunk:
            break
        body += chunk
    return status, headers, body


def check(cond, what):
    if not cond:
        raise SystemExit("mock_server: " + what)


# A reply that waits for a marker leaves the other connections alone.
held = connect()
held.sendall(post("/held", "{}"))
quick = connect()
quick.sendall(post("/quick", "{}"))
st, _, body = read_reply(quick)
check(st == 200 and json.loads(body) == {"quick": True}, "quick reply: %r" % body)
ready, _, _ = select.select([held], [], [], 0)
check(not ready, "a held reply arrived before its marker")
open(run + "/release", "w").close()
st, _, body = read_reply(held)
check(st == 200 and json.loads(body) == {"held": True}, "held reply: %r" % body)

# Keep-alive: two requests, one connection.
s = connect()
s.sendall(post("/ka", "{}"))
check(json.loads(read_reply(s)[2]) == {"ka": 1}, "first keep-alive reply")
s.sendall(post("/ka", "{}"))
check(json.loads(read_reply(s)[2]) == {"ka": 2}, "second keep-alive reply")
s.close()

# A chunked request body reaches the log as parsed JSON.
s = connect()
s.sendall(b"POST /chunked HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
          b"4\r\n{\"a\"\r\n4\r\n:12}\r\n0\r\n\r\n")
check(json.loads(read_reply(s)[2]) == {"chunked": True}, "chunked reply")
s.close()

# A truncated stream promises 64 bytes more than it sends and then closes.
s = connect()
s.sendall(post("/cut", "{}"))
st, headers, body = read_reply(s)
check(int(headers["content-length"]) - len(body) == 64,
      "truncated stream promised %s sent %d" % (headers["content-length"], len(body)))
check(s.recv(1) == b"", "the connection stayed open after a truncated stream")
s.close()

# GET expands the base URL.
s = connect()
s.sendall(b"GET /url HTTP/1.1\r\nHost: x\r\n\r\n")
check(read_reply(s)[2].decode() == url + "/x", "GET did not expand {{BASE_URL}}")
s.close()

# Requests that differ only in content take their own step.
for text, want in (("a second one", "second"), ("a first one", "first")):
    s = connect()
    s.sendall(post("/by-body", json.dumps({"t": text})))
    got = json.loads(read_reply(s)[2])
    check(got == {"which": want}, "body match: %r" % got)
    s.close()

# The last item selects the step: a keyword in older items does not, and a
# repeated step answers again.
for msgs, want in (
        ([{"role": "user", "content": "agents 4"}], "agents"),
        ([{"role": "user", "content": "agents 4"},
          {"role": "tool", "content": "ok"}], "tool"),
        ([{"role": "user", "content": [{"type": "text", "text": "agents"}]}],
         "agents")):
    s = connect()
    s.sendall(post("/last", json.dumps({"messages": msgs})))
    got = json.loads(read_reply(s)[2])
    check(got == {"last": want}, "last match: %r" % got)
    s.close()
s = connect()
s.sendall(post("/last", json.dumps({"input": [{"type": "message",
                                                "role": "user",
                                                "content": "other"}]})))
check(read_reply(s)[0] == 500, "an unmatched last item took a step")
s.close()

records = [json.loads(line) for line in open(run + "/requests.jsonl")]
check(len(records) == 13, "%d requests recorded" % len(records))
ka = [r["client_port"] for r in records if r["path"] == "/ka"]
check(len(ka) == 2 and ka[0] == ka[1], "keep-alive used two connections")
chunked = [r for r in records if r["path"] == "/chunked"][0]
check(chunked["body"] == {"a": 12}, "chunked body: %r" % chunked["body"])
check(records[0]["content_type"] == "application/json", "content type not recorded")
PYEOF
	fail "mock server behaviour"

[ ! -e "$TEST_DIR/stream-wait-failed" ] || fail "a marker wait expired"
mock_stop 12
pass
