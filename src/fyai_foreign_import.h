/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FOREIGN_IMPORT_H
#define FYAI_FOREIGN_IMPORT_H

#include <stdbool.h>

struct fyai_ctx;
struct fy_generic_builder;

enum fyai_foreign_source {
	FYAI_FOREIGN_AUTO,
	FYAI_FOREIGN_CLAUDE_CODE,
	FYAI_FOREIGN_CODEX,
};

const char *fyai_foreign_source_name(enum fyai_foreign_source source);
fy_generic fyai_foreign_sessions(struct fyai_ctx *ctx,
				 struct fy_generic_builder *gb,
				 enum fyai_foreign_source source,
				 const char *cwd, bool all);
int fyai_foreign_import_dry_run(struct fyai_ctx *ctx, const char *path,
				enum fyai_foreign_source source, bool json);
int fyai_foreign_import_list(struct fyai_ctx *ctx,
			     enum fyai_foreign_source source,
			     const char *source_root, bool all, bool json);
int fyai_foreign_import_view(struct fyai_ctx *ctx, const char *path,
			     enum fyai_foreign_source source,
			     const char *title);
int fyai_foreign_preview(struct fyai_ctx *ctx, const char *path,
			 enum fyai_foreign_source source, int max_exchanges,
			 int max_rows);
int fyai_foreign_import_session(struct fyai_ctx *ctx,
				enum fyai_foreign_source source,
				const char *source_root, const char *session,
				bool dry_run, bool json);

#endif
