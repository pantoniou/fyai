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

/* libfyvterm creates over a configuration struct; every case here wants
 * the same plain 24x80 terminal. */
static struct fyvt *vterm_open(void)
{
	struct fyvt_cfg cfg;

	fyvt_cfg_default(&cfg);
	cfg.rows = 24;
	cfg.cols = 80;
	return fyvt_create(&cfg);
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
