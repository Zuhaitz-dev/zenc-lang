// SPDX-License-Identifier: MIT
// Unit tests of src/utils/format_expr.c.
#include "parser/parser.h"
#include "utils/emitter.h"
#include "utils/format_expr.h"
#include <string.h>
#include "unit_test.h"

static ASTNode *int_literal(unsigned long long value)
{
    ASTNode *node = ast_create(NODE_EXPR_LITERAL);
    node->literal.kind = LITERAL_INT;
    node->literal.int_val = value;
    node->type_info = type_new(TYPE_INT);
    return node;
}

// Called while code is being generated (into a buffer here), it gives the expression's C
// and leaves the emitter as it was: what is emitted before and after the call is kept.
UTEST(format_expression_as_c, keeps_what_is_emitted_around_it)
{
    static ZenCompiler compiler;
    static ParserContext ctx;
    memset(&compiler, 0, sizeof(compiler));
    memset(&ctx, 0, sizeof(ctx));
    ctx.compiler = &compiler;
    ctx.config = &compiler.config;

    Emitter *e = &ctx.cg.emitter;
    emitter_init_buffer(e);
    emitter_printf(e, "before;");
    char *c = format_expression_as_c(&ctx, int_literal(42));
    emitter_printf(e, "after;");
    char *out = emitter_take_string(e);

    ASSERT_TRUE(c != NULL);
    ASSERT_TRUE(strstr(c, "42") != NULL);
    ASSERT_TRUE(out != NULL);
    ASSERT_STREQ("before;after;", out);
    zfree(out);
}
