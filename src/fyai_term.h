/*
 * fyai_term.h - the full-screen terminal verb
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FYAI_TERM_H
#define FYAI_TERM_H

struct fyai_ctx;

/* Run `fyai term`: one program on a pseudo-terminal, drawn by fyai. */
int fyai_term_verb(struct fyai_ctx *ctx);

#endif
