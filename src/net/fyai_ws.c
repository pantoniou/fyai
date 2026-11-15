/* SPDX-License-Identifier: MIT */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <openssl/sha.h>

#include "fyai_auth_util.h"
#include "fyai_ws.h"

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

int fyai_ws_accept_key(const char *key, char *out, size_t size)
{
	unsigned char digest[SHA_DIGEST_LENGTH], *raw;
	char text[64], *encoded;
	size_t raw_len = 0, n;

	if (!key || strlen(key) != 24)
		return -1;
	raw = fyai_base64_decode(key, &raw_len);
	if (!raw)
		return -1;
	free(raw);
	if (raw_len != 16)
		return -1;
	n = (size_t)snprintf(text, sizeof(text), "%s%s", key, WS_GUID);
	SHA1((const unsigned char *)text, n, digest);
	encoded = fyai_base64_encode(digest, sizeof(digest));
	if (!encoded)
		return -1;
	n = strlen(encoded);
	if (n + 1 > size) {
		free(encoded);
		return -1;
	}
	memcpy(out, encoded, n + 1);
	free(encoded);
	return 0;
}

size_t fyai_ws_header(unsigned char hdr[FYAI_WS_HEADER_MAX], int opcode,
		      bool fin, size_t len, const unsigned char *mask_key)
{
	size_t n = 2, i;
	unsigned char m = mask_key ? 0x80 : 0;

	hdr[0] = (unsigned char)((fin ? 0x80 : 0) | (opcode & 0x0f));
	if (len < 126) {
		hdr[1] = m | (unsigned char)len;
	} else if (len <= 0xffff) {
		hdr[1] = m | 126;
		hdr[2] = (unsigned char)(len >> 8);
		hdr[3] = (unsigned char)len;
		n = 4;
	} else {
		hdr[1] = m | 127;
		for (i = 0; i < 8; i++)
			hdr[2 + i] = (unsigned char)((uint64_t)len >> (56 - 8 * i));
		n = 10;
	}
	if (mask_key) {
		memcpy(hdr + n, mask_key, 4);
		n += 4;
	}
	return n;
}

size_t fyai_ws_encode(unsigned char *out, int opcode, const void *data, size_t n,
		      const unsigned char *mask_key)
{
	size_t h = fyai_ws_header(out, opcode, true, n, mask_key);

	if (n)
		memcpy(out + h, data, n);
	if (mask_key)
		fyai_ws_mask(out + h, n, mask_key, 0);
	return h + n;
}

int fyai_ws_client_request(const char *path, const char *authority,
			   const char *bearer, const unsigned char nonce[16],
			   char key[FYAI_WS_KEY_SIZE], char *out, size_t size)
{
	char *text = fyai_base64_encode(nonce, 16);
	char auth[1024] = "";
	int n;

	if (!text)
		return -1;
	if (strlen(text) >= FYAI_WS_KEY_SIZE) {
		free(text);
		return -1;
	}
	strcpy(key, text);
	free(text);
	if (bearer && snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n",
			       bearer) >= (int)sizeof(auth))
		return -1;
	n = snprintf(out, size, "GET %s HTTP/1.1\r\nHost: %s\r\n"
		     "Upgrade: websocket\r\nConnection: Upgrade\r\n"
		     "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n%s\r\n",
		     path, authority, key, auth);
	memset(auth, 0, sizeof(auth));
	return n < 0 || (size_t)n >= size ? -1 : n;
}

int fyai_ws_server_response(const char *key, char *out, size_t size)
{
	char accept[32];
	int n;

	if (fyai_ws_accept_key(key, accept, sizeof(accept)))
		return -1;
	n = snprintf(out, size, "HTTP/1.1 101 Switching Protocols\r\n"
		     "Upgrade: websocket\r\nConnection: Upgrade\r\n"
		     "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
	return n < 0 || (size_t)n >= size ? -1 : n;
}

int fyai_ws_client_check(const char *head, const char *key)
{
	static const char field[] = "\r\nSec-WebSocket-Accept:";
	char want[32];
	const char *accept;
	int status;

	if (strncmp(head, "HTTP/1.1 ", 9))
		return -1;
	status = atoi(head + 9);
	if (status != 101)
		return status > 0 ? status : -1;
	if (fyai_ws_accept_key(key, want, sizeof(want)))
		return -3;
	accept = strcasestr(head, field);
	if (!accept)
		return -2;
	accept += sizeof(field) - 1;
	while (*accept == ' ')
		accept++;
	return !strncmp(accept, want, strlen(want)) &&
	       accept[strlen(want)] == '\r' ? 0 : -3;
}

void fyai_ws_mask(unsigned char *data, size_t len, const unsigned char key[4],
		  size_t offset)
{
	size_t i;

	for (i = 0; i < len; i++)
		data[i] ^= key[(offset + i) & 3];
}

void fyai_ws_init(struct fyai_ws *ws, bool server, size_t max)
{
	memset(ws, 0, sizeof(*ws));
	ws->server = server;
	ws->max = max;
	ws->head_need = 2;
}

void fyai_ws_release(struct fyai_ws *ws)
{
	fyai_buf_free(&ws->buf);
}

static bool ws_opcode_valid(int op)
{
	return op == FYAI_WS_CONTINUATION || op == FYAI_WS_TEXT ||
	       op == FYAI_WS_BINARY || op == FYAI_WS_CLOSE ||
	       op == FYAI_WS_PING || op == FYAI_WS_PONG;
}

/* Read the fixed part of a header; set how many bytes it needs in all. */
static int ws_head_decode(struct fyai_ws *ws)
{
	unsigned char b0 = ws->head[0], b1 = ws->head[1];
	size_t need = 2;
	unsigned int len7 = b1 & 0x7f;

	if (b0 & 0x70)
		return -1;
	ws->opcode = b0 & 0x0f;
	ws->fin = b0 & 0x80;
	ws->masked = b1 & 0x80;
	if (!ws_opcode_valid(ws->opcode))
		return -1;
	if (len7 == 126)
		need += 2;
	else if (len7 == 127)
		need += 8;
	if (ws->masked)
		need += 4;
	ws->head_need = need;
	return 0;
}

/* The header is whole: take the length and the key. */
static int ws_head_finish(struct fyai_ws *ws)
{
	unsigned int len7 = ws->head[1] & 0x7f;
	size_t at = 2, i;
	uint64_t len;

	if (len7 < 126) {
		len = len7;
	} else if (len7 == 126) {
		len = ((uint64_t)ws->head[2] << 8) | ws->head[3];
		at = 4;
		if (len < 126)
			return -1;
	} else {
		len = 0;
		for (i = 0; i < 8; i++)
			len = (len << 8) | ws->head[2 + i];
		at = 10;
		if ((len >> 63) || len <= 0xffff)
			return -1;
	}
	/* A client masks every frame and a server none. */
	if (ws->masked != ws->server)
		return -1;
	if (ws->masked)
		memcpy(ws->key, ws->head + at, 4);
	if (ws->opcode >= FYAI_WS_CLOSE) {
		if (!ws->fin || len > 125)
			return -1;
	} else {
		if (ws->opcode == FYAI_WS_CONTINUATION && !ws->message)
			return -1;
		if (ws->opcode != FYAI_WS_CONTINUATION && ws->message)
			return -1;
		if (len > ws->max || ws->buf.len + len > ws->max ||
		    fyai_buf_reserve(&ws->buf, ws->buf.len + (size_t)len + 1))
			return -1;
	}
	ws->length = len;
	ws->got = 0;
	return 0;
}

int fyai_ws_feed(struct fyai_ws *ws, const unsigned char *in, size_t n,
		 size_t *usedp, struct fyai_ws_message *msg)
{
	size_t used = 0, take, i;
	unsigned char *dst;

	*usedp = 0;
	/* A message that was returned ends the buffer that held it. */
	if (!ws->message && ws->buf.len)
		fyai_buf_consume(&ws->buf, ws->buf.len);
	while (used < n) {
		if (ws->head_have < ws->head_need) {
			ws->head[ws->head_have++] = in[used++];
			if (ws->head_have == 2 && ws->head_need == 2 &&
			    ws_head_decode(ws))
				return -1;
			if (ws->head_have < ws->head_need)
				continue;
			if (ws_head_finish(ws))
				return -1;
			if (ws->length)
				continue;
		} else if (ws->got < ws->length) {
			take = n - used;
			if (take > ws->length - ws->got)
				take = (size_t)(ws->length - ws->got);
			if (ws->opcode >= FYAI_WS_CLOSE) {
				dst = ws->control + ws->got;
			} else {
				dst = (unsigned char *)ws->buf.data + ws->buf.len +
				      ws->got;
			}
			memcpy(dst, in + used, take);
			if (ws->masked)
				for (i = 0; i < take; i++)
					dst[i] ^= ws->key[(ws->got + i) & 3];
			ws->got += take;
			used += take;
			if (ws->got < ws->length)
				continue;
		}
		/* The frame is whole. */
		ws->head_have = 0;
		ws->head_need = 2;
		*usedp = used;
		if (ws->opcode >= FYAI_WS_CLOSE) {
			msg->opcode = ws->opcode;
			msg->data = ws->control;
			msg->size = (size_t)ws->length;
			return 1;
		}
		if (ws->opcode != FYAI_WS_CONTINUATION)
			ws->message = ws->opcode;
		ws->buf.len += (size_t)ws->length;
		ws->buf.data[ws->buf.len] = '\0';
		if (ws->fin) {
			msg->opcode = ws->message;
			msg->data = (const unsigned char *)ws->buf.data;
			msg->size = ws->buf.len;
			ws->message = 0;
			return 1;
		}
	}
	*usedp = used;
	return 0;
}
