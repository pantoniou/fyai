/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai_test.h"
#include "fyai_test_registry.h"
#include "utils.h"

FYAI_TEST_ENTRY(utils, wire_text_utf8, utils_wire_text_utf8)
FYAI_TEST_ENTRY(utils, close_fds_except, utils_close_fds_except)
FYAI_TEST_ENTRY(utils, utf8_length, utils_utf8_length)

int utils_wire_text_utf8(void)
{
	static const char valid[] = "plain\n\xe6\x97\xa5 e\xcc\x81";
	static const unsigned char invalid[][4] = {
		{ 0xc0, 0x80 },		/* overlong NUL */
		{ 0xe0, 0x80, 0x80 },	/* overlong three-byte value */
		{ 0xed, 0xa0, 0x80 },	/* UTF-16 surrogate */
		{ 0xf4, 0x90, 0x80, 0x80 }, /* above U+10FFFF */
		{ 0xf5, 0x80, 0x80, 0x80 }, /* invalid lead byte */
	};
	static const size_t lengths[] = { 2, 3, 3, 4, 4 };
	size_t i;

	FYAI_TCHECK(data_is_wire_text(valid, strlen(valid)));
	FYAI_TCHECK(!data_is_wire_text("\033[2J", 4));
	for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		FYAI_TCHECK(!data_is_wire_text((const char *)invalid[i],
					       lengths[i]));
	return 0;
}

int utils_close_fds_except(void)
{
	pid_t child;
	int status;

	child = fork();
	FYAI_TCHECK(child >= 0);
	if (!child) {
		int first, keep, last;

		first = open("/dev/null", O_RDONLY);
		if (first < 0)
			_exit(1);
		keep = fcntl(first, F_DUPFD_CLOEXEC, first + 16);
		last = keep >= 0 ? fcntl(first, F_DUPFD_CLOEXEC, keep + 1) : -1;
		if (last < 0)
			_exit(1);
		fyai_close_fds_except(first, keep);
		if (fcntl(first, F_GETFD) >= 0 || fcntl(last, F_GETFD) >= 0 ||
		    fcntl(keep, F_GETFD) < 0 || fcntl(STDERR_FILENO, F_GETFD) < 0)
			_exit(1);
		fyai_close_fds_except(first, -1);
		_exit(fcntl(keep, F_GETFD) < 0 ? 0 : 1);
	}
	while (waitpid(child, &status, 0) < 0) {
		FYAI_TCHECK(errno == EINTR);
	}
	FYAI_TCHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	return 0;
}

int utils_utf8_length(void)
{
	FYAI_TCHECK(fyai_utf8_length("") == 0);
	FYAI_TCHECK(fyai_utf8_length("abc") == 3);
	/* Two, three and four byte sequences count once. */
	FYAI_TCHECK(fyai_utf8_length("\xc3\xa9") == 1);
	FYAI_TCHECK(fyai_utf8_length("a\xe2\x86\x92" "b") == 3);
	FYAI_TCHECK(fyai_utf8_length("\xf0\x9f\x98\x80") == 1);
	return 0;
}
