/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */

#ifndef LIBENA_TEST_FRAMEWORK_H
#define LIBENA_TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define TEST_ASSERT(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "[FAIL] %s:%d: assertion failed: %s\n", \
			__FILE__, __LINE__, #cond); \
		abort(); \
	} \
} while (0)

#define TEST_ASSERT_EQ(actual, expected) do { \
	int64_t _act = (int64_t)(actual); \
	int64_t _exp = (int64_t)(expected); \
	if (_act != _exp) { \
		fprintf(stderr, "[FAIL] %s:%d: expected %s == %s (actual=0x%lx / %ld, expected=0x%lx / %ld)\n", \
			__FILE__, __LINE__, #actual, #expected, \
			(unsigned long)_act, (long)_act, \
			(unsigned long)_exp, (long)_exp); \
		abort(); \
	} \
} while (0)

#define TEST_ASSERT_NE(actual, expected) do { \
	int64_t _act = (int64_t)(actual); \
	int64_t _exp = (int64_t)(expected); \
	if (_act == _exp) { \
		fprintf(stderr, "[FAIL] %s:%d: expected %s != %s (both=0x%lx / %ld)\n", \
			__FILE__, __LINE__, #actual, #expected, \
			(unsigned long)_act, (long)_act); \
		abort(); \
	} \
} while (0)

#define TEST_ASSERT_NOT_NULL(ptr) do { \
	if ((ptr) == NULL) { \
		fprintf(stderr, "[FAIL] %s:%d: expected non-NULL pointer for %s\n", \
			__FILE__, __LINE__, #ptr); \
		abort(); \
	} \
} while (0)

#define TEST_ASSERT_NULL(ptr) do { \
	if ((ptr) != NULL) { \
		fprintf(stderr, "[FAIL] %s:%d: expected NULL pointer for %s (actual=%p)\n", \
			__FILE__, __LINE__, #ptr, (void *)(ptr)); \
		abort(); \
	} \
} while (0)

#define TEST_ASSERT_STR_EQ(actual, expected) do { \
	const char *_act_s = (const char *)(actual); \
	const char *_exp_s = (const char *)(expected); \
	if (strcmp(_act_s, _exp_s) != 0) { \
		fprintf(stderr, "[FAIL] %s:%d: string mismatch: expected \"%s\", got \"%s\"\n", \
			__FILE__, __LINE__, _exp_s, _act_s); \
		abort(); \
	} \
} while (0)

#endif /* LIBENA_TEST_FRAMEWORK_H */
