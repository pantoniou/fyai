/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "fyai_test.h"
#include "fyai_test_registry.h"
#include "utils.h"

FYAI_TEST_ENTRY(utils, wire_text_utf8, utils_wire_text_utf8)

int utils_wire_text_utf8(void)
{
	static const char valid[] = "plain\n\xe6\x97\xa5 e\xcc\x81";
	static const unsigned char invalid[][4] = {
		{ 0xc0, 0x80 },		/* overlong NUL */
		{ 0xe0, 0x80, 0x80 },	/* overlong three-byte value */
		{ 0xed, 0xa0, 0x80 },	/* UTF-16 surrogate */
		{ 0xf4, 0x90, 0x80, 0x80 }, /* above U+10FFFF */
		{ 0xf5, 0x80, 0x80, 0x80 }, /* invalid lead byte */
	};
	static const size_t lengths[] = { 2, 3, 3, 4, 4 };
	size_t i;

	FYAI_TCHECK(data_is_wire_text(valid, strlen(valid)));
	FYAI_TCHECK(!data_is_wire_text("\033[2J", 4));
	for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		FYAI_TCHECK(!data_is_wire_text((const char *)invalid[i],
					       lengths[i]));
	return 0;
}
