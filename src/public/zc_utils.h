// SPDX-License-Identifier: MIT
#ifndef ZC_UTILS_H
#define ZC_UTILS_H

/**
 * @file zc_utils.h
 * @brief Public API for libzc-utils: memory, vectors, file I/O.
 */

#include <stdio.h>
#include <stddef.h>

// Memory allocation (from zalloc.h arena system)

/** @brief Allocate memory from the arena. Never returns NULL (aborts on OOM). */
void *xmalloc(size_t size);
/** @brief Reallocate memory within the arena. Never returns NULL. */
void *xrealloc(void *ptr, size_t new_size);
/** @brief Allocate zero-initialized memory. Never returns NULL. */
void *xcalloc(size_t n, size_t size);
/** @brief Duplicate a string using arena allocation. Never returns NULL. */
char *xstrdup(const char *s);

/**
 * @brief No-op free for arena-allocated memory.
 * Actually frees nothing — the arena is reclaimed as a whole.
 */
#define zfree(ptr) ((void)0)

/**
 * @brief Redirect standard allocation to the arena.
 */
#define malloc(sz) xmalloc(sz)
#define realloc(p, s) xrealloc(p, s)
#define calloc(n, s) xcalloc(n, s)

// Emitter (output buffer)

#include "emitter.h"

// File I/O
struct CompilerConfig;

/** @brief Load a file into a heap-allocated buffer. Caller must free. */
char *load_file(const char *fn, const char *relative_to, struct CompilerConfig *cfg);
/** @brief Resolve an import path using include paths and root config. Caller must free. */
char *z_resolve_path(const char *fn, const char *relative_to, struct CompilerConfig *cfg);

#endif // ZC_UTILS_H
