/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The one entry point every harness in tools/fuzz/ defines, in libFuzzer's
 * spelling, so that each file builds two ways from one source: against
 * libFuzzer in the clang trees, for a campaign, and against replay.c
 * everywhere else, which runs a committed corpus and a fixed-seed run of
 * damaged copies of it on every build.
 *
 * An input is always handed over in an allocation of exactly its own length,
 * so a read one byte past the end is a report under the checkers.
 */

#ifndef FNL_FUZZ_H
#define FNL_FUZZ_H

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#endif /* FNL_FUZZ_H */
