/*
 * fyai_curl_test.c - tests for event-driven curl transfers
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_curl.h"
#include "fyai_event.h"
#include "fyai_test.h"

#define TEST_BOUND_MS 5000

struct completion {
	volatile bool done;
	unsigned int count;
};

struct server_ready {
	volatile bool ready;
};

struct write_cancel {
	struct fyai_curl_transfer *transfer;
	unsigned int calls;
};

static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx = { .cfg = &test_cfg };

static size_t discard_data(void *ptr, size_t size, size_t nmemb,
			   void *userdata)
{
	(void)ptr;
	(void)userdata;
	return size * nmemb;
}

static size_t cancel_from_write(void *ptr, size_t size, size_t nmemb,
				void *userdata)
{
	struct write_cancel *cancel;

	(void)ptr;
	cancel = userdata;
	FYAI_TCHECK(cancel->transfer);
	cancel->calls++;
	fyai_curl_cancel(cancel->transfer);
	return size * nmemb;
}

static void transfer_complete(struct fyai_curl_transfer *transfer,
			      void *userdata)
{
	struct completion *completion;

	FYAI_TCHECK(fyai_curl_done(transfer));
	completion = userdata;
	completion->count++;
	completion->done = true;
}

static CURL *easy_create(const char *url)
{
	CURL *easy;

	easy = curl_easy_init();
	FYAI_TCHECK(easy);
	FYAI_TCHECK(curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK);
	FYAI_TCHECK(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,
				     discard_data) == CURLE_OK);
	return easy;
}

static enum fyai_event_action server_ready_cb(const struct fyai_event *ev)
{
	struct server_ready *ready;
	char byte;

	ready = ev->userdata;
	if (read(ev->fd, &byte, 1) == 1)
		ready->ready = true;
	return FYAIEA_CONTINUE;
}

static void server_child(int listener, int ready_fd, int release_fd)
{
	char request[1024];
	static const char response[] =
		"HTTP/1.1 200 OK\r\n"
		"Content-Length: 2\r\n"
		"Connection: close\r\n\r\n"
		"ok";
	int clients[2];
	char byte;
	int i;

	for (i = 0; i < 2; i++) {
		clients[i] = accept(listener, NULL, NULL);
		if (clients[i] < 0)
			_exit(1);
		if (read(clients[i], request, sizeof(request)) <= 0)
			_exit(1);
	}
	if (write(ready_fd, "r", 1) != 1)
		_exit(1);
	if (read(release_fd, &byte, 1) != 1)
		_exit(1);
	for (i = 0; i < 2; i++) {
		if (write(clients[i], response, sizeof(response) - 1) !=
		    (ssize_t)(sizeof(response) - 1))
			_exit(1);
		close(clients[i]);
	}
	close(listener);
	_exit(0);
}

static void test_completion_is_deferred(void)
{
	struct fyai_curl_transfer *transfer;
	struct fyai_event_loop *el;
	struct completion completion;
	CURL *easy;
	int rc;

	memset(&completion, 0, sizeof(completion));
	easy = easy_create("file:///etc/hosts");
	transfer = fyai_curl_submit(&test_ctx, easy, transfer_complete,
				    &completion);
	FYAI_TCHECK(transfer);
	FYAI_TCHECK(!completion.done);

	el = fyai_ctx_loop(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_loop_run_until(el, &completion.done, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(completion.count == 1);
	FYAI_TCHECK(fyai_curl_collect(transfer) == CURLE_OK);

	fyai_curl_transfer_destroy(transfer);
	curl_easy_cleanup(easy);
	printf("ok - curl completion is deferred\n");
}

static void test_cancel_is_deferred(void)
{
	struct fyai_curl_transfer *transfer;
	struct fyai_event_loop *el;
	struct completion completion;
	CURL *easy;
	int rc;

	memset(&completion, 0, sizeof(completion));
	easy = easy_create("http://127.0.0.1:1/");
	transfer = fyai_curl_submit(&test_ctx, easy, transfer_complete,
				    &completion);
	FYAI_TCHECK(transfer);

	fyai_curl_cancel(transfer);
	FYAI_TCHECK(fyai_curl_done(transfer));
	FYAI_TCHECK(!completion.done);

	el = fyai_ctx_loop(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_loop_run_until(el, &completion.done, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(completion.count == 1);
	FYAI_TCHECK(fyai_curl_collect(transfer) ==
		    CURLE_ABORTED_BY_CALLBACK);

	fyai_curl_transfer_destroy(transfer);
	curl_easy_cleanup(easy);
	printf("ok - curl cancellation is deferred\n");
}

static void test_cancel_from_write_callback(void)
{
	struct fyai_curl_transfer *transfer;
	struct fyai_event_loop *el;
	struct completion completion;
	struct write_cancel cancel;
	CURL *easy;
	int rc;

	memset(&completion, 0, sizeof(completion));
	memset(&cancel, 0, sizeof(cancel));
	easy = easy_create("file:///etc/hosts");
	FYAI_TCHECK(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,
				     cancel_from_write) == CURLE_OK);
	FYAI_TCHECK(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &cancel) ==
		    CURLE_OK);
	transfer = fyai_curl_submit(&test_ctx, easy, transfer_complete,
				    &completion);
	FYAI_TCHECK(transfer);
	cancel.transfer = transfer;

	el = fyai_ctx_loop(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_loop_run_until(el, &completion.done, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(cancel.calls > 0);
	FYAI_TCHECK(completion.count == 1);
	FYAI_TCHECK(fyai_curl_collect(transfer) ==
		    CURLE_ABORTED_BY_CALLBACK);

	fyai_curl_transfer_destroy(transfer);
	curl_easy_cleanup(easy);
	printf("ok - curl cancellation from write callback\n");
}

static void test_simultaneous_transfers(void)
{
	struct fyai_curl_transfer *first;
	struct fyai_curl_transfer *second;
	struct fyai_event_loop *el;
	struct fyai_event_source *ready_src;
	struct completion a;
	struct completion b;
	struct server_ready ready;
	struct sockaddr_in addr;
	socklen_t addr_len;
	CURL *easy_a;
	CURL *easy_b;
	char url[128];
	int ready_pipe[2];
	int release_pipe[2];
	int listener;
	int status;
	int rc;
	pid_t pid;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	memset(&ready, 0, sizeof(ready));
	memset(&addr, 0, sizeof(addr));
	listener = socket(AF_INET, SOCK_STREAM, 0);
	FYAI_TCHECK(listener >= 0);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	FYAI_TCHECK(!bind(listener, (struct sockaddr *)&addr, sizeof(addr)));
	FYAI_TCHECK(!listen(listener, 2));
	addr_len = sizeof(addr);
	FYAI_TCHECK(!getsockname(listener, (struct sockaddr *)&addr, &addr_len));
	FYAI_TCHECK(!pipe(ready_pipe));
	FYAI_TCHECK(!pipe(release_pipe));

	pid = fork();
	FYAI_TCHECK(pid >= 0);
	if (!pid) {
		close(ready_pipe[0]);
		close(release_pipe[1]);
		server_child(listener, ready_pipe[1], release_pipe[0]);
	}
	close(listener);
	close(ready_pipe[1]);
	close(release_pipe[0]);

	snprintf(url, sizeof(url), "http://127.0.0.1:%u/",
		 (unsigned int)ntohs(addr.sin_port));
	easy_a = easy_create(url);
	easy_b = easy_create(url);
	FYAI_TCHECK(curl_easy_setopt(easy_a, CURLOPT_FRESH_CONNECT, 1L) ==
		    CURLE_OK);
	FYAI_TCHECK(curl_easy_setopt(easy_b, CURLOPT_FRESH_CONNECT, 1L) ==
		    CURLE_OK);
	first = fyai_curl_submit(&test_ctx, easy_a, transfer_complete, &a);
	second = fyai_curl_submit(&test_ctx, easy_b, transfer_complete, &b);
	FYAI_TCHECK(first);
	FYAI_TCHECK(second);
	FYAI_TCHECK(!a.done && !b.done);

	el = fyai_ctx_loop(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_event_add_fd(el, ready_pipe[0], FYAIEV_READ,
			       server_ready_cb, &ready, &ready_src);
	FYAI_TCHECK(!rc);
	rc = fyai_event_loop_run_until(el, &ready.ready, TEST_BOUND_MS);
	FYAI_TCHECK(!rc);
	fyai_event_source_remove(ready_src);
	close(ready_pipe[0]);
	FYAI_TCHECK(!fyai_curl_done(first));
	FYAI_TCHECK(!fyai_curl_done(second));
	FYAI_TCHECK(write(release_pipe[1], "g", 1) == 1);
	close(release_pipe[1]);

	while (!a.done || !b.done) {
		rc = fyai_event_loop_step(el, TEST_BOUND_MS);
		FYAI_TCHECK(rc >= 0);
	}
	FYAI_TCHECK(a.count == 1);
	FYAI_TCHECK(b.count == 1);
	FYAI_TCHECK(fyai_curl_collect(first) == CURLE_OK);
	FYAI_TCHECK(fyai_curl_collect(second) == CURLE_OK);

	fyai_curl_transfer_destroy(first);
	fyai_curl_transfer_destroy(second);
	curl_easy_cleanup(easy_a);
	curl_easy_cleanup(easy_b);
	FYAI_TCHECK(waitpid(pid, &status, 0) == pid);
	FYAI_TCHECK(WIFEXITED(status) && !WEXITSTATUS(status));
	printf("ok - simultaneous curl transfers\n");
}

int main(void)
{
	FYAI_TCHECK(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);

	test_completion_is_deferred();
	test_cancel_is_deferred();
	test_cancel_from_write_callback();
	test_simultaneous_transfers();

	fyai_curl_cleanup(&test_ctx);
	if (test_ctx.el) {
		fyai_event_loop_destroy(test_ctx.el);
		test_ctx.el = NULL;
	}
	fyai_event_pool_drain(&test_ctx);
	curl_global_cleanup();
	return 0;
}
