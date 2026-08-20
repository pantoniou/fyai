/*
 * fyai_vterm_test.c - smoke test for the vendored libvterm
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 *
 * Proves the vendored copy under third_party/libvterm builds, links, and
 * decodes plain text and one SGR attribute correctly. The terminal
 * rendering oracles that CLAUDE.md describes build on this same library;
 * this test only guards the vendoring itself.
 */

#include <string.h>

#include <vterm.h>

#include "fyai_test.h"
#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(vterm, plain_text, vterm_plain_text)
FYAI_TEST_ENTRY(vterm, sgr_bold, vterm_sgr_bold)
FYAI_TEST_ENTRY(vterm, sgr_dim_overline, vterm_sgr_dim_overline)
FYAI_TEST_ENTRY(vterm, da1_response, vterm_da1_response)
FYAI_TEST_ENTRY(vterm, kitty_disambiguate, vterm_kitty_disambiguate)
FYAI_TEST_ENTRY(vterm, sync_output_mode, vterm_sync_output_mode)

/* Captures bytes vterm pushes back at the "terminal" (replies, key output)
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
	VTerm *vt;
	VTermScreen *screen;
	VTermScreenCell cell;
	VTermPos pos;
	static const char bytes[] = "hi";

	vt = vterm_new(24, 80);
	FYAI_TCHECK(vt != NULL);
	screen = vterm_obtain_screen(vt);
	vterm_screen_reset(screen, 1);

	FYAI_TCHECK(vterm_input_write(vt, bytes, strlen(bytes)) == strlen(bytes));

	pos.row = 0;
	pos.col = 0;
	FYAI_TCHECK(vterm_screen_get_cell(screen, pos, &cell));
	FYAI_TCHECK(cell.chars[0] == 'h');

	pos.col = 1;
	FYAI_TCHECK(vterm_screen_get_cell(screen, pos, &cell));
	FYAI_TCHECK(cell.chars[0] == 'i');

	pos.col = 2;
	FYAI_TCHECK(vterm_screen_get_cell(screen, pos, &cell));
	FYAI_TCHECK(cell.chars[0] == 0);

	vterm_free(vt);
	return 0;
}

/* Neovim commit 2368a9edbd added SGR 2 (dim) and 53 (overline). */
int vterm_sgr_dim_overline(void)
{
	VTerm *vt;
	VTermScreen *screen;
	VTermScreenCell cell;
	VTermPos pos;
	static const char bytes[] = "\x1b[2;53mD";

	vt = vterm_new(24, 80);
	FYAI_TCHECK(vt != NULL);
	screen = vterm_obtain_screen(vt);
	vterm_screen_reset(screen, 1);

	FYAI_TCHECK(vterm_input_write(vt, bytes, strlen(bytes)) == strlen(bytes));

	pos.row = 0;
	pos.col = 0;
	FYAI_TCHECK(vterm_screen_get_cell(screen, pos, &cell));
	FYAI_TCHECK(cell.chars[0] == 'D');
	FYAI_TCHECK(cell.attrs.dim == 1);
	FYAI_TCHECK(cell.attrs.overline == 1);

	vterm_free(vt);
	return 0;
}

/* Neovim commits 977e91b424 and 112092271b modernized the DA1 (device
 * attributes) reply from "?1;2c" to "?61;22;52c". */
int vterm_da1_response(void)
{
	VTerm *vt;
	struct capture cap = { .len = 0 };
	static const char bytes[] = "\x1b[c";

	vt = vterm_new(24, 80);
	FYAI_TCHECK(vt != NULL);
	vterm_obtain_state(vt);
	vterm_output_set_callback(vt, capture_output, &cap);

	FYAI_TCHECK(vterm_input_write(vt, bytes, strlen(bytes)) == strlen(bytes));
	FYAI_TCHECK(strcmp(cap.buf, "\x1b[?61;22;52c") == 0);

	vterm_free(vt);
	return 0;
}

/* Neovim commit 6f0bde11cc added the kitty keyboard protocol. CSI > 1 u
 * pushes "disambiguate" mode; a subsequent Ctrl+a must then encode as
 * CSI 97;5u instead of the legacy 0x01 control byte. */
int vterm_kitty_disambiguate(void)
{
	VTerm *vt;
	VTermState *state;
	struct capture cap = { .len = 0 };
	static const char enable[] = "\x1b[>1u";

	vt = vterm_new(24, 80);
	FYAI_TCHECK(vt != NULL);
	state = vterm_obtain_state(vt);
	vterm_output_set_callback(vt, capture_output, &cap);

	FYAI_TCHECK(vterm_input_write(vt, enable, strlen(enable)) == strlen(enable));

	vterm_keyboard_unichar(vt, 'a', VTERM_MOD_CTRL);
	FYAI_TCHECK(strcmp(cap.buf, "\x1b[97;5u") == 0);

	(void)state;
	vterm_free(vt);
	return 0;
}

/* Neovim commit b38173e493 added DEC mode 2026 (synchronized output). */
int vterm_sync_output_mode(void)
{
	VTerm *vt;
	struct capture cap = { .len = 0 };
	static const char enable[] = "\x1b[?2026h";
	static const char query[] = "\x1b[?2026$p";

	vt = vterm_new(24, 80);
	FYAI_TCHECK(vt != NULL);
	vterm_obtain_state(vt);
	vterm_output_set_callback(vt, capture_output, &cap);

	FYAI_TCHECK(vterm_input_write(vt, enable, strlen(enable)) == strlen(enable));
	FYAI_TCHECK(vterm_input_write(vt, query, strlen(query)) == strlen(query));
	FYAI_TCHECK(strcmp(cap.buf, "\x1b[?2026;1$y") == 0);

	vterm_free(vt);
	return 0;
}

int vterm_sgr_bold(void)
{
	VTerm *vt;
	VTermScreen *screen;
	VTermScreenCell cell;
	VTermPos pos;
	static const char bytes[] = "\x1b[1mB\x1b[0mN";

	vt = vterm_new(24, 80);
	FYAI_TCHECK(vt != NULL);
	screen = vterm_obtain_screen(vt);
	vterm_screen_reset(screen, 1);

	FYAI_TCHECK(vterm_input_write(vt, bytes, strlen(bytes)) == strlen(bytes));

	pos.row = 0;
	pos.col = 0;
	FYAI_TCHECK(vterm_screen_get_cell(screen, pos, &cell));
	FYAI_TCHECK(cell.chars[0] == 'B');
	FYAI_TCHECK(cell.attrs.bold == 1);

	pos.col = 1;
	FYAI_TCHECK(vterm_screen_get_cell(screen, pos, &cell));
	FYAI_TCHECK(cell.chars[0] == 'N');
	FYAI_TCHECK(cell.attrs.bold == 0);

	vterm_free(vt);
	return 0;
}
