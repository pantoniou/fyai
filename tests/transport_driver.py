#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Drive `fyai transport` over its control channel, as a supervisor does.

Usage: transport_driver.py FYAI_BIN MOCK_URL ARENA_DIR

The driver is also the agent: it holds the agent end of a SOCK_SEQPACKET pair,
which the kernel tags with its PID and UID. It checks the streaming path, the
grant and the profile reload, the event on a closed channel, the log, the
hardening of the process, and the end of the process on shutdown and on a
closed control channel. Every wait is a blocking read with a deadline.
"""

import json
import os
import socket
import struct
import subprocess
import sys

SCALE = float(os.environ.get("FYAI_TIMEOUT_SCALE", "1"))
DEADLINE = 15.0 * SCALE

HDR = struct.Struct("<IHHQQQII")
MAGIC = 0x31545946
REQUEST, CANCEL, CREDIT, START, BODY, END, ERROR = 1, 2, 3, 4, 5, 6, 7


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


class Transport:
    def __init__(self, fyai, arena):
        self.ctl, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.ctl.settimeout(DEADLINE)
        env = dict(os.environ)
        # A credential in the environment that only the transport may read.
        env["TRANSPORT_TEST_KEY"] = "sk-transport-test-0123456789"
        self.proc = subprocess.Popen(
            [fyai, "--color", "off", "transport", "--control-fd",
             str(child.fileno()), "--arena", arena],
            pass_fds=[child.fileno()], env=env,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=open("transport.stderr", "wb"))
        child.close()
        self.seq = 0
        self.events = []

    def request(self, op, fd=None, **kw):
        self.seq += 1
        msg = dict(op=op, seq=self.seq, **kw)
        data = json.dumps(msg).encode()
        if fd is None:
            self.ctl.send(data)
        else:
            socket.send_fds(self.ctl, [data], [fd])
        while True:
            r = json.loads(self.ctl.recv(65536))
            if r["op"] == "event":
                self.events.append(r)
                continue
            check(r["seq"] == self.seq, "reply for another request: %r" % r)
            return r

    def ok(self, op, fd=None, **kw):
        r = self.request(op, fd, **kw)
        check(r["op"] == "ok", "%s failed: %r" % (op, r))

    def refused(self, op, fd=None, **kw):
        r = self.request(op, fd, **kw)
        check(r["op"] == "error", "%s was accepted: %r" % (op, r))
        return r["message"]

    def event(self, name):
        """Wait for an event of the given name; the channel is otherwise idle."""
        for e in self.events:
            if e["event"] == name:
                return e
        while True:
            r = json.loads(self.ctl.recv(65536))
            if r["op"] == "event":
                self.events.append(r)
                if r["event"] == name:
                    return r


class Agent:
    def __init__(self, tr, exec_id, grant):
        self.sock, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.sock.settimeout(DEADLINE)
        self.id = exec_id
        tr.ok("admit", theirs.fileno(), id=exec_id, parent=0, pid=os.getpid(),
              uid=os.getuid(), grant=grant)
        theirs.close()
        self.rid = 0

    def send(self, kind, rid, payload=b"", seq=0, flags=0):
        self.sock.send(HDR.pack(MAGIC, 1, kind, self.id, rid, seq,
                                len(payload), flags) + payload)

    def call(self, profile, body, **hdr):
        """One request; return (start, body bytes, terminal kind, terminal)."""
        self.rid += 1
        header = json.dumps(dict(profile=profile, **hdr)).encode()
        self.send(REQUEST, self.rid,
                  struct.pack("<I", len(header)) + header + body)
        start, data = None, b""
        expect = 0
        while True:
            frame = self.sock.recv(65536)
            magic, ver, kind, xid, rid, seq, ln, flags = HDR.unpack(frame[:40])
            check(magic == MAGIC and xid == self.id and rid == self.rid,
                  "frame for another request")
            check(seq == expect, "sequence %d, expected %d" % (seq, expect))
            expect += 1
            pl = frame[40:]
            if kind == START:
                start = json.loads(pl)
            elif kind == BODY:
                data += pl
            elif kind in (END, ERROR):
                return start, data, kind, (json.loads(pl) if pl else None)


def chat(text):
    return json.dumps({"model": "mock-model",
                       "messages": [{"role": "user", "content": text}]}).encode()


def reply_text(data):
    return json.loads(data)["choices"][0]["message"]["content"]


def main(fyai, mock_url, arena):
    url = mock_url + "/v1/chat/completions"
    tr = Transport(fyai, arena)
    try:
        # Nothing works before init, and init takes a level that we can enforce.
        check("init" in tr.refused("profiles", profiles=[]), "profiles before init")
        tr.refused("init", level="auto")
        tr.refused("init", level="none")
        tr.refused("init", level="level-a")	# needs a cgroup
        tr.ok("init", level="level-b", log=False)
        tr.refused("init", level="level-b")

        prof = [{"name": "main", "url": url, "auth": "none", "tag": "chat"},
                {"name": "side", "url": url, "auth": "none", "tag": "side"}]
        tr.ok("profiles", profiles=prof)
        tr.refused("profiles", profiles=[{"name": "x", "url": "http://evil.example/",
                                          "auth": "none"}])
        tr.refused("profiles", profiles=[{"name": "x", "url": "https://a.example/",
                                          "auth": "bearer"}])	# no credential source
        tr.refused("profiles", profiles=[{"name": "x", "url": "https://a.example/",
                                          "auth": "none", "credential": "env:K"}])

        agent = Agent(tr, 7, [{"profile": "main"}])

        # A request streams the provider reply, in order.
        start, data, kind, _ = agent.call("main", chat("hi"),
                                          content_type="application/json")
        check(kind == END and start["status"] == 200, "first request: %r %r" % (kind, start))
        check(start["tag"] == "chat", "tag missing from the start frame")
        check(reply_text(data) == "first reply", "wrong reply: %r" % data)

        # A profile the grant lacks is refused, whatever the set holds.
        start, data, kind, err = agent.call("side", chat("hi"),
                                            content_type="application/json")
        check(kind == ERROR and err["code"] == -1 and start is None,
              "ungranted profile: %r %r" % (kind, err))

        # A request that names a URL or a header is refused.
        start, data, kind, err = agent.call("main", chat("hi"),
                                            url="http://evil.example/")
        check(kind == ERROR and err["code"] == -1, "a url was accepted")
        start, data, kind, err = agent.call("main", chat("hi"),
                                            headers={"Authorization": "x"})
        check(kind == ERROR and err["code"] == -1, "a header was accepted")

        # The grant changes for the next request.
        tr.ok("grant", id=7, grant=[{"profile": "main"}, {"profile": "side"}])
        start, data, kind, _ = agent.call("side", chat("hi"),
                                          content_type="application/json")
        check(kind == END and start["tag"] == "side", "granted profile refused")
        check(reply_text(data) == "second reply", "wrong second reply")
        tr.ok("grant", id=7, grant=[])
        start, data, kind, err = agent.call("main", chat("hi"),
                                            content_type="application/json")
        check(kind == ERROR, "an empty grant reached a provider")
        tr.refused("grant", id=99, grant=[])
        tr.refused("grant", id=7, grant=[{"profile": "bad name"}])

        # A reload replaces the profile; the next request sees the new tag.
        tr.ok("grant", id=7, grant=[{"profile": "main"}])
        tr.ok("profiles", profiles=[{"name": "main", "url": url, "auth": "none",
                                     "tag": "reloaded"}])
        start, data, kind, _ = agent.call("main", chat("hi"),
                                          content_type="application/json")
        check(kind == END and start["tag"] == "reloaded", "reload not applied")
        check(reply_text(data) == "third reply", "wrong third reply")

        # A credential source can be a chain: the first that holds a value wins.
        tr.ok("credential", name="k", value="sk-mem-0123456789")
        tr.ok("profiles", profiles=[
            {"name": "main", "url": url, "auth": "bearer",
             "credential": "env:NO_SUCH_VARIABLE_XYZ|secret:no-such-secret|mem:k"},
            {"name": "empty", "url": url, "auth": "bearer",
             "credential": "env:NO_SUCH_VARIABLE_XYZ|mem:absent"}])
        tr.ok("grant", id=7, grant=[{"profile": "main"}, {"profile": "empty"}])
        start, data, kind, err = agent.call("empty", chat("hi"),
                                            content_type="application/json")
        check(kind == ERROR and "credential" in err["message"],
              "a chain with no value reached a provider: %r" % (err,))
        start, data, kind, _ = agent.call("main", chat("hi"),
                                          content_type="application/json")
        check(kind == END and reply_text(data) == "fourth reply", "chain not resolved")
        seen = [json.loads(l) for l in open(os.path.join(
            os.path.dirname(os.path.dirname(arena)), "requests.jsonl"))]
        check(seen[-1]["auth"] == "Bearer sk-mem-0123456789",
              "the provider saw %r" % seen[-1]["auth"])
        tr.ok("profiles", profiles=[{"name": "main", "url": url, "auth": "none",
                                     "tag": "reloaded"}])
        tr.ok("grant", id=7, grant=[{"profile": "main"}])

        # The log records the traffic, and not the key.
        tr.ok("log", on=True)
        start, data, kind, _ = agent.call("main", chat("log me"),
                                          content_type="application/json")
        check(kind == END and reply_text(data) == "fifth reply", "fifth request")
        tr.ok("log", on=False)
        log_path = os.path.join(os.path.dirname(arena), "logs", "transport.yaml")
        log = open(log_path).read()
        for want in ("event: request", "event: response", "event: end",
                     "kind: transport-wire", "log me"):
            check(want in log, "the log lacks %r" % want)
        check("sk-transport-test-0123456789" not in log, "the log holds the key")

        # The process cannot be read by another process of the same user.
        for name in ("environ", "mem"):
            try:
                open("/proc/%d/%s" % (tr.proc.pid, name), "rb").read(1)
            except PermissionError:
                pass
            else:
                raise Fail("/proc/PID/%s is readable" % name)
        try:
            os.listdir("/proc/%d/fd" % tr.proc.pid)
        except PermissionError:
            pass
        else:
            raise Fail("/proc/PID/fd is readable")

        # A closed agent channel is retired, and the supervisor is told.
        agent.sock.close()
        ev = tr.event("retired")
        check(ev["id"] == 7, "retired event names another execution")

        # An admission needs a descriptor, a live process, and a free id.
        tr.refused("admit", id=8, parent=0, pid=os.getpid(), uid=os.getuid(),
                   grant=[])
        a2, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        r = tr.request("admit", theirs.fileno(), id=8, parent=0, pid=2 ** 22 + 1,
                       uid=os.getuid(), grant=[])
        check(r["op"] == "error", "a process that does not exist was admitted")
        a2.close()
        theirs.close()

        # Shutdown is acknowledged, and the process ends.
        tr.ok("shutdown")
        check(tr.proc.wait(DEADLINE) == 0, "transport did not exit cleanly")
    finally:
        if tr.proc.poll() is None:
            tr.proc.terminate()
            tr.proc.wait()

    # A closed control channel ends the transport too.
    tr = Transport(fyai, arena)
    try:
        tr.ok("init", level="level-b", log=False)
        tr.ctl.close()
        check(tr.proc.wait(DEADLINE) == 0, "transport survived its channel")
    finally:
        if tr.proc.poll() is None:
            tr.proc.terminate()
            tr.proc.wait()

    # A bad start is refused with a message, not a crash.
    for args in (["transport"], ["transport", "--control-fd", "9"]):
        p = subprocess.run([fyai, "--color", "off"] + args, stdin=subprocess.DEVNULL,
                           capture_output=True, timeout=DEADLINE)
        check(p.returncode != 0, "%r started" % args)
    print("OK")


if __name__ == "__main__":
    try:
        main(*sys.argv[1:4])
    except Fail as e:
        print("FAIL: %s" % e)
        sys.exit(1)
