/*
 * fyai_ui.c - libfytimui adapter driven by fyai's event loop
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libfytimui.h>
#include <libfymd4c.h>

#include "fyai.h"
#include "fyai_agent.h"
#include "fyai_diag.h"
#include "fyai_display.h"
#include "fyai_event.h"
#include "fyai_markdown.h"
#include "fyai_sink.h"
#include "fyai_session.h"
#include "fyai_terminal.h"
#include "fyai_terminal_session.h"
#include "fyai_ui.h"
#include "fyai_tools.h"
#include <sys/ioctl.h>
#include "fyai_terminal_view.h"
#include "utils.h"

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
	/* Shared pane for live terminal and text tiles. */
	struct fytim_workpane *work_pane;
	int work_tiles;
	int zoom_rows;		/* current terminal-relative zoom cap */
	struct fytim_workband *pending_band;
	struct fytim_workband *message_band;
	/* The surface holding the keys, and where its bytes go. */
	fyai_ui_keys_fn keys_fn;
	void *keys_data;
	struct fyai_editor_request *editor_request;
	char *tool_title;
	char *tool_command;
	char *tool_error;	/* short failure cause, shown beside the mark */
	char *tool_body;
	char *status_bottom;
	char *editor_path;
	size_t tool_body_len;
	int activity_phase;
	unsigned int activity_interval_ms;
	off_t capture_out;
	off_t capture_err;
	bool capture;
	bool recalled;
	bool frame_pending;
	int render_cols;		/* width used to wrap screen rows */
	bool reflow_pending;		/* live rows require reflow */
	bool repaint_pending;		/* transcript requires repaint */
	fyai_event_ms_t next_frame_ms;
};

static char *ui_indicator(struct fyai_ui *ui,
			  enum fymd_indicator_state state, size_t frame,
			  unsigned int *interval_msp)
{
	return markdown_indicator_margin(ui->chrome_renderer, state, frame,
					 interval_msp);
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

static int ui_append_shell_command(struct fyai_cfg *cfg,
				   struct response_buffer *out,
				   const char *command)
{
	struct response_buffer rendered = {};
	size_t start;
	size_t rows;
	size_t i;
	int saved;
	int rc;

	/* Reserve the command marker columns. */
	saved = fyai_width_reserve_begin(cfg, 2 + FYAI_TOOL_MARKER_WIDTH);
	rc = fyai_render_fenced_buffer(cfg, command, strlen(command), "sh",
				       &rendered, 0);
	fyai_width_reserve_end(cfg, saved);
	if (rc)
		goto out;
	while (rendered.len && (rendered.data[rendered.len - 1] == '\n' ||
				rendered.data[rendered.len - 1] == '\r'))
		rendered.len--;
	if (out->len && out->data[out->len - 1] != '\n') {
		rc = response_buffer_append(out, "\n");
		if (rc)
			goto out;
	}
	/* Align command continuation rows with the first row. */
	for (start = 0, rows = 0, i = 0; i <= rendered.len; i++) {
		if (i < rendered.len && rendered.data[i] != '\n')
			continue;
		rc = response_buffer_append(out, "  ");
		if (rc)
			goto out;
		rc = response_buffer_append(out, rows ? FYAI_TOOL_MARKER_PAD :
						       FYAI_TOOL_MARKER);
		if (rc)
			goto out;
		rc = response_buffer_reserve(out, out->len + i - start + 1);
		if (rc)
			goto out;
		memcpy(out->data + out->len, rendered.data + start, i - start);
		out->len += i - start;
		out->data[out->len] = '\0';
		rc = response_buffer_append(out, "\n");
		if (rc)
			goto out;
		start = i + 1;
		rows++;
	}
out:
	free(rendered.data);
	return rc;
}

/* Render persistent band chrome and its committed transcript form. */
static int ui_tool_render(struct fyai_ui *ui, const char *first_margin,
			  bool commit)
{
	struct response_buffer head = {0};
	struct response_buffer out = {0};
	const char *body;
	size_t body_len;
	size_t title_len;
	int preview;
	int rc = -1;

	if (!ui->tool_band)
		return 0;
	title_len = ui->tool_title ? strlen(ui->tool_title) : 0;
	/* Render the failure mark and cause in the shared title row. */
	if (markdown_render_tool_head(ui->ctx->cfg,
			ui->tool_title ? ui->tool_title : "shell",
			title_len ? title_len : 5, ui->tool_error,
			first_margin, "  ", &head))
		goto out;
	if (ui->tool_command &&
	    ui_append_shell_command(ui->ctx->cfg, &head, ui->tool_command))
		goto out;
	response_buffer_trim(&head);
	if (fytim_workband_set_top(ui->tool_band,
				   head.len ? head.data : NULL) != FYTIM_OK)
		goto out;
	body = ui->tool_body;
	body_len = ui->tool_body_len;
	/* Remove the renderer's leading row below separate chrome. */
	while (body_len && (*body == '\n' || *body == '\r')) {
		body++;
		body_len--;
	}
	preview = ui->ctx->cfg->tool_preview_lines;
	(void)fytim_workband_set_max_rows(ui->tool_band,
					  preview > 0 ? preview : 1);
	if (fytim_workband_set(ui->tool_band, body, body_len) != FYTIM_OK)
		goto out;
	if (!commit) {
		rc = 0;
		goto out;
	}
	if (response_buffer_reserve(&out, head.len + body_len + 2))
		goto out;
	memcpy(out.data, head.data, head.len);
	out.len = head.len;
	if (body_len) {
		out.data[out.len++] = '\n';
		memcpy(out.data + out.len, body, body_len);
		out.len += body_len;
	}
	out.data[out.len] = '\0';
	response_buffer_trim(&out);
	if (fytim_workband_set_commit(ui->tool_band, out.data,
				      out.len) != FYTIM_OK)
		goto out;
	rc = 0;
out:
	free(head.data);
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
	ui->activity_interval_ms = interval_ms;
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
	static const char header_fmt[] =
		"\n\033[36m●\033[0m \033[1mpending\033[0m (%zu)";
	static const char line_prefix[] = "\n  › ";
	struct ui_line *line;
	char *buf, *p;
	size_t len = 0, text_len, count = 0;
	int header_len;

	for (line = ui->head; line; line = line->next) {
		text_len = strlen(line->text);
		if (text_len > SIZE_MAX - len - (sizeof(line_prefix) - 1))
			return;
		len += sizeof(line_prefix) - 1 + text_len;
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
	header_len = snprintf(NULL, 0, header_fmt, count);
	if (header_len < 0 || (size_t)header_len > SIZE_MAX - len - 1)
		return;
	len += (size_t)header_len;
	buf = malloc(len + 1);
	if (!buf) return;
	p = buf;
	p += snprintf(p, len + 1, header_fmt, count);
	for (line = ui->head; line; line = line->next) {
		memcpy(p, line_prefix, sizeof(line_prefix) - 1);
		p += sizeof(line_prefix) - 1;
		text_len = strlen(line->text);
		memcpy(p, line->text, text_len);
		p += text_len;
	}
	*p = '\0';
	(void)fytim_workband_set(ui->pending_band, buf, len);
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

static void ui_edit_complete_service(void *userdata)
{
	struct fyai_ui *ui;
	char *edited;
	size_t len;

	ui = userdata;
	edited = read_text_file(ui->editor_path);
	if (edited) {
		len = strlen(edited);
		while (len && (edited[len - 1] == '\n' ||
			       edited[len - 1] == '\r'))
			edited[--len] = '\0';
		(void)fytim_set_input(ui->ft, edited);
	}
	if (fyai_editor_collect(ui->editor_request))
		fyai_error(ui->ctx, "editor exited unsuccessfully");
	fyai_editor_destroy(ui->editor_request);
	ui->editor_request = NULL;
	(void)unlink(ui->editor_path);
	free(ui->editor_path);
	ui->editor_path = NULL;
	(void)fyai_ui_external_end(ui->ctx);
	free(edited);
}

static void ui_edit_complete(struct fyai_editor_request *request,
			     void *userdata)
{
	struct fyai_ui *ui;
	int rc;

	(void)request;
	ui = userdata;
	rc = fyai_event_defer(fyai_ctx_loop(ui->ctx),
			      ui_edit_complete_service, ui);
	if (rc)
		fyai_error(ui->ctx, "could not resume from external editor");
}

static int ui_edit_begin(struct fyai_ui *ui)
{
	const char *tmpdir;
	const char *current;
	char path[PATH_MAX];
	char *copy;
	int fd;
	int rc;

	tmpdir = getenv("TMPDIR");
	if (!tmpdir || !*tmpdir)
		tmpdir = "/tmp";
	rc = snprintf(path, sizeof(path), "%s/fyai-XXXXXX.md", tmpdir);
	fyai_error_check(ui->ctx, rc >= 0 && rc < (int)sizeof(path),
			 err_out, "could not format editor path");
	fd = mkstemps(path, 3);
	fyai_error_check(ui->ctx, fd >= 0, err_out,
			 "could not create editor file");
	current = fytim_input(ui->ft);
	if (current && *current) {
		rc = write(fd, current, strlen(current));
		fyai_error_check(ui->ctx, rc == (int)strlen(current), err_fd,
				 "could not write editor file");
	}
	close(fd);
	fd = -1;
	copy = strdup(path);
	fyai_error_check(ui->ctx, copy, err_file,
			 "could not retain editor path");
	rc = fyai_ui_external_begin(ui->ctx);
	fyai_error_check(ui->ctx, !rc, err_copy,
			 "could not suspend terminal UI");
	ui->editor_request = fyai_editor_submit(ui->ctx, path, false,
						ui_edit_complete, ui);
	fyai_error_check(ui->ctx, ui->editor_request, err_external,
			 "could not start editor");
	ui->editor_path = copy;
	return 0;

err_external:
	(void)fyai_ui_external_end(ui->ctx);
err_copy:
	free(copy);
err_file:
	(void)unlink(path);
err_fd:
	if (fd >= 0)
		close(fd);
err_out:
	return -1;
}

/* Repeat at a low rate to recover from a missed UI timer event. */
#define UI_HEAL_MS 500

static void ui_rearm(struct fyai_ui *ui)
{
	int ms = fytim_poll_timeout_ms(ui->ft);
	fyai_event_ms_t now, frame_ms;
	int rc;

	if (!ui->activity_paused && (ui->busy || ui->tool_band) &&
	    ui->activity_interval_ms &&
	    (ms < 1 || ui->activity_interval_ms < (unsigned int)ms))
		ms = (int)ui->activity_interval_ms;
	if (ui->frame_pending) {
		now = fyai_event_now_ms();
		frame_ms = ui->next_frame_ms - now;
		if (frame_ms < 1)
			frame_ms = 1;
		if (ms < 1 || frame_ms < ms)
			ms = (int)frame_ms;
	}
	/* Use the recovery interval when the UI has no deadline. */
	if (ms < 0)
		ms = UI_HEAL_MS;
	else if (ms < 1)
		ms = 1;
	rc = fyai_event_timer_rearm(ui->timer_src, ms, UI_HEAL_MS);
	fyai_error_check(ui->ctx, !rc, err_out,
			 "could not rearm the UI timer");
	return;

err_out:
	return;
}

/* Convert configured mouse controls to library flags. */
static unsigned int ui_work_controls(const struct fyai_ctx *ctx)
{
	const char *v = ctx->cfg->work_controls;

	if (!v || !strcmp(v, "none"))
		return 0;
	if (!strcmp(v, "zoom"))
		return FYTIM_WORKPANE_ZOOM | FYTIM_WORKPANE_CLOSE;
	return FYTIM_WORKPANE_ZOOM | FYTIM_WORKPANE_CLOSE |
	       FYTIM_WORKPANE_SCROLLBAR | FYTIM_WORKPANE_ARROWS;
}

/* Map "none" to absent chrome; preserve an empty rule string. */
static const char *ui_chrome_text(const char *v)
{
	if (!v || !strcmp(v, "none"))
		return NULL;
	return v;
}

/* Convert the zoom policy to a row cap. */
static int ui_zoom_rows(struct fyai_ui *ui)
{
	const char *policy = ui->ctx->cfg->work_zoom_rows;
	int rows = 1;

	(void)fytim_size(ui->ft, NULL, &rows);
	if (ui->ctx->cfg->work_zoom_fixed_rows > 0)
		return ui->ctx->cfg->work_zoom_fixed_rows;
	if (policy && !strcmp(policy, "half"))
		return (rows + 1) / 2;
	if (policy && !strcmp(policy, "quarter"))
		return (rows + 3) / 4;
	if (policy && !strcmp(policy, "full"))
		return 0;
	return rows;
}

static enum fyai_event_action ui_service(struct fyai_ui *ui)
{
	struct fytim_event ev;
	bool painted_frame;
	bool zoom_relayout = false;

	fyai_ui_drain_output(ui->ctx);
	if (ui_activity_refresh(ui))
		return FYAIEA_ABORT;
	painted_frame = ui->frame_pending;
	/* Consume this request before callbacks can queue another frame. */
	if (painted_frame)
		ui->frame_pending = false;
	if (fytim_pump(ui->ft) != FYTIM_OK)
		return FYAIEA_ABORT;
	/* Recalculate fractional zoom after a terminal resize. */
	if (fyai_ui_surface_zoomed(ui->ctx)) {
		int cap = ui_zoom_rows(ui);

		if (cap != ui->zoom_rows) {
			ui->zoom_rows = cap;
			(void)fytim_workpane_set_max_rows(ui->work_pane, cap);
			ui->frame_pending = true;
			zoom_relayout = true;
		}
	}
	/* Apply post-layout tile dimensions to terminals. */
	if (!zoom_relayout)
		fyai_tool_jobs_layout(ui->ctx);
	if (painted_frame) {
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
			/* Escape only now; ^C arrives as SIGINT. Both mean
			 * the same thing to the session. */
			fyai_ui_interrupt(ui->ctx);
			break;
		case FYTIM_EVENT_QUIT:
			ui->quit = true;
			ui->ready = true;
			ui->ctx->interrupt_pending = true;
			break;
		case FYTIM_EVENT_EDIT:
			ui_message_clear(ui);
			/* Ctrl-G cycles terminal-tile focus. */
			if (fyai_tools_focus_next(ui->ctx))
				break;
			if (!ui->editor_request)
				(void)ui_edit_begin(ui);
			break;
		case FYTIM_EVENT_RESIZE: {
			int cols = ev.width > 1 ? ev.width - 1 : 0;

			ui->ctx->cfg->render_width = cols;
			/* Record the initial size without requesting reflow. */
			if (!ui->render_cols || cols == ui->render_cols) {
				ui->render_cols = cols;
				break;
			}
			ui->render_cols = cols;
			/* Reset surface requests before solving the new layout. */
			fyai_tool_surfaces_resize(ui->ctx, ev.height, ev.width);
			/* Defer one live reflow until the resize burst ends. */
			ui->reflow_pending = true;
			/* Repaint committed rows from stored transcript data. */
			ui->repaint_pending = true;
			break;
		}
		case FYTIM_EVENT_REDRAW:
			/* Ctrl-L requests a clean repaint. */
			ui->repaint_pending = true;
			break;
		case FYTIM_EVENT_SURFACE_KEYS:
			/* The keys belong to a program, not to the prompt. */
			if (ui->keys_fn)
				ui->keys_fn(ui->keys_data, ev.text,
					    ev.text_len);
			break;
		case FYTIM_EVENT_SCROLLBACK:
			ui->activity_paused = true;
			break;
		case FYTIM_EVENT_SURFACE_ZOOM:
			/* Toggle zoom for the selected tile. */
			(void)fyai_ui_surface_zoom(ui->ctx,
				fytim_workpane_zoomed(ui->work_pane) ==
					ev.surface ? NULL : ev.surface);
			break;
		case FYTIM_EVENT_SURFACE_CLOSE:
		case FYTIM_EVENT_SURFACE_SCROLL:
			/* Route tile controls to the component that owns the work. */
			fyai_tools_surface_request(ui->ctx, ev.surface,
						   ev.type ==
						   FYTIM_EVENT_SURFACE_CLOSE
							? 0 : ev.delta);
			break;
		default:
			break;
		}
	}
	if (ui->reflow_pending) {
		ui->reflow_pending = false;
		fyai_sink_reflow(ui->ctx->sink);
		ui->frame_pending = true;
	}
	/* Delegated agents repaint only their live result screen. */
	if (ui->repaint_pending && fyai_agent_delegated(ui->ctx))
		ui->repaint_pending = false;
	/* Repaint retained live surfaces from a clean screen. */
	if (ui->repaint_pending && fyai_tool_surfaces_active(ui->ctx)) {
		ui->repaint_pending = false;
		fyai_ui_clear_screen(ui->ctx);
		ui->frame_pending = true;
	}
	if (ui->repaint_pending && !ui->busy) {
		ui->repaint_pending = false;
		fyai_ui_clear_screen(ui->ctx);
		(void)fyai_display_repaint(ui->ctx, 0);
		ui->frame_pending = true;
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
	/* Keep Ctrl-C as SIGINT so the watchdog can detect a stopped loop. */
	cfg.intr_signal = true;
	/* Grab the mouse only when work-pane controls require it. */
	cfg.mouse = ui_work_controls(ctx) != 0;
	ui->ft = fytim_create(&cfg);
	if (!ui->ft) goto fail;
	ui->tty_fd = ttyout;
	ttyout = -1;
	{
		int width = 0;
		(void)fytim_size(ui->ft, &width, NULL);
		ctx->cfg->render_width = width > 1 ? width - 1 : 0;
		/* Record the initial render width. */
		ui->render_cols = ctx->cfg->render_width;
	}
	if (!ctx->cfg->color || !strcmp(ctx->cfg->color, "auto"))
		ctx->cfg->color = "on";
	fyai_error_check(ctx, !fyai_ui_update_prompt_style(ctx), fail,
			 "failed to apply input bubble style");
	el = fyai_ctx_loop(ctx);
	if (!el || fyai_event_add_fd(el, fytim_poll_fd(ui->ft), FYAIEV_READ,
				     ui_cb, ui, &ui->input_src) ||
	    fyai_event_add_timer(el, 1, UI_HEAL_MS, ui_cb, ui, &ui->timer_src))
		goto fail;
	if (spool_open(&ui->out, STDOUT_FILENO) ||
	    spool_open(&ui->err, STDERR_FILENO))
		goto fail;
	/* Wake provider-bound turns when the terminal size changes. */
	(void)fyai_terminal_winch_open(ctx);
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

void fyai_ui_prompt_enabled(struct fyai_ctx *ctx, bool enabled)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;

	if (!ui || !ui->ft)
		return;
	(void)fytim_set_prompt_enabled(ui->ft, enabled);
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
	if (ui->editor_request) {
		fyai_editor_destroy(ui->editor_request);
		ui->editor_request = NULL;
	}
	if (ui->editor_path) {
		(void)unlink(ui->editor_path);
		free(ui->editor_path);
		ui->editor_path = NULL;
	}
	spool_restore(&ui->out, STDOUT_FILENO);
	spool_restore(&ui->err, STDERR_FILENO);
	fytim_destroy(ui->ft);
	fymd_renderer_destroy(ui->chrome_renderer);
	if (ui->tty_fd >= 0)
		close(ui->tty_fd);
	free(ui->tool_title);
	free(ui->tool_command);
	ui->tool_command = NULL;
	free(ui->tool_body);
	free(ui->status_bottom);
	ctx->cfg->color = ui->saved_color;
	ctx->cfg->render_width = 0;
	for (l = ui->head; l; l = n) { n = l->next; free(l->text); free(l); }
	free(ui);
	ctx->ui = NULL;
}

void fyai_ui_shell_begin(struct fyai_ctx *ctx, const char *title,
			 const char *command)
{
	struct fyai_ui *ui;

	fyai_ui_tool_begin(ctx, title);
	if (!fyai_ui_active(ctx))
		return;
	ui = ctx->ui;
	ui->tool_command = strdup(command ? command : "");
	ui->activity_phase = -1;
	(void)ui_activity_refresh(ui);
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

/* Restore the suspended external-editor state after an error. */
int fyai_ui_external_end(struct fyai_ctx *ctx)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;
	bool out_open = false;
	bool err_open = false;
	int rc;

	if (!ui)
		return 0;
	fyai_error_check(ctx, ui->external, err_out,
			 "external editor is not active");
	rc = spool_open(&ui->out, STDOUT_FILENO);
	fyai_error_check(ctx, !rc, err_spools,
			 "could not restore UI output spool");
	out_open = true;
	rc = spool_open(&ui->err, STDERR_FILENO);
	fyai_error_check(ctx, !rc, err_spools,
			 "could not restore UI error spool");
	err_open = true;
	if (ui->capture) {
		ui->capture_out = 0;
		ui->capture_err = 0;
	}
	ui->external = false;
	rc = fytim_resume(ui->ft);
	fyai_error_check(ctx, rc == FYTIM_OK, err_resume,
			 "could not resume terminal UI");
	return 0;

err_resume:
	ui->external = true;
err_spools:
	if (err_open)
		spool_restore(&ui->err, STDERR_FILENO);
	if (out_open)
		spool_restore(&ui->out, STDOUT_FILENO);
err_out:
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
	if (!ui) return NULL;
	while (!ui->head && !ui->quit) {
		ui->ready = false;
		if (fyai_event_loop_run_until(fyai_ctx_loop(ctx), &ui->ready, -1))
			return NULL;
	}
	return fyai_ui_take_line(ctx);
}

char *fyai_ui_take_line(struct fyai_ctx *ctx)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;
	struct ui_line *line;
	char *text;

	if (!ui || ui->quit || !ui->head)
		return NULL;
	line = ui->head; ui->head = line->next;
	if (!ui->head) ui->tail = &ui->head;
	ui_pending_refresh(ui);
	ui->ready = ui->head != NULL;
	text = line->text;
	free(line);
	return text;
}

bool fyai_ui_quit_requested(const struct fyai_ctx *ctx)
{
	const struct fyai_ui *ui = ctx ? ctx->ui : NULL;

	return !ui || (ui->quit && !ui->editor_request);
}

/* Schedule a frame to read the new terminal size. */
void fyai_ui_resized(struct fyai_ctx *ctx)
{
	struct fyai_ui *ui;

	if (!fyai_ui_active(ctx))
		return;
	ui = ctx->ui;
	ui->frame_pending = true;
	ui->next_frame_ms = fyai_event_now_ms();
	ui_rearm(ui);
}

void fyai_ui_clear_screen(struct fyai_ctx *ctx)
{
	if (!fyai_ui_active(ctx) || ctx->ui->capture)
		return;
	(void)fytim_clear_screen(ctx->ui->ft);
}

int fyai_ui_commit(struct fyai_ctx *ctx, const char *buf, size_t len)
{
	struct fyai_ui *ui;
	size_t written;

	if (!fyai_ui_active(ctx))
		return -1;
	ui = ctx->ui;
	if (ui->capture) {
		written = fwrite(buf, 1, len, stdout);
		return written == len ? 0 : -1;
	}
	return fytim_commit(ui->ft, buf, len) == FYTIM_OK ? 0 : -1;
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
	ui_rearm(ui);
}

/* Process Escape or SIGINT. */
void fyai_ui_interrupt(struct fyai_ctx *ctx)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;
	const char *input;

	if (!ui)
		return;
	input = fytim_input(ui->ft);
	if (!ui->busy && !ui->head && !ui->editor_request) {
		if (input && *input)
			(void)fytim_set_input(ui->ft, NULL);
		else {
			ui->quit = true;
			ui->ready = true;
		}
	}
	ctx->interrupt_pending = true;
	if (ui->busy)
		ui_recall_pending(ui);
}

void fyai_ui_signal(struct fyai_ctx *ctx, int signo)
{
	if (!fyai_ui_active(ctx)) return;
	if (signo != SIGINT) {
		ctx->ui->quit = true;
		ctx->ui->ready = true;
		fyai_editor_cancel(ctx->ui->editor_request);
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

/* Select the tile width for a band render and return the previous width. */
static int ui_band_width_begin(struct fyai_ctx *ctx,
			       const struct fytim_workband *band)
{
	int saved = ctx->cfg->render_width;
	int cols = fyai_ui_work_tile_cols(band);

	/* Zero denotes full width or an unpainted band. */
	if (cols > 0)
		ctx->cfg->render_width = cols;
	return saved;
}

static void ui_band_width_end(struct fyai_ctx *ctx, int saved)
{
	ctx->cfg->render_width = saved;
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
	int saved_width;

	if (!fyai_ui_active(ctx) || !band)
		return;
	ui = ctx->ui;
	saved_width = ui_band_width_begin(ctx, band);
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
	response_buffer_trim(&out);
	if (fytim_workband_set(band, out.data, out.len) != FYTIM_OK)
		goto out;
	now = fyai_event_now_ms();
	/* Defer the frame. A render callback must not read terminal input. */
	ui->frame_pending = true;
	if (!ctx->cfg->tool_update_interval_ms || now >= ui->next_frame_ms)
		ui->next_frame_ms = now;
	ui_rearm(ui);
out:
	ui_band_width_end(ctx, saved_width);
	free(margin);
	free(out.data);
}

void fyai_ui_shell_workband_update(struct fyai_ctx *ctx,
				   struct fytim_workband *band,
				   const char *title, const char *command,
				   const char *body, size_t len,
				   const char *first_margin)
{
	struct fyai_ui *ui;
	struct response_buffer top = {0};
	struct response_buffer head = {0};
	struct response_buffer out = {0};
	fyai_event_ms_t now;
	char *margin = NULL;
	size_t start;
	size_t end;
	int saved_width;

	if (!fyai_ui_active(ctx) || !band)
		return;
	ui = ctx->ui;
	saved_width = ui_band_width_begin(ctx, band);
	title = title ? title : "shell";
	margin = first_margin ? strdup(first_margin) :
		ui_indicator(ui, FYMD_INDICATOR_PENDING, 0, NULL);
	if (!margin)
		goto out;
	if (markdown_render_margins(ctx->cfg, title, strlen(title), &top,
				    margin, "  "))
		goto out;
	start = 0;
	while (start < top.len && (top.data[start] == '\n' ||
				   top.data[start] == '\r'))
		start++;
	end = top.len;
	while (end > start && (top.data[end - 1] == '\n' ||
				 top.data[end - 1] == '\r'))
		end--;
	if (response_buffer_reserve(&head, end - start + 2))
		goto out;
	memcpy(head.data, top.data + start, end - start);
	head.len = end - start;
	head.data[head.len++] = '\n';
	head.data[head.len] = '\0';
	if (ui_append_shell_command(ctx->cfg, &head, command))
		goto out;
	response_buffer_trim(&head);
	/* Keep the call title and command in persistent chrome. */
	if (fytim_workband_set_top(band, head.len ? head.data : NULL) !=
	    FYTIM_OK)
		goto out;
	/* Remove the renderer's leading row below separate chrome. */
	while (len && (*body == '\n' || *body == '\r')) {
		body++;
		len--;
	}
	if (fytim_workband_set_max_rows(band,
			ctx->cfg->tool_preview_lines > 0 ?
			ctx->cfg->tool_preview_lines : 1) != FYTIM_OK)
		goto out;
	if (fytim_workband_set(band, body, len) != FYTIM_OK)
		goto out;
	/* Commit chrome and output together. */
	if (response_buffer_reserve(&out, head.len + len + 2))
		goto out;
	memcpy(out.data, head.data, head.len);
	out.len = head.len;
	if (len) {
		out.data[out.len++] = '\n';
		memcpy(out.data + out.len, body, len);
		out.len += len;
	}
	out.data[out.len] = '\0';
	response_buffer_trim(&out);
	if (fytim_workband_set_commit(band, out.data, out.len) != FYTIM_OK)
		goto out;
	now = fyai_event_now_ms();
	ui->frame_pending = true;
	if (!ctx->cfg->tool_update_interval_ms || now >= ui->next_frame_ms)
		ui->next_frame_ms = now;
	ui_rearm(ui);
out:
	ui_band_width_end(ctx, saved_width);
	free(margin);
	free(top.data);
	free(head.data);
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
	/* Drain the previous result before the next invocation. */
	fyai_ui_drain_output(ctx);
	ui = ctx->ui;
	if (ui->tool_band) fytim_workband_destroy(ui->tool_band);
	free(ui->tool_title);
	free(ui->tool_command);
	ui->tool_command = NULL;
	free(ui->tool_error);
	ui->tool_error = NULL;
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

void fyai_ui_tool_end(struct fyai_ctx *ctx, bool ok, const char *cause)
{
	struct fyai_ui *ui;
	char *margin;
	if (!fyai_ui_active(ctx) || !ctx->ui->tool_band) return;
	ui = ctx->ui;
	free(ui->tool_error);
	ui->tool_error = (!ok && cause && *cause) ? strdup(cause) : NULL;
	margin = ui_indicator(ui, ok ? FYMD_INDICATOR_SUCCESS :
			      FYMD_INDICATOR_FAILURE, 0, NULL);
	if (margin) {
		(void)ui_tool_render(ui, margin, true);
		free(margin);
	}
	(void)fytim_workband_commit(ui->tool_band);
	ui->tool_band = NULL;
	free(ui->tool_title); ui->tool_title = NULL;
	free(ui->tool_command); ui->tool_command = NULL;
	free(ui->tool_error); ui->tool_error = NULL;
	free(ui->tool_body); ui->tool_body = NULL; ui->tool_body_len = 0;
}

/* ---- surfaces ----------------------------------------------------------- */

/* Convert one view cell to the display library's cell format. */
static void surface_cell(const struct fyai_term_cell *src,
			 struct fytim_cell *dst)
{
	int i;

	memset(dst, 0, sizeof(*dst));
	for (i = 0; i < FYAI_TERM_CELL_CHARS && i < FYTIM_CELL_CHARS; i++)
		dst->chars[i] = src->chars[i];
	dst->fg = src->fg.is_default ? FYTIM_COLOR_DEFAULT
		: ((uint32_t)src->fg.r << 16) | ((uint32_t)src->fg.g << 8) |
		  (uint32_t)src->fg.b;
	dst->bg = src->bg.is_default ? FYTIM_COLOR_DEFAULT
		: ((uint32_t)src->bg.r << 16) | ((uint32_t)src->bg.g << 8) |
		  (uint32_t)src->bg.b;
	if (src->bold)
		dst->attrs |= FYTIM_ATTR_BOLD;
	if (src->italic)
		dst->attrs |= FYTIM_ATTR_ITALIC;
	if (src->underline)
		dst->attrs |= FYTIM_ATTR_UNDERLINE;
	if (src->reverse)
		dst->attrs |= FYTIM_ATTR_REVERSE;
	if (src->strike)
		dst->attrs |= FYTIM_ATTR_STRIKE;
	dst->width = (unsigned char)(src->width > 1 ? 2 : 1);
}

/* Destroy the pane after its last tile retires. */
static void ui_work_pane_release(struct fyai_ui *ui)
{
	if (ui->work_tiles > 0)
		ui->work_tiles--;
	if (ui->work_tiles > 0 || !ui->work_pane)
		return;
	fytim_workpane_destroy(ui->work_pane);
	ui->work_pane = NULL;
	ui->zoom_rows = 0;
}

/* Apply current grid and chrome configuration to the pane. */
static void ui_work_pane_configure(struct fyai_ui *ui)
{
	const struct fyai_cfg *cfg = ui->ctx->cfg;
	int cols = 0;

	/* A stack layout uses one tile column. */
	if (cfg->work_layout && !strcmp(cfg->work_layout, "stack"))
		cols = 1;
	else if (cfg->work_layout && !strcmp(cfg->work_layout, "columns"))
		cols = cfg->work_columns;
	(void)fytim_workpane_set_columns(ui->work_pane, cols);
	(void)fytim_workpane_set_min_tile_cols(ui->work_pane,
					       cfg->work_min_tile_cols);
	(void)fytim_workpane_set_max_rows(ui->work_pane, cfg->work_max_rows);
	(void)fytim_workpane_set_top(ui->work_pane,
				     ui_chrome_text(cfg->work_frame));
	(void)fytim_workpane_set_bottom(ui->work_pane,
					ui_chrome_text(cfg->work_frame));
	(void)fytim_workpane_set_tile_sep(ui->work_pane, cfg->tile_sep);
	(void)fytim_workpane_set_controls(ui->work_pane,
					  ui_work_controls(ui->ctx));
}

/* Open and configure the shared work pane. */
static int ui_work_pane_open(struct fyai_ui *ui)
{
	if (!ui->work_pane) {
		ui->work_pane = fytim_workpane_create(ui->ft);
		if (!ui->work_pane)
			return -1;
		ui->work_tiles = 0;
	}
	ui_work_pane_configure(ui);
	return 0;
}

struct fytim_workband *fyai_ui_work_tile_create(struct fyai_ctx *ctx)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;
	struct fytim_workband *band;

	if (!ui || !ui->ft)
		return NULL;
	if (ui_work_pane_open(ui))
		return NULL;
	band = fytim_workband_create_in(ui->work_pane);
	if (!band) {
		ui_work_pane_release(ui);
		return NULL;
	}
	(void)fytim_workband_set_max_rows(band,
					  ctx->cfg->tool_preview_lines + 2);
	ui->work_tiles++;
	return band;
}

/* Return the last granted tile width, or zero before layout. */
int fyai_ui_work_tile_cols(const struct fytim_workband *band)
{
	int cols = 0;

	if (!band || fytim_workband_granted_cols(band, &cols) != FYTIM_OK)
		return 0;
	return cols;
}

/* Retire a tile band and optionally commit it. */
void fyai_ui_work_tile_destroy(struct fyai_ctx *ctx,
			       struct fytim_workband *band, bool commit)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;

	if (!band)
		return;
	if (commit)
		(void)fytim_workband_commit(band);
	else
		fytim_workband_destroy(band);
	if (ui)
		ui_work_pane_release(ui);
}

struct fytim_surface *fyai_ui_surface_open(struct fyai_ctx *ctx, int rows,
					   int cols)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;
	struct fytim_surface *sf;

	if (!ui || !ui->ft)
		return NULL;
	if (ui_work_pane_open(ui))
		return NULL;
	sf = fytim_surface_open_in(ui->work_pane, rows, cols);
	if (!sf) {
		ui_work_pane_release(ui);
		return NULL;
	}
	(void)fytim_surface_set_bottom(sf, ui_chrome_text(ctx->cfg->tile_frame));
	ui->work_tiles++;
	return sf;
}

void fyai_ui_surface_close(struct fyai_ctx *ctx, struct fytim_surface *sf)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;

	if (!sf)
		return;
	if (ui && fytim_surface_has_keys(sf)) {
		ui->keys_fn = NULL;
		ui->keys_data = NULL;
	}
	fytim_surface_close(sf);
	if (ui)
		ui_work_pane_release(ui);
}

/* Read the terminal size from the active display. */
/* Return the display terminal descriptor. */
int fyai_ui_tty_fd(const struct fyai_ctx *ctx)
{
	const struct fyai_ui *ui = ctx ? ctx->ui : NULL;

	return ui ? ui->tty_fd : -1;
}

int fyai_ui_size(struct fyai_ctx *ctx, int *cols, int *rows)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;

	if (!ui || !ui->ft)
		return -1;
	return fytim_size(ui->ft, cols, rows) == FYTIM_OK ? 0 : -1;
}

/* Schedule a frame after a producer changes a surface. */
void fyai_ui_wake(struct fyai_ctx *ctx)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;

	if (!ui || !ui->ft)
		return;
	ui->frame_pending = true;
	ui->next_frame_ms = fyai_event_now_ms();
	ui_rearm(ui);
}

void fyai_ui_surface_commit(struct fyai_ctx *ctx, struct fytim_surface *sf)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;

	if (!sf)
		return;
	if (ui && fytim_surface_has_keys(sf)) {
		ui->keys_fn = NULL;
		ui->keys_data = NULL;
	}
	(void)fytim_surface_commit(sf);
	if (ui)
		ui_work_pane_release(ui);
}

int fyai_ui_surface_resize(struct fytim_surface *sf, int rows, int cols)
{
	if (!sf)
		return -1;
	return fytim_surface_resize(sf, rows, cols) == FYTIM_OK ? 0 : -1;
}

int fyai_ui_surface_request_rows(struct fytim_surface *sf, int rows)
{
	if (!sf || rows < 0)
		return -1;
	return fytim_surface_request_rows(sf, rows) == FYTIM_OK ? 0 : -1;
}

int fyai_ui_surface_granted_rows(const struct fytim_surface *sf)
{
	int rows = 0;

	if (!sf || fytim_surface_granted_rows(sf, &rows) != FYTIM_OK)
		return 0;
	return rows;
}

int fyai_ui_surface_set_head_frame(struct fyai_ctx *ctx,
				   struct fytim_surface *sf,
				   const char *title, const char *command,
				   const char *cause,
				   enum fyai_ui_mark mark, size_t frame,
				   unsigned int *interval_msp)
{
	static const enum fymd_indicator_state states[] = {
		[FYAI_UI_MARK_RUNNING] = FYMD_INDICATOR_PENDING,
		[FYAI_UI_MARK_OK] = FYMD_INDICATOR_SUCCESS,
		[FYAI_UI_MARK_FAILED] = FYMD_INDICATOR_FAILURE,
	};
	struct response_buffer out = {0};
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;
	char *margin;
	int saved_width;
	int cols;
	int rc;

	if (!ui || !sf || !title)
		return -1;

	/* Render chrome at the granted tile width. */
	saved_width = ctx->cfg->render_width;
	cols = fyai_ui_surface_granted_cols(sf);
	if (cols > 0)
		ctx->cfg->render_width = cols;
	/* Render the marked title row used by work bands. */
	margin = ui_indicator(ui, states[mark], frame, interval_msp);
	rc = markdown_render_tool_head(ctx->cfg, title, strlen(title), cause,
				       margin ? margin : "  ", "  ", &out);
	free(margin);
	if (!rc) {
		/* Append the shell command below the title row. */
		response_buffer_trim(&out);
		if (command && *command)
			rc = ui_append_shell_command(ctx->cfg, &out, command);
	}
	if (!rc) {
		response_buffer_trim(&out);
		rc = fytim_surface_set_top(sf, out.data ? out.data : title) ==
		     FYTIM_OK ? 0 : -1;
	}
	ctx->cfg->render_width = saved_width;
	free(out.data);
	return rc;
}

int fyai_ui_surface_set_head(struct fyai_ctx *ctx, struct fytim_surface *sf,
			     const char *title, const char *command,
			     const char *cause, enum fyai_ui_mark mark)
{
	return fyai_ui_surface_set_head_frame(ctx, sf, title, command, cause,
					      mark, 0, NULL);
}

int fyai_ui_surface_granted_cols(const struct fytim_surface *sf)
{
	int cols = 0;

	if (!sf || fytim_surface_granted_cols(sf, &cols) != FYTIM_OK)
		return 0;
	return cols;
}

int fyai_ui_surface_set_margin(struct fytim_surface *sf, const char *text)
{
	if (!sf)
		return -1;
	return fytim_surface_set_margin(sf, text) == FYTIM_OK ? 0 : -1;
}

int fyai_ui_surface_set_max_rows(struct fytim_surface *sf, int rows)
{
	if (!sf || rows < 0)
		return -1;
	return fytim_surface_set_max_rows(sf, rows) == FYTIM_OK ? 0 : -1;
}

int fyai_ui_surface_set_title(struct fytim_surface *sf, const char *top,
			      const char *bottom)
{
	if (!sf)
		return -1;
	if (fytim_surface_set_top(sf, top) != FYTIM_OK)
		return -1;
	return fytim_surface_set_bottom(sf, bottom) == FYTIM_OK ? 0 : -1;
}

void fyai_ui_surface_focus(struct fyai_ctx *ctx, struct fytim_surface *sf,
			   bool focused)
{
	const char *text;

	if (!sf)
		return;
	text = focused ? "focused · Ctrl-] returns to the prompt · "
			 "Ctrl-G moves focus" :
		ui_chrome_text(ctx->cfg->tile_frame);
	(void)fytim_surface_set_bottom(sf, text);
}

int fyai_ui_surface_publish(struct fytim_surface *sf,
			    struct fyai_terminal_view *view)
{
	struct fyai_term_cell src;
	struct fytim_cell *row;
	bool visible = false;
	int rows = 0, cols = 0;
	int first, last, r, c;
	int crow = 0, ccol = 0;

	if (!sf || !view || !fyai_terminal_view_dirty(view))
		return 0;

	fyai_terminal_view_size(view, &rows, &cols);
	row = calloc((size_t)cols, sizeof(*row));
	if (!row)
		return -1;

	/* Only the rows the program changed are copied; the library then
	 * writes only the cells that differ from what the terminal shows. */
	if (!fyai_terminal_view_take_damage(view, &first, &last)) {
		first = 0;
		last = -1;
	}
	for (r = first; r <= last && r < rows; r++) {
		for (c = 0; c < cols; c++) {
			if (!fyai_terminal_view_cell(view, r, c, &src))
				memset(&src, 0, sizeof(src));
			surface_cell(&src, &row[c]);
		}
		(void)fytim_surface_put_row(sf, r, row, cols);
	}
	free(row);

	fyai_terminal_view_cursor(view, &crow, &ccol, &visible);
	(void)fytim_surface_set_cursor(sf, crow, ccol, visible);
	return 1;
}

/* Zoom @sf to the full pane; NULL restores the grid. */
int fyai_ui_surface_zoom(struct fyai_ctx *ctx, struct fytim_surface *sf)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;

	if (!ui || !ui->work_pane)
		return -1;
	if (fytim_workpane_set_zoom(ui->work_pane, sf) != FYTIM_OK)
		return -1;
	if (sf) {
		ui->zoom_rows = ui_zoom_rows(ui);
		(void)fytim_workpane_set_max_rows(ui->work_pane,
						 ui->zoom_rows);
	} else {
		ui->zoom_rows = 0;
		(void)fytim_workpane_set_max_rows(ui->work_pane,
						 ctx->cfg->work_max_rows);
	}
	fyai_ui_wake(ctx);
	return 0;
}

struct fytim_surface *fyai_ui_surface_zoomed(const struct fyai_ctx *ctx)
{
	const struct fyai_ui *ui = ctx ? ctx->ui : NULL;

	return ui && ui->work_pane ? fytim_workpane_zoomed(ui->work_pane)
				   : NULL;
}

/* Publish emulator scroll extent to the surface. */
int fyai_ui_surface_scroll_extent(struct fytim_surface *sf, int total_rows,
				  int top_row)
{
	if (!sf)
		return -1;
	return fytim_surface_set_scroll_extent(sf, total_rows, top_row) ==
	       FYTIM_OK ? 0 : -1;
}

int fyai_ui_surface_keys(struct fyai_ctx *ctx, struct fytim_surface *sf,
			 bool take, fyai_ui_keys_fn cb, void *user)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;

	if (!ui || !sf)
		return -1;
	if (fytim_surface_set_keys(sf, take) != FYTIM_OK)
		return -1;
	ui->keys_fn = take ? cb : NULL;
	ui->keys_data = take ? user : NULL;
	return 0;
}

int fyai_ui_keys_return(struct fyai_ctx *ctx, const char *data, size_t len)
{
	struct fyai_ui *ui = ctx ? ctx->ui : NULL;

	if (!ui || !ui->ft || !data || !len)
		return -1;
	if (fytim_keys_return(ui->ft, data, len) != FYTIM_OK)
		return -1;
	/* Schedule a frame to read the returned bytes. */
	fyai_ui_wake(ctx);
	return 0;
}
