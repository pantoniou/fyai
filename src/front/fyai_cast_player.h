/* SPDX-License-Identifier: MIT */
#ifndef FYAI_CAST_PLAYER_H
#define FYAI_CAST_PLAYER_H

struct fyai_ctx;

/*
 * Play the recording @path on the alternate screen, with controls: play and
 * pause, steps in time, jumps between markers, a list of the markers, a
 * timeline that a click seeks, and the wheel. @speed is the starting speed.
 * Returns 0 when the user leaves, or -1 with the cause raised.
 */
int fyai_cast_play_interactive(struct fyai_ctx *ctx, const char *path,
			       double speed);

#endif
