/*
 * fyai_vterm_test.c - smoke test for libfyvterm
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 *
 * Proves libfyvterm is found, links, and decodes plain text and SGR
 * attributes correctly. The terminal rendering oracles that CLAUDE.md
 * describes build on this same library.
 */

#include <string.h>

#include <libfyvterm.h>

#include "fyai_test.h"
#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(vterm, plain_text, vterm_plain_text)
FYAI_TEST_ENTRY(vterm, sgr_bold, vterm_sgr_bold)
FYAI_TEST_ENTRY(vterm, sgr_dim_overline, vterm_sgr_dim_overline)
FYAI_TEST_ENTRY(vterm, da1_response, vterm_da1_response)
FYAI_TEST_ENTRY(vterm, kitty_disambiguate, vterm_kitty_disambiguate)
FYAI_TEST_ENTRY(vterm, sync_output_mode, vterm_sync_output_mode)

/* libfyvterm creates over a configuration struct; every case here wants
 * the same plain 24x80 terminal.
 *
 * A terminal that is obtained is not yet sized: the reset gives the state its
 * scroll region. A case that drives the state directly must reset it, as a
 * case that takes the screen resets that. Without the reset the region stays
 * zero high, which a debug build of the library stops on. */
static struct fyvt *vterm_open(void)
{
	struct fyvt_cfg cfg;

	fyvt_cfg_default(&cfg);
	cfg.rows = 24;
	cfg.cols = 80;
	return fyvt_create(&cfg);
}

/* Captures bytes libfyvterm pushes back at the "terminal" (replies, key output)
 * so a test can assert on them. */
struct capture {
	char buf[64];
	size_t len;
};

static void capture_output(const char *s, size_t len, void *user)
{
	struct capture *cap = user;

	FYAI_TCHECK(cap->len + len < sizeof(cap->buf));
	memcpy(cap->buf + cap->len, s, len);
	cap->len += len;
	cap->buf[cap->len] = '\0';
}

int vterm_plain_text(void)
{
	struct fyvt *vt;
	struct fyvt_screen *screen;
	struct fyvt_screen_cell cell;
	struct fyvt_pos pos;
	static const char bytes[] = "hi";

	vt = vterm_open();
	FYAI_TCHECK(vt != NULL);
	screen = fyvt_obtain_screen(vt);
	fyvt_screen_reset(screen, 1);

	FYAI_TCHECK(fyvt_input_write(vt, bytes, strlen(bytes)) == strlen(bytes));

	pos.row = 0;
	pos.col = 0;
	FYAI_TCHECK(fyvt_screen_get_cell(screen, pos, &cell));
	FYAI_TCHECK(cell.chars[0] == 'h');

	pos.col = 1;
	FYAI_TCHECK(fyvt_screen_get_cell(screen, pos, &cell));
	FYAI_TCHECK(cell.chars[0] == 'i');

	pos.col = 2;
	FYAI_TCHECK(fyvt_screen_get_cell(screen, pos, &cell));
	FYAI_TCHECK(cell.chars[0] == 0);

	fyvt_destroy(vt);
	return 0;
}

/* libfyvterm carries Neovim's SGR 2 (dim) and 53 (overline). */
int vterm_sgr_dim_overline(void)
{
	struct fyvt *vt;
	struct fyvt_screen *screen;
	struct fyvt_screen_cell cell;
	struct fyvt_pos pos;
	static const char bytes[] = "\x1b[2;53mD";

	vt = vterm_open();
	FYAI_TCHECK(vt != NULL);
	screen = fyvt_obtain_screen(vt);
	fyvt_screen_reset(screen, 1);

	FYAI_TCHECK(fyvt_input_write(vt, bytes, strlen(bytes)) == strlen(bytes));

	pos.row = 0;
	pos.col = 0;
	FYAI_TCHECK(fyvt_screen_get_cell(screen, pos, &cell));
	FYAI_TCHECK(cell.chars[0] == 'D');
	FYAI_TCHECK(cell.attrs.dim == 1);
	FYAI_TCHECK(cell.attrs.overline == 1);

	fyvt_destroy(vt);
	return 0;
}

/* libfyvterm carries Neovim's modernized DA1 (device attributes) reply:
 * "?61;22;52c" and not "?1;2c". */
int vterm_da1_response(void)
{
	struct fyvt *vt;
	struct fyvt_state *state;
	struct capture cap = { .len = 0 };
	static const char bytes[] = "\x1b[c";

	vt = vterm_open();
	FYAI_TCHECK(vt != NULL);
	state = fyvt_obtain_state(vt);
	FYAI_TCHECK(state != NULL);
	fyvt_state_reset(state, 1);
	fyvt_output_set_callback(vt, capture_output, &cap);

	FYAI_TCHECK(fyvt_input_write(vt, bytes, strlen(bytes)) == strlen(bytes));
	FYAI_TCHECK(strcmp(cap.buf, "\x1b[?61;22;52c") == 0);

	fyvt_destroy(vt);
	return 0;
}

/* libfyvterm carries Neovim's kitty keyboard protocol. CSI > 1 u
 * pushes "disambiguate" mode; a subsequent Ctrl+a must then encode as
 * CSI 97;5u instead of the legacy 0x01 control byte. */
int vterm_kitty_disambiguate(void)
{
	struct fyvt *vt;
	struct fyvt_state *state;
	struct capture cap = { .len = 0 };
	static const char enable[] = "\x1b[>1u";

	vt = vterm_open();
	FYAI_TCHECK(vt != NULL);
	state = fyvt_obtain_state(vt);
	FYAI_TCHECK(state != NULL);
	fyvt_state_reset(state, 1);
	fyvt_output_set_callback(vt, capture_output, &cap);

	FYAI_TCHECK(fyvt_input_write(vt, enable, strlen(enable)) == strlen(enable));

	fyvt_keyboard_unichar(vt, 'a', FYVT_MOD_CTRL);
	FYAI_TCHECK(strcmp(cap.buf, "\x1b[97;5u") == 0);

	fyvt_destroy(vt);
	return 0;
}

/* libfyvterm carries Neovim's DEC mode 2026 (synchronized output). */
int vterm_sync_output_mode(void)
{
	struct fyvt *vt;
	struct fyvt_state *state;
	struct capture cap = { .len = 0 };
	static const char enable[] = "\x1b[?2026h";
	static const char query[] = "\x1b[?2026$p";

	vt = vterm_open();
	FYAI_TCHECK(vt != NULL);
	state = fyvt_obtain_state(vt);
	FYAI_TCHECK(state != NULL);
	fyvt_state_reset(state, 1);
	fyvt_output_set_callback(vt, capture_output, &cap);

	FYAI_TCHECK(fyvt_input_write(vt, enable, strlen(enable)) == strlen(enable));
	FYAI_TCHECK(fyvt_input_write(vt, query, strlen(query)) == strlen(query));
	FYAI_TCHECK(strcmp(cap.buf, "\x1b[?2026;1$y") == 0);

	fyvt_destroy(vt);
	return 0;
}

int vterm_sgr_bold(void)
{
	struct fyvt *vt;
	struct fyvt_screen *screen;
	struct fyvt_screen_cell cell;
	struct fyvt_pos pos;
	static const char bytes[] = "\x1b[1mB\x1b[0mN";

	vt = vterm_open();
	FYAI_TCHECK(vt != NULL);
	screen = fyvt_obtain_screen(vt);
	fyvt_screen_reset(screen, 1);

	FYAI_TCHECK(fyvt_input_write(vt, bytes, strlen(bytes)) == strlen(bytes));

	pos.row = 0;
	pos.col = 0;
	FYAI_TCHECK(fyvt_screen_get_cell(screen, pos, &cell));
	FYAI_TCHECK(cell.chars[0] == 'B');
	FYAI_TCHECK(cell.attrs.bold == 1);

	pos.col = 1;
	FYAI_TCHECK(fyvt_screen_get_cell(screen, pos, &cell));
	FYAI_TCHECK(cell.chars[0] == 'N');
	FYAI_TCHECK(cell.attrs.bold == 0);

	fyvt_destroy(vt);
	return 0;
}
