/*
 * fyai_marker.h - the prompt marker and its busy form
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FYAI_MARKER_H
#define FYAI_MARKER_H

/*
 * The columns a glyph takes, or 0 or less when the caller does not know. A
 * printable glyph with no known width takes one column.
 */
typedef int (*fyai_marker_width_fn)(unsigned int cp);

/* The columns of @marker, which can hold SGR sequences. @width can be NULL. */
int fyai_marker_cols(const char *marker, fyai_marker_width_fn width);

/*
 * The marker that shows while a typed line would wait: @glyph, or an ellipsis
 * (U+2026) when @glyph is NULL or empty, padded with blanks to the columns of
 * @marker, so the input does not move. A glyph wider than @marker is not cut.
 * The SGR sequences that start @marker go before the glyph, and a reset
 * follows it when @marker holds any, so the colour of the marker stays and
 * does not reach the blanks. Returns a malloc'd string, or NULL.
 */
char *fyai_marker_busy(const char *marker, const char *glyph,
		       fyai_marker_width_fn width);

#endif
