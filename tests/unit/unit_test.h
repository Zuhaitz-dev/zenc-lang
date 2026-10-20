// SPDX-License-Identifier: MIT
// Include it after the compiler headers, instead of utest.h. The compiler's arena.h
// turns malloc, realloc and calloc into the arena allocators; utest.h must use the C
// library ones, because unit_main.c frees its memory with the C library's free().
#undef malloc
#undef realloc
#undef calloc
#include "utest.h"
