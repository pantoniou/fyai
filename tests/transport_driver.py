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

import faulthandler
import json
import os
import socket
import struct
import subprocess
import sys

SCALE = float(os.environ.get("FYAI_TIMEOUT_SCALE", "1"))
# A bound on a failure, not a delay: a slow runner can stall for minutes.
DEADLINE = 120.0 * SCALE

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


class Via:
    """A control connection of an agent, other than the primary."""

    def __init__(self, sock):
        self.sock = sock
        self.seq = 0

    def request(self, op, fd=None, **kw):
        self.seq += 1
        data = json.dumps(dict(op=op, seq=self.seq, **kw)).encode()
        if fd is None:
            self.sock.send(data)
        else:
            socket.send_fds(self.sock, [data], [fd])
        return json.loads(self.sock.recv(65536))

    def ok(self, op, fd=None, **kw):
        r = self.request(op, fd, **kw)
        check(r["op"] == "ok", "%s failed: %r" % (op, r))

    def refused(self, op, fd=None, **kw):
        r = self.request(op, fd, **kw)
        check(r["op"] == "error", "%s was accepted: %r" % (op, r))
        return r["message"]


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


def other_agent(tr, exec_id, pid, grant):
    """Admit an execution of another process; return its channel."""
    sock, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    sock.settimeout(DEADLINE)
    tr.ok("admit", theirs.fileno(), id=exec_id, parent=0, pid=pid,
          uid=os.getuid(), grant=grant)
    theirs.close()
    return sock


def connection(tr, exec_id):
    """Give an execution a control connection through the primary."""
    sock, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    sock.settimeout(DEADLINE)
    tr.ok("ctl", theirs.fileno(), id=exec_id)
    theirs.close()
    return Via(sock)


def run_agent_connections(tr, url, procs):
    # Two unrelated executions: 20 and 21.
    a20 = other_agent(tr, 20, procs[0].pid, [{"profile": "main"}])
    a21 = other_agent(tr, 21, procs[1].pid, [{"profile": "main"}])
    tr.refused("ctl", id=99)	# no such execution
    via = connection(tr, 20)
    check("primary" in via.refused("init", level="level-b"), "init from an agent")
    check("primary" in via.refused("shutdown"), "shutdown from an agent")
    check("primary" in via.refused("credential", name="k", value="sk-evil"),
          "credential from an agent")
    check("primary" in via.refused("log", on=True), "log from an agent")

    # Only the primary connection can have a credential sent to a command that
    # the user configured, and the requester never reads it: the socket does.
    check("primary" in via.refused("envgrant", names=["TRANSPORT_TEST_KEY"]),
          "envgrant from an agent")
    tr.refused("envgrant", names=["TRANSPORT_TEST_KEY"])	# no socket
    mine, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    mine.settimeout(DEADLINE)
    tr.ok("envgrant", theirs.fileno(),
          names=["TRANSPORT_TEST_KEY", "NO_SUCH_VARIABLE_XYZ"])
    theirs.close()
    check(mine.recv(65536) == b"TRANSPORT_TEST_KEY=sk-transport-test-0123456789\0",
          "the credential grant")
    mine.close()
    r = tr.request("fetch", names=["TRANSPORT_TEST_KEY"])
    check(r["op"] == "error", "the old fetch op still answers")

    # Only the user session, the primary connection, changes the profiles. An
    # agent can neither add one nor replace one nor replace the set.
    for merge in (True, False):
        check("primary" in via.refused("profiles", merge=merge, profiles=[
            {"name": "extra", "url": url, "auth": "none", "tag": "extra"}]),
            "an agent changed the profiles (merge=%r)" % merge)
    check("primary" in via.refused("profiles", merge=True, profiles=[
        {"name": "main", "url": url + "x", "auth": "none"}]), "redefined url")
    tr.ok("profiles", merge=True, profiles=[
        {"name": "extra", "url": url, "auth": "none", "tag": "extra"}])
    st = tr.request("status")
    check(sorted(st["profiles"]) == ["extra", "main"], "profiles: %r" % st)

    # A grant names profiles of the set. An agent grants nothing to itself and
    # nothing to another branch of the tree, and no more than it holds.
    check("descendant" in via.refused("grant", id=20,
          grant=[{"profile": "main"}, {"profile": "extra"}]), "self grant")
    check("descendant" in via.refused("grant", id=21, grant=[]), "grant of another")
    check("descendant" in via.refused("retire", id=21), "retire of another")
    check("set does not have" in tr.refused("grant", id=21,
          grant=[{"profile": "ghost"}]), "a grant of a profile that is not there")
    b21 = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    r = via.request("admit", b21[1].fileno(), parent=21, pid=procs[3].pid,
                    uid=os.getuid(), grant=[])
    check(r["op"] == "error" and "descendant" in r["message"],
          "admission under another execution: %r" % r)
    r = via.request("ctl", b21[1].fileno(), id=21)
    check(r["op"] == "error" and "descendant" in r["message"],
          "connection for another execution: %r" % r)
    r = via.request("admit", b21[1].fileno(), parent=20, pid=procs[3].pid,
                    uid=os.getuid(), grant=[{"profile": "extra"}])
    check(r["op"] == "error" and "does not hold" in r["message"],
          "a grant beyond the caller's own: %r" % r)
    r = via.request("admit", b21[1].fileno(), parent=20, pid=procs[3].pid,
                    uid=os.getuid(), grant=[{"profile": "ghost"}])
    check(r["op"] == "error" and "set does not have" in r["message"],
          "a grant of a profile that is not there: %r" % r)
    b21[0].close()
    b21[1].close()

    # An admission with no id gets one, and the reply says which.
    a2, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    r = via.request("admit", theirs.fileno(), parent=20, pid=os.getpid(),
                    uid=os.getuid(), grant=[{"profile": "main"}])
    theirs.close()
    check(r["op"] == "ok" and r["id"] >= 2, "auto admission: %r" % r)
    child = Agent.__new__(Agent)
    child.sock, child.id, child.rid = a2, r["id"], 0
    a2.settimeout(DEADLINE)
    # The child of 20 has a connection of its own, for its descendants.
    below = Via(None)
    below.sock, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    below.sock.settimeout(DEADLINE)
    via.ok("ctl", theirs.fileno(), id=r["id"])
    theirs.close()
    check("descendant" in below.refused("grant", id=r["id"],
          grant=[{"profile": "main"}, {"profile": "extra"}]), "a child widened itself")
    check("descendant" in below.refused("grant", id=20, grant=[]),
          "a child changed the grant of its parent")
    check("descendant" in below.refused("retire", id=20), "a child retired its parent")
    check("primary" in below.refused("profiles", merge=True, profiles=[
        {"name": "extra", "url": url, "auth": "none"}]), "a child added a profile")
    b20 = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    r = below.request("admit", b20[1].fileno(), parent=20, pid=procs[3].pid,
                      uid=os.getuid(), grant=[])
    check(r["op"] == "error" and "descendant" in r["message"],
          "a child admitted under its parent: %r" % r)
    b20[0].close()
    b20[1].close()
    # The profile of the supervisor serves the child, and a profile that the
    # child was not granted does not, until the user session grants it.
    start, data, kind, _ = child.call("main", chat("hi"),
                                      content_type="application/json")
    check(kind == END and reply_text(data) == "sixth reply", "main for a child")
    start, data, kind, err = child.call("extra", chat("hi"),
                                        content_type="application/json")
    check(kind == ERROR and err["code"] == -1, "an ungranted profile served a child")
    tr.ok("grant", id=child.id, grant=[{"profile": "main"}, {"profile": "extra"}])
    start, data, kind, _ = child.call("extra", chat("hi"),
                                      content_type="application/json")
    check(kind == END and start["tag"] == "extra", "granted profile unusable")
    check(reply_text(data) == "seventh reply", "wrong seventh reply")
    # A second admission with the same process is refused.
    a3, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    r = via.request("admit", theirs.fileno(), parent=20, pid=os.getpid(),
                    uid=os.getuid(), grant=[])
    check(r["op"] == "error", "the same process was admitted twice")
    a3.close()
    theirs.close()
    # The execution that 20 does not own is intact: the primary still acts on it.
    tr.ok("grant", id=21, grant=[])
    # Its end removes only that connection.
    via.sock.close()
    below.sock.close()
    tr.ok("log", on=False)
    for sock in (a20, a21, a2):
        sock.close()


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

        # The transport says what it enforces: its level, itself, and who is admitted.
        st = tr.request("status")
        check(st["op"] == "ok" and st["level"] == "level-b", "status: %r" % st)
        check(st["pid"] == tr.proc.pid, "status names another process")
        check(sorted(st["profiles"]) == ["main", "side"], "status profiles: %r" % st)
        check([e["id"] for e in st["executions"]] == [7]
              and st["executions"][0]["pid"] == os.getpid(), "status executions")
        check(st["active"] == 0 and st["log"] is False, "status counters: %r" % st)

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

        # A probe says whether a source holds a value, and never what it is.
        tr.ok("credential", name="k", value="sk-mem-0123456789")
        for src, want in (("env:TRANSPORT_TEST_KEY", True),
                          ("env:NO_SUCH_VARIABLE_XYZ", False),
                          ("env:NO_SUCH_VARIABLE_XYZ|mem:k", True),
                          ("env:NO_SUCH_VARIABLE_XYZ|secret:no-such-secret", False),
                          ("mem:absent", False)):
            r = tr.request("probe", credential=src)
            check(r["op"] == "ok" and r["found"] is want, "probe %s: %r" % (src, r))
            check("sk-" not in json.dumps(r), "a probe returned a value")
        tr.refused("probe")
        tr.refused("probe", credential="")

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

        # An agent has a control connection of its own. It can add profiles and
        # admit its children, and it cannot start or stop the transport.
        procs = [subprocess.Popen(["sleep", "1000"]) for _ in range(4)]
        try:
            run_agent_connections(tr, url, procs)
        finally:
            for p in procs:
                p.kill()
                p.wait()

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
            tr.proc.kill()
            tr.proc.wait()

    # A closed control channel ends the transport too.
    tr = Transport(fyai, arena)
    try:
        tr.ok("init", level="level-b", log=False)
        tr.ctl.close()
        check(tr.proc.wait(DEADLINE) == 0, "transport survived its channel")
    finally:
        if tr.proc.poll() is None:
            tr.proc.kill()
            tr.proc.wait()

    # A bad start is refused with a message, not a crash.
    for args in (["transport"], ["transport", "--control-fd", "9"]):
        p = subprocess.run([fyai, "--color", "off"] + args, stdin=subprocess.DEVNULL,
                           capture_output=True, timeout=DEADLINE)
        check(p.returncode != 0, "%r started" % args)
    print("OK")


if __name__ == "__main__":
    # A hang names its step: dump the stack before the case times out.
    faulthandler.dump_traceback_later(240.0 * SCALE, exit=True)
    try:
        main(*sys.argv[1:4])
    except Fail as e:
        print("FAIL: %s" % e)
        sys.exit(1)
