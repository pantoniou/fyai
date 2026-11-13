/*
 * fyai_media.h - the attachments that travel with a message
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The bytes of an image are a binary string in the image part of the message
 * that carries it, so they live in the arena with the conversation (see
 * doc/media.md). This interface defines media types and canonical parts.
 * Provider code derives the wire form at the request boundary.
 */

#ifndef FYAI_MEDIA_H
#define FYAI_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;

/* The media types fyai recognises, from the bytes themselves. */
enum fyai_media_type {
	FYAI_MEDIA_TYPE_UNKNOWN,
	FYAI_MEDIA_TYPE_PNG,
	FYAI_MEDIA_TYPE_JPEG,
	FYAI_MEDIA_TYPE_GIF,
	FYAI_MEDIA_TYPE_WEBP,
};

/* The wire name of @type ("image/png"), or NULL for unknown. */
const char *fyai_media_type_name(enum fyai_media_type type);
/* The type that @name names, or FYAI_MEDIA_TYPE_UNKNOWN. */
enum fyai_media_type fyai_media_type_parse(const char *name);
/* Whether a model can take @type as an image part. */
bool fyai_media_type_is_image(enum fyai_media_type type);

/* The size of the hex digest of an image, with its NUL. */
#define FYAI_MEDIA_DIGEST_SIZE (32 * 2 + 1)

/* One image, as a message carries it. */
struct fyai_media_image {
	enum fyai_media_type type;
	unsigned int width;
	unsigned int height;
	uint64_t bytes;
	/* The bytes, a string in the builder that read them. */
	fy_generic data;
	/* The lowercase hex BLAKE3-256 digest of the bytes, with its NUL. */
	char digest[FYAI_MEDIA_DIGEST_SIZE];
	/* The name the caller gave it, owned here, or NULL. */
	char *name;
};

void fyai_media_image_release(struct fyai_media_image *image);

/*
 * Read the type of an image from its leading bytes. Return 0 and set @typep,
 * or -1 with errno set to ENOTSUP for bytes of no recognised type.
 */
int fyai_media_sniff(const void *data, size_t len, enum fyai_media_type *typep);

/*
 * Read the pixel size of an image from its leading bytes, which must hold its
 * header. Return 0 and set @widthp and @heightp, or -1 with errno set to
 * EINVAL when the bytes are truncated or hold no size.
 */
int fyai_media_dimensions(const void *data, size_t len,
			  enum fyai_media_type type,
			  unsigned int *widthp, unsigned int *heightp);

/*
 * Copy @len bytes at @data into @gb and fill @image. @declared is the media
 * type the caller states, or NULL to take the type of the bytes; a stated
 * type that names no supported type, or does not match the bytes, is an
 * error. @name is stored with the image, or NULL. A diagnostic names what
 * failed; @ctx may be NULL when there is nothing to report to.
 */
int fyai_media_image_bytes(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			   const void *data, size_t len, const char *declared,
			   const char *name, struct fyai_media_image *image);

/*
 * The same for the bytes of @fd, a regular file, read from its start. A file
 * larger than @max_bytes is refused; 0 removes the bound.
 */
int fyai_media_image_read(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			  int fd, uint64_t max_bytes, const char *declared,
			  const char *name, struct fyai_media_image *image);

/* How many bytes of the head of a file are read to type it and size it. */
#define FYAI_MEDIA_HEAD_MAX (256 * 1024)

/* The part type of an image, and of a text run of a content sequence. */
#define FYAI_MEDIA_PART_IMAGE	"image"
#define FYAI_MEDIA_PART_TEXT	"text"

/*
 * A canonical message carries one image as a part of its content sequence
 * (doc/media.md). Content readers use these helpers to extract text and image
 * parts.
 */

/* The canonical part of @image, built in @gb; it holds @image->data. */
fy_generic fyai_media_image_part(struct fy_generic_builder *gb,
				 const struct fyai_media_image *image);

/* Whether @part is an image part of a content sequence. */
bool fyai_media_part_is_image(fy_generic part);

/* Whether @content carries at least one image part. */
bool fyai_media_content_has_images(fy_generic content);

/*
 * The most bytes of a text file that a message carries inline. A smaller
 * media/max_bytes lowers it.
 */
#define FYAI_MEDIA_TEXT_MAX	(256 * 1024)

/*
 * Whether @part is the text of a file the user attached: a text part with
 * the name of the file, {type: text, text, file, bytes, lines}.
 */
bool fyai_media_part_is_file(fy_generic part);
/* Whether @content carries a file part. */
bool fyai_media_content_has_files(fy_generic content);
/* Whether @content carries an image or a file part. */
bool fyai_media_content_has_attachments(fy_generic content);

/*
 * What the model reads of @content, built in @gb: its words, then the text of
 * each file part, e.g. "File: notes.txt" over a fenced block. A string
 * content is returned borrowed.
 */
fy_generic fyai_media_model_text(struct fy_generic_builder *gb,
				 fy_generic content);

/*
 * The image parts of @content, in order, built in @gb. A string content and
 * a content with no image part give an empty sequence.
 */
fy_generic fyai_media_parts(struct fy_generic_builder *gb, fy_generic content);

/*
 * The text of @content, built in @gb: a string content itself, borrowed, or
 * the joined text parts of a sequence. An image part, a file part and a
 * provider-specific part contribute no text. Every reader that wants the
 * words of a message and not its media uses this function.
 *
 * This is not what a provider sends: the model also reads the text of each
 * file part. A reader that wants what the model reads uses
 * fyai_media_model_text().
 */
fy_generic fyai_media_text(struct fy_generic_builder *gb, fy_generic content);

/* "12 lines, 340 B" or "1 line, 2 B": the size of a file part for a person. */
void fyai_media_file_size(char *buf, size_t sz, fy_generic part);

/*
 * The Markdown reference that names @part: the row the transcript shows and
 * the line the prose of an export writes for an attachment. The digest is the
 * hook a later reader resolves; the alt text describes the image without
 * reading its bytes. A file part is "[file: NAME, 12 lines, 340 B]".
 */
fy_generic fyai_media_part_reference(struct fy_generic_builder *gb,
				     fy_generic part);

/*
 * The text of @content with one reference line after it for each image,
 * built in @gb. This is what a reader that cannot draw an attachment shows,
 * and what an export writes: the words of the message, then the images it
 * names.
 */
fy_generic fyai_media_text_references(struct fy_generic_builder *gb,
				      fy_generic content);

/*
 * A copy of @text in which each line that is a whole reference row, as
 * fyai_media_part_reference() writes it, is bold and in the ref role of the
 * palette: **<fy-role name="ref">![image: ...](fyai-media:...)</fy-role>**.
 * It is for a UI Markdown render of text that markdown_ui_escape() made safe;
 * what is stored and exported keeps the plain row. The caller frees it.
 */
char *fyai_media_rows_styled(const char *text);

/* Whether @part is an image part that carries its bytes. */
bool fyai_media_part_has_bytes(fy_generic part);

/*
 * The bytes of the image that @part carries, base64 encoded, or NULL with a
 * cause when the part has none. A grammar takes the bytes at the request
 * boundary and stores nothing of them, so a conversation that gained an image
 * on one grammar continues on another.
 */
char *fyai_media_part_base64(struct fyai_ctx *ctx, fy_generic part);
/*
 * The same bytes as a data URL string generic in @gb, which two grammars carry
 * the image in; fy_invalid with a cause on failure.
 */
fy_generic fyai_media_part_data_url(struct fyai_ctx *ctx,
				    struct fy_generic_builder *gb,
				    fy_generic part);

/*
 * The content sequence of an imported message, rebuilt in @gb. Each image
 * part must carry its bytes, and its type, size and digest must be those of
 * the bytes; the parts are made again from the bytes, so an import keeps
 * nothing that the bytes do not prove. fy_invalid with a cause otherwise.
 */
fy_generic fyai_media_import_content(struct fyai_ctx *ctx,
				     struct fy_generic_builder *gb,
				     fy_generic content);

/*
 * The attachments of one invocation. They wait in the context and the next
 * user message takes them, so a line typed after an attachment sends it, and
 * a line typed later does not send it again.
 */

/* Whether the next user message carries an attachment. */
bool fyai_media_pending_any(const struct fyai_ctx *ctx);
/* Drop the attachments that no message took. */
void fyai_media_pending_clear(struct fyai_ctx *ctx);
/* Take the pending image parts, and clear them. */
fy_generic fyai_media_take_pending(struct fyai_ctx *ctx);

/*
 * @text with one reference row after it for each pending attachment: what the
 * user card shows, and what the stored output records so an offline view says

 * the same thing. The result belongs to the transient builder and is valid
 * until it is released, or NULL when nothing is pending and the caller should
 * show @text itself.
 */
const char *fyai_media_pending_display(struct fyai_ctx *ctx, const char *text);

/*
 * Copy the bytes of @fd, or the file at @path, and add them to the next
 * user message. @declared is the type the caller states, or NULL; @name is
 * what the image is called, or NULL to take the file name.
 */
int fyai_media_attach_fd(struct fyai_ctx *ctx, int fd, const char *declared,
			 const char *name);
int fyai_media_attach_path(struct fyai_ctx *ctx, const char *path,
			   const char *declared, const char *name);

/*
 * Copy @len bytes at @data and add them to the next user message as an image.
 * @declared is the type the caller states, or NULL to take the type of the
 * bytes; @name is what the image is called, or NULL.
 */
int fyai_media_attach_bytes(struct fyai_ctx *ctx, const void *data,
			    size_t len, const char *declared, const char *name);

/* @path with a leading "~" or "~/" taken from $HOME, malloc'd, or NULL. */
char *fyai_media_path(const char *path);

/*
 * Add the file at @path to the next user message by what it holds: an image
 * as an image part, a UTF-8 text file of at most FYAI_MEDIA_TEXT_MAX bytes as
 * a file part that the model reads inline. Any other file is refused with a
 * cause. One open of the file decides the type, the size and the bytes.
 * @partp, when not NULL, gets the part that was added.
 *
 * With @quiet, a path that names no regular file adds nothing, reports
 * nothing and returns 1, so that the caller can take the word as text.
 * Otherwise 0, or -1 with a cause.
 */
int fyai_media_attach_file(struct fyai_ctx *ctx, const char *path, bool quiet,
			   fy_generic *partp);

/*
 * Take the @PATH mentions of @line, which a user typed. Each one names a file
 * that is attached as fyai_media_attach_file() does, and the path stays in
 * the line: "look at @shot.png" becomes "look at shot.png". A mention starts
 * the line or follows a blank, so "a@b.com" is text. A path with blanks is
 * quoted, @"my shot.png" or @'my shot.png', or escapes them, @my\ shot.png.
 * "\@" and "@@" are a literal "@". A bare @WORD that names no file stays as
 * typed; a quoted or escaped one is an error, and so is an empty one. A quote
 * that does not close makes no mention: the "@" is text. @line is rewritten
 * in place. Return 0, or -1 with a cause, and then nothing is attached.
 */
int fyai_media_take_mentions(struct fyai_ctx *ctx, char *line);

/*
 * Read the system clipboard as an image and add it to the next user message.
 * No terminal escape can read a clipboard, so this runs a helper program.
 * media/clipboard_command names one; otherwise the helpers this system has
 * are tried in turn, and a failure names what was tried and why each failed.
 */
int fyai_media_attach_clipboard(struct fyai_ctx *ctx);

#endif
