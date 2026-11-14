/*
 * fyai_media_test.c - unit tests for message attachments
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai_media.h"
#include "fyai.h"
#include "fyai_turn.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"
#include "fyai_test_scratch.h"
#include "fyai_test_diag.h"

FYAI_TEST_ENTRY(media, sniff_types, media_sniff_types)
FYAI_TEST_ENTRY(media, dimensions, media_dimensions)
FYAI_TEST_ENTRY(media, type_names, media_type_names)
FYAI_TEST_ENTRY(media, content_parts, media_content_parts)
FYAI_TEST_ENTRY(media, content_references, media_content_references)
FYAI_TEST_ENTRY(media, rows_styled, media_rows_styled)
FYAI_TEST_ENTRY(media, image_roundtrip, media_image_roundtrip)
FYAI_TEST_ENTRY(media, image_limits, media_image_limits)
FYAI_TEST_ENTRY(media, import_content, media_import_content)
FYAI_TEST_ENTRY(media, pending_message, media_pending_message)
FYAI_TEST_ENTRY(media, pinned_root, media_pinned_root)
FYAI_TEST_ENTRY(media, mentions, media_mentions)

/*
 * Headers of each type, with the size in them: PNG 1024x768, JPEG 640x480,
 * GIF 32x16, WebP 800x600. Only the header is needed to type and size an
 * image, so no decoder runs.
 */
static const unsigned char png_head[] = {
	0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
	0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R',
	0x00, 0x00, 0x04, 0x00,	/* width 1024 */
	0x00, 0x00, 0x03, 0x00,	/* height 768 */
	0x08, 0x06, 0x00, 0x00, 0x00,
};

static const unsigned char jpeg_head[] = {
	0xff, 0xd8,			/* SOI */
	0xff, 0xe0, 0x00, 0x04, 0x00, 0x00,	/* APP0, 4 bytes */
	0xff, 0xc0, 0x00, 0x11, 0x08,		/* SOF0, 17 bytes, precision */
	0x01, 0xe0,			/* height 480 */
	0x02, 0x80,			/* width 640 */
	0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01,
	0xff, 0xd9,			/* EOI */
};

static const unsigned char gif_head[] = {
	'G', 'I', 'F', '8', '9', 'a',
	0x20, 0x00,			/* width 32 */
	0x10, 0x00,			/* height 16 */
	0x00, 0x00, 0x00,
};

static const unsigned char webp_head[] = {
	'R', 'I', 'F', 'F',
	0x2a, 0x00, 0x00, 0x00,		/* file size */
	'W', 'E', 'B', 'P',
	'V', 'P', '8', 'X',
	0x0a, 0x00, 0x00, 0x00,		/* chunk size */
	0x00, 0x00, 0x00, 0x00,		/* flags and reserved */
	0x1f, 0x03, 0x00,		/* canvas width - 1 = 799 */
	0x57, 0x02, 0x00,		/* canvas height - 1 = 599 */
};

int media_sniff_types(void)
{
	enum fyai_media_type type;
	int rc;

	type = FYAI_MEDIA_TYPE_UNKNOWN;
	rc = fyai_media_sniff(png_head, sizeof(png_head), &type);
	FYAI_TCHECK(!rc && type == FYAI_MEDIA_TYPE_PNG);
	rc = fyai_media_sniff(jpeg_head, sizeof(jpeg_head), &type);
	FYAI_TCHECK(!rc && type == FYAI_MEDIA_TYPE_JPEG);
	rc = fyai_media_sniff(gif_head, sizeof(gif_head), &type);
	FYAI_TCHECK(!rc && type == FYAI_MEDIA_TYPE_GIF);
	rc = fyai_media_sniff(webp_head, sizeof(webp_head), &type);
	FYAI_TCHECK(!rc && type == FYAI_MEDIA_TYPE_WEBP);

	/* Text, a truncated signature, and nothing at all are not images. */
	rc = fyai_media_sniff("hello", 5, &type);
	FYAI_TCHECK(rc < 0 && errno == ENOTSUP);
	rc = fyai_media_sniff(png_head, 4, &type);
	FYAI_TCHECK(rc < 0 && errno == ENOTSUP);
	rc = fyai_media_sniff(NULL, 0, &type);
	FYAI_TCHECK(rc < 0 && errno == ENOTSUP);
	return 0;
}

int media_dimensions(void)
{
	unsigned int width, height;
	int rc;

	rc = fyai_media_dimensions(png_head, sizeof(png_head),
				   FYAI_MEDIA_TYPE_PNG, &width, &height);
	FYAI_TCHECK(!rc && width == 1024 && height == 768);
	rc = fyai_media_dimensions(jpeg_head, sizeof(jpeg_head),
				   FYAI_MEDIA_TYPE_JPEG, &width, &height);
	FYAI_TCHECK(!rc && width == 640 && height == 480);
	rc = fyai_media_dimensions(gif_head, sizeof(gif_head),
				   FYAI_MEDIA_TYPE_GIF, &width, &height);
	FYAI_TCHECK(!rc && width == 32 && height == 16);
	rc = fyai_media_dimensions(webp_head, sizeof(webp_head),
				   FYAI_MEDIA_TYPE_WEBP, &width, &height);
	FYAI_TCHECK(!rc && width == 800 && height == 600);

	/* A header that stops before the size is not a size. */
	rc = fyai_media_dimensions(png_head, 20, FYAI_MEDIA_TYPE_PNG,
				   &width, &height);
	FYAI_TCHECK(rc < 0 && errno == EINVAL);
	rc = fyai_media_dimensions(gif_head, 8, FYAI_MEDIA_TYPE_GIF,
				   &width, &height);
	FYAI_TCHECK(rc < 0 && errno == EINVAL);
	rc = fyai_media_dimensions(webp_head, 24, FYAI_MEDIA_TYPE_WEBP,
				   &width, &height);
	FYAI_TCHECK(rc < 0 && errno == EINVAL);
	return 0;
}

int media_type_names(void)
{
	FYAI_TCHECK(!strcmp(fyai_media_type_name(FYAI_MEDIA_TYPE_PNG),
			    "image/png"));
	FYAI_TCHECK(!fyai_media_type_name(FYAI_MEDIA_TYPE_UNKNOWN));
	FYAI_TCHECK(fyai_media_type_parse("image/png") == FYAI_MEDIA_TYPE_PNG);
	FYAI_TCHECK(fyai_media_type_parse("PNG") == FYAI_MEDIA_TYPE_PNG);
	FYAI_TCHECK(fyai_media_type_parse("image/jpeg") == FYAI_MEDIA_TYPE_JPEG);
	FYAI_TCHECK(fyai_media_type_parse("jpg") == FYAI_MEDIA_TYPE_JPEG);
	FYAI_TCHECK(fyai_media_type_parse("image/webp") == FYAI_MEDIA_TYPE_WEBP);
	FYAI_TCHECK(fyai_media_type_parse("image/tiff") ==
		    FYAI_MEDIA_TYPE_UNKNOWN);
	FYAI_TCHECK(fyai_media_type_parse("") == FYAI_MEDIA_TYPE_UNKNOWN);
	FYAI_TCHECK(fyai_media_type_parse(NULL) == FYAI_MEDIA_TYPE_UNKNOWN);
	FYAI_TCHECK(fyai_media_type_is_image(FYAI_MEDIA_TYPE_GIF));
	FYAI_TCHECK(!fyai_media_type_is_image(FYAI_MEDIA_TYPE_UNKNOWN));
	return 0;
}

/* A canonical image part keeps the reference and the numbers a reader shows. */
int media_content_parts(void)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_SCOPE_LEADER,
	};
	struct fyai_media_image image;
	struct fy_generic_builder *gb;
	fy_generic content, part, text, images;
	const char *s;

	gb = fy_generic_builder_create(&gb_cfg);
	FYAI_TCHECK(gb);

	memset(&image, 0, sizeof(image));
	image.type = FYAI_MEDIA_TYPE_PNG;
	image.width = 1024;
	image.height = 768;
	image.bytes = 98304;
	strcpy(image.digest, "0123456789abcdef0123456789abcdef"
			     "0123456789abcdef0123456789abcdef");
	image.name = strdup("screenshot");
	FYAI_TCHECK(image.name);

	part = fyai_media_image_part(gb, &image);
	FYAI_TCHECK(fy_is_mapping(part));
	FYAI_TCHECK(fyai_media_part_is_image(part));
	s = fy_get(part, "media_type", "");
	FYAI_TCHECK(!strcmp(s, "image/png"));
	s = fy_get(part, "digest", "");
	FYAI_TCHECK(!strcmp(s, image.digest));
	FYAI_TCHECK(fy_get(part, "width", 0LL) == 1024);
	FYAI_TCHECK(fy_get(part, "height", 0LL) == 768);
	FYAI_TCHECK(fy_get(part, "bytes", 0LL) == 98304);
	s = fy_get(part, "name", "");
	FYAI_TCHECK(!strcmp(s, "screenshot"));

	/* A text run and a foreign part are not images. */
	FYAI_TCHECK(!fyai_media_part_is_image(
			fy_mapping(gb, "type", "text", "text", "hello")));
	FYAI_TCHECK(!fyai_media_part_is_image(
			fy_mapping(gb, "type", "tool_result")));
	FYAI_TCHECK(!fyai_media_part_is_image(fy_invalid));
	FYAI_TCHECK(!fyai_media_part_is_image(fy_value("hi")));

	/* A string content is its own text, and carries no image. */
	content = fy_value("plain words");
	text = fyai_media_text(gb, content);
	s = fy_castp(&text, "");
	FYAI_TCHECK(!strcmp(s, "plain words"));
	FYAI_TCHECK(!fyai_media_content_has_images(content));
	FYAI_TCHECK(fy_empty(fyai_media_parts(gb, content)));

	/* A sequence gives its text runs joined, and its image parts in order. */
	content = fy_sequence(gb,
		fy_mapping(gb, "type", "text", "text", "look at this"),
		part,
		fy_mapping(gb, "type", "text", "text", ": done"));
	FYAI_TCHECK(fyai_media_content_has_images(content));
	text = fyai_media_text(gb, content);
	s = fy_castp(&text, "");
	FYAI_TCHECK(!strcmp(s, "look at this: done"));
	images = fyai_media_parts(gb, content);
	FYAI_TCHECK(fy_is_sequence(images) && fy_len(images) == 1);
	FYAI_TCHECK(fy_equal(fy_get_at(images, 0), part));

	/* A null content has no text and no image. */
	text = fyai_media_text(gb, fy_null);
	FYAI_TCHECK(fy_is_string(text) && fy_empty(text));
	FYAI_TCHECK(!fyai_media_content_has_images(fy_null));

	free(image.name);
	fy_generic_builder_destroy(gb);
	return 0;
}

/*
 * The row a transcript shows and an export writes: the alt text describes the
 * image from the reference alone, and the link names the image by digest. The
 * same helper answers for a content and for the pending set of a message.
 */
int media_content_references(void)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_SCOPE_LEADER,
	};
	struct fyai_media_image image;
	struct fy_generic_builder *gb;
	fy_generic content, part, text, row;
	const char *s;

	gb = fy_generic_builder_create(&gb_cfg);
	FYAI_TCHECK(gb);

	memset(&image, 0, sizeof(image));
	image.type = FYAI_MEDIA_TYPE_PNG;
	image.width = 1024;
	image.height = 768;
	image.bytes = 98304;
	strcpy(image.digest, "0123456789abcdef0123456789abcdef"
			     "0123456789abcdef0123456789abcdef");
	image.name = strdup("screenshot");
	FYAI_TCHECK(image.name);
	part = fyai_media_image_part(gb, &image);
	FYAI_TCHECK(fy_is_valid(part));

	row = fyai_media_part_reference(gb, part);
	s = fy_castp(&row, "");
	FYAI_TCHECK(!strcmp(s, "![image: PNG 1024x768, 96 KiB]"
			    "(fyai-media:0123456789abcdef0123456789abcdef"
			    "0123456789abcdef0123456789abcdef)"));

	/* The text run comes first, and each image follows it on its own line. */
	content = fy_sequence(gb,
		fy_mapping(gb, "type", "text", "text", "look at this"),
		part);
	text = fyai_media_text_references(gb, content);
	s = fy_castp(&text, "");
	FYAI_TCHECK(!strcmp(s, "look at this\n\n![image: PNG 1024x768, 96 KiB]"
			    "(fyai-media:0123456789abcdef0123456789abcdef"
			    "0123456789abcdef0123456789abcdef)"));

	/* A content with no image is its plain text, with no row. */
	text = fyai_media_text_references(gb, fy_value("plain words"));
	s = fy_castp(&text, "");
	FYAI_TCHECK(!strcmp(s, "plain words"));

	/* A part that is not an image has no row. */
	FYAI_TCHECK(!fy_is_valid(fyai_media_part_reference(gb,
			fy_mapping(gb, "type", "text", "text", "hello"))));

	free(image.name);
	fy_generic_builder_destroy(gb);
	return 0;
}

/*
 * On a card, a whole reference row is bold and in the ref role; a row inside
 * other words, or a link that is not an attachment, is left as it is.
 */
int media_rows_styled(void)
{
	static const struct {
		const char *in;
		const char *out;
	} cases[] = {
		{ "look\n![image: PNG 1x1, 29 B](fyai-media:ab)",
		  "look\n**<fy-role name=\"ref\">![image: PNG 1x1, 29 B](fyai-media:ab)</fy-role>**" },
		{ "![image: GIF 2x2, 1 KiB](fyai-media:cd)\n",
		  "**<fy-role name=\"ref\">![image: GIF 2x2, 1 KiB](fyai-media:cd)</fy-role>**\n" },
		{ "![image: a picture](https://example.com/a.png)",
		  "![image: a picture](https://example.com/a.png)" },
		{ "see ![image: PNG 1x1, 29 B](fyai-media:ab) here",
		  "see ![image: PNG 1x1, 29 B](fyai-media:ab) here" },
		{ "", "" },
	};
	char *out;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		out = fyai_media_rows_styled(cases[i].in);
		FYAI_TCHECK(out);
		FYAI_TCHECK(!strcmp(out, cases[i].out));
		free(out);
	}
	FYAI_TCHECK(!fyai_media_rows_styled(NULL));
	return 0;
}

/*
 * An imported content is rebuilt from the bytes of each image: a part that
 * went through YAML and back is accepted, and a part whose bytes are missing
 * or whose metadata does not match them is refused.
 */
int media_import_content(void)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_SCOPE_LEADER,
	};
	struct fyai_media_image image;
	struct fyai_cfg cfg;
	struct fyai_ctx ctx;
	fy_generic part, ref, content, emitted, parsed, out, data, file;
	fy_generic_sized_string raw;
	char *text;
	int rc;

	memset(&cfg, 0, sizeof(cfg));
	memset(&ctx, 0, sizeof(ctx));
	ctx.cfg = &cfg;
	fyai_test_arena_of(&ctx)->gb = fyai_test_transient_of(&ctx)->gb = fyai_test_arena_of(&ctx)->durable_gb =
		fy_generic_builder_create(&gb_cfg);
	FYAI_TCHECK(fyai_test_arena_of(&ctx)->gb);
	cfg.gb = fyai_test_arena_of(&ctx)->gb;
	cfg.media_max_bytes = 1024;
	cfg.media_max_per_message = 2;
	rc = fyai_test_diag_setup(&cfg);
	FYAI_TCHECK(!rc);

	rc = fyai_media_image_bytes(&ctx, fyai_test_arena_of(&ctx)->gb, png_head, sizeof(png_head),
				    NULL, "shot", &image);
	FYAI_TCHECK(!rc);
	part = fyai_media_image_part(fyai_test_arena_of(&ctx)->gb, &image);
	/* The same part without its bytes. */
	image.data = fy_invalid;
	ref = fyai_media_image_part(fyai_test_arena_of(&ctx)->gb, &image);
	fyai_media_image_release(&image);
	FYAI_TCHECK(fyai_media_part_has_bytes(part));
	FYAI_TCHECK(fyai_media_part_is_image(ref));
	FYAI_TCHECK(!fyai_media_part_has_bytes(ref));
	content = fy_sequence(fyai_test_arena_of(&ctx)->gb,
		fy_mapping(fyai_test_arena_of(&ctx)->gb, "type", "text", "text", "look"), part);

	/* The bytes are !!binary in YAML and read back as the same bytes. */
	emitted = fy_emit(fyai_test_arena_of(&ctx)->gb, content, FYOPEF_DISABLE_DIRECTORY |
			  FYOPEF_MODE_YAML_1_2 | FYOPEF_STYLE_PRETTY |
			  FYOPEF_BINARY_BASE64, NULL);
	FYAI_TCHECK(fy_is_string(emitted));
	parsed = fy_parse(fyai_test_arena_of(&ctx)->gb, emitted, FYOPPF_DISABLE_DIRECTORY |
			  FYOPPF_MODE_YAML_1_2 | FYOPPF_INPUT_TYPE_STRING, NULL);
	FYAI_TCHECK(fy_is_sequence(parsed));
	out = fyai_media_import_content(&ctx, fyai_test_arena_of(&ctx)->gb, parsed);
	FYAI_TCHECK(fy_is_sequence(out) && fy_len(out) == 2);
	FYAI_TCHECK(fy_equal(fy_get_at_path(out, 0, "text"), "look"));
	FYAI_TCHECK(fy_equal(fy_get_at(out, 1), part));
	data = fy_get_at_path(out, 1, "data");
	raw = fy_castp(&data, fy_szstr_empty);
	FYAI_TCHECK(raw.size == sizeof(png_head));
	FYAI_TCHECK(!memcmp(raw.data, png_head, sizeof(png_head)));

	/*
	 * A file part holds its text in the part, as a text part does: an
	 * export writes it into the directive, and an import keeps it as it is.
	 */
	file = fy_mapping(fyai_test_arena_of(&ctx)->gb, "type", "text",
			  "text", "File: a.txt\n```\nx\n```", "file", "a.txt",
			  "bytes", 2LL, "lines", 1LL);
	out = fyai_media_import_content(&ctx, fyai_test_arena_of(&ctx)->gb, fy_sequence(fyai_test_arena_of(&ctx)->gb, file));
	FYAI_TCHECK(fy_is_sequence(out) && fy_equal(fy_get_at(out, 0), file));
	FYAI_TCHECK(fyai_media_part_is_file(fy_get_at(out, 0)));

	/* A digest that is not the digest of the bytes. */
	out = fyai_media_import_content(&ctx, fyai_test_arena_of(&ctx)->gb, fy_sequence(fyai_test_arena_of(&ctx)->gb,
		fy_assoc(fyai_test_arena_of(&ctx)->gb, part, "digest", "00")));
	FYAI_TCHECK(fy_is_invalid(out));
	text = fyai_diag_string(fyai_ctx_diag(&ctx));
	FYAI_TCHECK(text && strstr(text, "digest"));
	free(text);
	fyai_diag_reset(fyai_ctx_diag(&ctx));

	/* A size that the header of the bytes does not state. */
	out = fyai_media_import_content(&ctx, fyai_test_arena_of(&ctx)->gb, fy_sequence(fyai_test_arena_of(&ctx)->gb,
		fy_assoc(fyai_test_arena_of(&ctx)->gb, part, "width", 1LL)));
	FYAI_TCHECK(fy_is_invalid(out));
	fyai_diag_reset(fyai_ctx_diag(&ctx));

	/* A reference with no bytes. */
	out = fyai_media_import_content(&ctx, fyai_test_arena_of(&ctx)->gb, fy_sequence(fyai_test_arena_of(&ctx)->gb, ref));
	FYAI_TCHECK(fy_is_invalid(out));
	fyai_diag_reset(fyai_ctx_diag(&ctx));

	/* More bytes than the limit. */
	cfg.media_max_bytes = 8;
	out = fyai_media_import_content(&ctx, fyai_test_arena_of(&ctx)->gb, content);
	FYAI_TCHECK(fy_is_invalid(out));
	fyai_diag_reset(fyai_ctx_diag(&ctx));
	cfg.media_max_bytes = 1024;

	/* More images than one message takes. */
	out = fyai_media_import_content(&ctx, fyai_test_arena_of(&ctx)->gb,
		fy_sequence(fyai_test_arena_of(&ctx)->gb, part, part, part));
	FYAI_TCHECK(fy_is_invalid(out));
	fyai_diag_reset(fyai_ctx_diag(&ctx));

	fyai_test_diag_cleanup(&cfg);
	fy_generic_builder_destroy(fyai_test_arena_of(&ctx)->gb);
	return 0;
}

#ifdef __linux__
/* Write the bytes of @data to a scratch file and return its descriptor. */
static int media_scratch_image(const char *tag, const void *data, size_t len)
{
	const char *path;
	int fd;

	path = fyai_test_scratch_file(tag, ".bin");
	FYAI_TCHECK(path);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	FYAI_TCHECK(fd >= 0);
	FYAI_TCHECK(write(fd, data, len) == (ssize_t)len);
	FYAI_TCHECK(lseek(fd, 0, SEEK_SET) == 0);
	return fd;
}

/* Write the bytes of @data to the scratch file @path. */
static int media_scratch_write(const char *path, const void *data, size_t len)
{
	ssize_t wrote;
	int fd, rc;

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	FYAI_TCHECK(fd >= 0);
	wrote = write(fd, data, len);
	rc = wrote == (ssize_t)len ? 0 : -1;
	close(fd);
	return rc;
}

int media_image_roundtrip(void)
{
	struct fy_generic_builder *gb;
	struct fyai_media_image image;
	fy_generic data;
	fy_generic_sized_string raw;
	int fd, rc;

	gb = fy_generic_builder_create(NULL);
	FYAI_TCHECK(gb);
	fd = media_scratch_image("media-arena-png", png_head, sizeof(png_head));
	rc = fyai_media_image_read(NULL, gb, fd, 0, NULL, "clipboard", &image);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(image.type == FYAI_MEDIA_TYPE_PNG);
	FYAI_TCHECK(image.width == 1024 && image.height == 768);
	FYAI_TCHECK(image.bytes == sizeof(png_head));
	FYAI_TCHECK(strlen(image.digest) == FYAI_MEDIA_DIGEST_SIZE - 1);
	FYAI_TCHECK(image.name && !strcmp(image.name, "clipboard"));
	data = image.data;
	raw = fy_castp(&data, fy_szstr_empty);
	FYAI_TCHECK(raw.size == sizeof(png_head));
	FYAI_TCHECK(!memcmp(raw.data, png_head, sizeof(png_head)));
	fyai_media_image_release(&image);

	rc = fyai_media_image_read(NULL, gb, fd, 0, "image/png", NULL, &image);
	FYAI_TCHECK(!rc && fy_equal(data, image.data));
	rc = fyai_media_image_read(NULL, gb, fd, 0, "image/jpeg", NULL, &image);
	FYAI_TCHECK(rc < 0);
	close(fd);
	fy_generic_builder_destroy(gb);
	return 0;
}

int media_image_limits(void)
{
	struct fy_generic_builder *gb;
	struct fyai_media_image image;
	unsigned char zero_width[sizeof(png_head)];
	int fd, rc;

	gb = fy_generic_builder_create(NULL);
	FYAI_TCHECK(gb);
	fd = media_scratch_image("media-limits-png", png_head, sizeof(png_head));
	rc = fyai_media_image_read(NULL, gb, fd, 8, NULL, NULL, &image);
	FYAI_TCHECK(rc < 0);
	close(fd);
	fd = media_scratch_image("media-limits-text", "hello", 5);
	rc = fyai_media_image_read(NULL, gb, fd, 0, NULL, NULL, &image);
	FYAI_TCHECK(rc < 0);
	close(fd);
	fd = media_scratch_image("media-limits-empty", "", 0);
	rc = fyai_media_image_read(NULL, gb, fd, 0, NULL, NULL, &image);
	FYAI_TCHECK(rc < 0 && !image.digest[0] && !image.name);
	close(fd);
	fd = open(fyai_test_scratch_dir("media-limits-dir"),
		  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	FYAI_TCHECK(fd >= 0);
	rc = fyai_media_image_read(NULL, gb, fd, 0, NULL, NULL, &image);
	FYAI_TCHECK(rc < 0 && !image.digest[0] && !image.name);
	close(fd);
	memcpy(zero_width, png_head, sizeof(zero_width));
	memset(zero_width + 16, 0, 4);
	fd = media_scratch_image("media-limits-zero-width", zero_width, sizeof(zero_width));
	rc = fyai_media_image_read(NULL, gb, fd, 0, NULL, NULL, &image);
	FYAI_TCHECK(rc < 0 && !image.digest[0] && !image.name);
	close(fd);
	fy_generic_builder_destroy(gb);
	return 0;
}
/*
 * An attachment waits in the context and the next user message carries it, as
 * a text run and an image part. The message after it is text only, because an
 * attachment travels with one message.
 */
int media_pending_message(void)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_SCOPE_LEADER,
	};
	struct fyai_cfg cfg;
	struct fyai_ctx ctx;
	fy_generic msg, content, text, part;
	const char *path;
	int rc;

	path = fyai_test_scratch_file("media-pending-png", ".png");
	FYAI_TCHECK(path);
	rc = media_scratch_write(path, png_head, sizeof(png_head));
	FYAI_TCHECK(!rc);

	memset(&cfg, 0, sizeof(cfg));
	memset(&ctx, 0, sizeof(ctx));
	ctx.cfg = &cfg;
	fyai_test_arena_of(&ctx)->gb = fyai_test_transient_of(&ctx)->gb = fyai_test_arena_of(&ctx)->durable_gb =
		fy_generic_builder_create(&gb_cfg);
	FYAI_TCHECK(fyai_test_arena_of(&ctx)->gb);
	cfg.gb = fyai_test_arena_of(&ctx)->gb;
	cfg.media_max_bytes = 1024 * 1024;
	cfg.media_max_per_message = 2;
	ctx.media_pending = fy_seq_empty;

	rc = fyai_media_attach_path(&ctx, path, NULL, "shot");
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(fyai_media_pending_any(&ctx));

	msg = fyai_make_user_message(&ctx, "what is wrong here?");
	FYAI_TCHECK(fy_is_mapping(msg));
	FYAI_TCHECK(fy_equal(fy_get(msg, "role"), "user"));
	content = fy_get(msg, "content");
	FYAI_TCHECK(fy_is_sequence(content) && fy_len(content) == 2);
	FYAI_TCHECK(fy_equal(fy_get_at_path(content, 0, "type"), "text"));
	FYAI_TCHECK(fy_equal(fy_get_at_path(content, 0, "text"),
			     "what is wrong here?"));
	FYAI_TCHECK(fyai_media_part_is_image(fy_get_at(content, 1)));
	FYAI_TCHECK(fy_equal(fy_get_at_path(content, 1, "media_type"),
			     "image/png"));
	part = fy_get_at(content, 1);
	FYAI_TCHECK(fy_get(part, "width", 0LL) == 1024);
	FYAI_TCHECK(fy_equal(fy_get_at_path(content, 1, "name"), "shot"));

	/* The readers in the rest of the program still see the words. */
	text = fyai_media_text(fyai_test_arena_of(&ctx)->gb, content);
	FYAI_TCHECK(fy_equal(text, "what is wrong here?"));
	FYAI_TCHECK(fyai_media_content_has_images(content));
	FYAI_TCHECK(!fyai_media_pending_any(&ctx));

	/* The next message carries no attachment. */
	msg = fyai_make_user_message(&ctx, "and now?");
	FYAI_TCHECK(fy_is_string(fy_get(msg, "content")));

	/* An image with no words makes one part, not an empty text run. */
	rc = fyai_media_attach_path(&ctx, path, "image/png", "again");
	FYAI_TCHECK(!rc);
	msg = fyai_make_user_message(&ctx, "");
	content = fy_get(msg, "content");
	FYAI_TCHECK(fy_is_sequence(content) && fy_len(content) == 1);
	FYAI_TCHECK(fyai_media_part_is_image(fy_get_at(content, 0)));

	/* Two are allowed, a third is refused with a cause. */
	rc = fyai_media_attach_path(&ctx, path, NULL, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_media_attach_path(&ctx, path, NULL, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_media_attach_path(&ctx, path, NULL, NULL);
	FYAI_TCHECK(rc < 0);
	FYAI_TCHECK(fy_len(ctx.media_pending) == 2);
	fyai_diag_reset(fyai_ctx_diag(&ctx));
	fyai_media_pending_clear(&ctx);

	fy_generic_builder_destroy(fyai_test_arena_of(&ctx)->gb);
	return 0;
}

/*
 * A pinned root reads one published state, so a message that takes an
 * attachment would be a write. The attachment is refused before it is read.
 */
int media_pinned_root(void)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_SCOPE_LEADER,
	};
	struct fyai_cfg cfg;
	struct fyai_ctx ctx;
	const char *path;
	char line[1024];
	int rc;

	path = fyai_test_scratch_file("media-pinned-png", ".png");
	FYAI_TCHECK(path);
	rc = media_scratch_write(path, png_head, sizeof(png_head));
	FYAI_TCHECK(!rc);

	memset(&cfg, 0, sizeof(cfg));
	memset(&ctx, 0, sizeof(ctx));
	fyai_test_arena_of(&ctx)->gb = fyai_test_transient_of(&ctx)->gb = fyai_test_arena_of(&ctx)->durable_gb =
		fy_generic_builder_create(&gb_cfg);
	FYAI_TCHECK(fyai_test_arena_of(&ctx)->gb);
	cfg.gb = fyai_test_arena_of(&ctx)->gb;
	cfg.root_pinned = true;
	cfg.media_max_bytes = 1024 * 1024;
	cfg.media_max_per_message = 4;
	ctx.cfg = &cfg;
	ctx.media_pending = fy_seq_empty;

	rc = fyai_media_attach_path(&ctx, path, NULL, "shot");
	FYAI_TCHECK(rc < 0);
	FYAI_TCHECK(!fyai_media_pending_any(&ctx));

	/* a mention is an attachment too */
	snprintf(line, sizeof(line), "see @%s", path);
	FYAI_TCHECK(fyai_media_take_mentions(&ctx, line) < 0);
	FYAI_TCHECK(!fyai_media_pending_any(&ctx));

	fyai_diag_reset(fyai_ctx_diag(&ctx));
	fy_generic_builder_destroy(fyai_test_arena_of(&ctx)->gb);
	return 0;
}

/*
 * Take the mentions of @line with $HOME set to @home. It checks nothing in
 * between, so $HOME is restored on every path out.
 */
static int media_mentions_at_home(struct fyai_ctx *ctx, const char *home,
				  char *line)
{
	const char *old = getenv("HOME");
	char *saved = old ? strdup(old) : NULL;
	int rc;

	setenv("HOME", home, 1);
	rc = fyai_media_take_mentions(ctx, line);
	if (saved)
		setenv("HOME", saved, 1);
	else
		unsetenv("HOME");
	free(saved);
	return rc;
}

/*
 * @PATH mentions attach their files and leave the paths in the line: a text
 * file as a file part, an image as an image part. On an error nothing stays
 * attached. "\@" and "@@" are a literal "@", and @WORD of no file is text.
 */
int media_mentions(void)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_SCOPE_LEADER,
	};
	struct fyai_cfg cfg;
	struct fyai_ctx ctx;
	char line[2048], want[2048], notes[512], shot[512], blob[512];
	char fenced[512], empty[512];
	const char *dir;
	char *styled;
	fy_generic part, content, text;

	dir = fyai_test_scratch_dir("media-mentions");
	FYAI_TCHECK(dir);
	snprintf(notes, sizeof(notes), "%s/notes.txt", dir);
	snprintf(shot, sizeof(shot), "%s/my shot.png", dir);
	snprintf(blob, sizeof(blob), "%s/blob.bin", dir);
	FYAI_TCHECK(!media_scratch_write(notes, "a\nb\n", 4));
	FYAI_TCHECK(!media_scratch_write(shot, png_head, sizeof(png_head)));
	FYAI_TCHECK(!media_scratch_write(blob, "\x00\x01\x02", 3));
	snprintf(fenced, sizeof(fenced), "%s/fenced.txt", dir);
	snprintf(empty, sizeof(empty), "%s/empty.txt", dir);
	FYAI_TCHECK(!media_scratch_write(fenced, "x\n```\ny\n", 8));
	FYAI_TCHECK(!media_scratch_write(empty, "", 0));

	memset(&cfg, 0, sizeof(cfg));
	memset(&ctx, 0, sizeof(ctx));
	ctx.cfg = &cfg;
	fyai_test_arena_of(&ctx)->gb = fyai_test_transient_of(&ctx)->gb = fyai_test_arena_of(&ctx)->durable_gb =
		fy_generic_builder_create(&gb_cfg);
	FYAI_TCHECK(fyai_test_arena_of(&ctx)->gb);
	cfg.gb = fyai_test_arena_of(&ctx)->gb;
	cfg.media_max_bytes = 1024 * 1024;
	cfg.media_max_per_message = 8;
	ctx.media_pending = fy_seq_empty;
	FYAI_TCHECK(!fyai_test_diag_setup(&cfg));

	/* a text file and a quoted image path with a blank */
	snprintf(line, sizeof(line), "see @%s and @\"%s\"", notes, shot);
	snprintf(want, sizeof(want), "see %s and %s", notes, shot);
	FYAI_TCHECK(!fyai_media_take_mentions(&ctx, line));
	FYAI_TCHECK(!strcmp(line, want));
	FYAI_TCHECK(fy_len(ctx.media_pending) == 2);
	part = fy_get_at(ctx.media_pending, 0);
	FYAI_TCHECK(fyai_media_part_is_file(part));
	FYAI_TCHECK(fy_get(part, "lines", 0LL) == 2 && fy_get(part, "bytes", 0LL) == 4);
	FYAI_TCHECK(fyai_media_part_is_image(fy_get_at(ctx.media_pending, 1)));

	/* the readers: the words, what the model reads, and the row */
	content = fy_sequence(fyai_test_arena_of(&ctx)->gb,
		fy_mapping(fyai_test_arena_of(&ctx)->gb, "type", "text", "text", "hi"), part);
	FYAI_TCHECK(fy_equal(fyai_media_text(fyai_test_arena_of(&ctx)->gb, content), "hi"));
	text = fyai_media_model_text(fyai_test_arena_of(&ctx)->gb, content);
	FYAI_TCHECK(strstr(fy_castp(&text, ""), "hi\n\nFile: "));
	FYAI_TCHECK(strstr(fy_castp(&text, ""), "\n```\na\nb\n```"));
	text = fyai_media_text_references(fyai_test_arena_of(&ctx)->gb, content);
	FYAI_TCHECK(strstr(fy_castp(&text, ""), "hi\n\n[file: "));
	FYAI_TCHECK(strstr(fy_castp(&text, ""), ", 2 lines, 4 B]"));
	styled = fyai_media_rows_styled("[file: x, 1 line, 3 B]");
	FYAI_TCHECK(styled && !strcmp(styled,
		"**<fy-role name=\"ref\">[file: x, 1 line, 3 B]</fy-role>**"));
	free(styled);
	fyai_media_pending_clear(&ctx);

	/* the second mention fails: neither file stays attached */
	snprintf(line, sizeof(line), "x @%s @\"%s/none.txt\"", notes, dir);
	FYAI_TCHECK(fyai_media_take_mentions(&ctx, line) < 0);
	FYAI_TCHECK(!fyai_media_pending_any(&ctx));
	snprintf(line, sizeof(line), "x @%s", blob);
	FYAI_TCHECK(fyai_media_take_mentions(&ctx, line) < 0);
	FYAI_TCHECK(!fyai_media_pending_any(&ctx));
	fyai_diag_reset(fyai_ctx_diag(&ctx));

	/* literal "@", and words that name no file */
	snprintf(line, sizeof(line), "\\@a @@b c@d @nofile");
	FYAI_TCHECK(!fyai_media_take_mentions(&ctx, line));
	FYAI_TCHECK(!strcmp(line, "@a @b c@d @nofile"));
	FYAI_TCHECK(!fyai_media_pending_any(&ctx));

	/* ~/ is the home directory, and the row keeps the path as typed */
	snprintf(line, sizeof(line), "@~/notes.txt");
	FYAI_TCHECK(!media_mentions_at_home(&ctx, dir, line));
	FYAI_TCHECK(!strcmp(line, "~/notes.txt"));
	part = fy_get_at(ctx.media_pending, 0);
	FYAI_TCHECK(fy_equal(fy_get(part, "file"), "~/notes.txt"));
	fyai_media_pending_clear(&ctx);

	/* a text that holds a fence is wrapped in a longer one */
	snprintf(line, sizeof(line), "@%s", fenced);
	FYAI_TCHECK(!fyai_media_take_mentions(&ctx, line));
	part = fy_get_at(ctx.media_pending, 0);
	text = fy_get(part, "text", fy_invalid);
	snprintf(want, sizeof(want), "File: %s\n````\nx\n```\ny\n````", fenced);
	FYAI_TCHECK(!strcmp(fy_castp(&text, ""), want));
	fyai_media_pending_clear(&ctx);

	/* the same file twice is two parts, and each counts against the limit */
	snprintf(line, sizeof(line), "@%s @%s", notes, notes);
	FYAI_TCHECK(!fyai_media_take_mentions(&ctx, line));
	FYAI_TCHECK(fy_len(ctx.media_pending) == 2);
	fyai_media_pending_clear(&ctx);
	cfg.media_max_per_message = 1;
	snprintf(line, sizeof(line), "@%s @%s", notes, notes);
	FYAI_TCHECK(fyai_media_take_mentions(&ctx, line) < 0);
	FYAI_TCHECK(!fyai_media_pending_any(&ctx));
	cfg.media_max_per_message = 8;
	fyai_diag_reset(fyai_ctx_diag(&ctx));

	/* an empty file is a part of no lines */
	snprintf(line, sizeof(line), "@%s", empty);
	FYAI_TCHECK(!fyai_media_take_mentions(&ctx, line));
	part = fy_get_at(ctx.media_pending, 0);
	FYAI_TCHECK(fy_get(part, "lines", -1LL) == 0 && fy_get(part, "bytes", -1LL) == 0);
	fyai_media_pending_clear(&ctx);

	/* media/max_bytes lowers the limit of a text file */
	cfg.media_max_bytes = 3;
	snprintf(line, sizeof(line), "@%s", notes);
	FYAI_TCHECK(fyai_media_take_mentions(&ctx, line) < 0);
	cfg.media_max_bytes = 1024 * 1024;
	fyai_diag_reset(fyai_ctx_diag(&ctx));

	/* a quote that does not close is text, an empty quote is an error */
	snprintf(line, sizeof(line), "what does @\" mean?");
	FYAI_TCHECK(!fyai_media_take_mentions(&ctx, line));
	FYAI_TCHECK(!strcmp(line, "what does @\" mean?"));
	FYAI_TCHECK(!fyai_media_pending_any(&ctx));
	snprintf(line, sizeof(line), "see @\"\"");
	FYAI_TCHECK(fyai_media_take_mentions(&ctx, line) < 0);
	fyai_diag_reset(fyai_ctx_diag(&ctx));

	fyai_media_pending_clear(&ctx);
	fyai_test_diag_cleanup(&cfg);
	fy_generic_builder_destroy(fyai_test_arena_of(&ctx)->gb);
	return 0;
}

#else
int media_mentions(void)
{
	return 0;
}

int media_image_roundtrip(void)
{
	return 0;
}

int media_image_limits(void)
{
	return 0;
}

int media_pending_message(void)
{
	return 0;
}

int media_pinned_root(void)
{
	return 0;
}
#endif
