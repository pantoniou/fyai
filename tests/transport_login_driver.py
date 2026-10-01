#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The ChatGPT login at the transport: `fyai transport` reads the store.

Usage: transport_login_driver.py FYAI_BIN MOCK_URL ARENA_DIR

The case runs with a private XDG_STATE_HOME. The driver writes a login store
there, names it as the credential source of a profile, and checks that the
provider receives the token while the agent end of the
channel never carries one.
"""

import base64
import json
import os
import sys
import time

from transport_driver import Agent, END, Fail, Transport, chat, check, connection, reply_text


def enc(value):
    return base64.urlsafe_b64encode(json.dumps(value).encode()).rstrip(b"=").decode()


def write_store(expires_in, refresh="secret-refresh"):
    path = os.path.join(os.environ["XDG_STATE_HOME"], "fyai", "auth.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    jwt = ".".join((enc({"alg": "none"}), enc({
        "email": "user@example.com",
        "exp": int(time.time()) + expires_in,
        "https://api.openai.com/auth": {
            "chatgpt_account_id": "account-test",
            "chatgpt_plan_type": "pro"}}), enc("signature")))
    with open(path, "w") as f:
        json.dump({"type": "chatgpt", "client_id": "oaiapp_test",
                   "ext_agent_host_id": "urn:uuid:00000000-0000-4000-8000-000000000000",
                   "subject": "user-test",
                   "registrations": {"oaiapp_test": {
                       "email": "user@example.com", "subject": "user-test"}},
                   "scope": "openid offline_access resource.invoke "
                            "chatgpt.tokens.use.direct",
                   "access_token": "secret-access",
                   "refresh_token": refresh, "id_token": jwt,
                   "expires_at": int(time.time()) + expires_in}, f)
    os.chmod(path, 0o600)


def main(fyai, mock_url, arena):
    url = mock_url + "/v1/chat/completions"
    tr = Transport(fyai, arena)
    try:
        tr.ok("init", level="level-b", log=False)

        # No login: the source holds nothing.
        r = tr.request("probe", credential="oauth:chatgpt")
        check(r["op"] == "ok" and r["found"] is False, "probe without a login: %r" % r)
        r = tr.request("probe", credential="oauth:other")
        check(r["op"] == "ok" and r["found"] is False, "probe of an unknown login")

        write_store(3600)
        r = tr.request("probe", credential="oauth:chatgpt")
        check(r["op"] == "ok" and r["found"] is True, "probe with a login: %r" % r)
        check("secret-" not in json.dumps(r), "a probe returned a token")

        tr.ok("profiles", profiles=[
            {"name": "main", "url": url, "auth": "bearer", "tag": "chat",
             "credential": "oauth:chatgpt"}])
        agent = Agent(tr, 7, [{"profile": "main"}])
        # The profiles and the grant of an execution, with a source name and no value.
        tr.ok("profiles", profiles=[
            {"name": "main", "url": url, "auth": "bearer", "tag": "chat",
             "credential": "oauth:chatgpt"},
            {"name": "other", "url": url, "auth": "none"}], merge=True)
        r = tr.request("describe", id=7)
        check(r["op"] == "ok" and r["id"] == 7, "describe: %r" % r)
        rows = {p["name"]: p for p in r["profiles"]}
        check(rows["main"]["granted"] is True and rows["other"]["granted"] is False,
              "describe grants: %r" % rows)
        check(rows["main"]["credential"] == "oauth:chatgpt", "no source name")
        check("secret-" not in json.dumps(r), "describe returned a token")
        tr.refused("describe", id=99)
        tr.refused("describe")		# the default is execution 1, which is absent

        start, data, kind, _ = agent.call("main", chat("hi"),
                                          content_type="application/json")
        check(kind == END and start["status"] == 200, "request: %r" % (start,))
        check(reply_text(data) == "first reply", "wrong reply")
        seen = [json.loads(l) for l in open(os.path.join(
            os.path.dirname(os.path.dirname(arena)), "requests.jsonl"))]
        check(seen[-1]["auth"] == "Bearer secret-access",
              "the provider saw %r" % seen[-1]["auth"])
        check("secret-access" not in json.dumps(start), "the start frame has a token")

        # The commands of the login run here, on the primary connection, and
        # their results hold no token.
        r = tr.request("cmd", words=["auth", "openai", "status"])
        check(r["op"] == "ok" and r["data"]["status"] == "signed_in",
              "status: %r" % r)
        check(r["data"]["client_id"] == "oaiapp_test", "status client: %r" % r)
        r = tr.request("cmd", words=["auth", "openai", "info"], format=1)
        check(r["op"] == "ok" and "storage" in r["data"], "info: %r" % r)
        r = tr.request("cmd", words=["auth", "openai", "accounts"])
        check(r["op"] == "ok" and r["data"][0]["client_id"] == "oaiapp_test",
              "accounts: %r" % r)
        check("secret-" not in json.dumps(r), "accounts returned a token")
        for words in (["auth", "openai", "login"], ["branch", "list"],
                      ["auth", "openai", "status", "--bogus"], [], ["auth"] * 9):
            r = tr.request("cmd", words=words)
            check(r["op"] == "error", "%r ran at the transport: %r" % (words, r))
        via = connection(tr, 7)
        check("primary" in via.refused("cmd", words=["auth", "openai", "status"]),
              "a command from an agent")

        # Logout clears the tokens of the store. The record has no refresh
        # token, so the case sends no revocation.
        write_store(3600, refresh="")
        r = tr.request("cmd", words=["auth", "openai", "logout"])
        check(r["op"] == "ok", "logout: %r" % r)
        with open(os.path.join(os.environ["XDG_STATE_HOME"], "fyai", "auth.json")) as f:
            check(json.load(f)["access_token"] == "", "logout kept the access token")

        # Logged out between requests: the next one is refused, not sent bare.
        os.unlink(os.path.join(os.environ["XDG_STATE_HOME"], "fyai", "auth.json"))
        start, data, kind, err = agent.call("main", chat("hi"),
                                            content_type="application/json")
        check(kind != END and start is None, "a request went out with no login")
        tr.ok("shutdown")
        check(tr.proc.wait(30) == 0, "transport did not exit cleanly")
    finally:
        if tr.proc.poll() is None:
            tr.proc.kill()
            tr.proc.wait()
    print("OK")


if __name__ == "__main__":
    try:
        main(*sys.argv[1:4])
    except Fail as e:
        print("FAIL: %s" % e)
        sys.exit(1)
