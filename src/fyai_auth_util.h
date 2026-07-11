/* SPDX-License-Identifier: MIT */
#ifndef FYAI_AUTH_UTIL_H
#define FYAI_AUTH_UTIL_H

#include <stddef.h>

char *fyai_base64url_encode(const unsigned char *data, size_t len);
unsigned char *fyai_base64url_decode(const char *text, size_t *lenp);

#endif
