// SPDX-License-Identifier: MIT
#include "plugins/plugin_manager.h"
#include "parser.h"
#include "utils/format_expr.h"
#include "utils/colors.h"
#include "utils/utils.h"
#include "constants.h"
#include "ast/primitives.h"
#include <ctype.h>
#include "analysis/const_fold.h"

void enter_scope(ParserContext *ctx)
{
    Scope *s = symbol_scope_create(ctx->current_scope, NULL);
    ctx->current_scope = s;
}

void exit_scope(ParserContext *ctx)
{
    if (!ctx->current_scope || ctx->current_scope == ctx->global_scope)
    {
        return;
    }

    ZenSymbol *sym = ctx->current_scope->symbols;
    while (sym)
    {
        if (!sym->is_used && strcmp(sym->name, "self") != 0 && sym->name[0] != '_')
        {
        }
        sym = sym->next;
    }

    ctx->current_scope = ctx->current_scope->parent;
}

// ** The set of all_symbols, by name, kind and place **

static unsigned all_symbols_seen_hash(const ZenSymbol *s)
{
    unsigned h = 2166136261u; // FNV-1a
    for (const unsigned char *c = (const unsigned char *)s->name; *c; c++)
    {
        h = (h ^ *c) * 16777619u;
    }
    h = (h ^ (unsigned)s->decl_token.line) * 16777619u;
    h = (h ^ (unsigned)s->decl_token.col) * 16777619u;
    h = (h ^ (unsigned)s->kind) * 16777619u;
    return h;
}

// Whether a symbol like `s` is in the set. `slot` is where it is, or would go.
static int all_symbols_seen_find(ParserContext *ctx, const ZenSymbol *s, unsigned *slot)
{
    if (!ctx->all_symbols_seen)
    {
        return 0;
    }
    unsigned mask = ctx->all_symbols_seen_cap - 1;
    unsigned i = all_symbols_seen_hash(s) & mask;
    while (ctx->all_symbols_seen[i])
    {
        const ZenSymbol *curr = ctx->all_symbols_seen[i];
        if (curr->kind == s->kind && curr->decl_token.line == s->decl_token.line &&
            curr->decl_token.col == s->decl_token.col && strcmp(curr->name, s->name) == 0)
        {
            *slot = i;
            return 1;
        }
        i = (i + 1) & mask;
    }
    *slot = i;
    return 0;
}

static void all_symbols_seen_add(ParserContext *ctx, ZenSymbol *s)
{
    // Kept at most half full.
    if ((ctx->all_symbols_seen_count + 1) * 2 > ctx->all_symbols_seen_cap)
    {
        unsigned old_cap = ctx->all_symbols_seen_cap;
        ZenSymbol **old = ctx->all_symbols_seen;
        unsigned cap = old_cap ? old_cap * 2 : 256;
        ctx->all_symbols_seen = xmalloc(cap * sizeof(ZenSymbol *));
        memset(ctx->all_symbols_seen, 0, cap * sizeof(ZenSymbol *));
        ctx->all_symbols_seen_cap = cap;
        for (unsigned i = 0; i < old_cap; i++)
        {
            if (old[i])
            {
                unsigned j = all_symbols_seen_hash(old[i]) & (cap - 1);
                while (ctx->all_symbols_seen[j])
                {
                    j = (j + 1) & (cap - 1);
                }
                ctx->all_symbols_seen[j] = old[i];
            }
        }
        zfree(old);
    }
    unsigned slot = 0;
    if (!all_symbols_seen_find(ctx, s, &slot))
    {
        ctx->all_symbols_seen[slot] = s;
        ctx->all_symbols_seen_count++;
    }
}

void register_symbol_to_lsp(ParserContext *ctx, ZenSymbol *s)
{
    if (!ctx || !s)
    {
        return;
    }

    // Already there? A set by name, kind and place answers it: this runs for every
    // symbol, and many share a name (every function's `i`).
    unsigned slot = 0;
    if (s->name)
    {
        if (all_symbols_seen_find(ctx, s, &slot))
        {
            return;
        }
    }

    ZenSymbol *lsp_copy = xmalloc(sizeof(ZenSymbol));
    memcpy(lsp_copy, s, sizeof(ZenSymbol));
    lsp_copy->original = s;
    lsp_copy->next = ctx->all_symbols;
    ctx->all_symbols = lsp_copy;
    if (s->name)
    {
        lsp_copy->name = xstrdup(s->name);
    }
    if (s->cfg_condition)
    {
        lsp_copy->cfg_condition = xstrdup(s->cfg_condition);
    }

    lsp_copy->is_local = s->is_local;

    // After the name is the copy's own: these read it.
    symbol_index_added(&ctx->all_symbols_index, ctx->all_symbols, lsp_copy, SYMBOL_LINK_ALL);
    if (lsp_copy->name)
    {
        all_symbols_seen_add(ctx, lsp_copy);
    }
    if (lsp_copy->kind == SYM_STRUCT || lsp_copy->kind == SYM_ENUM)
    {
        lsp_copy->type_next = ctx->all_type_symbols;
        ctx->all_type_symbols = lsp_copy;
        type_name_note(ctx, lsp_copy->name);
    }
}
