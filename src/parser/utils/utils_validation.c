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

void register_type_usage(ParserContext *ctx, const char *name, Token t)
{
    if (ctx->is_speculative)
    {
        return;
    }

    TypeUsage *u = xmalloc(sizeof(TypeUsage));
    u->name = xstrdup(name);
    u->location = t;
    u->next = ctx->pending_type_validations;
    ctx->pending_type_validations = u;
}

int validate_types(ParserContext *ctx)
{
    int errors = 0;
    TypeUsage *u = ctx->pending_type_validations;
    while (u)
    {
        ASTNode *def = find_struct_def(ctx, u->name);
        if (!def)
        {
            const char *alias = find_type_alias(ctx, u->name);
            if (!alias)
            {
                SelectiveImport *si = find_selective_import(ctx, u->name);
                if (!si && !is_extern_symbol(ctx, u->name))
                {
                    if (!is_trait(u->name) && TYPE_UNKNOWN == find_primitive_kind(u->name))
                    {
                        int whitelisted = 0;
                        if (ctx->config->c_type_whitelist)
                        {
                            char **ptr = ctx->config->c_type_whitelist;
                            while (*ptr)
                            {
                                if (strcmp(u->name, *ptr) == 0)
                                {
                                    whitelisted = 1;
                                    break;
                                }
                                ptr++;
                            }
                        }

                        if (whitelisted)
                        {
                            u = u->next;
                            continue;
                        }

                        if (!ctx->config->mode_lsp && !ctx->config->mode_doc)
                        {
                            char msg[MAX_SHORT_MSG_LEN];
                            snprintf(msg, sizeof(msg),
                                     "Unknown type '%s' (assuming external C struct)", u->name);
                            const char *hint = get_closest_type_hint(ctx, u->name);
                            if (hint)
                            {
                                char help[MAX_MANGLED_NAME_LEN];
                                snprintf(help, sizeof(help), "Did you mean '%s'?", hint);
                                zwarn_with_suggestion(u->location, msg, help);
                            }
                            else
                            {
                                zwarn_at(u->location, "%s", msg);
                            }
                        }
                    }
                }
            }
        }
        u = u->next;
    }
    return errors == 0;
}

void propagate_vector_inner_types(ParserContext *ctx)
{
    StructRef *ref = ctx->parsed_structs_list;
    while (ref)
    {
        ASTNode *strct = ref->node;
        if (strct && strct->kind == NODE_STRUCT && strct->type_info &&
            strct->type_info->kind == TYPE_VECTOR && !strct->type_info->inner)
        {
            if (strct->strct.fields && strct->strct.fields->type_info)
            {
                strct->type_info->inner = strct->strct.fields->type_info;
            }
        }
        ref = ref->next;
    }
}

void propagate_drop_traits(ParserContext *ctx)
{
    int changed = 1;
    while (changed)
    {
        changed = 0;

        StructRef *ref = ctx->parsed_structs_list;
        while (ref)
        {
            ASTNode *strct = ref->node;
            if (strct && strct->kind == NODE_STRUCT && strct->type_info &&
                !strct->type_info->traits.has_drop)
            {
                ASTNode *field = strct->strct.fields;
                while (field)
                {
                    Type *ft = field->type_info;
                    if (ft)
                    {
                        if (ft->kind == TYPE_VECTOR)
                        {
                            strct->type_info->traits.has_drop = 1;
                            ft->traits.has_drop = 1;
                            changed = 1;
                            break;
                        }
                        if (ft->kind == TYPE_FUNCTION && ft->traits.has_drop && !ft->is_raw)
                        {
                            strct->type_info->traits.has_drop = 1;
                            ft->traits.has_drop = 1;
                            changed = 1;
                            break;
                        }
                        if (ft->kind == TYPE_STRUCT && ft->name)
                        {
                            ASTNode *fdef = find_struct_def(ctx, ft->name);
                            if (fdef && fdef->type_info && fdef->type_info->traits.has_drop)
                            {
                                strct->type_info->traits.has_drop = 1;
                                ft->traits.has_drop = 1;
                                changed = 1;
                                break;
                            }
                        }
                    }
                    field = field->next;
                }
            }
            ref = ref->next;
        }

        ASTNode *ins = ctx->instantiated_structs;
        while (ins)
        {
            if (ins->kind == NODE_STRUCT && ins->type_info && !ins->type_info->traits.has_drop)
            {
                ASTNode *field = ins->strct.fields;
                while (field)
                {
                    Type *ft = field->type_info;
                    if (ft)
                    {
                        if (ft->kind == TYPE_VECTOR)
                        {
                            ins->type_info->traits.has_drop = 1;
                            ft->traits.has_drop = 1;
                            changed = 1;
                            break;
                        }
                        if (ft->kind == TYPE_FUNCTION && ft->traits.has_drop && !ft->is_raw)
                        {
                            ins->type_info->traits.has_drop = 1;
                            ft->traits.has_drop = 1;
                            changed = 1;
                            break;
                        }
                        if (ft->kind == TYPE_STRUCT && ft->name)
                        {
                            ASTNode *fdef = find_struct_def(ctx, ft->name);
                            if (fdef && fdef->type_info && fdef->type_info->traits.has_drop)
                            {
                                ins->type_info->traits.has_drop = 1;
                                ft->traits.has_drop = 1;
                                changed = 1;
                                break;
                            }
                        }
                    }
                    field = field->next;
                }
            }
            ins = ins->next;
        }
    }
}

void fix_type_refs_has_drop(ParserContext *ctx)
{
    if (!ctx)
    {
        return;
    }

    for (StructRef *ref = ctx->parsed_funcs_list; ref; ref = ref->next)
    {
        ASTNode *fn = ref->node;
        if (!fn || fn->kind != NODE_FUNCTION)
        {
            continue;
        }

        for (int i = 0; i < fn->func.count; i++)
        {
            Type *at = fn->func.arg_types[i];
            if (at && at->kind == TYPE_STRUCT && at->name && !at->traits.has_drop)
            {
                ASTNode *def = find_struct_def(ctx, at->name);
                if (def && def->type_info && def->type_info->traits.has_drop)
                {
                    at->traits.has_drop = 1;
                }
            }
        }

        if (fn->func.ret_type_info && fn->func.ret_type_info->kind == TYPE_STRUCT &&
            fn->func.ret_type_info->name && !fn->func.ret_type_info->traits.has_drop)
        {
            ASTNode *def = find_struct_def(ctx, fn->func.ret_type_info->name);
            if (def && def->type_info && def->type_info->traits.has_drop)
            {
                fn->func.ret_type_info->traits.has_drop = 1;
            }
        }
    }

    for (StructRef *ref = ctx->parsed_structs_list; ref; ref = ref->next)
    {
        ASTNode *strct = ref->node;
        if (!strct || strct->kind != NODE_STRUCT)
        {
            continue;
        }
        for (ASTNode *field = strct->strct.fields; field; field = field->next)
        {
            Type *ft = field->type_info;
            if (ft && ft->kind == TYPE_STRUCT && ft->name && !ft->traits.has_drop)
            {
                ASTNode *def = find_struct_def(ctx, ft->name);
                if (def && def->type_info && def->type_info->traits.has_drop)
                {
                    ft->traits.has_drop = 1;
                }
            }
        }
    }

    for (ASTNode *ins = ctx->instantiated_structs; ins; ins = ins->next)
    {
        if (ins->kind != NODE_STRUCT)
        {
            continue;
        }
        for (ASTNode *field = ins->strct.fields; field; field = field->next)
        {
            Type *ft = field->type_info;
            if (ft && ft->kind == TYPE_STRUCT && ft->name && !ft->traits.has_drop)
            {
                ASTNode *def = find_struct_def(ctx, ft->name);
                if (def && def->type_info && def->type_info->traits.has_drop)
                {
                    ft->traits.has_drop = 1;
                }
            }
        }
    }
}

typedef struct
{
    const char **names;
    int count;
    int cap;
} DefinedFunctions;

// Adds a defined function name; returns 0 if it was already defined.
static int defined_functions_add(DefinedFunctions *set, const char *name)
{
    for (const char **n = set->names; n < set->names + set->count; n++)
    {
        if (strcmp(*n, name) == 0)
        {
            return 0;
        }
    }
    if (set->count == set->cap)
    {
        set->cap = set->cap ? set->cap * 2 : 64;
        set->names = xrealloc(set->names, sizeof(const char *) * (size_t)set->cap);
    }
    *(set->names + set->count) = name;
    set->count++;
    return 1;
}

// A function with a body, emitted unconditionally under its C name.
static void check_function_definition(ASTNode *f, const char *owner, DefinedFunctions *set,
                                      int *errors)
{
    if (f->kind != NODE_FUNCTION || !f->func.body || !f->func.name || f->func.generic_params ||
        f->cfg_condition)
    {
        return;
    }
    if (defined_functions_add(set, f->func.name))
    {
        return;
    }

    size_t owner_len = owner ? strlen(owner) : 0;
    if (owner && strncmp(f->func.name, owner, owner_len) == 0 &&
        strncmp(f->func.name + owner_len, "__", 2) == 0)
    {
        zerror_at(f->token, "Redefinition of method '%s::%s'", owner, f->func.name + owner_len + 2);
    }
    else
    {
        zerror_at(f->token, "Redefinition of function '%s'", f->func.name);
    }
    (*errors)++;
}

static void check_redefinitions_in(ASTNode *node, DefinedFunctions *set, int *errors)
{
    // Definitions between `#if` and `#endif` may be alternatives: not checked.
    int preproc_depth = 0;
    for (; node; node = node->next)
    {
        if (node->kind == NODE_PREPROC_DIRECTIVE && node->raw_stmt.content)
        {
            const char *d = node->raw_stmt.content;
            while (*d == '#' || *d == ' ' || *d == '\t')
            {
                d++;
            }
            if (strncmp(d, "if", 2) == 0)
            {
                preproc_depth++;
            }
            else if (strncmp(d, "endif", 5) == 0 && preproc_depth > 0)
            {
                preproc_depth--;
            }
            continue;
        }
        if (node->kind == NODE_IMPORT)
        {
            check_redefinitions_in(node->import_stmt.module_root, set, errors);
            continue;
        }
        if (preproc_depth > 0 || node->cfg_condition)
        {
            continue;
        }
        if (node->kind == NODE_FUNCTION)
        {
            check_function_definition(node, NULL, set, errors);
        }
        else if (node->kind == NODE_IMPL)
        {
            for (ASTNode *m = node->impl.methods; m; m = m->next)
            {
                check_function_definition(m, node->impl.struct_name, set, errors);
            }
        }
        else if (node->kind == NODE_IMPL_TRAIT)
        {
            for (ASTNode *m = node->impl_trait.methods; m; m = m->next)
            {
                check_function_definition(m, NULL, set, errors);
            }
        }
    }
}

int validate_function_definitions(ParserContext *ctx, ASTNode *root)
{
    // MISRA mode reports duplicate identifiers itself (rules 5.8 and 5.9).
    if (ctx->config->mode_lsp || ctx->config->misra_mode || !root || root->kind != NODE_ROOT)
    {
        return 1;
    }
    DefinedFunctions set = {0};
    int errors = 0;
    check_redefinitions_in(root->root.children, &set, &errors);
    return errors == 0;
}
