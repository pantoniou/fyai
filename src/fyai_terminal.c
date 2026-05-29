/*
 * fyai_terminal.c - terminal capabilities and theme detection
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef __APPLE__
#include <fcntl.h>
#include <poll.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#ifndef __APPLE__
#include <termios.h>
#endif
#include <unistd.h>

#include "fyai_terminal.h"

int markdown_render_width(void)
{
	struct winsize ws;
	const char *env;
	int width;

	/*
	 * markdown renderers may fill lines (notably code-block backgrounds) to the
	 * width it is given. Handing it the full terminal width makes those
	 * lines touch the right edge, where the terminal arms a pending
	 * auto-wrap; the trailing newline then lands on the already-
	 * wrapped next line and shows up as a spurious blank line. Reserve one
	 * column so a filled line never reaches the edge.
	 */
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 1)
		return ws.ws_col - 1;
	env = getenv("COLUMNS");
	if (!env || !*env)
		return 0;
	width = atoi(env);
	return width > 0 ? width : 0;
}

bool terminal_is_tty(int fd)
{
	return isatty(fd) == 1;
}

bool ansi_color_on(const char *color, int fd)
{
	if (color && !strcmp(color, "on"))
		return true;
	if (color && !strcmp(color, "off"))
		return false;
	return terminal_is_tty(fd);
}

bool markdown_color_enabled(const char *color)
{
	return ansi_color_on(color, STDOUT_FILENO);
}

#ifndef __APPLE__
static bool osc11_reply_is_light(const char *s)
{
	const char *p;
	unsigned long r = 0, g = 0, b = 0;
	double rf, gf, bf;
	int nr = 0, ng = 0, nb = 0;

	p = strstr(s, "rgb:");
	if (!p)
		return false;
	if (sscanf(p + 4, "%lx%n/%lx%n/%lx%n", &r, &nr, &g, &ng, &b, &nb) != 3)
		return false;
	rf = (double)r / ((1UL << (4 * nr)) - 1);
	gf = (double)g / ((1UL << (4 * (ng - nr - 1))) - 1);
	bf = (double)b / ((1UL << (4 * (nb - ng - 1))) - 1);
	return (0.2126 * rf + 0.7152 * gf + 0.0722 * bf) > 0.5;
}
#endif

const char *terminal_detect_theme(void)
{
	const char *env = getenv("COLORFGBG");
	const char *last;
#ifndef __APPLE__
	const char *result = "dark";
	struct termios old, raw;
	struct pollfd pfd;
	char buf[64];
	size_t off = 0;
	ssize_t n;
	int fd;
#endif
	int bg;

	if (env) {
		last = strrchr(env, ';');
		if (last && sscanf(last + 1, "%d", &bg) == 1)
			return bg >= 0 && bg <= 6 ? "dark" : "light";
	}
#ifdef __APPLE__
	return "dark";
#else
	fd = open("/dev/tty", O_RDWR | O_NOCTTY);
	if (fd < 0)
		return "dark";
	if (!terminal_is_tty(fd) || tcgetattr(fd, &old)) {
		close(fd);
		return "dark";
	}
	raw = old;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	tcsetattr(fd, TCSANOW, &raw);
	(void)!write(fd, FYAI_OSC_QUERY_BACKGROUND,
		     sizeof(FYAI_OSC_QUERY_BACKGROUND) - 1);
	pfd.fd = fd;
	pfd.events = POLLIN;
	while (off < sizeof(buf) - 1 && poll(&pfd, 1, 500) > 0) {
		n = read(fd, buf + off, sizeof(buf) - 1 - off);
		if (n <= 0)
			break;
		off += (size_t)n;
		buf[off] = '\0';
		if (strstr(buf, "rgb:") &&
		    (memchr(buf, '\\', off) || memchr(buf, '\a', off)))
			break;
	}
	buf[off] = '\0';
	tcsetattr(fd, TCSANOW, &old);
	close(fd);
	if (strstr(buf, "rgb:"))
		result = osc11_reply_is_light(buf) ? "light" : "dark";
	return result;
#endif
}
