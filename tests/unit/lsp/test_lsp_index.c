// SPDX-License-Identifier: MIT
// Unit tests of src/lsp/lsp_index.c. Lines and columns start at 0, as in the index.
#include "lsp/lsp_index.h"
#include "diagnostics/diagnostics.h"
#include <stdio.h>
#include <string.h>
#include "unit_test.h"

// Parses `src` as the LSP does (fault tolerant, built-ins registered) and returns the index of
// its AST.
static LSPIndex *index_source(const char *src)
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

    LSPIndex *index = lsp_index_new();
    lsp_build_index(index, root);
    return index;
}

// The line that go to definition reaches from (line, col), or -1 if there is no reference there.
static int definition_line_at(LSPIndex *index, int line, int col)
{
    LSPRange *r = lsp_find_at(index, line, col);
    if (!r || r->kind != RANGE_REFERENCE)
    {
        return -1;
    }
    return r->def_line;
}

UTEST(lsp_index, reference_in_an_unless)
{
    LSPIndex *index = index_source("fn main() {\n"
                                   "    let v = 5;\n"
                                   "    unless v > 10 {\n"
                                   "        let u = v;\n"
                                   "    }\n"
                                   "}\n");
    ASSERT_EQ(1, definition_line_at(index, 2, 11));
    ASSERT_EQ(1, definition_line_at(index, 3, 16));
}

UTEST(lsp_index, reference_in_a_ternary)
{
    LSPIndex *index = index_source("fn main() {\n"
                                   "    let v = 5;\n"
                                   "    let t = v > 2 ? v : 0;\n"
                                   "}\n");
    ASSERT_EQ(1, definition_line_at(index, 2, 12));
    ASSERT_EQ(1, definition_line_at(index, 2, 20));
}

UTEST(lsp_index, reference_in_an_array_literal)
{
    LSPIndex *index = index_source("fn main() {\n"
                                   "    let v = 5;\n"
                                   "    let a = [v, 2];\n"
                                   "}\n");
    ASSERT_EQ(1, definition_line_at(index, 2, 13));
}

UTEST(lsp_index, reference_in_a_repeat)
{
    LSPIndex *index = index_source("fn main() {\n"
                                   "    let v = 5;\n"
                                   "    repeat 2 {\n"
                                   "        let c = v;\n"
                                   "    }\n"
                                   "}\n");
    ASSERT_EQ(1, definition_line_at(index, 3, 16));
}

UTEST(lsp_index, reference_in_a_lambda)
{
    LSPIndex *index = index_source("fn main() {\n"
                                   "    let v = 5;\n"
                                   "    let f = fn(x: int) -> int { return x + v; };\n"
                                   "}\n");
    ASSERT_EQ(1, definition_line_at(index, 2, 43));
}

UTEST(lsp_index, reference_in_a_do_while)
{
    LSPIndex *index = index_source("fn main() {\n"
                                   "    let v = 5;\n"
                                   "    do {\n"
                                   "        let d = v;\n"
                                   "    } while 0;\n"
                                   "}\n");
    ASSERT_EQ(1, definition_line_at(index, 3, 16));
}

UTEST(lsp_index, struct_literal_value_is_a_reference_and_its_field_no_definition)
{
    LSPIndex *index = index_source("struct Point {\n"
                                   "    x: int;\n"
                                   "    y: int;\n"
                                   "}\n"
                                   "\n"
                                   "fn main() {\n"
                                   "    let v = 5;\n"
                                   "    let p = Point { x: v, y: 2 };\n"
                                   "}\n");
    ASSERT_EQ(6, definition_line_at(index, 7, 23));
    LSPRange *on_type = lsp_find_at(index, 7, 12);
    ASSERT_TRUE(!on_type || on_type->kind != RANGE_DEFINITION);
}
