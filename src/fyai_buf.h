/* SPDX-License-Identifier: MIT */
/*
 * A growing byte buffer. The content is data[off..len). A zeroed buffer is
 * empty and valid. Every call that adds bytes keeps a NUL after the content,
 * so a buffer of text is a C string at data + off. A buffer that is read from
 * the front, a queue, takes the bytes out with fyai_buf_consume(); the others
 * keep @off at 0.
 */
#ifndef FYAI_BUF_H
#define FYAI_BUF_H

#include <stddef.h>

struct fyai_buf {
	char *data;
	size_t len;
	size_t cap;
	size_t off;
};

/* Make room for @need bytes in all, the content and its NUL included. */
int fyai_buf_reserve(struct fyai_buf *buf, size_t need);
int fyai_buf_append(struct fyai_buf *buf, const char *text);
int fyai_buf_append_data(struct fyai_buf *buf, const void *data, size_t len);
/* Append @len bytes and a newline. */
int fyai_buf_append_line(struct fyai_buf *buf, const void *data, size_t len);

/*
 * Count @n bytes at the end as content and return where they go, or NULL when
 * memory runs out. The pointer is valid until the next call that adds to
 * @buf.
 */
char *fyai_buf_extend(struct fyai_buf *buf, size_t n);

/* Drop the newlines and carriage returns at the end of the content. */
void fyai_buf_trim(struct fyai_buf *buf);

/* The bytes of the content. */
size_t fyai_buf_pending(const struct fyai_buf *buf);

/* Take @n bytes from the front. A buffer that is empty then starts again. */
void fyai_buf_consume(struct fyai_buf *buf, size_t n);

/* Free the memory and leave an empty buffer. */
void fyai_buf_free(struct fyai_buf *buf);

#endif
