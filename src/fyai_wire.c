/* SPDX-License-Identifier: MIT */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "fyai_scratch.h"
#include "fyai_wire.h"

enum wire_tag {
	WIRE_NULL,
	WIRE_FALSE,
	WIRE_TRUE,
	WIRE_INT,
	WIRE_STRING,
	WIRE_HEX,
	WIRE_SEQUENCE,
	WIRE_MAPPING,
};

void fyai_wire_writer_init(struct fyai_wire_writer *writer, int fd)
{
	writer->fd = fd;
	writer->error = 0;
	writer->used = 0;
	writer->total = 0;
}

static int wire_write_all(int fd, const unsigned char *data, size_t length)
{
	ssize_t amount;

	while (length) {
		amount = write(fd, data, length);
		if (amount < 0 && errno == EINTR)
			continue;
		if (amount <= 0) {
			if (amount == 0)
				errno = EIO;
			return -1;
		}
		data += amount;
		length -= (size_t)amount;
	}
	return 0;
}

int fyai_wire_flush(struct fyai_wire_writer *writer)
{
	int rc;

	if (writer->error) {
		errno = writer->error;
		return -1;
	}
	if (writer->fd < 0 || !writer->used)
		return 0;
	rc = wire_write_all(writer->fd, writer->buffer, writer->used);
	writer->used = 0;
	if (rc)
		writer->error = errno;
	return rc;
}

void fyai_wire_put_bytes(struct fyai_wire_writer *writer, const void *data, size_t length)
{
	const unsigned char *bytes = data;
	size_t room;

	writer->total += length;
	if (writer->fd < 0 || writer->error)
		return;
	while (length) {
		room = sizeof(writer->buffer) - writer->used;
		if (!room) {
			if (fyai_wire_flush(writer))
				return;
			room = sizeof(writer->buffer);
		}
		if (room > length)
			room = length;
		memcpy(writer->buffer + writer->used, bytes, room);
		writer->used += room;
		bytes += room;
		length -= room;
	}
}

void fyai_wire_put_varint(struct fyai_wire_writer *writer, uint64_t value)
{
	unsigned char bytes[10];
	size_t count = 0;

	while (value >= 0x80) {
		bytes[count++] = (unsigned char)(value | 0x80);
		value >>= 7;
	}
	bytes[count++] = (unsigned char)value;
	fyai_wire_put_bytes(writer, bytes, count);
}

static void wire_put_tag(struct fyai_wire_writer *writer, enum wire_tag tag)
{
	unsigned char byte = (unsigned char)tag;

	fyai_wire_put_bytes(writer, &byte, 1);
}

static int wire_hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/* True when text is an even number of lowercase hex digits. */
static bool wire_is_hex(const char *text, size_t length)
{
	size_t i;

	if (!length || (length & 1))
		return false;
	for (i = 0; i < length; i++)
		if (wire_hex_digit(text[i]) < 0)
			return false;
	return true;
}

static void wire_put_string(struct fyai_wire_writer *writer, const char *text, size_t length)
{
	unsigned char bytes[128];
	size_t i, used = 0;

	if (wire_is_hex(text, length)) {
		wire_put_tag(writer, WIRE_HEX);
		fyai_wire_put_varint(writer, length / 2);
		for (i = 0; i < length; i += 2) {
			bytes[used++] = (unsigned char)(wire_hex_digit(text[i]) << 4 |
							wire_hex_digit(text[i + 1]));
			if (used == sizeof(bytes) || i + 2 == length) {
				fyai_wire_put_bytes(writer, bytes, used);
				used = 0;
			}
		}
		return;
	}
	wire_put_tag(writer, WIRE_STRING);
	fyai_wire_put_varint(writer, length);
	fyai_wire_put_bytes(writer, text, length);
}

static int wire_put_depth(struct fyai_wire_writer *writer, fy_generic value, unsigned int depth)
{
	fy_generic key, child;
	const char *text;
	size_t length;
	long long number;

	if (depth > FYAI_WIRE_MAX_DEPTH) {
		errno = ELOOP;
		return -1;
	}
	if (fy_is_null(value)) {
		wire_put_tag(writer, WIRE_NULL);
	} else if (fy_is_bool(value)) {
		wire_put_tag(writer, fy_cast(value, false) ? WIRE_TRUE : WIRE_FALSE);
	} else if (fy_is_int(value)) {
		number = fy_cast(value, 0LL);
		wire_put_tag(writer, WIRE_INT);
		fyai_wire_put_varint(writer, ((uint64_t)number << 1) ^ (uint64_t)(number >> 63));
	} else if (fy_is_string(value)) {
		text = fy_castp(&value, "");
		length = strlen(text);
		wire_put_string(writer, text, length);
	} else if (fy_is_sequence(value)) {
		wire_put_tag(writer, WIRE_SEQUENCE);
		fyai_wire_put_varint(writer, fy_len(value));
		fy_foreach(child, value)
			if (wire_put_depth(writer, child, depth + 1))
				return -1;
	} else if (fy_is_mapping(value)) {
		wire_put_tag(writer, WIRE_MAPPING);
		fyai_wire_put_varint(writer, fy_len(value));
		fy_foreach_key_value(key, child, value) {
			if (wire_put_depth(writer, key, depth + 1) ||
			    wire_put_depth(writer, child, depth + 1))
				return -1;
		}
	} else {
		errno = ENOTSUP;
		return -1;
	}
	if (writer->error) {
		errno = writer->error;
		return -1;
	}
	return 0;
}

int fyai_wire_put_value(struct fyai_wire_writer *writer, fy_generic value)
{
	return wire_put_depth(writer, value, 0);
}

void fyai_wire_reader_init(struct fyai_wire_reader *reader, const void *data, size_t length)
{
	reader->position = data;
	reader->end = (const unsigned char *)data + length;
}

int fyai_wire_get_varint(struct fyai_wire_reader *reader, uint64_t *value)
{
	unsigned int shift = 0;
	unsigned char byte;

	*value = 0;
	while (reader->position < reader->end && shift < 64) {
		byte = *reader->position++;
		*value |= (uint64_t)(byte & 0x7f) << shift;
		if (!(byte & 0x80))
			return 0;
		shift += 7;
	}
	errno = EBADMSG;
	return -1;
}

static const unsigned char *wire_get_bytes(struct fyai_wire_reader *reader, uint64_t length)
{
	const unsigned char *bytes = reader->position;

	if (length > (uint64_t)(reader->end - reader->position)) {
		errno = EBADMSG;
		return NULL;
	}
	reader->position += length;
	return bytes;
}

static int wire_check(fy_generic value)
{
	if (fy_is_valid(value))
		return 0;
	errno = ENOMEM;
	return -1;
}

static int wire_get_depth(struct fyai_wire_reader *reader, struct fy_generic_builder *gb,
			  struct fyai_scratch *scratch, fy_generic *value, unsigned int depth)
{
	static const char digits[] = "0123456789abcdef";
	fy_generic *items;
	const unsigned char *bytes;
	char *text;
	uint64_t count, number, i;
	unsigned char tag;

	if (depth > FYAI_WIRE_MAX_DEPTH) {
		errno = ELOOP;
		return -1;
	}
	if (reader->position >= reader->end) {
		errno = EBADMSG;
		return -1;
	}
	tag = *reader->position++;
	switch (tag) {
	case WIRE_NULL:
		*value = fy_null;
		return 0;
	case WIRE_FALSE:
		*value = fy_false;
		return 0;
	case WIRE_TRUE:
		*value = fy_true;
		return 0;
	case WIRE_INT:
		if (fyai_wire_get_varint(reader, &number))
			return -1;
		*value = fy_value(gb, (long long)(number >> 1) ^ -(long long)(number & 1));
		return wire_check(*value);
	case WIRE_STRING:
	case WIRE_HEX:
		if (fyai_wire_get_varint(reader, &count))
			return -1;
		bytes = wire_get_bytes(reader, count);
		if (!bytes)
			return -1;
		if (tag == WIRE_STRING) {
			*value = fy_gb_string_size_create(gb, (const char *)bytes, count);
		} else {
			text = fyai_scratch_alloc(scratch, count * 2 + 1);
			if (!text)
				return -1;
			for (i = 0; i < count; i++) {
				text[i * 2] = digits[bytes[i] >> 4];
				text[i * 2 + 1] = digits[bytes[i] & 15];
			}
			*value = fy_gb_string_size_create(gb, text, count * 2);
		}
		return wire_check(*value);
	case WIRE_SEQUENCE:
	case WIRE_MAPPING:
		if (fyai_wire_get_varint(reader, &count))
			return -1;
		/* Every item takes at least one byte; the count cannot exceed the rest. */
		if (count > (uint64_t)(reader->end - reader->position)) {
			errno = EBADMSG;
			return -1;
		}
		items = fyai_scratch_alloc(scratch, (count * (tag == WIRE_MAPPING ? 2 : 1) + 1) *
							    sizeof(*items));
		if (!items)
			return -1;
		for (i = 0; i < count * (tag == WIRE_MAPPING ? 2 : 1); i++)
			if (wire_get_depth(reader, gb, scratch, &items[i], depth + 1))
				return -1;
		*value = tag == WIRE_MAPPING ? fy_gb_mapping_create(gb, count, items) :
					       fy_gb_sequence_create(gb, count, items);
		return wire_check(*value);
	default:
		errno = EBADMSG;
		return -1;
	}
}

int fyai_wire_get_value(struct fyai_wire_reader *reader, struct fy_generic_builder *gb,
			struct fyai_scratch *scratch, fy_generic *value)
{
	return wire_get_depth(reader, gb, scratch, value, 0);
}
