/*
 * fyai_auth_util.c - small OAuth encoding helpers
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_AUTH

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "fyai_auth_util.h"
#include "fyai.h"
#include "utils.h"

const char *fyai_auth_state_path(struct fyai_ctx *ctx, const char *name,
				 bool create)
{
	struct fyai_cfg *cfg;
	struct fy_generic_builder *gb;
	const char *base;
	const char *home;
	const char *dir;

	cfg = ctx->cfg;
	dir = cfg->auth_state_dir;
	if (!dir) {
		base = getenv("XDG_STATE_HOME");
		if (base && *base)
			dir = fy_gb_intern_string(cfg->gb,
				fy_sprintfa("%s/fyai", base));
		if (!dir) {
			home = getenv("HOME");
			if (home && *home)
				dir = fy_gb_intern_string(cfg->gb,
					fy_sprintfa("%s/.local/state/fyai",
						    home));
		}
		cfg->auth_state_dir = dir ? dir : "";
	}
	dir = cfg->auth_state_dir;
	if (!dir || !*dir ||
	    (create && (fyai_mkdir_p(dir) || mkdir_private(dir))))
		return "";
	gb = fyai_ctx_transient_gb(ctx);
	if (!gb)
		return "";
	return fy_gb_intern_string(gb, fy_sprintfa("%s/%s", dir, name));
}

int fyai_auth_store_lock(struct fyai_ctx *ctx, bool nonblock)
{
	const char *path;
	int operation;
	int saved_errno;
	int fd;

	path = fyai_auth_state_path(ctx, "auth.lock", true);
	if (!path || !*path)
		return -1;
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -1;
	operation = LOCK_EX | (nonblock ? LOCK_NB : 0);
	if (!flock(fd, operation))
		return fd;
	saved_errno = errno;
	close(fd);
	if (nonblock &&
	    (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN))
		return -2;
	errno = saved_errno;
	return -1;
}

void fyai_auth_store_unlock(int fd)
{
	if (fd < 0)
		return;
	flock(fd, LOCK_UN);
	close(fd);
}

char *fyai_auth_store_read(struct fyai_ctx *ctx, const char *name)
{
	const char *path;
	struct stat st;
	char *text;
	size_t total;
	ssize_t count;
	int fd;

	text = NULL;
	fd = -1;
	path = fyai_auth_state_path(ctx, name, false);
	if (!path || !*path)
		goto out;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		goto out;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 077) ||
	    st.st_size < 0 || st.st_size > 1024 * 1024)
		goto out;
	text = malloc((size_t)st.st_size + 1);
	if (!text)
		goto out;
	total = 0;
	while (total < (size_t)st.st_size) {
		count = read(fd, text + total, (size_t)st.st_size - total);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			free(text);
			text = NULL;
			goto out;
		}
		total += (size_t)count;
	}
	text[total] = '\0';

out:
	if (fd >= 0)
		close(fd);
	return text;
}

int fyai_auth_store_write(struct fyai_ctx *ctx, const char *name,
			  const char *text)
{
	const char *path;
	const char *tmp;
	const char *dir;
	size_t total;
	size_t len;
	ssize_t count;
	int fd;
	int dfd;
	int rc;

	fd = -1;
	dfd = -1;
	rc = -1;
	path = fyai_auth_state_path(ctx, name, true);
	if (!path || !*path)
		goto out;
	tmp = fy_sprintfa("%s.tmp.%ld", path, (long)getpid());
	len = strlen(text);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
		  0600);
	if (fd < 0)
		goto out;
	total = 0;
	while (total < len) {
		count = write(fd, text + total, len - total);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			goto unlink_out;
		total += (size_t)count;
	}
	if (fsync(fd) || close(fd))
		goto unlink_closed;
	fd = -1;
	if (rename(tmp, path))
		goto unlink_closed;
	dir = fy_sprintfa("%.*s", (int)(strrchr(path, '/') - path), path);
	dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd >= 0)
		rc = fsync(dfd);
	else
		rc = 0;
	goto out;

unlink_out:
	close(fd);
	fd = -1;
unlink_closed:
	unlink(tmp);
out:
	if (fd >= 0)
		close(fd);
	if (dfd >= 0)
		close(dfd);
	return rc;
}

int fyai_auth_store_delete(struct fyai_ctx *ctx, const char *name)
{
	const char *path;

	path = fyai_auth_state_path(ctx, name, false);
	if (!path || !*path)
		return -1;
	if (!unlink(path) || errno == ENOENT)
		return 0;
	fyai_error(ctx, "could not remove authentication state %s: %s",
		   path, strerror(errno));
	return -1;
}

char *fyai_base64url_encode(const unsigned char *data, size_t len)
{
	size_t cap = 4 * ((len + 2) / 3) + 1;
	char *out = malloc(cap);
	int n;

	if (!out)
		return NULL;
	n = EVP_EncodeBlock((unsigned char *)out, data, (int)len);
	if (n < 0) {
		free(out);
		return NULL;
	}
	while (n && out[n - 1] == '=')
		n--;
	for (int i = 0; i < n; i++) {
		if (out[i] == '+')
			out[i] = '-';
		else if (out[i] == '/')
			out[i] = '_';
	}
	out[n] = '\0';
	return out;
}

unsigned char *fyai_base64url_decode(const char *text, size_t *lenp)
{
	size_t len = strlen(text), pad = (4 - len % 4) % 4;
	char *tmp = malloc(len + pad + 1);
	unsigned char *out;
	int n;

	if (!tmp)
		return NULL;

	memcpy(tmp, text, len);
	for (size_t i = 0; i < len; i++) {
		if (tmp[i] == '-')
			tmp[i] = '+';
		else if (tmp[i] == '_')
			tmp[i] = '/';
	}
	memset(tmp + len, '=', pad);
	tmp[len + pad] = '\0';
	out = malloc(3 * ((len + pad) / 4) + 1);
	if (!out) {
		free(tmp);
		return NULL;
	}
	n = EVP_DecodeBlock(out, (unsigned char *)tmp, (int)(len + pad));
	free(tmp);
	if (n < 0) {
		free(out);
		return NULL;
	}
	n -= (int)pad;
	out[n] = '\0';
	*lenp = (size_t)n;
	return out;
}
