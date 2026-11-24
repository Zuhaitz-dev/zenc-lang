// SPDX-License-Identifier: MIT
#include "../arena.h"
#include "symbols.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Scope *symbol_scope_create(Scope *parent, const char *name)
{
    Scope *s = xmalloc(sizeof(Scope));
    memset(s, 0, sizeof(Scope));
    s->parent = parent;
    if (name)
    {
        s->name = xstrdup(name);
    }
    return s;
}

// ** The name index **

// A list this short is as quick to walk as to index.
#define SYMBOL_INDEX_MIN 32

#define SYMBOL_LINK(sym, link) (*(ZenSymbol **)((char *)(sym) + (link)))

static unsigned symbol_name_hash(const char *name)
{
    unsigned h = 2166136261u; // FNV-1a
    for (const unsigned char *c = (const unsigned char *)name; *c; c++)
    {
        h = (h ^ *c) * 16777619u;
    }
    return h;
}

// Makes the buckets anew from the whole list, each chain in the list's order.
static void symbol_index_build(SymbolIndex *ix, ZenSymbol *head, size_t link)
{
    unsigned buckets = 64;
    while (buckets < ix->count * 2)
    {
        buckets *= 2;
    }
    ix->buckets = xmalloc(buckets * sizeof(ZenSymbol *));
    memset(ix->buckets, 0, buckets * sizeof(ZenSymbol *));
    ix->bucket_count = buckets;

    // Oldest first, each put at its chain's head, leaves the newest at the head.
    ZenSymbol **all = xmalloc((ix->count ? ix->count : 1) * sizeof(ZenSymbol *));
    unsigned n = 0;
    for (ZenSymbol *sym = head; sym && n < ix->count; sym = sym->next)
    {
        all[n++] = sym;
    }
    while (n > 0)
    {
        ZenSymbol *sym = all[--n];
        unsigned slot = sym->name ? symbol_name_hash(sym->name) & (buckets - 1) : 0;
        SYMBOL_LINK(sym, link) = ix->buckets[slot];
        ix->buckets[slot] = sym;
    }
    zfree(all);
}

void symbol_index_added(SymbolIndex *ix, ZenSymbol *head, ZenSymbol *sym, size_t link)
{
    ix->count++;
    if (!ix->buckets)
    {
        if (ix->count >= SYMBOL_INDEX_MIN)
        {
            symbol_index_build(ix, head, link);
        }
        return;
    }
    if (ix->count > ix->bucket_count * 2)
    {
        symbol_index_build(ix, head, link); // more buckets
        return;
    }
    unsigned slot = sym->name ? symbol_name_hash(sym->name) & (ix->bucket_count - 1) : 0;
    SYMBOL_LINK(sym, link) = ix->buckets[slot];
    ix->buckets[slot] = sym;
}

int symbol_index_ready(const SymbolIndex *ix)
{
    return ix->buckets != NULL;
}

ZenSymbol *symbol_index_find(const SymbolIndex *ix, const char *name, const ZenSymbol *prev,
                             size_t link)
{
    ZenSymbol *sym = prev ? SYMBOL_LINK(prev, link)
                          : ix->buckets[symbol_name_hash(name) & (ix->bucket_count - 1)];
    while (sym)
    {
        if (sym->name && sym->name[0] == name[0] && strcmp(sym->name, name) == 0)
        {
            return sym;
        }
        sym = SYMBOL_LINK(sym, link);
    }
    return NULL;
}

ZenSymbol *symbol_add(Scope *s, const char *name, SymbolKind kind)
{
    if (!s || !name)
    {
        return NULL;
    }

    ZenSymbol *sym = xmalloc(sizeof(ZenSymbol));
    memset(sym, 0, sizeof(ZenSymbol));
    sym->name = xstrdup(name);
    sym->kind = kind;

    sym->next = s->symbols;
    s->symbols = sym;
    symbol_index_added(&s->index, s->symbols, sym, SYMBOL_LINK_SCOPE);

    return sym;
}

ZenSymbol *symbol_lookup_local(Scope *s, const char *name)
{
    if (!s || !name)
    {
        return NULL;
    }

    if (symbol_index_ready(&s->index))
    {
        return symbol_index_find(&s->index, name, NULL, SYMBOL_LINK_SCOPE);
    }

    ZenSymbol *curr = s->symbols;
    while (curr)
    {
        if (curr->name && curr->name[0] == name[0] && strcmp(curr->name, name) == 0)
        {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

ZenSymbol *symbol_lookup(Scope *s, const char *name)
{
    if (!name)
    {
        return NULL;
    }

    Scope *curr_scope = s;
    while (curr_scope)
    {
        ZenSymbol *sym = symbol_lookup_local(curr_scope, name);
        if (sym)
        {
            return sym;
        }
        curr_scope = curr_scope->parent;
    }
    return NULL;
}

ZenSymbol *symbol_lookup_kind(Scope *s, const char *name, SymbolKind kind)
{
    if (!name)
    {
        return NULL;
    }

    Scope *curr_scope = s;
    while (curr_scope)
    {
        if (symbol_index_ready(&curr_scope->index))
        {
            ZenSymbol *named = NULL;
            while ((named = symbol_index_find(&curr_scope->index, name, named,
                                              SYMBOL_LINK_SCOPE)) != NULL)
            {
                if (named->kind == kind)
                {
                    return named;
                }
            }
            curr_scope = curr_scope->parent;
            continue;
        }

        ZenSymbol *sym = curr_scope->symbols;
        while (sym)
        {
            if (sym->kind == kind && sym->name && sym->name[0] == name[0] &&
                strcmp(sym->name, name) == 0)
            {
                return sym;
            }
            sym = sym->next;
        }
        curr_scope = curr_scope->parent;
    }
    return NULL;
}
