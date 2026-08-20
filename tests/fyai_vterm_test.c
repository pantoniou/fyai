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
