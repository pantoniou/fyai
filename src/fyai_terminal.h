/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TERMINAL_H
#define FYAI_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>

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
#define FYAI_OSC_QUERY_BACKGROUND	"\033]11;?\033\\"

int markdown_render_width(void);
bool terminal_is_tty(int fd);
bool ansi_color_on(const char *color, int fd);
bool markdown_color_enabled(const char *color);
const char *terminal_detect_theme(void);
bool terminal_text_at_line_start(const char *text, size_t len);
size_t terminal_trim_blank_rows(const char *text, size_t len);

#endif
