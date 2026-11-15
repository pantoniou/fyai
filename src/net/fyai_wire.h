/* SPDX-License-Identifier: MIT */
#ifndef FYAI_WIRE_H
#define FYAI_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_scratch;

/*
 * Binary encoding of generics for a pipe between two processes of one host.
 * Integers use the byte order of the host. A value is a tag byte and its body:
 *
 *	null, false, true	no body
 *	int			zigzag varint
 *	string			varint length and the bytes
 *	hex string		varint byte count and the bytes; the string is
 *				the lowercase hex digits of the bytes
 *	sequence		varint count and the values
 *	mapping			varint count and the key and value pairs
 *
 * A string of lowercase hex digits with an even length takes the hex form. A
 * float or an alias is not supported: the encoder fails with ENOTSUP.
 */

#define FYAI_WIRE_MAX_DEPTH 32

/*
 * Output sink. With a descriptor of -1 the writer only counts the bytes, so
 * one encoder gives both the size of a frame and its bytes.
 */
struct fyai_wire_writer {
	int fd;
	int error;
	size_t used;
	uint64_t total;
	unsigned char buffer[65536];
};

void fyai_wire_writer_init(struct fyai_wire_writer *writer, int fd);

void fyai_wire_put_varint(struct fyai_wire_writer *writer, uint64_t value);
void fyai_wire_put_bytes(struct fyai_wire_writer *writer, const void *data, size_t length);

/* Return 0, or -1 with errno set. The writer keeps the first error. */
int fyai_wire_put_value(struct fyai_wire_writer *writer, fy_generic value);

/* Write the buffered bytes; return 0, or -1 with errno set. */
int fyai_wire_flush(struct fyai_wire_writer *writer);

/* Input cursor over a frame in memory; every read checks the bounds. */
struct fyai_wire_reader {
	const unsigned char *position;
	const unsigned char *end;
};

void fyai_wire_reader_init(struct fyai_wire_reader *reader, const void *data, size_t length);

/* Return 0, or -1 with errno set to EBADMSG for a damaged frame. */
int fyai_wire_get_varint(struct fyai_wire_reader *reader, uint64_t *value);

/* Decode one value into gb. Scratch holds the temporaries of the decode. */
int fyai_wire_get_value(struct fyai_wire_reader *reader, struct fy_generic_builder *gb,
			struct fyai_scratch *scratch, fy_generic *value);

#endif
