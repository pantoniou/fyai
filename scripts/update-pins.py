#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Move the pinned revision of each libfy* dependency to the tip of its
upstream master branch.

Pins are in CMakeLists.txt, .github/workflows/release.yaml,
docker/Dockerfile.static and the static build scripts. The script edits the
files only. It does not commit.
"""

import argparse
import os
import re
import subprocess
import sys

DEPS = ("libfyaml", "libfypalette", "libfymd4c", "libfymermaid",
        "libfytimui", "libfyvterm")
FILES = ("CMakeLists.txt", ".github/workflows/release.yaml",
         "docker/Dockerfile.static", "scripts/build-static-native.sh",
         "scripts/build-static-asan.sh")
BASE_URL = os.environ.get("FYAI_PIN_BASE_URL", "https://github.com/pantoniou")
SHA = r"[0-9a-f]{40}"


def pin_patterns(dep):
    """Return the regexes that match a pin of @dep. Group 1 is the text
    before the revision and group 2 is the revision."""
    name = re.escape(dep)
    return [
        # fyai_fy_dep(libfyfoo <sha>)
        re.compile(r"(fyai_fy_dep\(%s\s+)(%s)" % (name, SHA)),
        # FetchContent_Declare(libfyfoo GIT_REPOSITORY <url> GIT_TAG <sha>)
        re.compile(r"(FetchContent_Declare\(%s\s+GIT_REPOSITORY\s+\S+\s+"
                   r"GIT_TAG\s+)(%s)" % (name, SHA)),
        # git -C /tmp/libfyfoo checkout <sha>
        re.compile(r"(git -C \S*/%s checkout\s+)(%s)" % (name, SHA)),
        # LIBFYFOO_REF=<sha>, LIBFYFOO_REF: <sha> and ${LIBFYFOO_REF:-<sha>}
        re.compile(r"(\b%s_REF(?::-|[:=])\s*)(%s)" % (name.upper(), SHA)),
    ]


def remote_tip(dep, branch):
    url = "%s/%s" % (BASE_URL, dep)
    try:
        out = subprocess.run(["git", "ls-remote", url,
                              "refs/heads/" + branch],
                             check=True, capture_output=True,
                             text=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        sys.exit("%s: %s: cannot query %s: %s" %
                 (sys.argv[0], dep, url, getattr(e, "stderr", e)))
    if not out.strip():
        sys.exit("%s: %s: cannot resolve refs/heads/%s" %
                 (sys.argv[0], dep, branch))
    return out.split()[0]


def rewrite(dep, sha, text):
    """Return the text with every pin of @dep set to @sha, and the list of
    revisions that were replaced."""
    old = []

    def sub(m):
        if m.group(2) != sha and m.group(2) not in old:
            old.append(m.group(2))
        return m.group(1) + sha

    for rx in pin_patterns(dep):
        text = rx.sub(sub, text)
    return text, old


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-n", "--dry-run", action="store_true",
                    help="report the changes and write nothing")
    ap.add_argument("-b", "--branch", default="master",
                    help="track BRANCH instead of master")
    ap.add_argument("deps", nargs="*", metavar="DEP",
                    help="limit the update to the named dependencies")
    args = ap.parse_args()

    for dep in args.deps:
        if dep not in DEPS:
            ap.error("unknown dependency: " + dep)
    deps = args.deps or DEPS

    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

    changed = False
    for dep in deps:
        sha = remote_tip(dep, args.branch)
        for path in FILES:
            if not os.path.isfile(path):
                continue
            with open(path) as f:
                text = f.read()
            new, old = rewrite(dep, sha, text)
            if not old:
                continue
            for rev in old:
                print("%-13s %s: %.12s -> %.12s" % (dep, path, rev, sha))
            changed = True
            if not args.dry_run:
                with open(path, "w") as f:
                    f.write(new)

    if not changed:
        print("all pins are current")
    elif args.dry_run:
        print("(dry run: no file written)")


if __name__ == "__main__":
    main()
