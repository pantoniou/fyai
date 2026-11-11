/* SPDX-License-Identifier: MIT */
/*
 * The WebSocket protocol of RFC 6455 as two pure parts: the key of the
 * handshake, and a codec that turns bytes into whole messages and messages
 * into frames. It reads and writes no descriptor and owns no event source.
 */
#ifndef FYAI_WS_H
#define FYAI_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum fyai_ws_opcode {
	FYAI_WS_CONTINUATION = 0x0,
	FYAI_WS_TEXT = 0x1,
	FYAI_WS_BINARY = 0x2,
	FYAI_WS_CLOSE = 0x8,
	FYAI_WS_PING = 0x9,
	FYAI_WS_PONG = 0xa,
};

/* The longest header of a frame: 2 bytes, 8 of length and 4 of mask. */
#define FYAI_WS_HEADER_MAX	14u

/*
 * The Sec-WebSocket-Accept value for the Sec-WebSocket-Key @key, in @out of
 * @size bytes. Returns 0, or -1 when @key is not 16 bytes in base64 or @out is
 * too small. 29 bytes hold the value and its end.
 */
int fyai_ws_accept_key(const char *key, char *out, size_t size);

/*
 * The header of a frame of @len payload bytes in @hdr. With @mask_key the
 * frame is masked, as every frame of a client is: the caller then applies
 * fyai_ws_mask() to the payload with the same key. Returns the header size.
 */
size_t fyai_ws_header(unsigned char hdr[FYAI_WS_HEADER_MAX], int opcode,
		      bool fin, size_t len, const unsigned char *mask_key);

/* XOR @len bytes with @key, which starts at byte @offset of the payload. */
void fyai_ws_mask(unsigned char *data, size_t len, const unsigned char key[4],
		  size_t offset);

/*
 * Write one whole frame of @n payload bytes to @out, which holds
 * FYAI_WS_HEADER_MAX + @n bytes. With @mask_key the frame is masked, as every
 * frame of a client is. Returns the bytes written.
 */
size_t fyai_ws_encode(unsigned char *out, int opcode, const void *data, size_t n,
		      const unsigned char *mask_key);

/* The handshake as text. @key is the Sec-WebSocket-Key; it takes 25 bytes. */
#define FYAI_WS_KEY_SIZE	25u

/*
 * The upgrade request of a client for @path on @authority, with an
 * Authorization: Bearer header when @bearer is not NULL. @nonce is 16 random
 * bytes; the key that they make goes to @key, where the check of the answer
 * reads it. Returns the length of the request in @out, or -1 when it does not
 * fit.
 */
int fyai_ws_client_request(const char *path, const char *authority,
			   const char *bearer, const unsigned char nonce[16],
			   char key[FYAI_WS_KEY_SIZE], char *out, size_t size);

/*
 * The 101 answer of a server to the Sec-WebSocket-Key @key. Returns its
 * length in @out, or -1 when @key is not valid or the answer does not fit.
 */
int fyai_ws_server_response(const char *key, char *out, size_t size);

/*
 * Check the answer @head, a request head that ends at its blank line, to the
 * request that sent @key. Returns 0 when the server switched protocols, the
 * HTTP status when it gave another, -1 when the status line is not HTTP/1.1,
 * -2 when it gave no Sec-WebSocket-Accept, and -3 when the value is wrong.
 */
int fyai_ws_client_check(const char *head, const char *key);

/*
 * The receiver. @server is true for the end that reads the frames of a
 * client, which must be masked. @max bounds a message, and a frame that would
 * pass it is a protocol error. Frames of one message are joined; a control
 * frame may stand among them.
 */
struct fyai_ws {
	bool server;
	size_t max;
	/* The state of the frame in progress. */
	unsigned char head[FYAI_WS_HEADER_MAX];
	size_t head_have, head_need;
	int opcode;		/* of the frame in progress */
	bool fin;
	bool masked;
	unsigned char key[4];
	uint64_t length, got;
	/* The message in progress. */
	int message;		/* TEXT or BINARY, or 0 between messages */
	unsigned char *data;
	size_t size, cap;
	unsigned char control[125];
};

struct fyai_ws_message {
	int opcode;		/* TEXT, BINARY, CLOSE, PING or PONG */
	const unsigned char *data;
	size_t size;		/* valid until the next call of fyai_ws_feed() */
};

void fyai_ws_init(struct fyai_ws *ws, bool server, size_t max);
void fyai_ws_release(struct fyai_ws *ws);

/*
 * Read from @in of @n bytes until one message is whole. Sets *@usedp to the
 * bytes taken and returns 1 with @msg, or 0 when @in ended inside one, or -1 on
 * a protocol error: reserved bits, an unknown opcode, a frame that is not
 * masked for a server or is masked for a client, a control frame that is long
 * or in pieces, a continuation with nothing to continue, a length that is not
 * the shortest form, or a message over the maximum. The call after a message
 * continues with the bytes that remain.
 */
int fyai_ws_feed(struct fyai_ws *ws, const unsigned char *in, size_t n,
		 size_t *usedp, struct fyai_ws_message *msg);

#endif
