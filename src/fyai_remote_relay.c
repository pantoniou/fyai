/* SPDX-License-Identifier: MIT */
/*
 * `fyai remote relay URL`: the client end of the HTTP transport. It carries
 * standard input and output, as lines, over the WebSocket of a server at an
 * http:// or https:// URL. A client of such a target runs it on a socket pair,
 * as it runs ssh, so the client code reads lines from a socket and knows no
 * HTTP.
 */
#include "config.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_cmd_int.h"
#include "fyai_net.h"
#include "fyai_remote_http.h"
#include "fyai_ws.h"
#include "fyai_ws_client.h"

#define FYAI_MODULE FYAIEM_REMOTE

#define RELAY_MESSAGE_MAX	(16UL << 20)

/* The bearer token: $FYAI_REMOTE_TOKEN, else the first line of ~/.fyai/serve.token. */
static char *relay_token(struct fyai_ctx *ctx)
{
	const char *env = getenv("FYAI_REMOTE_TOKEN"), *home = getenv("HOME");
	char path[PATH_MAX], text[256], *token;
	struct stat st;
	size_t n;
	FILE *fp;

	if (env && *env) {
		token = strdup(env);
		fyai_error_check(ctx, token, err, "cannot keep the token");
		return token;
	}
	fyai_error_check(ctx, home && *home, err, "no token: set FYAI_REMOTE_TOKEN");
	snprintf(path, sizeof(path), "%s/.fyai/serve.token", home);
	fp = fopen(path, "re");
	fyai_error_check(ctx, fp, err, "no token: set FYAI_REMOTE_TOKEN, or put "
			 "the token of the server in '%s'", path);
	if (fstat(fileno(fp), &st) || (st.st_mode & 077)) {
		fclose(fp);
		fyai_error(ctx, "the token file '%s' must be a file that only you "
			   "can read", path);
		goto err;
	}
	n = fgets(text, sizeof(text), fp) ? strcspn(text, "\r\n") : 0;
	fclose(fp);
	fyai_error_check(ctx, n, err, "the token file '%s' is empty", path);
	text[n] = '\0';
	token = strdup(text);
	memset(text, 0, sizeof(text));
	fyai_error_check(ctx, token, err, "cannot keep the token");
	return token;
err:
	return NULL;
}

/* Write all of @data to standard output. */
static int relay_write_all(int fd, const void *data, size_t n)
{
	const unsigned char *p = data;
	ssize_t put;

	while (n) {
		put = write(fd, p, n);
		if (put < 0 && errno == EINTR)
			continue;
		if (put <= 0)
			return -1;
		p += put;
		n -= (size_t)put;
	}
	return 0;
}

/* Take the messages that the bytes of the server hold. Returns 1 at the close. */
static int relay_messages(struct fyai_ctx *ctx, struct fyai_ws_client *ws, bool readable)
{
	struct fyai_ws_message msg;
	int r, op_rc;

	while ((r = fyai_ws_client_recv(ws, &msg, readable)) > 0) {
		readable = false;
		if (msg.opcode == FYAI_WS_CLOSE)
			return 1;
		if (msg.opcode == FYAI_WS_PING) {
			op_rc = fyai_ws_client_send(ws, FYAI_WS_PONG, msg.data,
						    msg.size);
			fyai_error_check(ctx, !op_rc, err,
					 "write: %s", strerror(errno));
		} else if (msg.opcode == FYAI_WS_TEXT) {
			op_rc = relay_write_all(STDOUT_FILENO, msg.data,
						msg.size);
			if (!op_rc)
				op_rc = relay_write_all(STDOUT_FILENO, "\n", 1);
			fyai_error_check(ctx, !op_rc, err,
					 "write: %s", strerror(errno));
		}
	}
	fyai_error_check(ctx, r != -1, err, "the server sent a frame that is not valid");
	return r == -2 ? 1 : 0;
err:
	return -1;
}

/* Lines of standard input are messages; messages are lines of standard output. */
static int relay_pump(struct fyai_ctx *ctx, struct fyai_ws_client *ws)
{
	struct pollfd pfd[2];
	unsigned char *in = NULL, chunk[65536];
	size_t in_len = 0, in_cap = 0, start;
	ssize_t got;
	int rc = -1, r, op_rc;
	unsigned char *nl, *grown;

	for (;;) {
		/* Messages that came with the answer, then those that follow it. */
		r = relay_messages(ctx, ws, false);
		if (r) {
			rc = r < 0 ? -1 : 0;
			goto out;
		}
		pfd[0] = (struct pollfd){ .fd = STDIN_FILENO, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = fyai_ws_client_fd(ws), .events = POLLIN };
		/* Bytes that TLS has read already do not wake the socket. */
		if (fyai_ws_client_buffered(ws)) {
			pfd[0].revents = 0;
			pfd[1].revents = POLLIN;
		} else if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			fyai_error(ctx, "poll: %s", strerror(errno));
			goto out;
		}
		if (pfd[0].revents) {
			got = read(STDIN_FILENO, chunk, sizeof(chunk));
			if (got <= 0) {
				/* The client has no more: end the WebSocket. */
				(void)fyai_ws_client_send(ws, FYAI_WS_CLOSE, "\x03\xe8", 2);
				rc = 0;
				goto out;
			}
			if (in_len + (size_t)got > in_cap) {
				in_cap = in_cap ? in_cap : 65536;
				while (in_cap < in_len + (size_t)got)
					in_cap *= 2;
				grown = realloc(in, in_cap);
				fyai_error_check(ctx, grown, out, "cannot keep the input");
				in = grown;
			}
			memcpy(in + in_len, chunk, (size_t)got);
			in_len += (size_t)got;
			for (start = 0; (nl = memchr(in + start, '\n', in_len - start));) {
				op_rc = fyai_ws_client_send(ws, FYAI_WS_TEXT,
							    in + start,
							    (size_t)(nl - (in + start)));
				fyai_error_check(ctx, !op_rc, out,
						 "write: %s", strerror(errno));
				start = (size_t)(nl - in) + 1;
			}
			in_len -= start;
			memmove(in, in + start, in_len);
			fyai_error_check(ctx, in_len <= RELAY_MESSAGE_MAX, out,
					 "a frame of the client is too long");
		}
		if (pfd[1].revents) {
			/* The loop above takes the frames of these bytes. */
			r = relay_messages(ctx, ws, true);
			if (r) {
				rc = r < 0 ? -1 : 0;
				goto out;
			}
		}
	}
out:
	free(in);
	return rc;
}

int fyai_cmd_remote_relay(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	struct fyai_ws_client_opts opts = {
		.path = FYAI_HTTP_SESSION_PATH,
		.max_message = RELAY_MESSAGE_MAX,
		/* The pin and the extra roots come from the environment of the client. */
		.pin = getenv("FYAI_REMOTE_PIN"),
		.ca_file = getenv("FYAI_REMOTE_CA"),
	};
	struct fyai_ws_client ws;
	char *token = NULL;
	const char *url = fyai_cmd_arg_str(call, "url");
	int rc = -1;

	(void)result;
	signal(SIGPIPE, SIG_IGN);
	if (fyai_net_url_parse(ctx, url, &opts.url))
		return -1;
	token = relay_token(ctx);
	if (!token)
		return -1;
	opts.bearer = token;
	if (!fyai_ws_client_open(ctx, &opts, &ws)) {
		rc = relay_pump(ctx, &ws);
		fyai_ws_client_close(&ws);
	}
	memset(token, 0, strlen(token));
	free(token);
	return rc;
}
