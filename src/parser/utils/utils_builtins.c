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

void register_builtins(ParserContext *ctx)
{
    // Built-ins are not identifiers of the program: MISRA's rule 5.10 (no identifier named like
    // a standard macro) must not reject `true` and `false`.
    void (*check_standard_macro_name)(Token tok, const char *name) =
        ctx->hook_check_standard_macro_name;
    ctx->hook_check_standard_macro_name = NULL;

    Type *t = type_new(TYPE_BOOL);
    t->is_const = 1;
    add_symbol(ctx, "true", "bool", t, 0);

    t = type_new(TYPE_BOOL);
    t->is_const = 1;
    add_symbol(ctx, "false", "bool", t, 0);

    Type *void_t = type_new(TYPE_VOID);
    add_symbol(ctx, "free", "void", void_t, 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;

    add_symbol(ctx, "strdup", "string", type_new(TYPE_STRING), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "malloc", "void*", type_new_ptr(void_t), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "realloc", "void*", type_new_ptr(void_t), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "calloc", "void*", type_new_ptr(void_t), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "puts", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "printf", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "strcmp", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "strlen", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "strcpy", "string", type_new(TYPE_STRING), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "strcat", "string", type_new(TYPE_STRING), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "memset", "void*", type_new_ptr(void_t), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "memcpy", "void*", type_new_ptr(void_t), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "exit", "void", void_t, 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;

    add_symbol(ctx, "stdin", "void*", type_new_ptr(void_t), 0);
    add_symbol(ctx, "stdout", "void*", type_new_ptr(void_t), 0);
    add_symbol(ctx, "stderr", "void*", type_new_ptr(void_t), 0);

    add_symbol(ctx, "fopen", "void*", type_new_ptr(void_t), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "fclose", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "fread", "usize", type_new(TYPE_USIZE), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "fwrite", "usize", type_new(TYPE_USIZE), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "fseek", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "ftell", "long", type_new(TYPE_I64), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "rewind", "void", void_t, 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "fprintf", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "vprintf", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "vfprintf", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "sprintf", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "vsnprintf", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "snprintf", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "feof", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "ferror", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "mkdir", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "rmdir", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "chdir", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "getcwd", "string", type_new(TYPE_STRING), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "system", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "getenv", "string", type_new(TYPE_STRING), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "fgets", "string", type_new(TYPE_STRING), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;
    add_symbol(ctx, "usleep", "int", type_new(TYPE_INT), 0);
    ctx->current_scope->symbols->kind = SYM_FUNCTION;

    ASTNode *va_def = ast_create(NODE_STRUCT);
    va_def->strct.name = xstrdup("va_list");
    register_struct_def(ctx, "va_list", va_def);
    register_impl(ctx, "Copy", "va_list");

    ctx->hook_check_standard_macro_name = check_standard_macro_name;
}

void register_comptime_builtins(ParserContext *ctx)
{
    Type *void_t = type_new(TYPE_VOID);
    add_symbol(ctx, "yield", "void", void_t, 0);
    add_symbol(ctx, "code", "void", void_t, 0);
    add_symbol(ctx, "compile_warn", "void", void_t, 0);
    add_symbol(ctx, "compile_error", "void", void_t, 0);

    Type *string_t = type_new(TYPE_STRING);
    add_symbol(ctx, "__COMPTIME_TARGET__", "string", string_t, 0);
    add_symbol(ctx, "__COMPTIME_FILE__", "string", string_t, 0);

    register_extern_symbol(ctx, "yield");
    register_extern_symbol(ctx, "code");
    register_extern_symbol(ctx, "compile_warn");
    register_extern_symbol(ctx, "compile_error");
}
