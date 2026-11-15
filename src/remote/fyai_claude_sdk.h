/* SPDX-License-Identifier: MIT */
#ifndef FYAI_CLAUDE_SDK_H
#define FYAI_CLAUDE_SDK_H

#include <stdbool.h>

/* The Claude Code version that this front end reports to an SDK. */
#define FYAI_CLAUDE_SDK_VERSION	"2.0.0"

/*
 * True when a Claude Agent SDK runs this program as its CLI: the program is
 * named `claude`, or the arguments select a stream-json input.
 */
bool fyai_claude_sdk_argv(int argc, char *const argv[]);

/*
 * The command line of an SDK with the claude-sdk command after the program
 * name, as a vector of @argc + 1 arguments and a NULL. The caller owns the
 * vector, not the strings. NULL when the command line is not of an SDK, asks
 * for the version, or cannot be allocated.
 */
char **fyai_claude_sdk_args(int argc, char *argv[]);

/*
 * The version line that an SDK reads, when the command line of an SDK asks
 * for it, else NULL.
 */
const char *fyai_claude_sdk_version(int argc, char *const argv[]);

#endif
