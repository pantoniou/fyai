/* fyai_ui.c - libfytimui adapter driven by fyai's event loop. */
#define FYAI_MODULE FYAIEM_DISPLAY

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libfytimui.h>

#include "fyai.h"
#include "fyai_diag.h"
#include "fyai_display.h"
#include "fyai_event.h"
#include "fyai_markdown.h"
#include "fyai_session.h"
#include "fyai_ui.h"

struct ui_line { struct ui_line *next; char *text; };
struct ui_spool { int saved, reader; off_t off; };

struct fyai_ui {
	struct fyai_ctx *ctx;
	struct fytim *ft;
	struct fyai_event_source *input_src, *timer_src;
	struct ui_line *head, **tail;
	struct ui_spool out, err;
	int tty_fd;
	const char *saved_color;
	volatile bool ready;
	bool quit;
	bool busy;
	struct fytim_workband *tool_band;
	struct fytim_workband *pending_band;
	char *tool_title;
	char *tool_body;
	size_t tool_body_len;
	int activity_phase;
};

static int ui_tool_render(struct fyai_ui *ui, const char *first_margin,
			  bool commit)
{
	struct response_buffer out = {0};
	size_t title_len;
	int rc = -1;

	if (!ui->tool_band)
		return 0;
	title_len = ui->tool_title ? strlen(ui->tool_title) : 0;
	if (markdown_render_margins(ui->ctx->cfg,
			ui->tool_title ? ui->tool_title : "shell",
			title_len ? title_len : 5, &out, first_margin, "  "))
		goto out;
	if (ui->tool_body_len) {
		if (out.len && out.data[out.len - 1] != '\n') {
			if (response_buffer_reserve(&out, out.len + 2))
				goto out;
			out.data[out.len++] = '\n';
			out.data[out.len] = '\0';
		}
		if (response_buffer_reserve(&out,
					    out.len + ui->tool_body_len + 1))
			goto out;
		memcpy(out.data + out.len, ui->tool_body, ui->tool_body_len);
		out.len += ui->tool_body_len;
		out.data[out.len] = '\0';
	}
	if (fytim_workband_set(ui->tool_band, out.data, out.len) != FYTIM_OK)
		goto out;
	if (commit &&
	    fytim_workband_set_commit(ui->tool_band, out.data, out.len) != FYTIM_OK)
		goto out;
	rc = 0;
out:
	free(out.data);
	return rc;
}

static int ui_activity_refresh(struct fyai_ui *ui)
{
	struct timespec ts;
	int phase;
	const char *dot, *marker;

	if (!ui->busy && !ui->tool_band)
		return 0;
	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return -1;
	phase = (int)((ts.tv_sec * 2 + ts.tv_nsec / 500000000L) & 1);
	if (phase == ui->activity_phase)
		return 0;
	ui->activity_phase = phase;
	/*
	 * Host-side blinking is intentional. SGR blink is inconsistently
	 * implemented by terminals. The invocation itself is the work-band
	 * header, so only its leading activity cell changes.
	 */
	dot = phase ? "  " : "\033[93m●\033[0m ";
	marker = phase ? "  " : "\033[93m●\033[0m ";
	if (ui->tool_band && ui_tool_render(ui, dot, false))
		return -1;
	if (ui->busy && fytim_set_marker(ui->ft, marker) != FYTIM_OK)
		return -1;
	return 0;
}

static void ui_pending_refresh(struct fyai_ui *ui)
{
	struct ui_line *line;
	char *buf, *p;
	size_t len = 0, count = 0;

	for (line = ui->head; line; line = line->next) {
		len += strlen(line->text) + 5;
		count++;
	}
	if (!count) {
		if (ui->pending_band)
			fytim_workband_destroy(ui->pending_band);
		ui->pending_band = NULL;
		return;
	}
	if (!ui->pending_band) {
		ui->pending_band = fytim_workband_create(ui->ft);
		if (!ui->pending_band) return;
		(void)fytim_workband_set_max_rows(ui->pending_band, 5);
	}
	buf = malloc(len + 64);
	if (!buf) return;
	p = buf;
	p += sprintf(p, "\n\033[36m●\033[0m \033[1mpending\033[0m (%zu)", count);
	for (line = ui->head; line; line = line->next)
		p += sprintf(p, "\n  › %s", line->text);
	(void)fytim_workband_set(ui->pending_band, buf, (size_t)(p - buf));
	free(buf);
}

static int spool_open(struct ui_spool *s, int target)
{
	char path[] = "/tmp/fyai-ui-XXXXXX";
	int writer = -1;

	memset(s, 0, sizeof(*s));
	s->saved = s->reader = -1;
	writer = mkstemp(path);
	if (writer < 0)
		return -1;
	s->reader = open(path, O_RDONLY | O_CLOEXEC);
	unlink(path);
	if (s->reader < 0)
		goto fail;
	s->saved = dup(target);
	if (s->saved < 0 || dup2(writer, target) < 0)
		goto fail;
	close(writer);
	return 0;
fail:
	if (writer >= 0) close(writer);
	if (s->reader >= 0) close(s->reader);
	if (s->saved >= 0) close(s->saved);
	s->saved = s->reader = -1;
	return -1;
}

static void spool_restore(struct ui_spool *s, int target)
{
	if (s->saved >= 0) {
		fflush(target == STDOUT_FILENO ? stdout : stderr);
		(void)dup2(s->saved, target);
		close(s->saved);
	}
	if (s->reader >= 0) close(s->reader);
	s->saved = s->reader = -1;
}

static void spool_drain(struct fyai_ui *ui, struct ui_spool *s)
{
	char buf[4096];
	ssize_t n;

	if (s->reader < 0) return;
	while ((n = pread(s->reader, buf, sizeof(buf), s->off)) > 0) {
		(void)fytim_commit(ui->ft, buf, (size_t)n);
		s->off += n;
	}
}

void fyai_ui_drain_output(struct fyai_ctx *ctx)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;
	if (!ui) return;
	fflush(stdout); fflush(stderr);
	spool_drain(ui, &ui->out);
	spool_drain(ui, &ui->err);
}

static void ui_queue(struct fyai_ui *ui, const char *text)
{
	struct ui_line *line = calloc(1, sizeof(*line));
	if (!line) return;
	line->text = strdup(text ? text : "");
	if (!line->text) { free(line); return; }
	*ui->tail = line;
	ui->tail = &line->next;
	ui->ready = true;
	if (ui->busy)
		ui_pending_refresh(ui);
}

static void ui_rearm(struct fyai_ui *ui)
{
	int ms = fytim_poll_timeout_ms(ui->ft);
	if (ms < 1) ms = 1;
	(void)fyai_event_timer_rearm(ui->timer_src, ms, 0);
}

static enum fyai_event_action ui_service(struct fyai_ui *ui)
{
	struct fytim_event ev;

	fyai_ui_drain_output(ui->ctx);
	if (ui_activity_refresh(ui))
		return FYAIEA_ABORT;
	if (fytim_pump(ui->ft) != FYTIM_OK)
		return FYAIEA_ABORT;
	while (fytim_next_event(ui->ft, &ev)) {
		switch (ev.type) {
		case FYTIM_EVENT_LINE:
			ui_queue(ui, ev.text);
			(void)fytim_history_add(ui->ft, ev.text);
			break;
		case FYTIM_EVENT_INTERRUPT:
			ui->ctx->interrupt_pending = true;
			break;
		case FYTIM_EVENT_QUIT:
			ui->quit = true;
			ui->ready = true;
			ui->ctx->interrupt_pending = true;
			break;
		case FYTIM_EVENT_EDIT: {
			char *edited;
			(void)fytim_suspend(ui->ft);
			spool_restore(&ui->out, STDOUT_FILENO);
			spool_restore(&ui->err, STDERR_FILENO);
			edited = fyai_edit_line(fytim_input(ui->ft));
			(void)spool_open(&ui->out, STDOUT_FILENO);
			(void)spool_open(&ui->err, STDERR_FILENO);
			(void)fytim_resume(ui->ft);
			if (edited) { (void)fytim_set_input(ui->ft, edited); free(edited); }
			break;
		}
		case FYTIM_EVENT_RESIZE:
			ui->ctx->cfg->render_width = ev.width > 1 ? ev.width - 1 : 0;
			break;
		default:
			break;
		}
	}
	ui_rearm(ui);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action ui_cb(const struct fyai_event *ev)
{
	return ui_service(ev->userdata);
}

static void ui_complete_cb(void *user, const char *text,
			   struct fytim_completions *comps)
{
	fyai_session_completion(user, text, comps);
}

int fyai_ui_open(struct fyai_ctx *ctx)
{
	struct fytim_cfg cfg;
	struct fyai_ui *ui;
	struct fyai_event_loop *el;
	int ttyout = -1;

	if (!ctx || !ctx->cfg->interactive || !isatty(STDIN_FILENO) ||
	    !isatty(STDOUT_FILENO))
		return 0;
	ui = calloc(1, sizeof(*ui));
	if (!ui) return -1;
	ui->ctx = ctx;
	ui->saved_color = ctx->cfg->color;
	ui->tail = &ui->head;
	ui->out.saved = ui->out.reader = -1;
	ui->err.saved = ui->err.reader = -1;
	ui->tty_fd = -1;
	ctx->ui = ui;
	ttyout = dup(STDOUT_FILENO);
	if (ttyout < 0) goto fail;
	fytim_cfg_default(&cfg);
	cfg.output_fd = ttyout;
	cfg.title = "fyai";
	ui->ft = fytim_create(&cfg);
	if (!ui->ft) goto fail;
	ui->tty_fd = ttyout;
	ttyout = -1;
	{
		int width = 0;
		(void)fytim_size(ui->ft, &width, NULL);
		ctx->cfg->render_width = width > 1 ? width - 1 : 0;
	}
	if (!ctx->cfg->color || !strcmp(ctx->cfg->color, "auto"))
		ctx->cfg->color = "on";
	fyai_error_check(ctx, !fyai_ui_update_prompt_style(ctx), fail,
			 "failed to apply input bubble style");
	el = fyai_ctx_loop(ctx);
	if (!el || fyai_event_add_fd(el, fytim_poll_fd(ui->ft), FYAIEV_READ,
				     ui_cb, ui, &ui->input_src) ||
	    fyai_event_add_timer(el, 1, 0, ui_cb, ui, &ui->timer_src))
		goto fail;
	if (spool_open(&ui->out, STDOUT_FILENO) ||
	    spool_open(&ui->err, STDERR_FILENO))
		goto fail;
	(void)fytim_set_marker(ui->ft, ctx->cfg->prompt_marker &&
			       *ctx->cfg->prompt_marker ? ctx->cfg->prompt_marker : "❯ ");
	(void)fytim_history_set_max_len(ui->ft, 1000);
	(void)fytim_set_complete_fn(ui->ft, ui_complete_cb, ctx);
	ui_rearm(ui);
	return 0;
fail:
	if (ttyout >= 0) close(ttyout);
	fyai_ui_close(ctx);
	return -1;
}

void fyai_ui_close(struct fyai_ctx *ctx)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;
	struct ui_line *l, *n;
	if (!ui) return;
	fyai_ui_drain_output(ctx);
	if (ui->ft)
		(void)fytim_pump(ui->ft);
	fyai_event_source_remove(ui->timer_src);
	fyai_event_source_remove(ui->input_src);
	spool_restore(&ui->out, STDOUT_FILENO);
	spool_restore(&ui->err, STDERR_FILENO);
	fytim_destroy(ui->ft);
	if (ui->tty_fd >= 0)
		close(ui->tty_fd);
	free(ui->tool_title);
	free(ui->tool_body);
	ctx->cfg->color = ui->saved_color;
	ctx->cfg->render_width = 0;
	for (l = ui->head; l; l = n) { n = l->next; free(l->text); free(l); }
	free(ui);
	ctx->ui = NULL;
}

bool fyai_ui_active(const struct fyai_ctx *ctx) { return ctx && ctx->ui; }

int fyai_ui_update_prompt_style(struct fyai_ctx *ctx)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;
	const char *on, *off = NULL;

	if (!ui)
		return 0;
	if (!ctx->cfg->markdown ||
	    !markdown_reverse_pair(ctx->cfg, &on, &off))
		on = NULL;
	(void)off;
	return fytim_set_prompt_style(ui->ft, on) == FYTIM_OK ? 0 : -1;
}

void fyai_ui_history_load(struct fyai_ctx *ctx, const char *path)
{
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	FILE *fp;

	if (!fyai_ui_active(ctx) || !path || !(fp = fopen(path, "r"))) return;
	while ((len = getline(&line, &cap, fp)) >= 0) {
		while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (len) (void)fytim_history_add(ctx->ui->ft, line);
	}
	free(line);
	fclose(fp);
}

void fyai_ui_history_save(struct fyai_ctx *ctx, const char *path,
			  const char *line)
{
	FILE *fp;
	if (!fyai_ui_active(ctx) || !path || !line || !(fp = fopen(path, "a"))) return;
	fprintf(fp, "%s\n", line);
	fclose(fp);
}

char *fyai_ui_readline(struct fyai_ctx *ctx)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;
	struct ui_line *line;
	if (!ui) return NULL;
	while (!ui->head && !ui->quit) {
		ui->ready = false;
		if (fyai_event_loop_run_until(fyai_ctx_loop(ctx), &ui->ready, -1))
			return NULL;
	}
	if (ui->quit) return NULL;
	line = ui->head; ui->head = line->next;
	if (!ui->head) ui->tail = &ui->head;
	ui_pending_refresh(ui);
	ui->ready = ui->head != NULL;
	{
		char *text = line->text;
		free(line);
		return text;
	}
}

int fyai_ui_commit(struct fyai_ctx *ctx, const char *buf, size_t len)
{
	return fyai_ui_active(ctx) && fytim_commit(ctx->ui->ft, buf, len) == FYTIM_OK ? 0 : -1;
}

int fyai_ui_tail_apply(struct fyai_ctx *ctx, const struct markdown_update *u)
{
	return fyai_ui_active(ctx) &&
		fytim_tail_apply(ctx->ui->ft, u->backtrack, u->content,
				 u->content_len, u->freeze) == FYTIM_OK ? 0 : -1;
}

void fyai_ui_tail_finish(struct fyai_ctx *ctx, const char *buf, size_t len)
{
	if (!fyai_ui_active(ctx)) return;
	if (len) (void)fytim_commit(ctx->ui->ft, buf, len);
	(void)fytim_tail_set(ctx->ui->ft, NULL, 0);
}

void fyai_ui_set_busy(struct fyai_ctx *ctx, bool busy)
{
	struct fyai_ui *ui;

	if (!fyai_ui_active(ctx)) return;
	ui = ctx->ui;
	ui->busy = busy;
	(void)fytim_set_status_row(ui->ft, 0, busy ? " working" : " ready");
	if (busy) {
		ui->activity_phase = -1;
		(void)ui_activity_refresh(ui);
	} else {
		(void)fytim_set_marker(ui->ft,
			ctx->cfg->prompt_marker && *ctx->cfg->prompt_marker ?
			ctx->cfg->prompt_marker : "❯ ");
	}
}

void fyai_ui_signal(struct fyai_ctx *ctx, int signo)
{
	if (!fyai_ui_active(ctx)) return;
	if (signo != SIGINT) {
		ctx->ui->quit = true;
		ctx->ui->ready = true;
	}
}

void fyai_ui_update_banner(struct fyai_ctx *ctx, const char *top, const char *bottom)
{
	if (!fyai_ui_active(ctx)) return;
	(void)fytim_set_header(ctx->ui->ft, top);
	(void)fytim_set_status_row(ctx->ui->ft, 1, bottom);
}

struct fytim_workband *fyai_ui_workband_create(struct fyai_ctx *ctx)
{
	return fyai_ui_active(ctx) ? fytim_workband_create(ctx->ui->ft) : NULL;
}

void fyai_ui_tool_begin(struct fyai_ctx *ctx, const char *title)
{
	struct fyai_ui *ui;
	if (!fyai_ui_active(ctx)) return;
	ui = ctx->ui;
	if (ui->tool_band) fytim_workband_destroy(ui->tool_band);
	free(ui->tool_title);
	free(ui->tool_body);
	ui->tool_body = NULL;
	ui->tool_body_len = 0;
	ui->tool_title = strdup(title ? title : "tool");
	ui->tool_band = fytim_workband_create(ui->ft);
	if (!ui->tool_band) return;
	ui->activity_phase = -1;
	(void)fytim_workband_set_max_rows(ui->tool_band,
		ctx->cfg->tool_preview_lines + 2);
	(void)ui_activity_refresh(ui);
}

void fyai_ui_tool_update(struct fyai_ctx *ctx, const char *body, size_t len)
{
	struct fyai_ui *ui;
	const char *margin;
	if (!fyai_ui_active(ctx) || !ctx->ui->tool_band) return;
	ui = ctx->ui;
	free(ui->tool_body);
	ui->tool_body = len ? malloc(len) : NULL;
	ui->tool_body_len = ui->tool_body ? len : 0;
	if (ui->tool_body)
		memcpy(ui->tool_body, body, len);
	margin = ui->activity_phase ?
		 "  " : "\033[93m●\033[0m ";
	(void)ui_tool_render(ui, margin, false);
}

void fyai_ui_tool_end(struct fyai_ctx *ctx, bool ok)
{
	struct fyai_ui *ui;
	const char *margin;
	if (!fyai_ui_active(ctx) || !ctx->ui->tool_band) return;
	ui = ctx->ui;
	margin = ok ? "\033[32m●\033[0m " : "\033[31m●\033[0m ";
	(void)ui_tool_render(ui, margin, true);
	(void)fytim_workband_commit(ui->tool_band);
	ui->tool_band = NULL;
	free(ui->tool_title); ui->tool_title = NULL;
	free(ui->tool_body); ui->tool_body = NULL; ui->tool_body_len = 0;
}
