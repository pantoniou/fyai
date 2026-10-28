/* SPDX-License-Identifier: MIT */
#ifndef FYAI_HOST_BACKEND_H
#define FYAI_HOST_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "fyai_host.h"

/*
 * The interface between the host core (fyai_host.c) and the operating system
 * primitives of a platform: processes, descriptors, identity of a peer and the
 * small differences of the C library. A backend lives in one file and exports
 * one table. The core calls the functions of fyai_host.h through the table of
 * the backend that this build selects.
 *
 *   Linux   fyai_host_linux.c   /proc, pidfd, memfd, prctl
 *   macOS   fyai_host_macos.c   dyld, sysctl, arc4random
 *   other   fyai_host_none.c    the portable defaults only
 *
 * A member that is NULL is not supported. The core then fails with ENOTSUP, or
 * uses the portable default that fyai_host.h names.
 */
#if defined(__linux__)
#define FYAI_HOST_BACKEND_LINUX 1
#elif defined(__APPLE__)
#define FYAI_HOST_BACKEND_MACOS 1
#else
#define FYAI_HOST_BACKEND_NONE 1
#endif

/* The system hands the credentials of the sender of a message to the receiver. */
#ifdef FYAI_HOST_BACKEND_LINUX
#define FYAI_HOST_PEER_CREDENTIALS 1
#endif

struct fyai_host_backend {
	const char *name;
	int (*close_range)(unsigned int from, unsigned int to);
	bool (*process_reads_stdin)(pid_t pid);
	size_t (*process_children)(pid_t pid, pid_t *out, size_t max);
	int (*pipe)(int fd[2]);
	int (*exec_self)(const char *const argv[]);
	ssize_t (*executable)(char *path, size_t capacity);
	int (*boot_id)(char id[37]);
	int (*seqpacket_pair)(int fd[2]);
	bool (*stack_bounds)(uintptr_t *low, uintptr_t *high);
	int (*pidfd_open)(pid_t pid);
	int (*pidfd_pid)(int pidfd, pid_t *pid);
	int (*pidfd_alive)(int pidfd);
	int (*in_cgroup)(const char *root, pid_t pid);
	int (*ns_ino)(pid_t pid, int ns, uint64_t *ino);
	int (*channel_prepare)(int channel);
	int (*harden)(const char **step);
	int (*memfd)(const char *name, bool cloexec);
	bool (*mount_root)(const char *path);
	void (*random)(void *buf, size_t size);
	void (*sort)(void *base, size_t count, size_t size,
		     int (*compare)(const void *a, const void *b, void *arg), void *arg);
	int (*openpty)(int *master, int *slave);
	/* The program for opening a URL in the browser of the user. */
	const char *open_url;
	/* The credential isolation of the transport can run: memfd, pidfd and peer credentials. */
	bool isolation;
};

#ifdef FYAI_HOST_BACKEND_LINUX
extern const struct fyai_host_backend fyai_host_backend_linux;
#endif
#ifdef FYAI_HOST_BACKEND_MACOS
extern const struct fyai_host_backend fyai_host_backend_macos;
#endif
#ifdef FYAI_HOST_BACKEND_NONE
extern const struct fyai_host_backend fyai_host_backend_none;
#endif

#endif
