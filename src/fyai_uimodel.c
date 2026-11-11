/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_DISPLAY

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_uidraw.h"
#include "fyai_scratch.h"
#include "fyai_uimodel.h"
#include "utils.h"

#define UIMODEL_SUBSCRIBERS_MAX	8

/*
 * A store keeps two builders. @gb[@cur] owns @root; a write resets the other,
 * copies the new root into it and makes it current. The memory of a store is
 * thus bounded by two roots, whatever the number of writes.
 */
struct uimodel_store {
	struct fy_generic_builder *gb[2];
	int cur;
	fy_generic root;
	unsigned long long seq;
};

struct uimodel_subscriber {
	fyai_uimodel_fn fn;
	void *data;
};

struct uimodel_text_subscriber {
	fyai_uimodel_text_fn fn;
	void *data;
};

/* An append-only text that a store names by its id. */
struct uimodel_text {
	struct uimodel_text *next;
	long long id;
	struct fyai_buf buf;
};

struct fyai_uimodel {
	struct uimodel_store store[FYAI_UI_STORE_COUNT];
	struct uimodel_text *texts;
	long long next_text;
	/* What a writer builds a root in; the outermost begin resets it. */
	struct fy_generic_builder *scratch;
	int depth;
	struct uimodel_subscriber sub[UIMODEL_SUBSCRIBERS_MAX];
	size_t nsub;
	struct uimodel_text_subscriber tsub[UIMODEL_SUBSCRIBERS_MAX];
	size_t ntsub;
	/* A mirror holds the model of another process: the frames of that
	 * process write it, and the writers of this one do not. */
	bool mirror, applying;
	fyai_uimodel_intent_fn intent;
	void *intent_data;
	const struct fyai_uimodel_tile_ops *tiles;
	void *tiles_data;
};

static const char *const store_names[FYAI_UI_STORE_COUNT] = {
	[FYAI_UI_SESSION] = "session",
	[FYAI_UI_PANELS] = "panels",
	[FYAI_UI_QUESTIONS] = "questions",
	[FYAI_UI_LOG] = "log",
	[FYAI_UI_WORK] = "work",
	[FYAI_UI_TRANSCRIPT] = "transcript",
	[FYAI_UI_STATE] = "state",
};

const char *fyai_ui_store_name(enum fyai_ui_store store)
{
	return (unsigned int)store < FYAI_UI_STORE_COUNT ?
		store_names[store] : "?";
}

/* The model of @ctx, or NULL when it has none. */
static struct fyai_uimodel *uimodel(struct fyai_ctx *ctx)
{
	return ctx ? ctx->uimodel : NULL;
}

struct fy_generic_builder *fyai_uimodel_builder_new(struct fyai_ctx *ctx)
{
	struct fy_generic_builder *gb;

	if (!uimodel(ctx))
		return NULL;
	gb = fy_generic_builder_create(NULL);
	fyai_error_check(ctx, gb, err, "cannot make a builder of the UI model");
	return gb;
err:
	return NULL;
}

int fyai_uimodel_create(struct fyai_ctx *ctx)
{
	struct fyai_uimodel *m;
	int i;

	if (!ctx || ctx->uimodel || ctx->uimodel_detached)
		return 0;
	m = calloc(1, sizeof(*m));
	fyai_error_check(ctx, m, err, "cannot make the UI model");
	for (i = 0; i < FYAI_UI_STORE_COUNT; i++)
		m->store[i].root = fy_invalid;
	ctx->uimodel = m;
	m->scratch = fyai_uimodel_builder_new(ctx);
	if (!m->scratch)
		goto err;
	for (i = 0; i < FYAI_UI_STORE_COUNT; i++) {
		m->store[i].gb[0] = fyai_uimodel_builder_new(ctx);
		m->store[i].gb[1] = fyai_uimodel_builder_new(ctx);
		if (!m->store[i].gb[0] || !m->store[i].gb[1])
			goto err;
	}
	return 0;
err:
	fyai_uimodel_release(ctx);
	return -1;
}

fy_generic fyai_uimodel_get(struct fyai_ctx *ctx, enum fyai_ui_store store)
{
	struct fyai_uimodel *m = uimodel(ctx);

	if (!m || (unsigned int)store >= FYAI_UI_STORE_COUNT)
		return fy_invalid;
	return m->store[store].root;
}

unsigned long long fyai_uimodel_seq(struct fyai_ctx *ctx,
				    enum fyai_ui_store store)
{
	struct fyai_uimodel *m = uimodel(ctx);

	if (!m || (unsigned int)store >= FYAI_UI_STORE_COUNT)
		return 0;
	return m->store[store].seq;
}

struct fy_generic_builder *fyai_uimodel_begin(struct fyai_ctx *ctx)
{
	struct fyai_uimodel *m = uimodel(ctx);

	if (!m)
		return NULL;
	if (!m->depth)
		fy_generic_builder_reset(m->scratch);
	m->depth++;
	return m->scratch;
}

void fyai_uimodel_end(struct fyai_ctx *ctx)
{
	struct fyai_uimodel *m = uimodel(ctx);

	if (m && m->depth > 0)
		m->depth--;
}

int fyai_uimodel_set(struct fyai_ctx *ctx, enum fyai_ui_store store,
		     fy_generic root)
{
	struct fyai_uimodel *m = uimodel(ctx);
	struct uimodel_store *s;
	struct fy_generic_builder *gb;
	fy_generic copy;
	int next;
	size_t i;

	if (!m || (unsigned int)store >= FYAI_UI_STORE_COUNT)
		return -1;
	if (m->mirror && !m->applying)
		return 0;
	s = &m->store[store];
	if (fy_is_valid(s->root) && fy_equal(s->root, root))
		return 0;
	next = !s->cur;
	gb = s->gb[next];
	/* It held the root before the current one, which nobody keeps. */
	fy_generic_builder_reset(gb);
	copy = fy_gb_internalize(gb, root);
	fyai_error_check(ctx, fy_is_valid(copy), err,
			 "cannot copy the %s store", fyai_ui_store_name(store));
	s->cur = next;
	s->root = copy;
	s->seq++;
	/* The renderer of this process draws the change before a consumer
	 * sends it. */
	fyai_uidraw_store(ctx, store, s->root);
	for (i = 0; i < m->nsub; i++)
		m->sub[i].fn(m->sub[i].data, store, s->seq, s->root);
	return 1;
err:
	return -1;
}

int fyai_uimodel_set_member(struct fyai_ctx *ctx, enum fyai_ui_store store,
			    const char *key, fy_generic value)
{
	struct fy_generic_builder *gb;
	fy_generic root;
	int rc;

	if ((unsigned int)store >= FYAI_UI_STORE_COUNT)
		return -1;
	gb = fyai_uimodel_begin(ctx);
	if (!gb)
		return -1;
	root = fyai_uimodel_get(ctx, store);
	if (!fy_is_mapping(root))
		root = fy_map_empty;
	rc = 0;
	if (!fy_equal(fy_get(root, key, fy_invalid), value)) {
		root = fy_assoc(gb, root, key, value);
		rc = fy_is_valid(root) ? fyai_uimodel_set(ctx, store, root) : -1;
		if (fy_is_invalid(root))
			fyai_error(ctx, "cannot change the %s store",
				   fyai_ui_store_name(store));
	}
	fyai_uimodel_end(ctx);
	return rc;
}

int fyai_uimodel_subscribe(struct fyai_ctx *ctx, fyai_uimodel_fn fn,
			   void *data)
{
	struct fyai_uimodel *m = uimodel(ctx);

	if (!m || !fn)
		return -1;
	fyai_error_check(ctx, m->nsub < UIMODEL_SUBSCRIBERS_MAX, err,
			 "the UI model has no room for another consumer");
	m->sub[m->nsub].fn = fn;
	m->sub[m->nsub].data = data;
	m->nsub++;
	return 0;
err:
	return -1;
}

void fyai_uimodel_unsubscribe(struct fyai_ctx *ctx, fyai_uimodel_fn fn,
			      void *data)
{
	struct fyai_uimodel *m = uimodel(ctx);
	size_t i;

	if (!m)
		return;
	for (i = 0; i < m->nsub; i++) {
		if (m->sub[i].fn != fn || m->sub[i].data != data)
			continue;
		memmove(&m->sub[i], &m->sub[i + 1],
			(m->nsub - i - 1) * sizeof(m->sub[0]));
		m->nsub--;
		return;
	}
}

static struct uimodel_text *uimodel_text_find(struct fyai_uimodel *m,
					      long long id)
{
	struct uimodel_text *t;

	for (t = m ? m->texts : NULL; t; t = t->next)
		if (t->id == id)
			return t;
	return NULL;
}

long long fyai_uimodel_text_open(struct fyai_ctx *ctx)
{
	struct fyai_uimodel *m = uimodel(ctx);
	struct uimodel_text *t;

	if (!m || m->mirror)
		return 0;
	t = calloc(1, sizeof(*t));
	fyai_error_check(ctx, t, err, "cannot keep a text of the UI model");
	t->id = ++m->next_text;
	t->next = m->texts;
	m->texts = t;
	return t->id;
err:
	return 0;
}

int fyai_uimodel_text_append(struct fyai_ctx *ctx, long long id,
			     const char *data, size_t len)
{
	struct fyai_uimodel *m = uimodel(ctx);
	struct uimodel_text *t = uimodel_text_find(m, id);
	size_t offset, i;
	int rc;

	if (!t)
		return -1;
	offset = t->buf.len;
	rc = fyai_buf_append_data(&t->buf, data, len);
	fyai_error_check(ctx, !rc, err, "cannot grow a text of the UI model");
	for (i = 0; i < m->ntsub; i++)
		m->tsub[i].fn(m->tsub[i].data, id, offset, data, len, false);
	return 0;
err:
	return -1;
}

const char *fyai_uimodel_text(struct fyai_ctx *ctx, long long id,
			      size_t *lenp)
{
	struct uimodel_text *t = uimodel_text_find(uimodel(ctx), id);

	*lenp = t ? t->buf.len : 0;
	return t && t->buf.data ? t->buf.data : "";
}

void fyai_uimodel_text_close(struct fyai_ctx *ctx, long long id)
{
	struct fyai_uimodel *m = uimodel(ctx);
	struct uimodel_text **tp, *t;

	size_t i;

	for (tp = m ? &m->texts : NULL; tp && (t = *tp); tp = &t->next) {
		if (t->id != id)
			continue;
		*tp = t->next;
		for (i = 0; i < m->ntsub; i++)
			m->tsub[i].fn(m->tsub[i].data, id, t->buf.len, NULL, 0,
				      true);
		free(t->buf.data);
		free(t);
		return;
	}
}

void fyai_uimodel_text_foreach(struct fyai_ctx *ctx, fyai_uimodel_text_fn fn,
			       void *data)
{
	struct fyai_uimodel *m = uimodel(ctx);
	struct uimodel_text *t;

	for (t = m ? m->texts : NULL; t; t = t->next)
		fn(data, t->id, 0, t->buf.data ? t->buf.data : "", t->buf.len,
		   false);
}

int fyai_uimodel_text_subscribe(struct fyai_ctx *ctx, fyai_uimodel_text_fn fn,
				void *data)
{
	struct fyai_uimodel *m = uimodel(ctx);

	if (!m || !fn)
		return -1;
	fyai_error_check(ctx, m->ntsub < UIMODEL_SUBSCRIBERS_MAX, err,
			 "the UI model has no room for another text consumer");
	m->tsub[m->ntsub].fn = fn;
	m->tsub[m->ntsub].data = data;
	m->ntsub++;
	return 0;
err:
	return -1;
}

void fyai_uimodel_text_unsubscribe(struct fyai_ctx *ctx,
				   fyai_uimodel_text_fn fn, void *data)
{
	struct fyai_uimodel *m = uimodel(ctx);
	size_t i;

	for (i = 0; m && i < m->ntsub; i++) {
		if (m->tsub[i].fn != fn || m->tsub[i].data != data)
			continue;
		memmove(&m->tsub[i], &m->tsub[i + 1],
			(m->ntsub - i - 1) * sizeof(m->tsub[0]));
		m->ntsub--;
		return;
	}
}

bool fyai_uimodel_consumed(struct fyai_ctx *ctx)
{
	struct fyai_uimodel *m = uimodel(ctx);

	return m && m->nsub > 0;
}

void fyai_uimodel_state_update(struct fyai_ctx *ctx)
{
	fy_generic display;

	if (!fyai_uimodel_consumed(ctx) || fyai_uimodel_mirrored(ctx))
		return;
	/* The stored configuration: the layers of this invocation, such as
	 * the Markdown that a worker forces, are not the client's. */
	display = fy_get(ctx->arena_config, "display", fy_map_empty);
	if (fyai_uimodel_set_member(ctx, FYAI_UI_STATE, "display",
				    fy_is_mapping(display) ? display :
				    fy_map_empty) < 0)
		fyai_warning(ctx, "cannot write the display configuration to "
			     "the UI model");
}

/* The byte offset of the first difference of @a and @b. */
static size_t uimodel_diff_at(const char *a, const char *b)
{
	size_t i;

	for (i = 0; a[i] && a[i] == b[i]; i++)
		;
	return i;
}

/*
 * Append one record to the check file. One write(2) on an append-only
 * descriptor keeps the records of concurrent processes whole. A failure
 * is not reported: the trace has the record.
 */
static void uimodel_check_record(const char *path, const char *record)
{
	ssize_t n;
	int fd;

	fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0)
		return;
	do {
		n = write(fd, record, strlen(record));
	} while (n < 0 && errno == EINTR);
	close(fd);
}

bool fyai_uimodel_check(struct fyai_ctx *ctx, enum fyai_ui_store store,
			const char *what, const char *model,
			const char *oracle)
{
	const char *path = getenv("FYAI_UI_MODEL_CHECK");
	const char *record;
	size_t at;

	/* A mirror has no legacy path: its oracle is in the other process. */
	if (ctx && ctx->uimodel && ctx->uimodel->mirror)
		return true;
	model = model ? model : "";
	oracle = oracle ? oracle : "";
	if (!strcmp(model, oracle))
		return true;
	at = uimodel_diff_at(model, oracle);
	fyai_diag_tracef("ui-model", "%s/%s diverges at byte %zu",
			 fyai_ui_store_name(store), what, at);
	if (fy_str_empty(path))
		return false;
	record = fy_sprintfa("pid %ld: %s/%s diverges at byte %zu\n"
			     "--- model\n%s\n--- oracle\n%s\n---\n",
			     (long)getpid(), fyai_ui_store_name(store), what,
			     at, model, oracle);
	uimodel_check_record(path, record);
	return false;
}

int fyai_uimodel_mirror(struct fyai_ctx *ctx, fyai_uimodel_intent_fn intent,
			void *data)
{
	struct fyai_uimodel *m = uimodel(ctx);

	if (!m)
		return -1;
	m->mirror = true;
	m->intent = intent;
	m->intent_data = data;
	return 0;
}

bool fyai_uimodel_mirrored(struct fyai_ctx *ctx)
{
	struct fyai_uimodel *m = uimodel(ctx);

	return m && m->mirror;
}

int fyai_uimodel_mirror_store(struct fyai_ctx *ctx, const char *name,
			      fy_generic root)
{
	struct fyai_uimodel *m = uimodel(ctx);
	int store, rc;

	if (!m || !m->mirror)
		return -1;
	for (store = 0; store < FYAI_UI_STORE_COUNT; store++)
		if (!strcmp(store_names[store], name))
			break;
	/* A store this build does not know is not an error of the peer. */
	if (store == FYAI_UI_STORE_COUNT)
		return 0;
	m->applying = true;
	rc = fyai_uimodel_set(ctx, store, root);
	m->applying = false;
	return rc < 0 ? -1 : 0;
}

int fyai_uimodel_mirror_text(struct fyai_ctx *ctx, long long id, size_t offset,
			     const char *data, size_t len, bool closed)
{
	struct fyai_uimodel *m = uimodel(ctx);
	struct uimodel_text *t;

	if (!m || !m->mirror || id <= 0)
		return -1;
	t = uimodel_text_find(m, id);
	if (closed) {
		if (t)
			fyai_uimodel_text_close(ctx, id);
		return 0;
	}
	if (!t) {
		t = calloc(1, sizeof(*t));
		fyai_error_check(ctx, t, err, "cannot keep a text of the UI model");
		t->id = id;
		t->next = m->texts;
		m->texts = t;
	}
	/* The deltas of one text arrive in order: a gap is a defect. */
	fyai_error_check(ctx, offset == t->buf.len, err,
			 "text %lld of the peer skips from %zu to %zu", id,
			 t->buf.len, offset);
	return fyai_uimodel_text_append(ctx, id, data, len);
err:
	return -1;
}

void fyai_uimodel_mirror_tiles(struct fyai_ctx *ctx,
			       const struct fyai_uimodel_tile_ops *ops,
			       void *data)
{
	struct fyai_uimodel *m = uimodel(ctx);

	if (!m || !m->mirror)
		return;
	m->tiles = ops;
	m->tiles_data = data;
}

bool fyai_uimodel_keys(struct fyai_ctx *ctx, struct fytim_surface *sf,
		       const char *data, size_t len)
{
	struct fyai_uimodel *m = uimodel(ctx);

	return m && m->mirror && m->tiles && m->tiles->keys &&
	       m->tiles->keys(m->tiles_data, sf, data, len);
}

bool fyai_uimodel_tile_kept(struct fyai_ctx *ctx, struct fytim_surface *sf)
{
	struct fyai_uimodel *m = uimodel(ctx);

	return m && m->mirror && m->tiles && m->tiles->kept &&
	       m->tiles->kept(m->tiles_data, sf);
}

int fyai_uimodel_intent(struct fyai_ctx *ctx, const char *subtype,
			fy_generic request)
{
	struct fyai_uimodel *m = uimodel(ctx);

	if (!m || !m->mirror || !m->intent)
		return -1;
	return m->intent(m->intent_data, subtype, request);
}

void fyai_uimodel_view_detach(struct fyai_ctx *view)
{
	view->uimodel = NULL;
	view->uilog = NULL;
	view->uiwork = NULL;
	view->uitranscript = NULL;
	view->uidraw = NULL;
	view->ask_entries = NULL;
	view->uimodel_detached = true;
}

void fyai_uimodel_release(struct fyai_ctx *ctx)
{
	struct fyai_uimodel *m = uimodel(ctx);
	int i;

	fyai_uidraw_release(ctx);
	if (!m)
		return;
	for (i = 0; i < FYAI_UI_STORE_COUNT; i++) {
		fy_generic_builder_destroy(m->store[i].gb[0]);
		fy_generic_builder_destroy(m->store[i].gb[1]);
	}
	fy_generic_builder_destroy(m->scratch);
	while (m->texts)
		fyai_uimodel_text_close(ctx, m->texts->id);
	free(m);
	ctx->uimodel = NULL;
}

void fyai_uimodel_abandon(struct fyai_ctx *ctx)
{
	struct fyai_uimodel *m = uimodel(ctx);

	/* The copy is the child's own memory; its consumers are the parent's. */
	if (m)
		m->nsub = m->ntsub = 0;
	fyai_uidraw_abandon(ctx);
	fyai_uimodel_release(ctx);
}
