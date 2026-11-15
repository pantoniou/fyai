/* SPDX-License-Identifier: MIT */
#include "config.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_buf.h"

int fyai_buf_reserve(struct fyai_buf *buf, size_t need)
{
	size_t cap;
	char *data;

	if (need <= buf->cap)
		return 0;

	cap = buf->cap ? buf->cap : 4096;
	while (cap < need) {
		if (cap > SIZE_MAX / 2)
			return -1;
		cap *= 2;
	}

	data = realloc(buf->data, cap);
	if (!data)
		return -1;

	buf->data = data;
	buf->cap = cap;
	return 0;
}

char *fyai_buf_extend(struct fyai_buf *buf, size_t n)
{
	char *at;

	/* A queue that was read to its end starts again at its front. */
	if (buf->off && buf->off == buf->len)
		buf->off = buf->len = 0;
	if (n > SIZE_MAX - buf->len - 1 || fyai_buf_reserve(buf, buf->len + n + 1))
		return NULL;
	at = buf->data + buf->len;
	buf->len += n;
	buf->data[buf->len] = '\0';
	return at;
}

int fyai_buf_append_data(struct fyai_buf *buf, const void *data, size_t len)
{
	char *at = fyai_buf_extend(buf, len);

	if (!at)
		return -1;
	if (len)
		memcpy(at, data, len);
	return 0;
}

int fyai_buf_append(struct fyai_buf *buf, const char *text)
{
	return fyai_buf_append_data(buf, text, strlen(text));
}

int fyai_buf_append_line(struct fyai_buf *buf, const void *data, size_t len)
{
	int rc;

	rc = fyai_buf_append_data(buf, data, len);
	if (rc)
		return rc;
	return fyai_buf_append_data(buf, "\n", 1);
}

void fyai_buf_trim(struct fyai_buf *buf)
{
	while (buf->len > buf->off && (buf->data[buf->len - 1] == '\n' ||
				       buf->data[buf->len - 1] == '\r'))
		buf->len--;
	if (buf->data)
		buf->data[buf->len] = '\0';
}

size_t fyai_buf_pending(const struct fyai_buf *buf)
{
	return buf->len - buf->off;
}

void fyai_buf_consume(struct fyai_buf *buf, size_t n)
{
	buf->off += n;
	if (buf->off >= buf->len)
		buf->off = buf->len = 0;
	if (buf->data)
		buf->data[buf->len] = '\0';
}

void fyai_buf_free(struct fyai_buf *buf)
{
	free(buf->data);
	memset(buf, 0, sizeof(*buf));
}
