/*
 * fyai_marker.c - the prompt marker and its busy form
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_diag.h"
#include "fyai_marker.h"

#define MARKER_ELLIPSIS "\xe2\x80\xa6"
#define MARKER_RESET "\x1b[0m"

/* The length of the SGR or other CSI sequence at @p, or 0 when none starts. */
static size_t marker_csi_len(const unsigned char *p)
{
	size_t n;

	if (p[0] != 0x1b || p[1] != '[')
		return 0;
	for (n = 2; p[n] && (p[n] < 0x40 || p[n] > 0x7e); n++)
		;
	return p[n] ? n + 1 : n;
}

/* Decode the character at @p into @cp and return its length in bytes. */
static int marker_decode(const unsigned char *p, unsigned int *cp)
{
	if (*p < 0x80) {
		*cp = *p;
		return 1;
	}
	if ((*p & 0xe0) == 0xc0 && p[1]) {
		*cp = (unsigned int)(*p & 0x1f) << 6 | (p[1] & 0x3f);
		return 2;
	}
	if ((*p & 0xf0) == 0xe0 && p[1] && p[2]) {
		*cp = (unsigned int)(*p & 0x0f) << 12 |
		      (unsigned int)(p[1] & 0x3f) << 6 | (p[2] & 0x3f);
		return 3;
	}
	if ((*p & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) {
		*cp = (unsigned int)(*p & 0x07) << 18 |
		      (unsigned int)(p[1] & 0x3f) << 12 |
		      (unsigned int)(p[2] & 0x3f) << 6 | (p[3] & 0x3f);
		return 4;
	}
	*cp = '?';
	return 1;
}

int fyai_marker_cols(const char *marker, fyai_marker_width_fn width)
{
	const unsigned char *p = (const unsigned char *)marker;
	unsigned int cp;
	int cols = 0, n, w;
	size_t len;

	while (p && *p) {
		len = marker_csi_len(p);
		if (len) {
			p += len;
			continue;
		}
		n = marker_decode(p, &cp);
		w = width ? width(cp) : 0;
		if (w > 0)
			cols += w;
		else if (cp >= 0x20 && cp != 0x7f)
			cols++;
		p += n;
	}
	return cols;
}

char *fyai_marker_busy(const char *marker, const char *glyph,
		       fyai_marker_width_fn width)
{
	const unsigned char *p = (const unsigned char *)marker;
	const unsigned char *start;
	bool has_csi = false;
	size_t len, prefix, size;
	int cols, gcols;
	char *out, *q;
	size_t glen;

	if (!marker)
		return NULL;
	if (!glyph || !*glyph)
		glyph = MARKER_ELLIPSIS;
	glen = strlen(glyph);
	/* the sequences before the first glyph keep their colour */
	for (start = p; (len = marker_csi_len(start)); start += len)
		;
	prefix = (size_t)(start - p);
	for (; *p; p++) {
		if (p[0] == 0x1b && p[1] == '[') {
			has_csi = true;
			break;
		}
	}
	cols = fyai_marker_cols(marker, width);
	gcols = fyai_marker_cols(glyph, width);
	if (gcols < 1)
		gcols = 1;
	/* A glyph wider than the marker cannot keep the columns. */
	cols = cols > gcols ? cols - gcols : 0;
	size = prefix + glen + sizeof(MARKER_RESET) - 1 + (size_t)cols + 1;
	out = malloc(size);
	fyai_error_check(NULL, out, err_out,
			 "could not allocate the busy prompt marker");
	q = out;
	memcpy(q, marker, prefix);
	q += prefix;
	memcpy(q, glyph, glen);
	q += glen;
	if (has_csi) {
		memcpy(q, MARKER_RESET, sizeof(MARKER_RESET) - 1);
		q += sizeof(MARKER_RESET) - 1;
	}
	memset(q, ' ', (size_t)cols);
	q += cols;
	*q = '\0';
	return out;

err_out:
	return NULL;
}
