// SPDX-License-Identifier: MIT
#ifndef STRING_LIST_H
#ifndef ZC_ALLOW_INTERNAL
#error "utils/string_list.h is internal to Zen C. Include the appropriate public header instead."
#endif

#define STRING_LIST_H

/**
 * @brief Growable list of distinct strings in the libc heap.
 *
 * Unlike zvec, which allocates from the arena, it survives arena rewinds (e.g. the LSP's
 * read-only requests). Zero-initialize it and release it with string_list_free().
 */
typedef struct
{
    char **items;
    int count;
    int capacity;
} StringList;

int string_list_contains(const StringList *list, const char *text);

/**
 * @brief Appends a copy of `text`, unless it is empty or already in the list.
 */
void string_list_add(StringList *list, const char *text);

void string_list_free(StringList *list);

#endif
