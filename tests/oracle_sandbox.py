# SPDX-License-Identifier: MIT
"""Keep and restore the sandbox of an oracle case.

    oracle_sandbox.py keep SANDBOX KEEP
    oracle_sandbox.py restore SANDBOX KEEP

`keep` copies each entry of SANDBOX, except the directory of KEEP, to KEEP.
`restore` removes each entry of SANDBOX, except the directory of KEEP, and
copies the kept entries back. The second run of the case then starts from the
same files, at the same paths, as the first run did.
"""
import os
import shutil
import sys


def entries(sandbox, keep):
    top = os.path.relpath(keep, sandbox).split(os.sep)[0]
    return [e for e in sorted(os.listdir(sandbox)) if e != top]


def copy(src, dst):
    if os.path.isdir(src) and not os.path.islink(src):
        shutil.copytree(src, dst, symlinks=True)
    else:
        shutil.copy2(src, dst, follow_symlinks=False)


def remove(path):
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path)
    else:
        os.unlink(path)


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ('keep', 'restore'):
        sys.exit(__doc__)
    op, sandbox, keep = sys.argv[1:]
    if op == 'keep':
        os.makedirs(keep)
        for e in entries(sandbox, keep):
            copy(os.path.join(sandbox, e), os.path.join(keep, e))
        return
    for e in entries(sandbox, keep):
        remove(os.path.join(sandbox, e))
    for e in sorted(os.listdir(keep)):
        copy(os.path.join(keep, e), os.path.join(sandbox, e))


if __name__ == '__main__':
    main()
