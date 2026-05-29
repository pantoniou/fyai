// SPDX-License-Identifier: MIT
/*
 * Build hosts with recent glibc redirect some C23 functions to GLIBC_2.38
 * symbols when compiling in GNU C2x mode. fyai does not need the C23 semantic
 * differences here, and mostly-static binaries should run on older glibc.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __GLIBC__
extern long int fyai_glibc_strtol(const char *nptr, char **endptr, int base);
extern unsigned long int fyai_glibc_strtoul(const char *nptr, char **endptr,
					    int base);
extern long long int fyai_glibc_strtoll(const char *nptr, char **endptr,
					int base);
extern unsigned long long int fyai_glibc_strtoull(const char *nptr,
						  char **endptr, int base);
extern int fyai_glibc_vsscanf(const char *str, const char *format,
			      va_list ap);

__asm__(".symver fyai_glibc_strtol,strtol@GLIBC_2.2.5");
__asm__(".symver fyai_glibc_strtoul,strtoul@GLIBC_2.2.5");
__asm__(".symver fyai_glibc_strtoll,strtoll@GLIBC_2.2.5");
__asm__(".symver fyai_glibc_strtoull,strtoull@GLIBC_2.2.5");
__asm__(".symver fyai_glibc_vsscanf,vsscanf@GLIBC_2.2.5");

long int __isoc23_strtol(const char *nptr, char **endptr, int base)
{
	return fyai_glibc_strtol(nptr, endptr, base);
}

unsigned long int __isoc23_strtoul(const char *nptr, char **endptr, int base)
{
	return fyai_glibc_strtoul(nptr, endptr, base);
}

long long int __isoc23_strtoll(const char *nptr, char **endptr, int base)
{
	return fyai_glibc_strtoll(nptr, endptr, base);
}

unsigned long long int __isoc23_strtoull(const char *nptr, char **endptr,
					 int base)
{
	return fyai_glibc_strtoull(nptr, endptr, base);
}

int __isoc23_vsscanf(const char *str, const char *format, va_list ap)
{
	return fyai_glibc_vsscanf(str, format, ap);
}

int __isoc23_sscanf(const char *str, const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = fyai_glibc_vsscanf(str, format, ap);
	va_end(ap);

	return ret;
}
#endif
