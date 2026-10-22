// SPDX-License-Identifier: MIT
#include "unit_parse.h"
#include "diagnostics/diagnostics.h"
#include <stdio.h>
#include <string.h>

ASTNode *parse_source(const char *src)
{
    static ZenCompiler compiler;
    static ParserContext ctx;
    memset(&compiler, 0, sizeof(compiler));
    memset(&ctx, 0, sizeof(ctx));
    compiler.config.quiet = 1;
    ctx.compiler = &compiler;
    ctx.config = &compiler.config;
    ctx.current_filename = "test.zc";
    ctx.is_fault_tolerant = 1;
    module_state_init(&ctx.imports);
    ctx.cg.hoist_out = tmpfile();
    token_set_parser_ctx(&ctx);
    diag_set_parser_ctx(&ctx);
    register_builtins(&ctx);

    Lexer l;
    lexer_init(&l, src, ctx.config, ctx.current_filename);
    ASTNode *root = parse_program(&ctx, &l);

    token_set_parser_ctx(NULL);
    diag_set_parser_ctx(NULL);
    if (ctx.cg.hoist_out)
    {
        fclose(ctx.cg.hoist_out);
    }
    return root;
}
