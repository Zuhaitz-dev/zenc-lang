// SPDX-License-Identifier: MIT
#include "parser.h"
#include "constants.h"
#include "expr_internal.h"
#include "ast/ast.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ASTNode *parse_expression(ParserContext *ctx, Lexer *l)
{
    ASTNode *n = parse_expr_prec(ctx, l, PREC_NONE);
    // Return a safe sentinel instead of NULL — eliminates null-deref crashes
    // across all ~150 call sites without needing individual NULL checks.
    if (!n)
    {
        n = ast_create(NODE_ERRONEOUS);
        n->token = lexer_peek(l);
    }
    return n;
}

Precedence get_token_precedence(Token t)
{
    if (t.kind == TOK_INT || t.kind == TOK_FLOAT || t.kind == TOK_STRING || t.kind == TOK_IDENT ||
        t.kind == TOK_FSTRING)
    {
        return PREC_NONE;
    }

    if (t.kind == TOK_QUESTION)
    {
        return PREC_CALL;
    }

    if (t.kind == TOK_ARROW && t.start[0] == '-')
    {
        return PREC_CALL;
    }

    if (t.kind == TOK_Q_DOT)
    {
        return PREC_CALL;
    }

    if (t.kind == TOK_QQ)
    {
        return PREC_OR;
    }

    if (t.kind == TOK_AND)
    {
        return PREC_AND;
    }

    if (t.kind == TOK_OR)
    {
        return PREC_OR;
    }

    if (t.kind == TOK_QQ_EQ)
    {
        return PREC_ASSIGNMENT;
    }

    if (t.kind == TOK_PIPE)
    {
        return PREC_TERM;
    }

    if (t.kind == TOK_LANGLE || t.kind == TOK_RANGLE)
    {
        return PREC_COMPARISON;
    }

    if (t.kind == TOK_OP)
    {
        if (is_token(t, "=") || is_token(t, "+=") || is_token(t, "-=") || is_token(t, "*=") ||
            is_token(t, "/=") || is_token(t, "%=") || is_token(t, "|=") || is_token(t, "&=") ||
            is_token(t, "^=") || is_token(t, "<<=") || is_token(t, ">>=") || is_token(t, "**="))
        {
            return PREC_ASSIGNMENT;
        }

        if (is_token(t, "||") || is_token(t, "or"))
        {
            return PREC_OR;
        }

        if (is_token(t, "&&") || is_token(t, "and"))
        {
            return PREC_AND;
        }

        if (is_token(t, "|"))
        {
            return PREC_TERM;
        }

        if (is_token(t, "^"))
        {
            return PREC_TERM;
        }

        if (is_token(t, "&"))
        {
            return PREC_TERM;
        }

        if (is_token(t, "<<") || is_token(t, ">>"))
        {
            return PREC_TERM;
        }

        if (is_token(t, "==") || is_token(t, "!="))
        {
            return PREC_EQUALITY;
        }

        if (is_token(t, "<") || is_token(t, ">") || is_token(t, "<=") || is_token(t, ">="))
        {
            return PREC_COMPARISON;
        }

        if (is_token(t, "+") || is_token(t, "-"))
        {
            return PREC_TERM;
        }

        if (is_token(t, "*") || is_token(t, "/") || is_token(t, "%"))
        {
            return PREC_FACTOR;
        }

        if (is_token(t, "**"))
        {
            return PREC_POWER;
        }

        if (is_token(t, "."))
        {
            return PREC_CALL;
        }

        if (is_token(t, "|>"))
        {
            return PREC_TERM;
        }
    }

    if (t.kind == TOK_LBRACKET || t.kind == TOK_LPAREN)
    {
        return PREC_CALL;
    }

    if (is_token(t, "??"))
    {
        return PREC_OR;
    }

    if (is_token(t, "\?\?="))
    {
        return PREC_ASSIGNMENT;
    }

    return PREC_NONE;
}

// Helper to check if a variable name is in a list.
static int is_in_list(const char *name, char **list, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (0 == strcmp(name, list[i]))
        {
            return 1;
        }
    }
    return 0;
}

// Appends a copy of `name` to the list.
static void add_name(char ***list, int *count, const char *name)
{
    *list = xrealloc(*list, sizeof(char *) * (size_t)(*count + 1));
    (*list)[*count] = xstrdup(name);
    (*count)++;
}

// Recursively find all variable references in an expression/statement.
static void find_var_refs(ASTNode *node, char ***refs, int *ref_count)
{
    if (!node)
    {
        return;
    }

    if (node->kind == NODE_EXPR_VAR)
    {
        add_name(refs, ref_count, node->var_ref.name);
    }

    // A println lists the symbols it uses; an f-string in an expression keeps its
    // expressions in an array, which ast_children() leaves out.
    if (node->kind == NODE_RAW_STMT)
    {
        for (int i = 0; i < node->raw_stmt.used_symbol_count; i++)
        {
            add_name(refs, ref_count, node->raw_stmt.used_symbols[i]);
        }
        for (int i = 0; i < node->raw_stmt.interp_expr_count; i++)
        {
            find_var_refs(node->raw_stmt.interp_exprs[i], refs, ref_count);
        }
        return;
    }

    // A nested lambda was analyzed when it was parsed: from outside it needs what it
    // captures, not the names in its body (its own parameters among them).
    if (node->kind == NODE_LAMBDA)
    {
        for (int i = 0; i < node->lambda.capture_count; i++)
        {
            add_name(refs, ref_count, node->lambda.captured_vars[i]);
        }
        return;
    }

    // A comptime block runs while compiling: the names in it (yield among them) are not
    // variables the lambda uses at run time.
    if (node->kind == NODE_COMPTIME)
    {
        return;
    }

    ASTChildren children = ast_children(node);
    for (int i = 0; i < children.count; i++)
    {
        for (ASTNode *child = *children.slots[i]; child; child = child->next)
        {
            find_var_refs(child, refs, ref_count);
        }
    }
}

// Helper to find variable declarations in a subtree
static void find_declared_vars(ASTNode *node, char ***decls, int *count)
{
    if (!node)
    {
        return;
    }

    if (node->kind == NODE_VAR_DECL)
    {
        add_name(decls, count, node->var_decl.name);
    }

    if (node->kind == NODE_MATCH_CASE)
    {
        if (node->match_case.binding_names)
        {
            for (int i = 0; i < node->match_case.binding_count; i++)
            {
                if (node->match_case.binding_names[i])
                {
                    add_name(decls, count, node->match_case.binding_names[i]);
                }
            }
        }
    }

    // Struct literal fields are NODE_VAR_DECLs named after the field, the locals of a nested
    // lambda are its own and those of a comptime block live while compiling: none declares
    // anything here.
    if (node->kind == NODE_EXPR_STRUCT_INIT || node->kind == NODE_LAMBDA ||
        node->kind == NODE_COMPTIME)
    {
        return;
    }

    ASTChildren children = ast_children(node);
    for (int i = 0; i < children.count; i++)
    {
        for (ASTNode *child = *children.slots[i]; child; child = child->next)
        {
            find_declared_vars(child, decls, count);
        }
    }
}

// Analyze lambda body to find captured variables.
void analyze_lambda_captures(ParserContext *ctx, ASTNode *lambda)
{
    if (!lambda || lambda->kind != NODE_LAMBDA)
    {
        return;
    }

    char **all_refs = NULL;
    int num_refs = 0;
    find_var_refs(lambda->lambda.body, &all_refs, &num_refs);

    char **local_decls = NULL;
    int num_local_decls = 0;
    find_declared_vars(lambda->lambda.body, &local_decls, &num_local_decls);

    char **captures = xmalloc(sizeof(char *) * 32);
    char **capture_types = xmalloc(sizeof(char *) * 32);
    Type **captured_types_info = xmalloc(sizeof(Type *) * 32);
    int *capture_modes = xmalloc(sizeof(int) * 32);
    int capture_count = 0;

    for (int i = 0; i < lambda->lambda.explicit_capture_count; i++)
    {
        const char *var_name = lambda->lambda.explicit_captures[i];
        if (!is_in_list(var_name, captures, capture_count))
        {
            captures[capture_count] = xstrdup(var_name);
            capture_modes[capture_count] = lambda->lambda.explicit_capture_modes[i];

            Type *t = find_symbol_type_info(ctx, var_name);
            captured_types_info[capture_count] = t;
            capture_types[capture_count] = t ? type_to_string(t) : xstrdup("int");
            capture_count++;
        }
    }

    for (int i = 0; i < num_refs; i++)
    {
        const char *var_name = all_refs[i];

        if (is_in_list(var_name, lambda->lambda.param_names, lambda->lambda.count))
        {
            continue;
        }

        if (is_in_list(var_name, local_decls, num_local_decls))
        {
            continue;
        }

        if (is_in_list(var_name, captures, capture_count))
        {
            continue;
        }

        if (strcmp(var_name, "printf") == 0 || strcmp(var_name, "malloc") == 0 ||
            strcmp(var_name, "strcmp") == 0 || strcmp(var_name, "free") == 0 ||
            strcmp(var_name, "sprintf") == 0 || strcmp(var_name, "snprintf") == 0 ||
            strcmp(var_name, "fprintf") == 0 || strcmp(var_name, "stderr") == 0 ||
            strcmp(var_name, "stdout") == 0 || strcmp(var_name, "strcat") == 0 ||
            strcmp(var_name, "strcpy") == 0 || strcmp(var_name, "strlen") == 0 ||
            strcmp(var_name, "Vec_new") == 0 || strcmp(var_name, "Vec_push") == 0 ||
            strcmp(var_name, "NULL") == 0 || strcmp(var_name, "true") == 0 ||
            strcmp(var_name, "false") == 0)
        {
            continue;
        }

        FuncSig *fs = ctx->func_registry;
        int is_func = 0;
        while (fs)
        {
            if (0 == strcmp(fs->name, var_name))
            {
                is_func = 1;
                break;
            }
            fs = fs->next;
        }
        if (is_func)
        {
            continue;
        }

        Scope *s = ctx->current_scope;
        int is_local = 0;
        int is_found = 0;
        while (s)
        {
            ZenSymbol *cur = s->symbols;
            while (cur)
            {
                if (0 == strcmp(cur->name, var_name))
                {
                    is_found = 1;
                    if (s->parent != NULL)
                    {
                        is_local = 1;
                    }
                    break;
                }
                cur = cur->next;
            }
            if (is_found)
            {
                break;
            }
            s = s->parent;
        }

        if (is_found && !is_local)
        {
            continue;
        }

        int found = 0;
        for (int j = 0; j < capture_count; j++)
        {
            if (strcmp(var_name, captures[j]) == 0)
            {
                found = 1;
                break;
            }
        }
        if (found)
        {
            continue;
        }

        captures[capture_count] = xstrdup(var_name);
        capture_modes[capture_count] = lambda->lambda.default_capture_mode;

        Type *t = find_symbol_type_info(ctx, var_name);
        captured_types_info[capture_count] = t;
        if (t)
        {
            capture_types[capture_count] = type_to_string(t);
        }
        else
        {
            capture_types[capture_count] = xstrdup("int");
        }
        capture_count++;
    }

    lambda->lambda.captured_vars = captures;
    lambda->lambda.captured_types = capture_types;
    lambda->lambda.captured_types_info = captured_types_info;
    lambda->lambda.capture_modes = capture_modes;
    lambda->lambda.capture_count = capture_count;

    if (local_decls)
    {
        for (int i = 0; i < num_local_decls; i++)
        {
            zfree(local_decls[i]);
        }
        zfree(local_decls);
    }
    for (int i = 0; i < num_refs; i++)
    {
        zfree(all_refs[i]);
        zfree(all_refs);
    }
}
