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

typedef void (*test_hook_fn)(void);

static test_hook_fn g_test_setup_hook = NULL;
static test_hook_fn g_test_teardown_hook = NULL;
static size_t g_test_active_allocs = 0;
static size_t g_test_total_alloc_bytes = 0;

static inline void test_register_setup(test_hook_fn fn)
{
	g_test_setup_hook = fn;
}

static inline void test_register_teardown(test_hook_fn fn)
{
	g_test_teardown_hook = fn;
}

static inline void test_track_alloc(void *ptr, size_t sz)
{
	if (ptr) {
		g_test_active_allocs++;
		g_test_total_alloc_bytes += sz;
	}
}

static inline void test_track_free(void *ptr)
{
	if (ptr && g_test_active_allocs > 0)
		g_test_active_allocs--;
}

static inline size_t test_get_active_allocs(void)
{
	return g_test_active_allocs;
}

static inline void test_reset_alloc_tracking(void)
{
	g_test_active_allocs = 0;
	g_test_total_alloc_bytes = 0;
}

static inline void *test_malloc(size_t sz)
{
	void *ptr = malloc(sz);
	test_track_alloc(ptr, sz);
	return ptr;
}

static inline void *test_calloc(size_t nmemb, size_t sz)
{
	void *ptr = calloc(nmemb, sz);
	test_track_alloc(ptr, nmemb * sz);
	return ptr;
}

static inline void test_free(void *ptr)
{
	test_track_free(ptr);
	free(ptr);
}

#define RUN_TEST(fn) do { \
	if (g_test_setup_hook) \
		g_test_setup_hook(); \
	size_t _alloc_before = g_test_active_allocs; \
	printf("[TEST] Running %s...\n", #fn); \
	fn(); \
	if (g_test_teardown_hook) \
		g_test_teardown_hook(); \
	size_t _alloc_after = g_test_active_allocs; \
	if (_alloc_after > _alloc_before) { \
		fprintf(stderr, "[FAIL] %s leaked memory: %zu unfreed allocations\n", \
			#fn, _alloc_after - _alloc_before); \
		abort(); \
	} \
	printf("[PASS] %s passed\n", #fn); \
} while (0)

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
