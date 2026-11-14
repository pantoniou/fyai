/*
 * fyai_front.h - the front end component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_FRONT_H
#define FYAI_FRONT_H

#include <stdbool.h>

struct fyai_browser;
struct fyai_catalog_update_request;
struct fyai_cmd_call;
struct fyai_config_edit_request;
struct fyai_diag;
struct fyai_display_output;
struct fyai_loop;
struct fyai_sink;
struct fyai_ui;
struct fyai_uidraw;
struct fyai_uilog;
struct fyai_uimodel;
struct fyai_uitranscript;
struct fyai_uiwork;
struct fyai_workpane_manager;

/*
 * Everything that presents the run to the user and takes the user's input:
 * the one rendering sink, the terminal UI, the model that renderers read and
 * its stores, the open transcript document, the work pane, the browser, the
 * command in flight and the requests of the editor and the catalogue update.
 * It is the part that the TUI split moves to the other thread. The component
 * stands on the loop and the diagnostics. The owner is struct fyai_app; a
 * view context keeps a copy of its own.
 */
struct fyai_front {
	/* Dependencies. */
	struct fyai_loop *loop;
	struct fyai_diag *diag;

	/* The one rendering component. Every byte the user sees goes here. */
	struct fyai_sink *sink;
	struct fyai_ui *ui;
	/* The state that renderers read (src/fyai_uimodel.h). */
	struct fyai_uimodel *uimodel;
	struct fyai_uiwork *uiwork;	/* the bands of the display (src/fyai_uiwork.h) */
	/* A copy that renders stored output: it has no UI model. */
	bool uimodel_detached;
	/*
	 * The process publishes the model and draws nothing: a remote worker,
	 * whose client draws the model it replicates.
	 */
	bool uidraw_off;
	struct fyai_uitranscript *uitranscript;	/* the live document (src/fyai_uitranscript.h) */
	struct fyai_uilog *uilog;	/* the span of a command (src/fyai_uilog.h) */
	struct fyai_uidraw *uidraw;	/* the renderer of the model (src/fyai_uidraw.h) */
	struct fyai_browser *browser;
	/* The one owner of work-pane geometry, focus, and zoom. */
	struct fyai_workpane_manager *workpane;
	/*
	 * The sole progressive transcript document for the active user or
	 * assistant output. Owned by this context, never by a signal handler.
	 */
	struct fyai_display_output *display_output;
	struct fyai_config_edit_request *config_edit;
	struct fyai_catalog_update_request *catalog_update;
	struct fyai_cmd_call *cmd_call;	/* the active async command */
};

void fyai_front_init(struct fyai_front *front, struct fyai_loop *loop,
		     struct fyai_diag *diag);

#endif
