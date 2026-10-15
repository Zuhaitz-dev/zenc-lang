// SPDX-License-Identifier: MIT
#include "string_list.h"
#include "zalloc.h"
#include <string.h>

int string_list_contains(const StringList *list, const char *text)
{
    for (int i = 0; i < list->count; i++)
    {
        if (strcmp(list->items[i], text) == 0)
        {
            return 1;
        }
    }
    return 0;
}

void string_list_add(StringList *list, const char *text)
{
    if (!text[0] || string_list_contains(list, text))
    {
        return;
    }
    if (list->count == list->capacity)
    {
        int capacity = list->capacity ? list->capacity * 2 : 8;
        char **items = libc_realloc(list->items, (size_t)capacity * sizeof(char *));
        if (!items)
        {
            return;
        }
        list->items = items;
        list->capacity = capacity;
    }
    size_t length = strlen(text) + 1;
    char *copy = libc_malloc(length);
    if (copy)
    {
        memcpy(copy, text, length);
        list->items[list->count++] = copy;
    }
}

void string_list_free(StringList *list)
{
    for (int i = 0; i < list->count; i++)
    {
        libc_free(list->items[i]);
    }
    libc_free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}
