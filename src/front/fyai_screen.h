/*
 * fyai_screen.h - the terminal screen component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_SCREEN_H
#define FYAI_SCREEN_H

struct fyai_browser;
struct fyai_catalog_update_request;
struct fyai_cmd_call;
struct fyai_complete_cache;
struct fyai_config_edit_request;
struct fyai_diag;
struct fyai_loop;
struct fyai_ui;
struct fyai_workpane_manager;

/*
 * The terminal UI and what the user does through it: the work pane, the
 * browser, the command in flight and the requests of the editor and the
 * catalogue update. It is the part that the TUI split moves to the other
 * thread. It stands on the loop and the diagnostics. The owner is struct
 * fyai_app; a view context keeps a copy of its own, with no UI.
 */
struct fyai_screen {
	/* Dependencies. */
	struct fyai_loop *loop;
	struct fyai_diag *diag;

	struct fyai_ui *ui;
	struct fyai_browser *browser;
	/* The one owner of work-pane geometry, focus, and zoom. */
	struct fyai_workpane_manager *workpane;
	struct fyai_config_edit_request *config_edit;
	struct fyai_catalog_update_request *catalog_update;
	struct fyai_cmd_call *cmd_call;	/* the active async command */
	bool quit;			/* quit without presenting the result */
	/* The candidates that the worker gave, in a client with no arena. */
	struct fyai_complete_cache *complete;
};

void fyai_screen_init(struct fyai_screen *screen, struct fyai_loop *loop,
		      struct fyai_diag *diag);
/* The child draws nothing of the parent: its UI, tiles and command. */
void fyai_screen_abandon(struct fyai_screen *screen);

#endif
