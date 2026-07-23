/*
 * fyai_config_test_stubs.c - narrow stubs for the config-root unit test
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdbool.h>
#include <stdio.h>

#include "commands.h"
#include "fyai.h"

int fyai_ui_external_begin(struct fyai_ctx *ctx)
{
	(void)ctx;
	return 0;
}

int fyai_ui_external_end(struct fyai_ctx *ctx)
{
	(void)ctx;
	return 0;
}
#include "fyai_markdown.h"

const char *fyai_api_to_string(enum fyai_api_mode api)
{
	switch (api) {
	case FYAI_API_CHAT_COMPLETIONS:
		return "chat-completions";
	case FYAI_API_MESSAGES:
		return "messages";
	case FYAI_API_RESPONSES:
	default:
		return "responses";
	}
}

const struct fyai_verb *fyai_id_to_verb(enum fyai_verb_id id)
{
	(void)id;
	return NULL;
}

enum fyai_verb_id fyai_get_verb_id(const char *name)
{
	(void)name;
	return FYAIVID_INVALID;
}

bool fyai_is_verb(const char *name)
{
	(void)name;
	return false;
}

int fyai_configure(struct fyai_cfg *cfg, int argc, char *argv[])
{
	(void)cfg;
	(void)argc;
	(void)argv;
	return -1;
}

void fyai_usage(FILE *fp, const char *progname, const char *color_mode)
{
	(void)fp;
	(void)progname;
	(void)color_mode;
}

void fyai_markdown_load_style(struct fyai_cfg *cfg)
{
	(void)cfg;
}

bool markdown_theme_valid(const char *name)
{
	(void)name;
	return true;
}

bool markdown_theme_selector_valid(const char *selector)
{
	(void)selector;
	return true;
}

const char *markdown_theme_names(char *buf, size_t bufsz)
{
	snprintf(buf, bufsz, "%s", "default");
	return buf;
}

int fyai_print_markdown(const char *text, struct fyai_cfg *cfg)
{
	(void)text;
	(void)cfg;
	return -1;
}
