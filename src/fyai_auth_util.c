/*
 * fyai_auth_util.c - small OAuth encoding helpers
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>

#include "fyai_auth_util.h"

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
