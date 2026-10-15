// SPDX-License-Identifier: MIT

#include "parser.h"
#include "constants.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ast/ast.h"
#include "utils/format_expr.h"
#include "plugins/plugin_manager.h"
#include "zen/zen_facts.h"
#include "zprep_plugin.h"
#include "analysis/move_check.h"

char *curr_func_ret = NULL;

static void auto_import_std_test(ParserContext *ctx)
{
    if (find_func(ctx, "expect_eq"))
    {
        return; // test module already loaded
    }
    load_std_module(ctx, "std/test.zc");
}

ASTNode *parse_test(ParserContext *ctx, Lexer *l)
{
    auto_import_std_test(ctx);

    lexer_next(l);
    Token t = lexer_next(l);
    if (t.kind != TOK_STRING && t.kind != TOK_RAW_STRING)
    {
        zpanic_at(t, "Test name must be a string literal");
        return NULL;
    }

    char *name = token_get_string_content(t);

    ASTNode *n = ast_create(NODE_TEST);
    n->test_stmt.name = name;
    n->test_stmt.module_prefix = ctx->imports.current_module_prefix;
    n->test_stmt.filename = ctx->current_filename;

    // The body is parsed once all declarations are known (parse_pending_bodies).
    PendingBody *pending = xcalloc(1, sizeof(PendingBody));
    pending->node = n;
    pending->name = name;
    pending->body_start = *l;
    pending->scope = ctx->current_scope;
    pending->in_method_with_self = ctx->in_method_with_self;
    pending->self_is_pointer = ctx->self_is_pointer;
    if (!skip_braced_block(l))
    {
        return NULL;
    }
    push_pending_body(ctx, pending);

    return n;
}

// The message of `assert`/`expect`. A string with `{...}` parts is interpolated
// like any other string: it becomes the C expression that builds the text, and
// is emitted the way a message variable is (`"%s", expr`), so it is only
// evaluated when the check fails.
static char *parse_check_message_string(ParserContext *ctx, Token st, int *is_literal)
{
    char *content = token_get_string_content(st);
    int has_interpolation = 0;
    for (const char *c = content; *c; c++)
    {
        // `\u{...}` is a unicode escape, not an interpolation
        if (*c == '{' && !(c - content >= 2 && (c[-1] == 'u' || c[-1] == 'U') && c[-2] == '\\'))
        {
            has_interpolation = 1;
            break;
        }
    }

    if (has_interpolation)
    {
        char *code = process_printf_sugar(ctx, st, content, 0, "stdout", NULL, NULL, 0, 0, 1);
        zfree(content);
        *is_literal = 0;
        return code;
    }
    zfree(content);

    *is_literal = 1;
    char *msg = xmalloc(st.len + 1);
    strncpy(msg, st.start, st.len);
    msg[st.len] = 0;
    return msg;
}

ASTNode *parse_assert(ParserContext *ctx, Lexer *l)
{
    Token tk = lexer_next(l);
    if (lexer_peek(l).kind == TOK_LPAREN)
    {
        lexer_next(l);
    }

    ASTNode *cond = parse_expression(ctx, l);

    char *msg = NULL;
    int is_literal = 0;
    if (lexer_peek(l).kind == TOK_COMMA)
    {
        lexer_next(l);
        Token st = lexer_next(l);
        if (st.kind == TOK_STRING)
        {
            msg = parse_check_message_string(ctx, st, &is_literal);
        }
        else if (st.kind == TOK_IDENT)
        {
            is_literal = 0;
            msg = xmalloc(st.len + 1);
            strncpy(msg, st.start, st.len);
            msg[st.len] = 0;
        }
        else
        {
            zpanic_at(st, "Expected message string");
            return NULL;
        }
    }

    if (lexer_peek(l).kind == TOK_RPAREN)
    {
        lexer_next(l);
    }
    if (lexer_peek(l).kind == TOK_SEMICOLON)
    {
        lexer_next(l);
    }

    ASTNode *n = ast_create(NODE_ASSERT);
    n->token = tk;
    n->assert_stmt.condition = cond;
    n->assert_stmt.message = msg;
    n->assert_stmt.message_is_literal = is_literal;
    return n;
}

ASTNode *parse_expect(ParserContext *ctx, Lexer *l)
{
    Token tk = lexer_next(l);
    if (lexer_peek(l).kind == TOK_LPAREN)
    {
        lexer_next(l);
    }

    ASTNode *cond = parse_expression(ctx, l);

    char *msg = NULL;
    int is_literal = 0;
    if (lexer_peek(l).kind == TOK_COMMA)
    {
        lexer_next(l);
        Token st = lexer_next(l);
        if (st.kind == TOK_STRING)
        {
            msg = parse_check_message_string(ctx, st, &is_literal);
        }
        else if (st.kind == TOK_IDENT)
        {
            is_literal = 0;
            msg = xmalloc(st.len + 1);
            strncpy(msg, st.start, st.len);
            msg[st.len] = 0;
        }
        else
        {
            zpanic_at(st, "Expected message string");
            return NULL;
        }
    }

    if (lexer_peek(l).kind == TOK_RPAREN)
    {
        lexer_next(l);
    }
    if (lexer_peek(l).kind == TOK_SEMICOLON)
    {
        lexer_next(l);
    }

    ASTNode *n = ast_create(NODE_EXPECT);
    n->token = tk;
    n->assert_stmt.condition = cond;
    n->assert_stmt.message = msg;
    n->assert_stmt.message_is_literal = is_literal;
    return n;
}

ASTNode *parse_return(ParserContext *ctx, Lexer *l)
{
    Token return_token = lexer_next(l);

    if (ctx->cg.in_defer_block)
    {
        zpanic_at(return_token, "'return' is not allowed inside a 'defer' block");
        return NULL;
    }

    ASTNode *n = ast_create(NODE_RETURN);
    n->token = return_token;

    int handled = 0;

    if (curr_func_ret && strncmp(curr_func_ret, "Tuple__", 7) == 0 &&
        lexer_peek(l).kind == TOK_LPAREN)
    {

        int is_tuple_lit = 0;
        int depth = 0;

        Lexer temp_l = *l;

        while (1)
        {
            Token t = lexer_next(&temp_l);
            if (t.kind == TOK_EOF)
            {
                break;
            }
            if (t.kind == TOK_SEMICOLON)
            {
                break;
            }

            if (t.kind == TOK_LPAREN)
            {
                depth++;
            }
            if (t.kind == TOK_RPAREN)
            {
                depth--;
                if (depth == 0)
                {
                    break;
                }
            }

            if (depth == 1 && t.kind == TOK_COMMA)
            {
                is_tuple_lit = 1;
                break;
            }
        }

        if (is_tuple_lit)
        {
            n->ret.value = parse_tuple_expression(ctx, l, curr_func_ret, NULL);
            if (n->ret.value)
            {
                n->ret.value->token = return_token;
            }
            handled = 1;
        }
    }

    if (!handled)
    {
        if (lexer_peek(l).kind == TOK_SEMICOLON)
        {
            n->ret.value = NULL;
        }
        else
        {
            n->ret.value = parse_expression(ctx, l);
        }
    }

    if (lexer_peek(l).kind == TOK_SEMICOLON)
    {
        lexer_next(l);
    }
    return n;
}
