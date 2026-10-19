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

static void sync_type_linkage_depth(ParserContext *ctx, Type *t, int depth)
{
    if (!t)
    {
        return;
    }
    // Guard against cyclic / pathologically deep type graphs (fuzz inputs can
    // construct self-referential or deeply nested generic types).
    if (depth > 512)
    {
        return;
    }
    if ((t->kind == TYPE_STRUCT || t->kind == TYPE_ENUM) && !t->link_name && t->name)
    {
        ASTNode *def = find_struct_def(ctx, t->name);
        if (def && def->link_name)
        {
            t->link_name = xstrdup(def->link_name);
        }
    }
    if (t->inner)
    {
        sync_type_linkage_depth(ctx, t->inner, depth + 1);
    }
    for (int i = 0; t->args && i < t->count; i++)
    {
        sync_type_linkage_depth(ctx, t->args[i], depth + 1);
    }
}

static void sync_type_linkage(ParserContext *ctx, Type *t)
{
    sync_type_linkage_depth(ctx, t, 0);
}

static void sync_link_names_recursive_depth(ParserContext *ctx, ASTNode *node, int depth)
{
    if (depth > 4096)
    {
        return;
    }

    for (; node; node = node->next)
    {
        if (node->type_info)
        {
            sync_type_linkage(ctx, node->type_info);
        }

        if (node->kind == NODE_FUNCTION)
        {
            if (node->func.ret_type_info)
            {
                sync_type_linkage(ctx, node->func.ret_type_info);
            }
            if (node->func.arg_types)
            {
                for (int i = 0; i < node->func.count; i++)
                {
                    sync_type_linkage(ctx, node->func.arg_types[i]);
                }
            }
        }

        ASTChildren children = ast_children(node);
        for (int i = 0; i < children.count; i++)
        {
            sync_link_names_recursive_depth(ctx, *children.slots[i], depth + 1);
        }
    }
}

static void sync_link_names_recursive(ParserContext *ctx, ASTNode *node)
{
    sync_link_names_recursive_depth(ctx, node, 0);
}

void audit_section_5(ParserContext *ctx, Scope *scope, const char *name, const char *link_name,
                     Token tok)
{
    if (!scope || !name)
    {
        return;
    }
    if (strcmp(name, "it") == 0 || strcmp(name, "self") == 0)
    {
        return;
    }

    if (ctx->config->misra_mode)
    {
        if (ctx->hook_check_standard_macro_name)
        {
            ctx->hook_check_standard_macro_name(tok, name);
        }
    }

    Scope *p = scope;
    int limit = (p == ctx->global_scope) ? 31 : 63;

    while (p)
    {
        ZenSymbol *sh = p->symbols;
        while (sh)
        {
            if (p != scope && strcmp(sh->name, name) == 0 && !ctx->silent_warnings)
            {
                if (ctx->config->misra_mode)
                {
                    zerror_at(tok, "MISRA Rule 5.3");
                }
                else
                {
                    warn_shadowing(tok, name);
                }
            }

            if (ctx->config->misra_mode)
            {
                const char *actual_name = link_name ? link_name : name;
                const char *sh_actual_name = sh->link_name ? sh->link_name : sh->name;

                if (strcmp(sh_actual_name, actual_name) != 0)
                {
                    if (ctx->hook_check_identifier_collision)
                    {
                        ctx->hook_check_identifier_collision(tok, sh_actual_name, actual_name,
                                                             limit);
                    }
                }
            }

            sh = sh->next;
        }
        p = p->parent;
    }
}

void sync_all_link_names(ParserContext *ctx, ASTNode *root)
{
    sync_link_names_recursive(ctx, root);
}
