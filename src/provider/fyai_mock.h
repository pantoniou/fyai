/*
 * fyai_mock.h - scenario-driven mock provider for the test suite
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_MOCK_H
#define FYAI_MOCK_H

#include <stdbool.h>

#include <libfyaml.h>

struct fyai_ctx;

/*
 * Serve @scenario (a YAML or JSON file with a `steps` sequence) on a loopback
 * port and write `port`, `requests.jsonl` and `served` in @rundir. Run on the
 * event loop of @ctx until the process is stopped. Parsed values live in @gb,
 * which must outlive the call. With @log, report each request on the status
 * stream. Return -1 with a diagnostic when the server cannot start.
 */
int fyai_mock_serve(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		    const char *scenario, const char *rundir, bool log);

#endif
