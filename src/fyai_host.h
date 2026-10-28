/* SPDX-License-Identifier: MIT */
#ifndef FYAI_HOST_H
#define FYAI_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * Operating system primitives that differ between platforms. The core is
 * fyai_host.c; the platform code is in the backends of fyai_host_backend.h.
 * Callers test a capability, never the name of a platform.
 */

/* Close the descriptors from @from to @to, both included, in the kernel. 0, or -1 when it cannot. */
int fyai_host_close_range(unsigned int from, unsigned int to);

/* The process blocked reading descriptor 0, or the children of a process. Else false and 0. */
bool fyai_host_process_reads_stdin(pid_t pid);
size_t fyai_host_process_children(pid_t pid, pid_t *out, size_t max);

/* Make a pipe whose ends close on exec. */
int fyai_host_pipe(int fd[2]);

/* Make a connected pair of sequenced-packet sockets that close on exec. ENOTSUP when there are none. */
int fyai_host_seqpacket_pair(int fd[2]);

/* Whether this program can execute itself again, and do it. */
bool fyai_host_exec_self_available(void);
int fyai_host_exec_self(const char *const argv[]);

/*
 * Write the absolute path of the running program to @path, which holds
 * @capacity bytes, with no terminating NUL guarantee. Returns the length, or -1
 * with errno set.
 */
ssize_t fyai_host_executable(char *path, size_t capacity);

/* Write the identity of this boot of the machine, 36 characters, and a NUL. */
int fyai_host_boot_id(char id[37]);

/* The bounds of the stack of the main thread. False when the system does not say. */
bool fyai_host_stack_bounds(uintptr_t *low, uintptr_t *high);

/*
 * Identity of a peer process. Each returns 0 or a file descriptor, or a negative
 * errno; -ENOSYS when the system has none. The pidfd names a process in every
 * namespace. pidfd_pid gives its PID here, in_cgroup tells whether @pid is in
 * the subtree @root of the cgroup, and ns_ino gives the inode of a namespace of
 * @pid (0 for this process). @ns is an enum fyai_transport_ns.
 */
int fyai_host_pidfd_open(pid_t pid);
int fyai_host_pidfd_pid(int pidfd, pid_t *pid);
int fyai_host_pidfd_alive(int pidfd);
int fyai_host_in_cgroup(const char *root, pid_t pid);
int fyai_host_ns_ino(pid_t pid, int ns, uint64_t *ino);
/* Ask the kernel to attach the credentials of the sender to each message of @channel. */
int fyai_host_channel_prepare(int channel);

/* The transport runs here: it needs the peer credentials of the system. */
bool fyai_host_isolation_available(void);

/*
 * Make this process unreadable to a process of the same user, with no core dump
 * and no new privileges, and end it with its parent. Returns 0. On failure
 * returns -1 with errno set and *@step naming what failed; *@step is NULL when
 * the parent is already gone.
 */
int fyai_host_harden(const char **step);

/* An anonymous file in memory, close-on-exec. Whether there is one, and make it. */
bool fyai_host_memfd_available(void);
int fyai_host_memfd(const char *name, bool cloexec);

/* True when @path is a mount root, or when the system cannot say; false on a system with no such mounts. */
bool fyai_host_mount_root(const char *path);

/* Fill @buf with @size bytes from the entropy source of the system. */
void fyai_host_random(void *buf, size_t size);

/* Sort @count items of @size bytes. The comparator takes @arg last, as GNU qsort_r does. */
void fyai_host_sort(void *base, size_t count, size_t size,
		    int (*compare)(const void *a, const void *b, void *arg), void *arg);

/* Open a pseudo-terminal: the master and the slave descriptor. Returns 0, or -1 with errno set. */
int fyai_host_openpty(int *master, int *slave);

/* The program that opens a URL in the browser of the user. */
const char *fyai_host_open_url_program(void);

#endif
