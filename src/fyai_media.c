/*
 * fyai_media.c - the attachments that travel with a message
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_MEDIA

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <libfyaml/libfyaml-blake3.h>

#include "fyai.h"
#include "fyai_auth_util.h"
#include "fyai_media.h"
#include "fyai_diag.h"
#include "fyai_session.h"
#include "utils.h"

/*
 * A unit test reads an image with no context: report only when there is one
 * to report to, and take the cleanup label either way.
 */
#define media_check(_ctx, _cond, _label, _fmt, ...) \
	do { \
		if (!(_cond)) { \
			if (_ctx) \
				fyai_error((_ctx), (_fmt), ## __VA_ARGS__); \
			goto _label; \
		} \
	} while (0)

static uint32_t media_be32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static uint32_t media_le24(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}

static uint32_t media_le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t media_le16(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t media_be16(const unsigned char *p)
{
	return (uint32_t)p[0] << 8 | (uint32_t)p[1];
}

const char *fyai_media_type_name(enum fyai_media_type type)
{
	switch (type) {
	case FYAI_MEDIA_TYPE_PNG:
		return "image/png";
	case FYAI_MEDIA_TYPE_JPEG:
		return "image/jpeg";
	case FYAI_MEDIA_TYPE_GIF:
		return "image/gif";
	case FYAI_MEDIA_TYPE_WEBP:
		return "image/webp";
	case FYAI_MEDIA_TYPE_UNKNOWN:
		break;
	}
	return NULL;
}

enum fyai_media_type fyai_media_type_parse(const char *name)
{
	static const struct {
		const char *name;
		enum fyai_media_type type;
	} names[] = {
		{ "image/png",		FYAI_MEDIA_TYPE_PNG },
		{ "png",		FYAI_MEDIA_TYPE_PNG },
		{ "image/jpeg",		FYAI_MEDIA_TYPE_JPEG },
		{ "image/jpg",		FYAI_MEDIA_TYPE_JPEG },
		{ "jpeg",		FYAI_MEDIA_TYPE_JPEG },
		{ "jpg",		FYAI_MEDIA_TYPE_JPEG },
		{ "image/gif",		FYAI_MEDIA_TYPE_GIF },
		{ "gif",		FYAI_MEDIA_TYPE_GIF },
		{ "image/webp",		FYAI_MEDIA_TYPE_WEBP },
		{ "webp",		FYAI_MEDIA_TYPE_WEBP },
	};
	size_t i;

	if (!name || !*name)
		return FYAI_MEDIA_TYPE_UNKNOWN;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!strcasecmp(name, names[i].name))
			return names[i].type;
	}
	return FYAI_MEDIA_TYPE_UNKNOWN;
}

bool fyai_media_type_is_image(enum fyai_media_type type)
{
	return type != FYAI_MEDIA_TYPE_UNKNOWN;
}

void fyai_media_image_release(struct fyai_media_image *image)
{
	if (!image)
		return;
	free(image->name);
	image->name = NULL;
}

int fyai_media_sniff(const void *data, size_t len, enum fyai_media_type *typep)
{
	const unsigned char *p = data;

	if (p && len >= 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) {
		*typep = FYAI_MEDIA_TYPE_PNG;
		return 0;
	}
	if (p && len >= 3 && p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff) {
		*typep = FYAI_MEDIA_TYPE_JPEG;
		return 0;
	}
	if (p && len >= 6 &&
	    (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6))) {
		*typep = FYAI_MEDIA_TYPE_GIF;
		return 0;
	}
	if (p && len >= 12 && !memcmp(p, "RIFF", 4) &&
	    !memcmp(p + 8, "WEBP", 4)) {
		*typep = FYAI_MEDIA_TYPE_WEBP;
		return 0;
	}
	errno = ENOTSUP;
	return -1;
}

/*
 * Walk the segment headers of a JPEG stream to its frame header, which holds
 * the size. A segment with no length field is skipped; a file whose frame
 * header lies beyond @len is not sized.
 */
static int media_dimensions_jpeg(const unsigned char *p, size_t len,
				 unsigned int *widthp, unsigned int *heightp,
				 size_t *walkp)
{
	size_t pos = 2;
	unsigned int marker;
	size_t segment;

	while (pos + 1 < len) {
		if (p[pos] != 0xff) {
			errno = EINVAL;
			return -1;
		}
		marker = p[pos + 1];
		pos += 2;
		/* Fill bytes of a marker: 0xff 0xff ... marker. */
		while (marker == 0xff && pos < len) {
			marker = p[pos];
			pos++;
		}
		/* A marker with no segment: TEM, RSTn, SOI. */
		if (marker == 0x01 || marker == 0xd8 ||
		    (marker >= 0xd0 && marker <= 0xd7))
			continue;
		/* EOI, or the start of the scan that follows the frame. */
		if (marker == 0xd9 || marker == 0xda)
			break;
		if (pos + 1 >= len)
			break;
		segment = (size_t)p[pos] << 8 | p[pos + 1];
		if (segment < 2) {
			errno = EINVAL;
			return -1;
		}
		/*
		 * SOF0-SOF15, less the markers of that range that are not a
		 * frame header: DHT, JPG, DAC.
		 */
		if (marker >= 0xc0 && marker <= 0xcf &&
		    marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
			if (pos + 7 > len)
				break;
			/* A JPEG segment states its values in network order. */
			*heightp = media_be16(p + pos + 3);
			*widthp = media_be16(p + pos + 5);
			return 0;
		}
		pos += segment;
	}
	if (walkp)
		*walkp = pos;
	errno = EINVAL;
	return -1;
}

/* The canvas of a WebP file, from its first chunk. */
static int media_dimensions_webp(const unsigned char *p, size_t len,
				 unsigned int *widthp, unsigned int *heightp)
{
	uint32_t bits;

	if (len < 20) {
		errno = EINVAL;
		return -1;
	}
	if (!memcmp(p + 12, "VP8X", 4)) {
		if (len < 30) {
			errno = EINVAL;
			return -1;
		}
		*widthp = media_le24(p + 24) + 1;
		*heightp = media_le24(p + 27) + 1;
		return 0;
	}
	if (!memcmp(p + 12, "VP8L", 4)) {
		if (len < 25 || p[20] != 0x2f) {
			errno = EINVAL;
			return -1;
		}
		bits = media_le32(p + 21);
		*widthp = (bits & 0x3fff) + 1;
		*heightp = ((bits >> 14) & 0x3fff) + 1;
		return 0;
	}
	if (!memcmp(p + 12, "VP8 ", 4)) {
		if (len < 30 || p[23] != 0x9d || p[24] != 0x01 || p[25] != 0x2a) {
			errno = EINVAL;
			return -1;
		}
		*widthp = media_le16(p + 26) & 0x3fff;
		*heightp = media_le16(p + 28) & 0x3fff;
		return 0;
	}
	errno = EINVAL;
	return -1;
}

int fyai_media_dimensions(const void *data, size_t len,
			  enum fyai_media_type type,
			  unsigned int *widthp, unsigned int *heightp)
{
	const unsigned char *p = data;

	if (!p) {
		errno = EINVAL;
		return -1;
	}
	switch (type) {
	case FYAI_MEDIA_TYPE_PNG:
		/* The signature, the IHDR length, then its type and size. */
		if (len < 26 || memcmp(p + 12, "IHDR", 4)) {
			errno = EINVAL;
			return -1;
		}
		*widthp = media_be32(p + 16);
		*heightp = media_be32(p + 20);
		return 0;
	case FYAI_MEDIA_TYPE_JPEG:
		return media_dimensions_jpeg(p, len, widthp, heightp, NULL);
	case FYAI_MEDIA_TYPE_GIF:
		/* The logical screen descriptor follows the version. */
		if (len < 10) {
			errno = EINVAL;
			return -1;
		}
		*widthp = media_le16(p + 6);
		*heightp = media_le16(p + 8);
		return 0;
	case FYAI_MEDIA_TYPE_WEBP:
		return media_dimensions_webp(p, len, widthp, heightp);
	case FYAI_MEDIA_TYPE_UNKNOWN:
		break;
	}
	errno = EINVAL;
	return -1;
}

/* Write the lowercase hex form of the BLAKE3-256 digest of @data. */
static int media_digest(char out[FYAI_MEDIA_DIGEST_SIZE], const void *data,
			size_t len)
{
	static const char hex[] = "0123456789abcdef";
	struct fy_blake3_hasher_cfg hcfg = { .num_threads = -1 };
	struct fy_blake3_hasher *hasher;
	const uint8_t *hash;
	size_t i;

	hasher = fy_blake3_hasher_create(&hcfg);
	if (!hasher)
		return -1;
	hash = fy_blake3_hash(hasher, data, len);
	for (i = 0; hash && i < (FYAI_MEDIA_DIGEST_SIZE - 1) / 2; i++) {
		out[i * 2] = hex[hash[i] >> 4];
		out[i * 2 + 1] = hex[hash[i] & 15];
	}
	out[i * 2] = '\0';
	fy_blake3_hasher_destroy(hasher);
	return hash ? 0 : -1;
}

int fyai_media_image_bytes(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			   const void *data, size_t len, const char *declared,
			   const char *name, struct fyai_media_image *image)
{
	fy_generic_sized_string bytes = { .data = data, .size = len };
	enum fyai_media_type type, want;
	unsigned int width = 0, height = 0;
	int rc;

	if (!gb || !image || (!data && len)) {
		errno = EINVAL;
		return -1;
	}
	memset(image, 0, sizeof(*image));
	image->data = fy_invalid;
	media_check(ctx, len, out, "an attachment holds no bytes");
	rc = fyai_media_sniff(data, len, &type);
	media_check(ctx, !rc, out, "the attachment is not a PNG, JPEG, GIF or "
		    "WebP image");

	/* A stated type must name a supported type and match the bytes. */
	if (declared && *declared) {
		want = fyai_media_type_parse(declared);
		media_check(ctx, want != FYAI_MEDIA_TYPE_UNKNOWN, out,
			    "the media type '%s' is not supported", declared);
		media_check(ctx, want == type, out,
			    "the media type '%s' does not match the attachment, "
			    "which is %s", declared, fyai_media_type_name(type));
	}

	rc = fyai_media_dimensions(data, len < FYAI_MEDIA_HEAD_MAX ?
				   len : FYAI_MEDIA_HEAD_MAX,
				   type, &width, &height);
	media_check(ctx, !rc, out, "could not read the size of the %s "
		    "attachment", fyai_media_type_name(type));
	media_check(ctx, width && height, out, "the %s attachment reports no "
		    "pixel size", fyai_media_type_name(type));

	rc = media_digest(image->digest, data, len);
	media_check(ctx, !rc, out, "could not hash the attachment");
	image->data = fy_value(gb, bytes);
	media_check(ctx, fy_is_string(image->data), out,
		    "could not copy %zu bytes of the attachment", len);
	image->type = type;
	image->width = width;
	image->height = height;
	image->bytes = len;
	if (name && *name) {
		image->name = strdup(name);
		media_check(ctx, image->name, out,
			    "could not copy the attachment name");
	}
	return 0;

out:
	fyai_media_image_release(image);
	memset(image, 0, sizeof(*image));
	image->data = fy_invalid;
	return -1;
}

/*
 * Read @len bytes of @fd from its start, owned by the caller, or NULL. A
 * @len of 0 is an empty file: the buffer has a byte, so a NULL is a failure.
 */
static unsigned char *media_read_all(struct fyai_ctx *ctx, int fd, size_t len)
{
	unsigned char *buf;
	size_t done = 0;
	ssize_t got;

	buf = malloc(len ? len : 1);
	media_check(ctx, buf, err, "could not allocate %zu bytes for an "
		    "attachment", len);
	while (done < len) {
		got = pread(fd, buf + done, len - done, (off_t)done);
		if (got < 0 && errno == EINTR)
			continue;
		media_check(ctx, got >= 0, err_free,
			    "could not read the attachment: %s",
			    strerror(errno));
		media_check(ctx, got, err_free, "the attachment ended early");
		done += (size_t)got;
	}
	return buf;

err_free:
	free(buf);
err:
	return NULL;
}

int fyai_media_image_read(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			  int fd, uint64_t max_bytes, const char *declared,
			  const char *name, struct fyai_media_image *image)
{
	unsigned char *data;
	struct stat st;
	int rc;

	if (!gb || !image || fd < 0) {
		errno = EINVAL;
		return -1;
	}
	memset(image, 0, sizeof(*image));
	image->data = fy_invalid;
	rc = fstat(fd, &st);
	media_check(ctx, !rc, err, "could not stat the attachment: %s",
		    strerror(errno));
	media_check(ctx, S_ISREG(st.st_mode), err,
		    "an attachment must be a regular file");
	media_check(ctx, st.st_size > 0, err, "an attachment holds no bytes");
	media_check(ctx, !max_bytes || (uint64_t)st.st_size <= max_bytes, err,
		    "the attachment holds %lld bytes, more than the %llu byte "
		    "limit", (long long)st.st_size,
		    (unsigned long long)max_bytes);
	media_check(ctx, (uint64_t)st.st_size <= SIZE_MAX, err,
		    "the attachment holds %lld bytes, more than fits in memory",
		    (long long)st.st_size);
	data = media_read_all(ctx, fd, (size_t)st.st_size);
	if (!data)
		goto err;
	rc = fyai_media_image_bytes(ctx, gb, data, (size_t)st.st_size,
				    declared, name, image);
	free(data);
	return rc;

err:
	return -1;
}

/*
 * The canonical part. The bytes travel in it; the digest names them and the
 * numbers describe them, so a reader can show an image without decoding it,
 * and an estimate can count the reference instead of the pixels.
 */
fy_generic fyai_media_image_part(struct fy_generic_builder *gb,
				 const struct fyai_media_image *image)
{
	fy_generic part;
	const char *type_name;

	if (!gb || !image)
		return fy_invalid;
	type_name = fyai_media_type_name(image->type);
	if (!type_name || !image->digest[0])
		return fy_invalid;

	part = fy_mapping(gb,
		"type", FYAI_MEDIA_PART_IMAGE,
		"media_type", type_name,
		"digest", image->digest,
		"width", (long long)image->width,
		"height", (long long)image->height,
		"bytes", (long long)image->bytes);
	if (fy_is_string(image->data))
		part = fy_assoc(gb, part, "data", image->data);
	if (fy_is_valid(part) && image->name && *image->name)
		part = fy_assoc(gb, part, "name", image->name);
	return part;
}

bool fyai_media_part_is_image(fy_generic part)
{
	return fy_equal(fy_get(part, "type", fy_invalid),
			FYAI_MEDIA_PART_IMAGE);
}

bool fyai_media_content_has_images(fy_generic content)
{
	fy_generic part;

	if (!fy_is_sequence(content))
		return false;
	fy_foreach(part, content) {
		if (fyai_media_part_is_image(part))
			return true;
	}
	return false;
}

bool fyai_media_part_is_file(fy_generic part)
{
	return fy_equal(fy_get(part, "type", fy_invalid), FYAI_MEDIA_PART_TEXT) &&
	       fy_is_string(fy_get(part, "file", fy_invalid));
}

bool fyai_media_content_has_files(fy_generic content)
{
	fy_generic part;

	if (!fy_is_sequence(content))
		return false;
	fy_foreach(part, content) {
		if (fyai_media_part_is_file(part))
			return true;
	}
	return false;
}

bool fyai_media_content_has_attachments(fy_generic content)
{
	return fyai_media_content_has_images(content) ||
	       fyai_media_content_has_files(content);
}

fy_generic fyai_media_parts(struct fy_generic_builder *gb, fy_generic content)
{
	fy_generic out, part;

	if (!gb)
		return fy_invalid;
	out = fy_seq_empty;
	if (!fy_is_sequence(content))
		return out;
	fy_foreach(part, content) {
		if (!fyai_media_part_is_image(part))
			continue;
		out = fy_append(gb, out, part);
	}
	return out;
}

/*
 * A string content is its own text and is returned borrowed, so a caller that
 * only wants to read it allocates nothing. Everything else is joined into
 * @gb. A part that is not a text run contributes nothing: an image carries no
 * words, and a provider-specific part is wire detail of another grammar.
 */
fy_generic fyai_media_text(struct fy_generic_builder *gb, fy_generic content)
{
	fy_generic chunks, part;

	if (fy_is_string(content))
		return content;
	if (!gb)
		return fy_invalid;
	chunks = fy_seq_empty;
	fy_foreach(part, content) {
		if (!fy_any_equal(fy_get(part, "type"), FYAI_MEDIA_PART_TEXT,
				  "output_text") || fyai_media_part_is_file(part))
			continue;
		chunks = fy_append(gb, chunks, fy_get(part, "text", ""));
	}
	return fyai_join_strings(gb, chunks);
}

fy_generic fyai_media_model_text(struct fy_generic_builder *gb,
				 fy_generic content)
{
	fy_generic chunks, part, text;
	const char *s;

	if (!fyai_media_content_has_files(content))
		return fyai_media_text(gb, content);
	text = fyai_media_text(gb, content);
	s = fy_castp(&text, "");
	chunks = fy_seq_empty;
	if (s && *s)
		chunks = fy_append(gb, chunks, text);
	fy_foreach(part, content) {
		if (!fyai_media_part_is_file(part))
			continue;
		if (!fy_empty(chunks))
			chunks = fy_append(gb, chunks, fy_value(gb, "\n\n"));
		chunks = fy_append(gb, chunks, fy_get(part, "text", ""));
	}
	return fyai_join_strings(gb, chunks);
}

/* "PNG": the subtype of @media_type, upper case, for the alt text. */
static void media_type_label(char *buf, size_t sz, const char *media_type)
{
	const char *slash;
	size_t i;

	slash = media_type ? strrchr(media_type, '/') : NULL;
	if (slash && slash[1])
		media_type = slash + 1;
	if (!media_type)
		media_type = "";
	for (i = 0; media_type[i] && i + 1 < sz; i++)
		buf[i] = (char)toupper((unsigned char)media_type[i]);
	buf[i] = '\0';
}

/* "29 B", "96 KiB", "4.0 MiB": a size a person reads, not a byte count. */
static void media_size_label(char *buf, size_t sz, long long bytes)
{
	if (bytes < 1024)
		snprintf(buf, sz, "%lld B", bytes);
	else if (bytes < 1024 * 1024)
		snprintf(buf, sz, "%lld KiB", (bytes + 512) / 1024);
	else
		snprintf(buf, sz, "%.1f MiB", (double)bytes / (1024.0 * 1024.0));
}

void fyai_media_file_size(char *buf, size_t sz, fy_generic part)
{
	char size[32];
	long long lines = fy_get(part, "lines", 0LL);

	media_size_label(size, sizeof(size), fy_get(part, "bytes", 0LL));
	snprintf(buf, sz, "%lld line%s, %s", lines, lines == 1 ? "" : "s", size);
}

fy_generic fyai_media_part_reference(struct fy_generic_builder *gb,
				     fy_generic part)
{
	char type[32], size[48];
	const char *mt, *dg;
	long long width, height, bytes;
	fy_generic file;
	char *name;

	if (gb && fyai_media_part_is_file(part)) {
		file = fy_get(part, "file");
		name = fyai_prompt_literal(fy_castp(&file, ""));
		if (!name) {
			fyai_warning(NULL, "could not escape the name of a file "
				     "part, so its row is left out");
			return fy_invalid;
		}
		fyai_media_file_size(size, sizeof(size), part);
		file = fy_stringf(gb, "[file: %s, %s]", name, size);
		free(name);
		return file;
	}
	if (!gb || !fyai_media_part_is_image(part))
		return fy_invalid;
	mt = fy_get(part, "media_type", "");
	dg = fy_get(part, "digest", "");
	width = fy_get(part, "width", 0LL);
	height = fy_get(part, "height", 0LL);
	bytes = fy_get(part, "bytes", 0LL);
	media_type_label(type, sizeof(type), mt);
	media_size_label(size, sizeof(size), bytes);
	return fy_stringf(gb, "![image: %s %lldx%lld, %s](fyai-media:%s)",
			  type, width, height, size, dg);
}

fy_generic fyai_media_text_references(struct fy_generic_builder *gb,
				      fy_generic content)
{
	fy_generic text, part, chunks;
	const char *s;

	if (!fyai_media_content_has_attachments(content))
		return fyai_media_text(gb, content);
	if (!gb)
		return fy_invalid;
	text = fyai_media_text(gb, content);
	chunks = fy_seq_empty;
	s = fy_castp(&text, "");
	if (s && *s)
		chunks = fy_append(gb, chunks, text);
	fy_foreach(part, content) {
		if (!fyai_media_part_is_image(part) &&
		    !fyai_media_part_is_file(part))
			continue;
		if (!fy_empty(chunks))
			chunks = fy_append(gb, chunks, fy_value(gb, "\n\n"));
		chunks = fy_append(gb, chunks,
				   fyai_media_part_reference(gb, part));
	}
	return fyai_join_strings(gb, chunks);
}

/* The end of the line at @s: its newline or the end of the text. */
static const char *media_line_end(const char *s)
{
	const char *e;

	e = strchr(s, '\n');
	return e ? e : s + strlen(s);
}

/* Whether @line, of @len bytes, is a whole row of fyai_media_part_reference(). */
static bool media_line_is_row(const char *line, size_t len)
{
	static const char head[] = "![image: ";
	static const char hook[] = "](fyai-media:";
	static const char file[] = "[file: ";
	const char *s;

	if (len > sizeof(file) - 1 && !memcmp(line, file, sizeof(file) - 1) &&
	    line[len - 1] == ']')
		return true;
	if (len < sizeof(head) - 1 || memcmp(line, head, sizeof(head) - 1) ||
	    line[len - 1] != ')')
		return false;
	for (s = line; s + sizeof(hook) - 1 <= line + len; s++) {
		if (!memcmp(s, hook, sizeof(hook) - 1))
			return true;
	}
	return false;
}

char *fyai_media_rows_styled(const char *text)
{
	static const char on[] = "**<fy-role name=\"ref\">";
	static const char off[] = "</fy-role>**";
	const char *s, *e;
	char *out, *o;
	size_t len, rows;

	if (!text)
		return NULL;
	rows = 0;
	for (s = text; *s; s = *e ? e + 1 : e) {
		e = media_line_end(s);
		if (media_line_is_row(s, (size_t)(e - s)))
			rows++;
	}
	out = malloc(strlen(text) + rows * (sizeof(on) + sizeof(off) - 2) + 1);
	if (!out)
		return NULL;
	o = out;
	for (s = text; *s; s = *e ? e + 1 : e) {
		e = media_line_end(s);
		len = (size_t)(e - s);
		if (media_line_is_row(s, len)) {
			memcpy(o, on, sizeof(on) - 1);
			o += sizeof(on) - 1;
		}
		memcpy(o, s, len);
		o += len;
		if (media_line_is_row(s, len)) {
			memcpy(o, off, sizeof(off) - 1);
			o += sizeof(off) - 1;
		}
		if (*e)
			*o++ = '\n';
	}
	*o = '\0';
	return out;
}

/* Whether the next user message carries an attachment. */
bool fyai_media_pending_any(const struct fyai_ctx *ctx)
{
	return ctx && !fy_empty(ctx->media_pending);
}

void fyai_media_pending_clear(struct fyai_ctx *ctx)
{
	if (ctx)
		ctx->media_pending = fy_seq_empty;
}

/*
 * Take the pending attachments, which the next user message carries, and
 * clear them: an attachment travels with one message, not with every message
 * after it.
 */
fy_generic fyai_media_take_pending(struct fyai_ctx *ctx)
{
	fy_generic pending;

	if (!ctx)
		return fy_seq_empty;
	pending = ctx->media_pending;
	ctx->media_pending = fy_seq_empty;
	return fy_is_sequence(pending) ? pending : fy_seq_empty;
}

const char *fyai_media_pending_display(struct fyai_ctx *ctx, const char *text)
{
	struct fy_generic_builder *gb;
	fy_generic content, shown, part;
	const char *s;

	if (!fyai_media_pending_any(ctx) || (gb = fyai_ctx_transient_gb(ctx)) == NULL)
		return NULL;
	content = fy_seq_empty;
	content = fy_append(gb, content,
			    fy_mapping(gb, "type", FYAI_MEDIA_PART_TEXT,
				       "text", fy_value(gb, text ? text : "")));
	fy_foreach(part, ctx->media_pending)
		content = fy_append(gb, content, part);
	shown = fyai_media_text_references(gb, content);
	s = fy_castp(&shown, "");
	return fy_gb_intern_string(gb, s);
}

/* Whether the next user message can take one more attachment. */
static int media_attach_check(struct fyai_ctx *ctx)
{
	/* A pinned root reads one published state: it adds no message state. */
	if (ctx->cfg->root_pinned) {
		fyai_error(ctx, "a pinned root is read-only: it takes no attachment");
		return -1;
	}
	if (ctx->cfg->media_max_per_message > 0 &&
	    (int)fy_len(ctx->media_pending) >= ctx->cfg->media_max_per_message) {
		fyai_error(ctx, "a message takes at most %d attachments",
			   ctx->cfg->media_max_per_message);
		return -1;
	}
	return 0;
}

static uint64_t media_max_bytes(const struct fyai_ctx *ctx)
{
	return ctx->cfg->media_max_bytes > 0 ?
	       (uint64_t)ctx->cfg->media_max_bytes : 0;
}

/*
 * Add @image to the next user message. The part is built in ctx->gb, which
 * holds the bytes, so it lives as long as the message that takes it.
 */
static int media_attach_image(struct fyai_ctx *ctx,
			      struct fyai_media_image *image)
{
	fy_generic part;

	part = fyai_media_image_part(ctx->gb, image);
	fyai_media_image_release(image);
	if (!fy_is_valid(part)) {
		fyai_error(ctx, "could not record the attachment");
		return -1;
	}
	ctx->media_pending = fy_append(ctx->gb, ctx->media_pending, part);
	return 0;
}

int fyai_media_attach_fd(struct fyai_ctx *ctx, int fd, const char *declared,
			 const char *name)
{
	struct fyai_media_image image;

	if (media_attach_check(ctx))
		return -1;
	if (fyai_media_image_read(ctx, ctx->gb, fd, media_max_bytes(ctx),
				  declared, name, &image))
		return -1;
	return media_attach_image(ctx, &image);
}

int fyai_media_attach_bytes(struct fyai_ctx *ctx, const void *data,
			    size_t len, const char *declared, const char *name)
{
	struct fyai_media_image image;
	uint64_t max_bytes;

	if (media_attach_check(ctx))
		return -1;
	max_bytes = media_max_bytes(ctx);
	if (max_bytes && len > max_bytes) {
		fyai_error(ctx, "the attachment holds %zu bytes, more than the "
			   "%llu byte limit", len, (unsigned long long)max_bytes);
		return -1;
	}
	if (fyai_media_image_bytes(ctx, ctx->gb, data, len, declared, name,
				   &image))
		return -1;
	return media_attach_image(ctx, &image);
}

char *fyai_media_path(const char *path)
{
	const char *home;
	char *out;

	if (!path)
		return NULL;
	if (path[0] != '~' || (path[1] && path[1] != '/'))
		return strdup(path);
	home = getenv("HOME");
	if (!home || !*home)
		return strdup(path);
	if (asprintf(&out, "%s%s", home, path + 1) < 0)
		return NULL;
	return out;
}

/* Add the file at @path to the next user message. */
int fyai_media_attach_path(struct fyai_ctx *ctx, const char *path,
			   const char *declared, const char *name)
{
	char *base = NULL, *full;
	const char *leaf;
	int fd, rc;

	if (!path || !*path) {
		fyai_error(ctx, "an attachment needs a path");
		return -1;
	}
	full = fyai_media_path(path);
	if (!full) {
		fyai_error(ctx, "could not expand '%s'", path);
		return -1;
	}
	fd = open(full, O_RDONLY | O_CLOEXEC);
	free(full);
	if (fd < 0) {
		fyai_error(ctx, "could not open '%s': %s", path, strerror(errno));
		return -1;
	}
	if (!name || !*name) {
		leaf = strrchr(path, '/');
		leaf = leaf ? leaf + 1 : path;
		if (*leaf)
			base = strdup(leaf);
	}
	rc = fyai_media_attach_fd(ctx, fd, declared, base ? base : name);
	free(base);
	close(fd);
	return rc;
}

/*
 * Whether @len bytes at @p are text: UTF-8, with no control character other
 * than the blanks of text (tab, newline, vertical tab, form feed and carriage
 * return). A binary file seldom passes: it holds a NUL or another control.
 */
static bool media_is_text(const unsigned char *p, size_t len)
{
	size_t i = 0, n, k;
	unsigned int cp, min;

	while (i < len) {
		if (p[i] < 0x80) {
			if ((p[i] < 0x20 && (p[i] < 0x09 || p[i] > 0x0d)) ||
			    p[i] == 0x7f)
				return false;
			i++;
			continue;
		}
		if ((p[i] & 0xe0) == 0xc0) {
			n = 1; cp = p[i] & 0x1f; min = 0x80;
		} else if ((p[i] & 0xf0) == 0xe0) {
			n = 2; cp = p[i] & 0x0f; min = 0x800;
		} else if ((p[i] & 0xf8) == 0xf0) {
			n = 3; cp = p[i] & 0x07; min = 0x10000;
		} else {
			return false;
		}
		for (k = 1; k <= n; k++) {
			if (i + k >= len || (p[i + k] & 0xc0) != 0x80)
				return false;
			cp = cp << 6 | (p[i + k] & 0x3f);
		}
		if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
			return false;
		i += n + 1;
	}
	return true;
}

/*
 * The text of a file as a message carries it: "File: PATH", then the bytes in
 * a fence longer than any run of backticks in them.
 */
static char *media_file_block(const char *path, const char *data, size_t len)
{
	struct fyai_buf out = {};
	size_t run = 0, longest = 0, i, fence;
	int rc;

	for (i = 0; i < len; i++) {
		run = data[i] == '`' ? run + 1 : 0;
		if (run > longest)
			longest = run;
	}
	fence = longest >= 3 ? longest + 1 : 3;
	rc = fyai_buf_append(&out, "File: ") ||
	     fyai_buf_append(&out, path) ||
	     fyai_buf_append(&out, "\n");
	for (i = 0; !rc && i < fence; i++)
		rc = fyai_buf_append_data(&out, "`", 1);
	rc = rc || fyai_buf_append(&out, "\n") ||
	     fyai_buf_append_data(&out, data, len);
	if (!rc && len && data[len - 1] != '\n')
		rc = fyai_buf_append(&out, "\n");
	for (i = 0; !rc && i < fence; i++)
		rc = fyai_buf_append_data(&out, "`", 1);
	if (rc) {
		free(out.data);
		return NULL;
	}
	return out.data;
}

/*
 * The most bytes of a text file that a message carries: FYAI_MEDIA_TEXT_MAX,
 * or media/max_bytes when that is smaller.
 */
static uint64_t media_text_max(const struct fyai_ctx *ctx)
{
	uint64_t max_bytes = media_max_bytes(ctx);

	return max_bytes && max_bytes < FYAI_MEDIA_TEXT_MAX ?
	       max_bytes : FYAI_MEDIA_TEXT_MAX;
}

/* Add the text file open as @fd, which @path names, as a file part. */
static int media_attach_text(struct fyai_ctx *ctx, const char *path, int fd,
			     uint64_t size)
{
	unsigned char *data;
	char *block;
	fy_generic part;
	long long lines = 0;
	uint64_t max = media_text_max(ctx);
	size_t i;

	if (size > max) {
		fyai_error(ctx, "'%s' holds %llu bytes, more than the %llu bytes of "
			   "text a message carries", path,
			   (unsigned long long)size, (unsigned long long)max);
		return -1;
	}
	data = media_read_all(ctx, fd, (size_t)size);
	if (!data)
		return -1;
	if (!media_is_text(data, (size_t)size)) {
		fyai_error(ctx, "'%s' is neither an image nor a text file", path);
		free(data);
		return -1;
	}
	for (i = 0; i < size; i++)
		lines += data[i] == '\n';
	if (size && data[size - 1] != '\n')
		lines++;
	block = media_file_block(path, (const char *)data, (size_t)size);
	free(data);
	if (!block) {
		fyai_error(ctx, "could not attach '%s'", path);
		return -1;
	}
	part = fy_mapping(ctx->gb,
		"type", FYAI_MEDIA_PART_TEXT,
		"text", fy_value(ctx->gb, block),
		"file", fy_value(ctx->gb, path),
		"bytes", (long long)size,
		"lines", lines);
	free(block);
	if (!fy_is_valid(part)) {
		fyai_error(ctx, "could not record '%s'", path);
		return -1;
	}
	ctx->media_pending = fy_append(ctx->gb, ctx->media_pending, part);
	return 0;
}

/* The part that the last attachment added, or fy_invalid. */
static fy_generic media_last_part(const struct fyai_ctx *ctx)
{
	size_t n = fy_len(ctx->media_pending);

	return n ? fy_get_at(ctx->media_pending, n - 1) : fy_invalid;
}

int fyai_media_attach_file(struct fyai_ctx *ctx, const char *path, bool quiet,
			   fy_generic *partp)
{
	unsigned char head[16];
	enum fyai_media_type type;
	struct stat st;
	const char *leaf;
	char *full;
	ssize_t got;
	int fd, rc = -1;

	if (!path || !*path) {
		fyai_error(ctx, "an attachment needs a path");
		return -1;
	}
	full = fyai_media_path(path);
	if (!full) {
		fyai_error(ctx, "could not expand '%s'", path);
		return -1;
	}
	/* Not blocking: a FIFO that nobody writes must not hold the line. */
	fd = open(full, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	free(full);
	if (fd < 0) {
		/* a word that names nothing is not a file */
		if (quiet && (errno == ENOENT || errno == ENOTDIR ||
			      errno == ENAMETOOLONG))
			return 1;
		fyai_error(ctx, "could not open '%s': %s", path, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		if (quiet) {
			rc = 1;
			goto out;
		}
		fyai_error(ctx, "'%s' is not a regular file", path);
		goto out;
	}
	if (media_attach_check(ctx))
		goto out;
	/*
	 * One open decides the type, the size and the bytes. The magic bytes
	 * say an image; the rest of the bytes say text.
	 */
	got = pread(fd, head, sizeof(head), 0);
	if (got > 0 && !fyai_media_sniff(head, (size_t)got, &type)) {
		leaf = strrchr(path, '/');
		leaf = leaf ? leaf + 1 : path;
		rc = fyai_media_attach_fd(ctx, fd, NULL, *leaf ? leaf : NULL);
	} else {
		rc = media_attach_text(ctx, path, fd, (uint64_t)st.st_size);
	}
	if (!rc && partp)
		*partp = media_last_part(ctx);
out:
	close(fd);
	return rc;
}

/*
 * The path of the mention at @s, just after the "@", unquoted and unescaped
 * into @out. *@endp is where the mention ends, *@explicitp says it was quoted
 * or escaped. -1 for a quote that does not close: the "@" is then text.
 */
static int media_mention_path(const char *s, char *out, const char **endp,
			      bool *explicitp)
{
	char quote = 0;
	size_t o = 0;

	*explicitp = false;
	if (*s == '"' || *s == '\'') {
		quote = *s++;
		*explicitp = true;
	}
	while (*s) {
		if (quote && *s == quote) {
			s++;
			break;
		}
		if (!quote && isspace((unsigned char)*s))
			break;
		if (*s == '\\' && s[1] && quote != '\'') {
			*explicitp = true;
			s++;
		}
		out[o++] = *s++;
		if (quote && !*s)
			return -1;
	}
	out[o] = '\0';
	*endp = s;
	return 0;
}

int fyai_media_take_mentions(struct fyai_ctx *ctx, char *line)
{
	fy_generic saved;
	const char *end;
	char *path = NULL;
	size_t i, o, len, plen;
	bool explicit, mention;
	int rc = -1, got;

	if (!line || !strchr(line, '@'))
		return 0;
	len = strlen(line);
	path = malloc(len + 1);
	if (!path) {
		fyai_error(ctx, "out of memory");
		return -1;
	}
	saved = ctx->media_pending;
	/*
	 * The line is rewritten in place. The write cursor @o never passes the
	 * read cursor @i, so the lookahead of line[i + 1] stays valid.
	 */
	for (i = o = 0; i < len; ) {
		/* "\@" and "@@" are a literal "@" */
		if ((line[i] == '\\' || line[i] == '@') && line[i + 1] == '@') {
			line[o++] = '@';
			i += 2;
			continue;
		}
		mention = line[i] == '@' &&
			  (i == 0 || isspace((unsigned char)line[i - 1])) &&
			  line[i + 1] && !isspace((unsigned char)line[i + 1]);
		/* a quote that does not close is no mention: the "@" is text */
		if (!mention ||
		    media_mention_path(line + i + 1, path, &end, &explicit)) {
			line[o++] = line[i++];
			continue;
		}
		if (!*path) {
			fyai_error(ctx, "the mention '%.*s' names no path",
				   (int)(end - (line + i)), line + i);
			goto out;
		}
		/* a bare @WORD that names no file is text */
		got = fyai_media_attach_file(ctx, path, !explicit, NULL);
		if (got < 0)
			goto out;
		if (got > 0) {
			line[o++] = line[i++];
			continue;
		}
		/* the path stays in the words, without its quoting */
		plen = strlen(path);
		memmove(line + o, path, plen);
		o += plen;
		i = (size_t)(end - line);
	}
	line[o] = '\0';
	rc = 0;
out:
	if (rc)
		ctx->media_pending = saved;
	free(path);
	return rc;
}

bool fyai_media_part_has_bytes(fy_generic part)
{
	fy_generic data;

	if (!fyai_media_part_is_image(part))
		return false;
	data = fy_get(part, "data", fy_invalid);
	return fy_is_string(data) && !fy_empty(data);
}

char *fyai_media_part_base64(struct fyai_ctx *ctx, fy_generic part)
{
	fy_generic_sized_string raw;
	fy_generic data;
	char *out;

	media_check(ctx, fyai_media_part_is_image(part), err,
		    "the part is not an image");
	media_check(ctx, fyai_media_part_has_bytes(part), err,
		    "an image part carries no bytes");
	data = fy_get(part, "data", fy_invalid);
	raw = fy_castp(&data, fy_szstr_empty);
	out = fyai_base64_encode((const unsigned char *)raw.data, raw.size);
	media_check(ctx, out, err, "could not encode the image bytes");
	return out;

err:
	return NULL;
}

/*
 * The text parts are kept as they are; the image parts are made again from
 * their bytes, and the metadata the export wrote must be what the bytes say.
 */
fy_generic fyai_media_import_content(struct fyai_ctx *ctx,
				     struct fy_generic_builder *gb,
				     fy_generic content)
{
	static const char *const fields[] = {
		"media_type", "digest", "width", "height", "bytes",
	};
	struct fyai_media_image image;
	fy_generic out, part, canonical, data, name;
	fy_generic_sized_string raw;
	long long limit;
	size_t count, i;
	int rc;

	limit = ctx->cfg->media_max_bytes;
	out = fy_seq_empty;
	count = 0;
	media_check(ctx, fy_is_sequence(content), err,
		    "import: the content of a message is not a sequence");
	fy_foreach(part, content) {
		media_check(ctx, fy_is_mapping(part), err,
			    "import: a content part is not a mapping");
		if (fy_equal(fy_get(part, "type"), FYAI_MEDIA_PART_TEXT)) {
			media_check(ctx, fy_is_string(fy_get(part, "text")), err,
				    "import: a text part has no text");
			out = fy_append(gb, out, part);
			continue;
		}
		media_check(ctx, fyai_media_part_is_image(part), err,
			    "import: a content part is neither text nor an "
			    "image");
		count++;
		media_check(ctx, ctx->cfg->media_max_per_message <= 0 ||
			    count <= (size_t)ctx->cfg->media_max_per_message,
			    err, "import: a message takes at most %d attachments",
			    ctx->cfg->media_max_per_message);
		media_check(ctx, fyai_media_part_has_bytes(part), err,
			    "import: an image part carries no bytes");
		data = fy_get(part, "data");
		raw = fy_castp(&data, fy_szstr_empty);
		media_check(ctx, limit <= 0 || raw.size <= (uint64_t)limit, err,
			    "import: an image holds %zu bytes, more than the "
			    "%lld byte limit", raw.size, limit);
		name = fy_get(part, "name", fy_invalid);
		media_check(ctx, fy_is_invalid(name) || fy_is_string(name), err,
			    "import: the name of an image is not a string");
		rc = fyai_media_image_bytes(ctx, gb, raw.data, raw.size,
					    fy_get(part, "media_type", ""),
					    fy_is_string(name) ?
						fy_castp(&name, "") : NULL,
					    &image);
		if (rc)
			goto err;
		canonical = fyai_media_image_part(gb, &image);
		fyai_media_image_release(&image);
		media_check(ctx, fy_is_valid(canonical), err,
			    "import: could not record an image");
		for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
			media_check(ctx, fy_equal(fy_get(part, fields[i]),
						  fy_get(canonical, fields[i])),
				    err, "import: the %s of an image does not "
				    "match its bytes", fields[i]);
		out = fy_append(gb, out, canonical);
	}
	return out;

err:
	return fy_invalid;
}

fy_generic fyai_media_part_data_url(struct fyai_ctx *ctx,
				    struct fy_generic_builder *gb,
				    fy_generic part)
{
	const char *type;
	fy_generic out;
	char *b64;

	type = fy_get(part, "media_type", "");
	if (!*type) {
		fyai_error(ctx, "an image part names no media type");
		return fy_invalid;
	}
	b64 = fyai_media_part_base64(ctx, part);
	if (!b64)
		return fy_invalid;
	out = fy_stringf(gb, "data:%s;base64,%s", type, b64);
	free(b64);
	return out;
}

/*
 * The helpers that read a clipboard, in the order they are tried. Each takes
 * the type it must produce; a Wayland session has the first two, an X11
 * session the next two, and macOS the last. Nothing here reads a clipboard
 * itself: a terminal escape writes to one, and no reply carries its bytes.
 */
static const struct media_clipboard_cmd {
	const char *prog;		/* the program, or NULL for a custom one */
	const char *args;		/* what it takes to produce a PNG */
	const char *needs;		/* an environment variable that names it */
} media_clipboard_cmds[] = {
	{ "wl-paste", "--no-newline --type image/png", "WAYLAND_DISPLAY" },
	{ "wl-paste", "--no-newline --type image/bmp", "WAYLAND_DISPLAY" },
	{ "xclip", "-selection clipboard -t image/png -o", "DISPLAY" },
	{ "xsel", "--clipboard --output", "DISPLAY" },
	{ "pngpaste", "-", NULL },
};

/*
 * Run one helper and attach what it wrote. A helper that
 * wrote nothing reports why, so the caller can name each attempt.
 */
static int media_clipboard_try(struct fyai_ctx *ctx, const char *command,
			       const char *what, char *why, size_t whysz)
{
	static const char *const env_keep[] = {
		"DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XAUTHORITY", NULL,
	};
	struct shell_command_opts opts = { .env_keep = env_keep };
	struct shell_command_result result;
	int rc = -1;
	size_t len;

	opts.timeout_ms = ctx->cfg->media_clipboard_timeout_ms > 0 ?
			  (unsigned int)ctx->cfg->media_clipboard_timeout_ms : 0;
	if (run_shell_command_capture_cb(ctx, command, &result, NULL, NULL,
					 NULL, &opts)) {
		snprintf(why, whysz, "%s could not run", what);
		shell_command_result_cleanup(&result);
		return -1;
	}
	len = result.stdout_len;
	if (result.timed_out) {
		snprintf(why, whysz, "%s took longer than %d ms", what,
			 ctx->cfg->media_clipboard_timeout_ms);
		goto out;
	}
	if (!len) {
		snprintf(why, whysz, "%s wrote nothing", what);
		goto out;
	}
	rc = fyai_media_attach_bytes(ctx, result.stdout_data, len, NULL,
				     "clipboard");
	if (rc)
		snprintf(why, whysz, "%s did not write an image", what);
out:
	shell_command_result_cleanup(&result);
	return rc;
}

/*
 * A configured clipboard command replaces helper detection. Otherwise, try
 * each helper available for the display and report the cause of each failure.
 */
int fyai_media_attach_clipboard(struct fyai_ctx *ctx)
{
	const struct media_clipboard_cmd *cmd;
	char attempts[512] = "";
	char why[192];
	char *line = NULL;
	const char *env;
	size_t used = 0;
	int rc;

	env = ctx->cfg->media_clipboard_command;
	if (env && *env)
		return media_clipboard_try(ctx, env, "the clipboard command",
					   why, sizeof(why)) ?
		       fyai_error(ctx, "%s", why), -1 : 0;

	for (cmd = media_clipboard_cmds;
	     cmd < media_clipboard_cmds +
		   sizeof(media_clipboard_cmds) / sizeof(media_clipboard_cmds[0]);
	     cmd++) {
		/* A helper of a display server that is not running cannot work. */
		if (cmd->needs && !getenv(cmd->needs))
			continue;
		rc = asprintf(&line, "%s %s", cmd->prog, cmd->args);
		if (rc < 0)
			break;
		why[0] = '\0';
		rc = media_clipboard_try(ctx, line, cmd->prog, why, sizeof(why));
		free(line);
		line = NULL;
		if (!rc)
			return 0;
		/* The first error of each attempt is dropped: the summary below
		 * says what each helper did, which is what a user can act on. */
		fyai_diag_reset(fyai_ctx_diag(ctx));
		if (used < sizeof(attempts)) {
			rc = snprintf(attempts + used, sizeof(attempts) - used,
				      "%s%s: %s", used ? "; " : "",
				      cmd->prog, why);
			if (rc > 0)
				used += (size_t)rc;
		}
	}
	fyai_error(ctx, "could not read an image from the clipboard%s%s",
		   used ? "; " : "", attempts);
	return -1;
}
