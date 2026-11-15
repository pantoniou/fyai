/*
 * fyai_bool.h - make true and false a _Bool, as C23 has them
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_BOOL_H
#define FYAI_BOOL_H

#include <stdbool.h>

/*
 * A C library's <stdbool.h> defines true and false as the ints 1 and 0
 * (musl 1.2.6 does, in every dialect). The preprocessor replaces the C23
 * keywords with those macros, so `true` is an int in a generic: libfyaml
 * types a generic by its C type, and a request field would go out as an
 * integer, which a provider rejects.
 *
 * Cast them so that the type of the value is _Bool wherever it is used. The
 * include above runs first, so the file's own <stdbool.h> is a no-op and
 * cannot put the plain macros back. This header is force-included into every
 * translation unit (see CMakeLists.txt), so no file can miss it.
 *
 * A cast is not a valid operand of #if, so these macros are never used in a
 * preprocessor expression.
 */
#ifdef true
#undef true
#define true ((_Bool)1)
#endif

#ifdef false
#undef false
#define false ((_Bool)0)
#endif

#endif /* FYAI_BOOL_H */
