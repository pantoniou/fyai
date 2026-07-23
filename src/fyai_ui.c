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
#include <libfymd4c.h>

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
	struct fymd_renderer *chrome_renderer;
	struct fyai_event_source *input_src, *timer_src;
	struct ui_line *head, **tail;
	struct ui_spool out, err;
	int tty_fd;
	const char *saved_color;
	volatile bool ready;
	bool quit;
	bool busy;
	bool activity_paused;
	bool external;
	struct fytim_workband *tool_band;
	struct fytim_workband *pending_band;
	struct fytim_workband *message_band;
	char *tool_title;
	char *tool_body;
	char *status_bottom;
	size_t tool_body_len;
	int activity_phase;
	off_t capture_out;
	off_t capture_err;
	bool capture;
	bool recalled;
	bool frame_pending;
	fyai_event_ms_t next_frame_ms;
};

static char *ui_indicator(struct fyai_ui *ui,
			  enum fymd_indicator_state state, size_t frame,
			  unsigned int *interval_msp)
{
	const char *glyph, *on, *off;
	char *margin;
	size_t len;
	unsigned int interval_ms;
	int rc;

	if (!ui->chrome_renderer)
		return strdup("  ");
	rc = fymd_renderer_get_indicator(ui->chrome_renderer, state, frame,
					 &glyph, &on, &off, &interval_ms);
	if (rc)
		return NULL;
	len = strlen(on) + strlen(glyph) + strlen(off) + 2;
	margin = malloc(len);
	if (!margin)
		return NULL;
	snprintf(margin, len, "%s%s%s ", on, glyph, off);
	if (interval_msp)
		*interval_msp = interval_ms;
	return margin;
}

static int ui_status_render(struct fyai_ui *ui, const char *activity)
{
	struct response_buffer out = {0};
	char *line, *p;
	size_t start, end, i;
	int rc;

	if (!ui->status_bottom)
		return 0;
	if (markdown_render_margins(ui->ctx->cfg, ui->status_bottom,
			strlen(ui->status_bottom), &out, activity, activity))
		return -1;
	start = 0;
	end = out.len;
	while (start < end &&
	       (out.data[start] == '\n' || out.data[start] == '\r'))
		start++;
	while (end > start &&
	       (out.data[end - 1] == '\n' || out.data[end - 1] == '\r'))
		end--;
	line = malloc(end - start + 1);
	if (!line) {
		free(out.data);
		return -1;
	}
	memcpy(line, out.data + start, end - start);
	line[end - start] = '\0';
	for (p = line, i = 0; i < end - start; i++)
		if (p[i] == '\n' || p[i] == '\r')
			p[i] = ' ';
	rc = fytim_set_status_row(ui->ft, 1, line) == FYTIM_OK ? 0 : -1;
	free(line);
	free(out.data);
	return rc;
}

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
	unsigned int interval_ms = 500;
	int phase;
	char *activity;
	int rc = -1;

	if (ui->activity_paused || (!ui->busy && !ui->tool_band))
		return 0;
	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return -1;
	activity = ui_indicator(ui, FYMD_INDICATOR_PENDING, 0,
				&interval_ms);
	if (!activity)
		return -1;
	free(activity);
	if (!interval_ms)
		interval_ms = 500;
	phase = (int)(((uint64_t)ts.tv_sec * 1000 +
		       (uint64_t)ts.tv_nsec / 1000000) / interval_ms);
	if (phase == ui->activity_phase)
		return 0;
	ui->activity_phase = phase;
	/*
	 * Host-side blinking is intentional. SGR blink is inconsistently
	 * implemented by terminals. The invocation itself is the work-band
	 * header, so only its leading activity cell changes. General turn
	 * activity uses a fixed-width slot in the existing status line.
	 */
	activity = ui_indicator(ui, FYMD_INDICATOR_PENDING,
				(size_t)phase, NULL);
	if (!activity)
		return -1;
	if (ui->tool_band && ui_tool_render(ui, activity, false))
		goto out;
	if (ui->busy && ui_status_render(ui, activity))
		goto out;
	rc = 0;
out:
	free(activity);
	return rc;
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
	struct response_buffer out = {0};
	char buf[4096];
	ssize_t n;

	if (s->reader < 0) return;
	while ((n = pread(s->reader, buf, sizeof(buf), s->off)) > 0) {
		if (response_buffer_reserve(&out, out.len + (size_t)n + 1))
			break;
		memcpy(out.data + out.len, buf, (size_t)n);
		out.len += (size_t)n;
		out.data[out.len] = '\0';
		s->off += n;
	}
	if (out.len)
		(void)fytim_commit(ui->ft, out.data, out.len);
	free(out.data);
}

static void spool_capture(struct ui_spool *s, off_t start,
			  struct response_buffer *out)
{
	char buf[4096];
	ssize_t n;

	if (s->reader < 0)
		return;
	while ((n = pread(s->reader, buf, sizeof(buf), start)) > 0) {
		if (response_buffer_reserve(out, out->len + (size_t)n + 1))
			break;
		memcpy(out->data + out->len, buf, (size_t)n);
		out->len += (size_t)n;
		out->data[out->len] = '\0';
		start += n;
	}
	s->off = start;
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
	if (ui->recalled) {
		line->next = ui->head;
		ui->head = line;
		if (!line->next)
			ui->tail = &line->next;
		ui->recalled = false;
	} else {
		*ui->tail = line;
		ui->tail = &line->next;
	}
	ui->ready = true;
	if (ui->busy)
		ui_pending_refresh(ui);
}

static bool ui_line_blank(const char *text)
{
	return !text || strspn(text, " \t\r\n") == strlen(text);
}

static void ui_recall_pending(struct fyai_ui *ui)
{
	struct ui_line *line;

	if (!ui || ui->recalled || !ui->head)
		return;
	line = ui->head;
	ui->head = line->next;
	if (!ui->head)
		ui->tail = &ui->head;
	(void)fytim_set_input(ui->ft, line->text);
	ui->recalled = true;
	free(line->text);
	free(line);
	ui_pending_refresh(ui);
	ui->ready = ui->head != NULL;
}

static void ui_message_clear(struct fyai_ui *ui)
{
	if (!ui || !ui->message_band)
		return;
	fytim_workband_destroy(ui->message_band);
	ui->message_band = NULL;
}

static void ui_rearm(struct fyai_ui *ui)
{
	int ms = fytim_poll_timeout_ms(ui->ft);
	fyai_event_ms_t now, frame_ms;

	if (ui->frame_pending) {
		now = fyai_event_now_ms();
		frame_ms = ui->next_frame_ms - now;
		if (frame_ms < 1)
			frame_ms = 1;
		if (ms < 1 || frame_ms < ms)
			ms = (int)frame_ms;
	}
	if (ms < 1) ms = 1;
	(void)fyai_event_timer_rearm(ui->timer_src, ms, 0);
}

static enum fyai_event_action ui_service(struct fyai_ui *ui)
{
	struct fytim_event ev;
	bool painted_frame;

	fyai_ui_drain_output(ui->ctx);
	if (ui_activity_refresh(ui))
		return FYAIEA_ABORT;
	painted_frame = ui->frame_pending;
	if (fytim_pump(ui->ft) != FYTIM_OK)
		return FYAIEA_ABORT;
	if (painted_frame) {
		ui->frame_pending = false;
		ui->next_frame_ms =
			fyai_event_now_ms() +
			ui->ctx->cfg->tool_update_interval_ms;
	}
	while (fytim_next_event(ui->ft, &ev)) {
		switch (ev.type) {
		case FYTIM_EVENT_LINE:
			ui_message_clear(ui);
			if (!ui_line_blank(ev.text)) {
				ui_queue(ui, ev.text);
				(void)fytim_history_add(ui->ft, ev.text);
			}
			break;
		case FYTIM_EVENT_INTERRUPT:
			ui->ctx->interrupt_pending = true;
			if (ui->busy)
				ui_recall_pending(ui);
			break;
		case FYTIM_EVENT_QUIT:
			ui->quit = true;
			ui->ready = true;
			ui->ctx->interrupt_pending = true;
			break;
		case FYTIM_EVENT_EDIT: {
			char *edited;
			ui_message_clear(ui);
			if (fyai_ui_external_begin(ui->ctx))
				break;
			edited = fyai_edit_line(ui->ctx, fytim_input(ui->ft));
			(void)fyai_ui_external_end(ui->ctx);
			if (edited) { (void)fytim_set_input(ui->ft, edited); free(edited); }
			break;
		}
		case FYTIM_EVENT_RESIZE:
			ui->ctx->cfg->render_width = ev.width > 1 ? ev.width - 1 : 0;
			break;
		case FYTIM_EVENT_SCROLLBACK:
			ui->activity_paused = true;
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
	fymd_renderer_destroy(ui->chrome_renderer);
	if (ui->tty_fd >= 0)
		close(ui->tty_fd);
	free(ui->tool_title);
	free(ui->tool_body);
	free(ui->status_bottom);
	ctx->cfg->color = ui->saved_color;
	ctx->cfg->render_width = 0;
	for (l = ui->head; l; l = n) { n = l->next; free(l->text); free(l); }
	free(ui);
	ctx->ui = NULL;
}

void fyai_ui_pane_begin(struct fyai_ctx *ctx)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;

	if (!ui)
		return;
	fyai_ui_drain_output(ctx);
	if (ui->message_band)
		fytim_workband_destroy(ui->message_band);
	ui->message_band = NULL;
	ui->capture_out = ui->out.off;
	ui->capture_err = ui->err.off;
	ui->capture = true;
}

void fyai_ui_pane_end(struct fyai_ctx *ctx, const char *title, bool error,
		      bool show_output)
{
	struct response_buffer out = {0};
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;
	const char *color;
	char *heading;
	size_t len;

	if (!ui || !ui->capture)
		return;
	fflush(stdout);
	fflush(stderr);
	spool_capture(&ui->out, ui->capture_out, &out);
	spool_capture(&ui->err, ui->capture_err, &out);
	ui->capture = false;
	if (!out.len) {
		free(out.data);
		return;
	}
	if (!show_output && !error) {
		(void)fytim_commit(ui->ft, out.data, out.len);
		free(out.data);
		return;
	}
	color = error ? "\033[31m" : "\033[36m";
	len = strlen(title ? title : "status") + 24;
	heading = malloc(len);
	if (!heading) {
		free(out.data);
		return;
	}
	snprintf(heading, len, "%s● %s\033[0m", color,
		 title ? title : (error ? "error" : "status"));
	ui->message_band = fytim_workband_create(ui->ft);
	if (ui->message_band) {
		(void)fytim_workband_set_max_rows(ui->message_band, 12);
		(void)fytim_workband_set_top(ui->message_band, heading);
		(void)fytim_workband_set(ui->message_band, out.data, out.len);
	}
	free(heading);
	free(out.data);
}

void fyai_ui_diag_drain(struct fyai_ctx *ctx, const char *title)
{
	struct fyai_diag *diag;

	if (!ctx || !ctx->cfg)
		return;
	diag = &ctx->cfg->diag;
	if (!fyai_ui_active(ctx) || !fyai_diag_got_error(diag)) {
		fyai_diag_drain(diag);
		return;
	}
	fyai_ui_pane_begin(ctx);
	fyai_diag_drain(diag);
	fyai_ui_pane_end(ctx, title ? title : "error", true, true);
}

bool fyai_ui_active(const struct fyai_ctx *ctx) { return ctx && ctx->ui; }

int fyai_ui_external_begin(struct fyai_ctx *ctx)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;

	if (!ui)
		return 0;
	fyai_error_check(ctx, !ui->external, err_out,
			 "external editor is already active");
	fyai_ui_drain_output(ctx);
	fyai_error_check(ctx, fytim_suspend(ui->ft) == FYTIM_OK, err_out,
			 "could not suspend terminal UI");
	spool_restore(&ui->out, STDOUT_FILENO);
	spool_restore(&ui->err, STDERR_FILENO);
	ui->external = true;
	return 0;
err_out:
	return -1;
}

int fyai_ui_external_end(struct fyai_ctx *ctx)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;
	bool out_open = false;

	if (!ui)
		return 0;
	fyai_error_check(ctx, ui->external, err_out,
			 "external editor is not active");
	fyai_error_check(ctx, !spool_open(&ui->out, STDOUT_FILENO), err_out,
			 "could not restore UI output spool");
	out_open = true;
	fyai_error_check(ctx, !spool_open(&ui->err, STDERR_FILENO), err_out,
			 "could not restore UI error spool");
	if (ui->capture) {
		ui->capture_out = 0;
		ui->capture_err = 0;
	}
	ui->external = false;
	fyai_error_check(ctx, fytim_resume(ui->ft) == FYTIM_OK, err_out,
			 "could not resume terminal UI");
	return 0;
err_out:
	if (out_open && ui->err.saved < 0)
		spool_restore(&ui->out, STDOUT_FILENO);
	return -1;
}

int fyai_ui_update_prompt_style(struct fyai_ctx *ctx)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;
	struct fymd_renderer_cfg rcfg;
	struct fymd_renderer *renderer = NULL;
	const char *on, *off = NULL;
	static const struct {
		enum fymd_style_element element;
		enum fytim_chrome_style slot;
	} styles[] = {
		{ FYMD_STYLE_HEADING, FYTIM_CHROME_HEADER },
		{ FYMD_STYLE_BLOCKQUOTE, FYTIM_CHROME_STATUS },
		{ FYMD_STYLE_RULE, FYTIM_CHROME_WORKBAND },
		{ FYMD_STYLE_STRONG, FYTIM_CHROME_MARKER },
	};
	size_t i;
	int rc = -1;

	if (!ui)
		return 0;
	if (!ctx->cfg->markdown) {
		fymd_renderer_destroy(ui->chrome_renderer);
		ui->chrome_renderer = NULL;
		(void)fytim_set_prompt_style(ui->ft, NULL);
		for (i = 0; i < sizeof(styles) / sizeof(styles[0]); i++)
			(void)fytim_set_chrome_style(ui->ft, styles[i].slot, NULL);
		return 0;
	}
	markdown_renderer_cfg(ctx->cfg, &rcfg, true,
			      ctx->cfg->theme_variant, 0);
	renderer = fymd_renderer_create(&rcfg);
	if (!renderer)
		return -1;
	if (fymd_renderer_get_reverse_pair(renderer, &on, &off) ||
	    fytim_set_prompt_style(ui->ft, on) != FYTIM_OK)
		goto out;
	for (i = 0; i < sizeof(styles) / sizeof(styles[0]); i++) {
		if (fymd_renderer_get_style_pair(renderer, styles[i].element,
						 &on, &off) ||
		    fytim_set_chrome_style(ui->ft, styles[i].slot, on) != FYTIM_OK)
			goto out;
	}
	rc = 0;
	fymd_renderer_destroy(ui->chrome_renderer);
	ui->chrome_renderer = renderer;
	renderer = NULL;
out:
	fymd_renderer_destroy(renderer);
	(void)off;
	return rc;
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
	if (busy) {
		ui->activity_paused = false;
		ui->activity_phase = -1;
		(void)ui_activity_refresh(ui);
	} else {
		(void)ui_status_render(ui, "  ");
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
	struct fyai_ui *ui;
	char *copy;
	char *activity;

	if (!fyai_ui_active(ctx)) return;
	ui = ctx->ui;
	copy = strdup(bottom ? bottom : "");
	if (!copy)
		return;
	free(ui->status_bottom);
	ui->status_bottom = copy;
	(void)fytim_set_header(ui->ft, top);
	activity = ui->busy ?
		ui_indicator(ui, FYMD_INDICATOR_PENDING,
			     (size_t)ui->activity_phase, NULL) :
		strdup("  ");
	if (activity) {
		(void)ui_status_render(ui, activity);
		free(activity);
	}
}

struct fytim_workband *fyai_ui_workband_create(struct fyai_ctx *ctx)
{
	struct fytim_workband *band;

	if (!fyai_ui_active(ctx))
		return NULL;
	band = fytim_workband_create(ctx->ui->ft);
	if (band)
		(void)fytim_workband_set_max_rows(band,
			ctx->cfg->tool_preview_lines + 2);
	return band;
}

void fyai_ui_workband_update(struct fyai_ctx *ctx,
			     struct fytim_workband *band,
			     const char *title, const char *body, size_t len,
			     const char *first_margin)
{
	struct fyai_ui *ui;
	struct response_buffer out = { 0 };
	fyai_event_ms_t now;
	size_t title_len;
	char *margin = NULL;

	if (!fyai_ui_active(ctx) || !band)
		return;
	ui = ctx->ui;
	title = title ? title : "tool";
	title_len = strlen(title);
	margin = first_margin ? strdup(first_margin) :
		ui_indicator(ui, FYMD_INDICATOR_PENDING, 0, NULL);
	if (!margin)
		goto out;
	if (markdown_render_margins(ctx->cfg, title,
				    title_len ? title_len : 4, &out,
				    margin, "  "))
		goto out;
	if (len) {
		if (out.len && out.data[out.len - 1] != '\n') {
			if (response_buffer_reserve(&out, out.len + 2))
				goto out;
			out.data[out.len++] = '\n';
			out.data[out.len] = '\0';
		}
		if (response_buffer_reserve(&out, out.len + len + 1))
			goto out;
		memcpy(out.data + out.len, body, len);
		out.len += len;
		out.data[out.len] = '\0';
	}
	if (fytim_workband_set(band, out.data, out.len) != FYTIM_OK)
		goto out;
	now = fyai_event_now_ms();
	if (!ctx->cfg->tool_update_interval_ms ||
	    now >= ui->next_frame_ms) {
		if (fytim_pump(ui->ft) == FYTIM_OK) {
			ui->frame_pending = false;
			ui->next_frame_ms = now +
				ctx->cfg->tool_update_interval_ms;
		}
	} else {
		ui->frame_pending = true;
		ui_rearm(ui);
	}
out:
	free(margin);
	free(out.data);
}

void fyai_ui_workband_destroy(struct fytim_workband *band)
{
	if (band)
		fytim_workband_destroy(band);
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
	char *margin;
	if (!fyai_ui_active(ctx) || !ctx->ui->tool_band) return;
	ui = ctx->ui;
	free(ui->tool_body);
	ui->tool_body = len ? malloc(len) : NULL;
	ui->tool_body_len = ui->tool_body ? len : 0;
	if (ui->tool_body)
		memcpy(ui->tool_body, body, len);
	margin = ui_indicator(ui, FYMD_INDICATOR_PENDING,
			      (size_t)ui->activity_phase, NULL);
	if (margin) {
		(void)ui_tool_render(ui, margin, false);
		free(margin);
	}
}

void fyai_ui_tool_end(struct fyai_ctx *ctx, bool ok)
{
	struct fyai_ui *ui;
	char *margin;
	if (!fyai_ui_active(ctx) || !ctx->ui->tool_band) return;
	ui = ctx->ui;
	margin = ui_indicator(ui, ok ? FYMD_INDICATOR_SUCCESS :
			      FYMD_INDICATOR_FAILURE, 0, NULL);
	if (margin) {
		(void)ui_tool_render(ui, margin, true);
		free(margin);
	}
	(void)fytim_workband_commit(ui->tool_band);
	ui->tool_band = NULL;
	free(ui->tool_title); ui->tool_title = NULL;
	free(ui->tool_body); ui->tool_body = NULL; ui->tool_body_len = 0;
}
