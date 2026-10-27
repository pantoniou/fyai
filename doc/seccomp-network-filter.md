# Seccomp UDP filter

Landlock ABI 10 adds UDP rights. A kernel below ABI 10 cannot restrict UDP
with Landlock. For that case, fyai installs a seccomp filter that denies UDP
sockets whole.

## When it applies

- The profile has a `network.udp` mapping that permits no destination port
  and no bind port.
- The kernel has no Landlock ABI 10.
- The build supports the architecture (x86-64 and AArch64).

A profile that lists a UDP port is not filtered. A seccomp filter sees only
the syscall number and the argument registers. The port is in a `sockaddr`
that `connect()` and `sendto()` receive by pointer, and a filter cannot read
it. Such a profile keeps UDP open on a kernel below ABI 10, and fyai warns.

## The filter

The filter is classic BPF. A seccomp filter cannot be eBPF. It allows every
syscall except these:

- any syscall of another architecture, and of the x32 ABI;
- `io_uring_setup()`, because a ring makes sockets without `socket()`; and
- `socket()` with an `AF_INET` or `AF_INET6` domain and a `SOCK_DGRAM` or
  `SOCK_RAW` type. A raw socket can build a UDP packet by hand.

Unix and netlink sockets, and IP stream sockets, stay available. The tool
installs the filter after `PR_SET_NO_NEW_PRIVS` and `landlock_restrict_self()`,
in the child, before `exec`. The filter applies to every descendant and no
process can remove it.

## Limits

- A socket that the process inherited is not closed.
- A reachable Unix socket can still act as a proxy for a process outside the
  sandbox.
- A process outside the sandbox keeps its own sockets. Yama `ptrace_scope`
  stops a tool from taking one with `pidfd_getfd()` on most systems.

## The build

`tools/fyai_mkseccomp.c` assembles the program from labeled instructions and
writes `embedded_seccomp_udp.inc`. It resolves each jump label to a relative
offset and fails the build for a missing label, a backward jump or an offset
above 255. The note of each instruction is a comment in the output. Each
operand is a C expression that the target compiler evaluates, so the file does
not depend on the build host. A cross build has no filter and fyai warns that
UDP stays open.
