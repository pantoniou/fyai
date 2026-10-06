/*
 * fyai_child_test.c - unit tests for the descriptors of a child before exec
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"
#include "utils.h"

FYAI_TEST_ENTRY(child, pass_status_number, child_pass_status_number)

/*
 * The status descriptor takes number 3. A passed descriptor that is number 3 in
 * the child must still be copied from the socket, and not from the status pipe.
 */
int child_pass_status_number(void)
{
	struct fyai_ctx ctx;
	struct fyai_child_spec spec;
	struct stat st;
	int sv[2], pipefd[2], status = 0;
	pid_t pid;

	memset(&ctx, 0, sizeof(ctx));
	FYAI_TCHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
	FYAI_TCHECK(!pipe(pipefd));
	pid = fork();
	FYAI_TCHECK(pid >= 0);
	if (!pid) {
		memset(&spec, 0, sizeof(spec));
		spec.in_fd = spec.out_fd = spec.err_fd = spec.ctty_fd = -1;
		spec.inherit_env = true;
		/* The status descriptor is out of the way, then a socket takes number 3. */
		if (dup2(pipefd[1], 20) < 0 || dup2(sv[0], 3) < 0)
			_exit(101);
		spec.status_fd = 20;
		spec.pass_fd[0] = 3;
		spec.pass_as[0] = 6;
		spec.pass_n = 1;
		if (fyai_child_exec_prepare(&ctx, &spec))
			_exit(102);
		if (fstat(6, &st) || !S_ISSOCK(st.st_mode))
			_exit(103);
		if (fstat(3, &st) || !S_ISFIFO(st.st_mode))
			_exit(104);
		_exit(0);
	}
	close(sv[0]);
	close(sv[1]);
	close(pipefd[0]);
	close(pipefd[1]);
	FYAI_TCHECK(waitpid(pid, &status, 0) == pid);
	FYAI_TCHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	return 0;
}
