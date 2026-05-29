/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TERMINAL_H
#define FYAI_TERMINAL_H

#include <stdbool.h>

#define FYAI_ANSI_RESET			"\033[0m"
#define FYAI_ANSI_BOLD			"\033[1m"
#define FYAI_ANSI_DIM			"\033[2m"
#define FYAI_ANSI_GREEN			"\033[32m"
#define FYAI_ANSI_CYAN			"\033[36m"
#define FYAI_ANSI_BRIGHT_YELLOW		"\033[1;33m"
#define FYAI_ANSI_ERASE_LINE		"\r\033[K"
#define FYAI_ANSI_ERASE_DOWN		"\r\033[J"
#define FYAI_ANSI_CURSOR_UP		"\033[1A"
#define FYAI_ANSI_CURSOR_UP_FMT		"\033[%zuA"
#define FYAI_ANSI_BG_LIGHT		"\033[47m"	/* white */
#define FYAI_ANSI_BG_DARK		"\033[40m"	/* black */
#define FYAI_ANSI_FG_LIGHT		"\033[37m"	/* white */
#define FYAI_ANSI_FG_DARK		"\033[30m"	/* black */
#define FYAI_ANSI_BOLD			"\033[1m"
#define FYAI_ANSI_BOLD_OFF		"\033[2m"
#define FYAI_OSC_QUERY_BACKGROUND	"\033]11;?\033\\"

int markdown_render_width(void);
bool terminal_is_tty(int fd);
bool ansi_color_on(const char *color, int fd);
bool markdown_color_enabled(const char *color);
const char *terminal_detect_theme(void);

#endif
