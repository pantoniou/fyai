/*
 * fyai_ws_test.c - unit tests for the WebSocket codec
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_ws.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(ws, accept_key_of_the_rfc, ws_accept_key)
FYAI_TEST_ENTRY(ws, a_masked_frame_of_the_rfc, ws_rfc_masked)
FYAI_TEST_ENTRY(ws, frames_round_trip_at_each_length, ws_round_trip)
FYAI_TEST_ENTRY(ws, bytes_in_any_split_make_the_same_messages, ws_split)
FYAI_TEST_ENTRY(ws, fragments_join_and_a_control_frame_may_stand_among_them, ws_fragments)
FYAI_TEST_ENTRY(ws, a_bad_frame_is_an_error, ws_errors)
FYAI_TEST_ENTRY(ws, the_handshake_of_a_client_and_a_server, ws_handshake)
FYAI_TEST_ENTRY(ws, an_encoded_frame_is_read_back, ws_encode)

static int failures;

static void check(bool ok, const char *what)
{
	if (ok)
		return;
	fprintf(stderr, "FAIL %s\n", what);
	failures++;
}

/* A frame of @opcode with @len bytes of @fill; masked by @key when not NULL. */
static size_t make_frame(unsigned char *out, int opcode, bool fin, size_t len,
			 int fill, const unsigned char *key)
{
	size_t n = fyai_ws_header(out, opcode, fin, len, key);

	memset(out + n, fill, len);
	if (key)
		fyai_ws_mask(out + n, len, key, 0);
	return n + len;
}

int ws_accept_key(void)
{
	char out[29];

	failures = 0;
	/* The example of RFC 6455 section 1.3. */
	check(!fyai_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", out, sizeof(out)) &&
	      !strcmp(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), "the key of the RFC");
	check(fyai_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ", out, sizeof(out)),
	      "a key that is not 24 characters");
	check(fyai_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", out, 10),
	      "an output that is too small");
	check(fyai_ws_accept_key("AAAAAAAAAAAAAAAAAAAAAAAA", out, sizeof(out)),
	      "a key of 18 bytes");
	return failures;
}

int ws_rfc_masked(void)
{
	/* "Hello" in a masked text frame, RFC 6455 section 5.7. */
	static const unsigned char wire[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
		0x7f, 0x9f, 0x4d, 0x51, 0x58 };
	struct fyai_ws ws;
	struct fyai_ws_message msg;
	size_t used;
	int rc;

	failures = 0;
	fyai_ws_init(&ws, true, 1024);
	rc = fyai_ws_feed(&ws, wire, sizeof(wire), &used, &msg);
	check(rc == 1 && used == sizeof(wire) && msg.opcode == FYAI_WS_TEXT &&
	      msg.size == 5 && !memcmp(msg.data, "Hello", 5), "Hello");
	fyai_ws_release(&ws);
	return failures;
}

int ws_round_trip(void)
{
	static const size_t sizes[] = { 0, 1, 125, 126, 127, 65535, 65536, 100000 };
	static const unsigned char key[4] = { 1, 2, 3, 4 };
	unsigned char *wire = malloc(100000 + FYAI_WS_HEADER_MAX);
	struct fyai_ws ws;
	struct fyai_ws_message msg;
	size_t i, n, used, j;
	bool same;
	int rc, masked;

	failures = 0;
	for (masked = 0; masked < 2; masked++)
		for (i = 0; i < sizeof(sizes) / sizeof(*sizes); i++) {
			n = make_frame(wire, FYAI_WS_BINARY, true, sizes[i], 'x',
				       masked ? key : NULL);
			fyai_ws_init(&ws, masked, 200000);
			rc = fyai_ws_feed(&ws, wire, n, &used, &msg);
			same = rc == 1 && used == n && msg.size == sizes[i] &&
			       msg.opcode == FYAI_WS_BINARY;
			for (j = 0; same && j < sizes[i]; j++)
				same = msg.data[j] == 'x';
			check(same, "a frame of one length");
			fyai_ws_release(&ws);
		}
	free(wire);
	return failures;
}

int ws_split(void)
{
	static const unsigned char key[4] = { 9, 8, 7, 6 };
	unsigned char wire[600], *p = wire;
	struct fyai_ws ws;
	struct fyai_ws_message msg;
	size_t used, total, i, at, count, end;
	int rc;

	failures = 0;
	p += make_frame(p, FYAI_WS_TEXT, true, 300, 'a', key);
	p += make_frame(p, FYAI_WS_TEXT, true, 0, 'b', key);
	p += make_frame(p, FYAI_WS_BINARY, true, 5, 'c', key);
	total = (size_t)(p - wire);
	/* Every split of the bytes into two reads gives the same three. */
	for (i = 1; i < total; i++) {
		fyai_ws_init(&ws, true, 1000);
		count = 0;
		for (at = 0; at < total;) {
			end = at < i ? i : total;
			rc = fyai_ws_feed(&ws, wire + at, end - at, &used, &msg);
			check(rc >= 0, "no error in a split");
			at += used;
			if (rc == 1)
				count++;
			else if (!used)
				break;
		}
		check(count == 3, "three messages in each split");
		fyai_ws_release(&ws);
	}
	return failures;
}

int ws_fragments(void)
{
	static const unsigned char key[4] = { 5, 5, 5, 5 };
	unsigned char wire[200], *p = wire;
	struct fyai_ws ws;
	struct fyai_ws_message msg;
	size_t used, at = 0, total;
	int kinds[4], k = 0, rc;

	failures = 0;
	p += make_frame(p, FYAI_WS_TEXT, false, 3, 'a', key);
	p += make_frame(p, FYAI_WS_PING, true, 2, 'p', key);
	p += make_frame(p, FYAI_WS_CONTINUATION, false, 3, 'b', key);
	p += make_frame(p, FYAI_WS_CONTINUATION, true, 3, 'c', key);
	total = (size_t)(p - wire);
	fyai_ws_init(&ws, true, 100);
	while (at < total && k < 4) {
		rc = fyai_ws_feed(&ws, wire + at, total - at, &used, &msg);
		check(rc == 1, "a message");
		if (rc != 1)
			break;
		at += used;
		kinds[k++] = msg.opcode;
		if (msg.opcode == FYAI_WS_TEXT)
			check(msg.size == 9 && !memcmp(msg.data, "aaabbbccc", 9),
			      "the pieces joined");
		else
			check(msg.opcode == FYAI_WS_PING && msg.size == 2,
			      "the ping among them");
	}
	check(k == 2 && kinds[0] == FYAI_WS_PING && kinds[1] == FYAI_WS_TEXT,
	      "the ping first, then the message");
	fyai_ws_release(&ws);
	return failures;
}

static bool refused(bool server, size_t max, const unsigned char *wire, size_t n)
{
	struct fyai_ws ws;
	struct fyai_ws_message msg;
	size_t used;
	int rc;

	fyai_ws_init(&ws, server, max);
	rc = fyai_ws_feed(&ws, wire, n, &used, &msg);
	fyai_ws_release(&ws);
	return rc < 0;
}

int ws_errors(void)
{
	static const unsigned char key[4] = { 1, 1, 1, 1 };
	unsigned char wire[300];
	size_t n;

	failures = 0;
	n = make_frame(wire, FYAI_WS_TEXT, true, 3, 'a', NULL);
	check(refused(true, 100, wire, n), "a frame of a client that is not masked");
	n = make_frame(wire, FYAI_WS_TEXT, true, 3, 'a', key);
	check(refused(false, 100, wire, n), "a frame of a server that is masked");
	n = make_frame(wire, FYAI_WS_TEXT, true, 3, 'a', key);
	wire[0] |= 0x40;
	check(refused(true, 100, wire, n), "a reserved bit");
	n = make_frame(wire, 3, true, 3, 'a', key);
	check(refused(true, 100, wire, n), "an opcode that is not defined");
	n = make_frame(wire, FYAI_WS_PING, false, 3, 'a', key);
	check(refused(true, 100, wire, n), "a control frame in pieces");
	n = make_frame(wire, FYAI_WS_PING, true, 126, 'a', key);
	check(refused(true, 400, wire, n), "a control frame of 126 bytes");
	n = make_frame(wire, FYAI_WS_CONTINUATION, true, 3, 'a', key);
	check(refused(true, 100, wire, n), "a continuation of nothing");
	n = make_frame(wire, FYAI_WS_TEXT, true, 101, 'a', key);
	check(refused(true, 100, wire, n), "a message over the maximum");
	/* The length 5 in the 16-bit form is not the shortest. */
	{
		static const unsigned char longform[] = { 0x82, 0xfe, 0, 5, 1, 1, 1, 1,
			0, 0, 0, 0, 0 };

		check(refused(true, 100, longform, sizeof(longform)),
		      "a length that is not the shortest form");
	}
	return failures;
}

int ws_handshake(void)
{
	/* The nonce is the 16 bytes that the key of RFC 6455 section 1.3 holds. */
	static const unsigned char nonce[] = "the sample nonce";
	char key[FYAI_WS_KEY_SIZE], request[512], answer[256], bad[256];
	char *p;
	int n;

	failures = 0;
	n = fyai_ws_client_request("/s", "h:1", "tok", nonce, key, request, sizeof(request));
	check(n > 0 && !strcmp(key, "dGhlIHNhbXBsZSBub25jZQ=="), "the key of the request");
	check(n > 0 && strstr(request, "GET /s HTTP/1.1\r\nHost: h:1\r\n") == request,
	      "the request line and the Host header");
	check(n > 0 && strstr(request, "Authorization: Bearer tok\r\n"), "the bearer");
	check(n > 0 && !strcmp(request + n - 4, "\r\n\r\n"), "the blank line");
	n = fyai_ws_client_request("/s", "h:1", NULL, nonce, key, request, sizeof(request));
	check(n > 0 && !strstr(request, "Authorization"), "a request without a bearer");
	check(fyai_ws_client_request("/s", "h:1", NULL, nonce, key, request, 20) < 0,
	      "a request that does not fit");

	n = fyai_ws_server_response(key, answer, sizeof(answer));
	check(n > 0 && strstr(answer, "HTTP/1.1 101 Switching Protocols\r\n") == answer,
	      "the answer of the server");
	check(n > 0 && strstr(answer, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"),
	      "the accept value of the RFC");
	check(fyai_ws_client_check(answer, key) == 0, "the client accepts the answer");
	check(fyai_ws_server_response("not a key", answer, sizeof(answer)) < 0,
	      "an answer to a key that is not valid");

	n = fyai_ws_server_response(key, answer, sizeof(answer));
	snprintf(bad, sizeof(bad), "%s", answer);
	p = strstr(bad, "s3pP");
	check(p != NULL, "the value is there to change");
	if (p)
		*p = 'X';
	check(fyai_ws_client_check(bad, key) == -3, "another accept value");
	check(fyai_ws_client_check("HTTP/1.1 101 Switching Protocols\r\n\r\n", key) == -2,
	      "no accept value");
	check(fyai_ws_client_check("HTTP/1.1 401 Unauthorized\r\n\r\n", key) == 401,
	      "a status that is not 101");
	check(fyai_ws_client_check("HTTP/1.0 200 OK\r\n\r\n", key) == -1,
	      "a status line that is not HTTP/1.1");
	return failures;
}

int ws_encode(void)
{
	static const unsigned char key[4] = { 1, 2, 3, 4 };
	unsigned char wire[FYAI_WS_HEADER_MAX + 300];
	struct fyai_ws ws;
	struct fyai_ws_message msg;
	char text[300];
	size_t n, used;
	int rc;

	failures = 0;
	memset(text, 'q', sizeof(text));
	/* A frame of a client is masked and a server reads it; the other way is plain. */
	n = fyai_ws_encode(wire, FYAI_WS_TEXT, text, sizeof(text), key);
	check(n == 4 + 4 + sizeof(text), "the size of a masked frame");
	fyai_ws_init(&ws, true, 1000);
	rc = fyai_ws_feed(&ws, wire, n, &used, &msg);
	check(rc == 1 && used == n && msg.opcode == FYAI_WS_TEXT && msg.size == sizeof(text) &&
	      !memcmp(msg.data, text, sizeof(text)), "a masked frame is read back");
	fyai_ws_release(&ws);

	n = fyai_ws_encode(wire, FYAI_WS_PONG, "p", 1, NULL);
	check(n == 3, "the size of a plain frame");
	fyai_ws_init(&ws, false, 1000);
	rc = fyai_ws_feed(&ws, wire, n, &used, &msg);
	check(rc == 1 && msg.opcode == FYAI_WS_PONG && msg.size == 1 && msg.data[0] == 'p',
	      "a plain frame is read back");
	fyai_ws_release(&ws);
	return failures;
}
