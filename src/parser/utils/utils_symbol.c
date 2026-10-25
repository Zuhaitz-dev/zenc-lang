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

void add_symbol(ParserContext *ctx, const char *n, const char *t, Type *type_info, int is_export)
{
    add_symbol_with_token(ctx, n, t, type_info, TOKEN_UNKNOWN, is_export);
}

void add_symbol_with_token(ParserContext *ctx, const char *n, const char *t, Type *type_info,
                           Token tok, int is_export)
{
    if (!ctx->current_scope)
    {
        if (!ctx->global_scope)
        {
            ctx->global_scope = symbol_scope_create(NULL, "Global");
        }
        ctx->current_scope = ctx->global_scope;
    }

    if (strcmp(n, "it") != 0 && strcmp(n, "self") != 0)
    {
        audit_section_5(ctx, ctx->current_scope, n, NULL, tok);
    }

    if (ctx->config->mode_lsp || ctx->config->misra_mode)
    {
        ZenSymbol *existing = symbol_lookup_local(ctx->current_scope, n);
        if (existing)
        {
            existing->type_name = t ? xstrdup(t) : NULL;
            existing->type_info = type_info;
            existing->decl_token = tok;
            return;
        }
    }

    ZenSymbol *s = symbol_add(ctx->current_scope, n, SYM_VARIABLE);
    s->is_local = (ctx->current_scope != ctx->global_scope);
    s->type_name = t ? xstrdup(t) : NULL;
    s->type_info = type_info;
    s->decl_token = tok;
    s->is_export = is_export;

    register_symbol_to_lsp(ctx, s);
}

Type *find_symbol_type_info(ParserContext *ctx, const char *n)
{
    ZenSymbol *sym = symbol_lookup(ctx->current_scope, n);
    if (sym)
    {
        return sym->type_info;
    }

    EnumVariantReg *ev = find_enum_variant(ctx, n);
    if (ev)
    {
        Type *t = type_new(TYPE_ENUM);
        t->name = xstrdup(ev->enum_name);
        return t;
    }

    return NULL;
}

char *find_symbol_type(ParserContext *ctx, const char *n)
{
    ZenSymbol *sym = symbol_lookup(ctx->current_scope, n);
    if (sym)
    {
        return sym->type_name ? xstrdup(sym->type_name) : NULL;
    }

    EnumVariantReg *ev = find_enum_variant(ctx, n);
    if (ev)
    {
        return xstrdup(ev->enum_name);
    }

    return NULL;
}

ZenSymbol *find_symbol_entry(ParserContext *ctx, const char *n)
{
    return symbol_lookup(ctx->current_scope, n);
}

ZenSymbol *find_symbol_in_all(ParserContext *ctx, const char *n)
{
    ZenSymbol *sym = ctx->all_symbols;
    while (sym)
    {
        if (sym->name[0] == n[0] && strcmp(sym->name, n) == 0)
        {
            return sym;
        }
        sym = sym->next;
    }
    return NULL;
}

void init_builtins(void)
{
    static int init = 0;
    if (init)
    {
        return;
    }
    init = 1;
}

void register_func(ParserContext *ctx, Scope *scope, const char *name, int count, char **defaults,
                   Type **arg_types, Type *ret_type, int is_varargs, int is_pure,
                   const char *link_name, Token decl_token, int is_export)
{
    if (ctx->config->mode_lsp || ctx->config->misra_mode)
    {
        FuncSig *existing = find_func(ctx, name);
        if (existing)
        {
            existing->decl_token = decl_token;
            existing->total_args = count;
            existing->defaults = defaults;
            existing->arg_types = arg_types;
            existing->ret_type = ret_type;
            existing->is_varargs = is_varargs;
            existing->is_pure = is_pure;
        }
    }

    FuncSig *f = NULL;
    if (ctx->config->mode_lsp || ctx->config->misra_mode)
    {
        f = find_func(ctx, name);
    }

    if (!f)
    {
        f = xmalloc(sizeof(FuncSig));
        f->name = xstrdup(name);
        f->next = ctx->func_registry;
        ctx->func_registry = f;
    }

    f->decl_token = decl_token;
    f->total_args = count;
    f->defaults = defaults;
    f->arg_types = arg_types;
    f->ret_type = ret_type;
    f->is_varargs = is_varargs;
    f->is_pure = is_pure;
    f->required = 0;

    Scope *target_scope = scope ? scope : ctx->current_scope;
    audit_section_5(ctx, target_scope ? target_scope : ctx->global_scope, name, link_name,
                    decl_token);

    ZenSymbol *sym = symbol_lookup_local(target_scope, name);
    if (!sym)
    {
        sym = symbol_add(scope ? scope : ctx->current_scope, name, SYM_FUNCTION);
    }
    else
    {
        sym->kind = SYM_FUNCTION;
    }
    sym->data.sig = f;
    sym->decl_token = decl_token;
    sym->is_export = is_export;
    if (link_name)
    {
        f->link_name = xstrdup(link_name);
        sym->link_name = f->link_name;
    }

    register_symbol_to_lsp(ctx, sym);

    Type *ft = type_new(TYPE_FUNCTION);
    ft->count = count;
    ft->args = arg_types;
    ft->inner = ret_type;
    ft->is_raw = 1;
    ft->traits.has_drop = 0;
    sym->type_info = ft;
}

void register_func_template(ParserContext *ctx, const char *name, const char *param, ASTNode *node)
{
    GenericFuncTemplate *t = xcalloc(1, sizeof(GenericFuncTemplate));
    t->name = xstrdup(name);
    t->generic_param = xstrdup(param);
    t->func_node = node;
    t->next = ctx->func_templates;
    ctx->func_templates = t;
}

void register_deprecated_func(ParserContext *ctx, const char *name, const char *reason)
{
    DeprecatedFunc *d = xmalloc(sizeof(DeprecatedFunc));
    d->name = xstrdup(name);
    d->reason = reason ? xstrdup(reason) : NULL;
    d->next = ctx->deprecated_funcs;
    ctx->deprecated_funcs = d;
}

GenericFuncTemplate *find_func_template(ParserContext *ctx, const char *name)
{
    GenericFuncTemplate *t = ctx->func_templates;
    while (t)
    {
        if (strcmp(t->name, name) == 0)
        {
            return t;
        }
        t = t->next;
    }
    return NULL;
}

void register_generic(ParserContext *ctx, char *name)
{
    for (int i = 0; i < ctx->known_generics_count; i++)
    {
        if (strcmp(ctx->known_generics[i], name) == 0)
        {
            return;
        }
    }
    ctx->known_generics[ctx->known_generics_count++] = xstrdup(name);
}

int is_known_generic(ParserContext *ctx, char *name)
{
    for (int i = 0; i < ctx->known_generics_count; i++)
    {
        if (strcmp(ctx->known_generics[i], name) == 0)
        {
            return 1;
        }
    }
    return 0;
}

int is_generic_dependent_str(ParserContext *ctx, const char *type_str)
{
    if (!type_str || !ctx)
    {
        return 0;
    }
    for (int i = 0; i < ctx->known_generics_count; i++)
    {
        const char *g = ctx->known_generics[i];
        const char *p = (char *)strstr(type_str, g);
        while (p)
        {
            int valid = 1;
            if (p > type_str && is_ident_char(*(p - 1)) && *(p - 1) != '_')
            {
                valid = 0;
            }
            if (valid)
            {
                const char *next = p + strlen(g);
                if (*next != '\0' && is_ident_char(*next) && *next != '_')
                {
                    if (strncmp(next, "Ptr", 3) != 0)
                    {
                        valid = 0;
                    }
                }
            }
            if (valid)
            {
                return 1;
            }
            p = strstr(p + 1, g);
        }
    }
    return 0;
}

void register_impl_template(ParserContext *ctx, const char *sname, const char *param, ASTNode *node)
{
    GenericImplTemplate *t = xmalloc(sizeof(GenericImplTemplate));
    t->struct_name = xstrdup(sname);
    t->generic_param = xstrdup(param);
    t->impl_node = node;
    t->next = ctx->impl_templates;
    ctx->impl_templates = t;

    Instantiation *inst = ctx->instantiations;
    while (inst)
    {
        if (inst->template_name && strcmp(inst->template_name, sname) == 0)
        {
            instantiate_methods(ctx, t, inst->name, inst->concrete_arg, inst->unmangled_arg);
        }
        inst = inst->next;
    }
}

void add_instantiated_func(ParserContext *ctx, ASTNode *fn)
{
    fn->next = ctx->instantiated_funcs;
    ctx->instantiated_funcs = fn;
}

void register_closure_type(ParserContext *ctx, Type *t)
{
    if (!ctx || !ctx->config->misra_mode || !t || t->kind != TYPE_FUNCTION || t->is_raw)
    {
        return;
    }
    char *name = closure_type_c_name(t);
    for (ClosureTypeReg *r = ctx->closure_types; r; r = r->next)
    {
        if (strcmp(r->c_name, name) == 0)
        {
            zfree(name);
            return;
        }
    }
    ClosureTypeReg *reg = xmalloc(sizeof(*reg));
    reg->c_name = name;
    reg->type = t;
    reg->next = ctx->closure_types;
    ctx->closure_types = reg;
}

void register_enum_variant(ParserContext *ctx, const char *vname, const char *ename, int tag)
{
    if (ctx->config->mode_lsp)
    {
        EnumVariantReg *existing = find_enum_variant_of(ctx, ename, vname);
        if (existing)
        {
            existing->tag_id = tag;
            return;
        }
    }

    audit_section_5(ctx, ctx->global_scope, vname, NULL, TOKEN_UNKNOWN);

    EnumVariantReg *r = xcalloc(1, sizeof(EnumVariantReg));
    r->enum_name = ename ? xstrdup(ename) : NULL;
    r->variant_name = vname ? xstrdup(vname) : NULL;
    r->tag_id = tag;
    r->next = ctx->enum_variants;
    ctx->enum_variants = r;
}

EnumVariantReg *find_enum_variant(ParserContext *ctx, const char *name)
{
    char *ename = NULL;
    const char *vname = name;
    const char *sep = (char *)strstr(name, "::");
    if (!sep)
    {
        sep = strstr(name, "__");
    }

    if (sep)
    {
        int elen = (int)(sep - name);
        ename = xmalloc((size_t)(elen + 1));
        strncpy(ename, name, (size_t)(elen));
        ename[elen] = 0;
        vname = sep + 2;
    }

    EnumVariantReg *r = ctx->enum_variants;
    while (r)
    {
        if (strcmp(r->variant_name, vname) == 0)
        {
            if (!ename || strcmp(r->enum_name, ename) == 0)
            {
                if (ename)
                {
                    zfree(ename);
                }
                return r;
            }
        }
        r = r->next;
    }
    if (ename)
    {
        zfree(ename);
    }
    return NULL;
}

// Several enums may share a variant name (`Slot::None`, `Attribute::None`), so
// a lookup that knows the enum must filter by it instead of taking the first
// variant with that name.
EnumVariantReg *find_enum_variant_of(ParserContext *ctx, const char *enum_name,
                                     const char *variant_name)
{
    if (!enum_name || !variant_name)
    {
        return NULL;
    }
    EnumVariantReg *r = ctx->enum_variants;
    while (r)
    {
        if (r->variant_name && r->enum_name && strcmp(r->variant_name, variant_name) == 0 &&
            strcmp(r->enum_name, enum_name) == 0)
        {
            return r;
        }
        r = r->next;
    }
    return NULL;
}

// The expression has no address of its own (a call result, a literal, an enum
// variant...). Taking its address for a `self` or `T*` parameter must go
// through a temporary (`&_rval`), or the C compiler rejects `&(rvalue)`.
int expr_is_rvalue(ParserContext *ctx, ASTNode *e)
{
    if (!e)
    {
        return 0;
    }
    switch (e->kind)
    {
    case NODE_EXPR_CALL:
    case NODE_EXPR_BINARY:
    case NODE_EXPR_STRUCT_INIT:
    case NODE_EXPR_CAST:
    case NODE_EXPR_LITERAL:
    case NODE_MATCH:
        return 1;
    case NODE_EXPR_MEMBER:
        // A field of a temporary is a temporary too (`pairs.get(0).id`), unless
        // it is reached through a pointer (`ptr->field` has an address).
        return !e->member.is_pointer_access && expr_is_rvalue(ctx, e->member.target);
    case NODE_EXPR_VAR:
    {
        // `Shape::Circle` written without (): the variant constructor's result.
        const char *sep = e->var_ref.name ? strstr(e->var_ref.name, "::") : NULL;
        if (!sep && e->var_ref.name)
        {
            sep = strstr(e->var_ref.name, "__");
        }
        if (!sep)
        {
            return 0;
        }
        size_t enum_len = (size_t)(sep - e->var_ref.name);
        char *enum_name = xmalloc(enum_len + 1);
        memcpy(enum_name, e->var_ref.name, enum_len);
        enum_name[enum_len] = 0;
        int is_variant = find_enum_variant_of(ctx, enum_name, sep + 2) != NULL;
        zfree(enum_name);
        return is_variant;
    }
    default:
        return 0;
    }
}

void register_lambda(ParserContext *ctx, ASTNode *node)
{
    LambdaRef *ref = xmalloc(sizeof(LambdaRef));
    ref->node = node;
    ref->next = NULL;

    // Keep the creation order, which is the order they are emitted in: a lambda defined inside
    // another one is created first, and the outer body uses its context and its function.
    LambdaRef **tail = &ctx->global_lambdas;
    while (*tail)
    {
        tail = &(*tail)->next;
    }
    *tail = ref;
}

void register_extern_symbol(ParserContext *ctx, const char *name)
{
    for (int i = 0; i < ctx->extern_symbol_count; i++)
    {
        if (strcmp(ctx->extern_symbols[i], name) == 0)
        {
            return;
        }
    }

    if (ctx->extern_symbol_count == 0)
    {
        ctx->extern_symbols = xmalloc(sizeof(char *) * 64);
    }
    else if (ctx->extern_symbol_count % 64 == 0)
    {
        ctx->extern_symbols =
            xrealloc(ctx->extern_symbols, sizeof(char *) * (size_t)(ctx->extern_symbol_count + 64));
    }

    ctx->extern_symbols[ctx->extern_symbol_count++] = xstrdup(name);
}

int is_extern_symbol(ParserContext *ctx, const char *name)
{
    for (int i = 0; i < ctx->extern_symbol_count; i++)
    {
        if (strcmp(ctx->extern_symbols[i], name) == 0)
        {
            return 1;
        }
    }
    return 0;
}

int should_suppress_undef_warning(ParserContext *ctx, const char *name)
{
    if (strcmp(name, "struct") == 0 || strcmp(name, "tv") == 0)
    {
        return 1;
    }

    if (is_extern_symbol(ctx, name))
    {
        return 1;
    }

    int is_all_caps = 1;
    for (const char *p = name; *p; p++)
    {
        if (islower((unsigned char)*p))
        {
            is_all_caps = 0;
            break;
        }
    }
    if (is_all_caps && name[0] != '\0')
    {
        return 1;
    }

    if (ctx->has_external_includes)
    {
        return 1;
    }

    return 0;
}

static char *find_last_sep(const char *s)
{
    const char *last = NULL;
    while ((s = strstr(s, "__")) != NULL)
    {
        last = s;
        s += 2;
    }
    return (char *)last;
}

char *find_method_owner_type_scoped(ParserContext *ctx, const char *struct_name,
                                    const char *method_name)
{
    size_t prefix_len = strlen(struct_name);

    for (FuncSig *f = ctx->func_registry; f; f = f->next)
    {
        if (!f->name)
        {
            continue;
        }

        const char *p = f->name;
        if (*p == '_')
        {
            p++;
        }
        if (strncasecmp(p, struct_name, prefix_len) != 0)
        {
            continue;
        }

        char *last_sep = find_last_sep(f->name);
        if (!last_sep)
        {
            continue;
        }
        if (strcmp(last_sep + 2, method_name) != 0)
        {
            continue;
        }

        size_t concrete_len = (size_t)(last_sep - f->name);
        char *concrete_type = xmalloc(concrete_len + 1);
        strncpy(concrete_type, f->name, concrete_len);
        concrete_type[concrete_len] = 0;

        if (ctx->cg.expected_init_type)
        {
            if (strcmp(concrete_type, ctx->cg.expected_init_type) == 0)
            {
                return concrete_type;
            }
            zfree(concrete_type);
            continue;
        }

        return concrete_type;
    }

    return NULL;
}
