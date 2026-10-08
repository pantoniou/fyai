# Seccomp network filter prototype

This prototype supplemented Landlock TCP port rules on kernels without
Landlock UDP rights. It is saved here for review. It is not active code.

The filter checked the syscall architecture, rejected x32 and legacy
`socketcall` requests, and allowed new IP sockets only for TCP streams.
It left Unix and netlink sockets available. The caller installed it after
`PR_SET_NO_NEW_PRIVS` and `landlock_restrict_self()` when `restrict_net` was set.

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <asm/unistd.h>

/* Landlock controls TCP ports; deny other network socket types. */
static int restrict_non_tcp_sockets(void)
{
#if defined(__x86_64__)
#define FYAI_SECCOMP_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define FYAI_SECCOMP_ARCH AUDIT_ARCH_AARCH64
#else
	errno = ENOTSUP;
	return -1;
#endif
#if defined(FYAI_SECCOMP_ARCH) && defined(__NR_socket)
	struct sock_filter filter[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, FYAI_SECCOMP_ARCH, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
#if defined(__x86_64__)
		BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, __X32_SYSCALL_BIT, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
#ifdef __NR_socketcall
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socketcall, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 13),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 11, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_NETLINK, 10, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET, 2, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET6, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1])),
		BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0xf),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SOCK_STREAM, 0, 3),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[2])),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 2, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, IPPROTO_TCP, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {
		.len = sizeof(filter) / sizeof(filter[0]),
		.filter = filter,
	};

	return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
#else
	errno = ENOTSUP;
	return -1;
#endif
}
```

The filter does not close inherited sockets. A reachable Unix socket can
still act as a network proxy. The numeric BPF jumps need review before any
reuse, and the code supports only x86-64 and AArch64. Landlock still provides
the TCP port policy. Landlock ABI 10 adds UDP bind and connect/send rights,
which are the active approach for UDP confinement.
