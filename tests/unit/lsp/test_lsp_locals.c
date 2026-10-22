// SPDX-License-Identifier: MIT
// Unit tests of src/lsp/lsp_locals.c. Lines are those of the source, starting at 1.
#include "lsp/lsp_locals.h"
#include "unit_parse.h"
#include <string.h>
#include "unit_test.h"

// The function called `name` in the program parsed from `src`.
static ASTNode *function_in(const char *src, const char *name)
{
    ASTNode *root = parse_source(src);
    ASTNode *node = root && root->kind == NODE_ROOT ? root->root.children : root;
    for (; node; node = node->next)
    {
        if (node->kind == NODE_FUNCTION && strcmp(node->func.name, name) == 0)
        {
            return node;
        }
    }
    return NULL;
}

UTEST(lsp_find_local_decl, a_let_in_the_body)
{
    ASTNode *func = function_in("fn main() {\n"
                                "    let v = 5;\n"
                                "}\n",
                                "main");
    ASTNode *decl = lsp_find_local_decl(func, "v");
    ASSERT_TRUE(decl != NULL);
    ASSERT_STREQ("v", decl->var_decl.name);
}

UTEST(lsp_find_local_decl, a_let_inside_unless)
{
    ASTNode *func = function_in("fn main() {\n"
                                "    unless 1 > 2 {\n"
                                "        let v = 5;\n"
                                "    }\n"
                                "}\n",
                                "main");
    ASSERT_TRUE(lsp_find_local_decl(func, "v") != NULL);
}

UTEST(lsp_find_local_decl, a_let_inside_a_match_case)
{
    ASTNode *func = function_in("fn main() {\n"
                                "    match 1 {\n"
                                "        _ => {\n"
                                "            let v = 5;\n"
                                "        },\n"
                                "    }\n"
                                "}\n",
                                "main");
    ASSERT_TRUE(lsp_find_local_decl(func, "v") != NULL);
}

UTEST(lsp_find_local_decl, a_let_inside_loop)
{
    ASTNode *func = function_in("fn main() {\n"
                                "    loop {\n"
                                "        let v = 5;\n"
                                "        break;\n"
                                "    }\n"
                                "}\n",
                                "main");
    ASSERT_TRUE(lsp_find_local_decl(func, "v") != NULL);
}

UTEST(lsp_find_local_decl, a_parameter_is_left_to_the_caller)
{
    ASTNode *func = function_in("fn take(v: int) {\n"
                                "    let w = v;\n"
                                "}\n",
                                "take");
    ASSERT_TRUE(lsp_find_local_decl(func, "v") == NULL);
}

UTEST(lsp_find_local_decl, a_missing_name)
{
    ASTNode *func = function_in("fn main() {\n"
                                "    let v = 5;\n"
                                "}\n",
                                "main");
    ASSERT_TRUE(lsp_find_local_decl(func, "w") == NULL);
}

UTEST(lsp_has_loop_before, a_loop_in_the_body)
{
    ASTNode *func = function_in("fn main() {\n"
                                "    loop {\n"
                                "        break;\n"
                                "    }\n"
                                "}\n",
                                "main");
    ASSERT_TRUE(lsp_has_loop_before(func, 3));
}

UTEST(lsp_has_loop_before, a_loop_inside_unless)
{
    ASTNode *func = function_in("fn main() {\n"
                                "    unless 1 > 2 {\n"
                                "        loop {\n"
                                "            break;\n"
                                "        }\n"
                                "    }\n"
                                "}\n",
                                "main");
    ASSERT_TRUE(lsp_has_loop_before(func, 4));
}

UTEST(lsp_has_loop_before, a_loop_after_the_line)
{
    ASTNode *func = function_in("fn main() {\n"
                                "    let v = 5;\n"
                                "    loop {\n"
                                "        break;\n"
                                "    }\n"
                                "}\n",
                                "main");
    ASSERT_FALSE(lsp_has_loop_before(func, 2));
}

UTEST(lsp_has_loop_before, no_loop)
{
    ASTNode *func = function_in("fn main() {\n"
                                "    let v = 5;\n"
                                "}\n",
                                "main");
    ASSERT_FALSE(lsp_has_loop_before(func, 3));
}

UTEST(lsp_has_loop_before, a_for_in_loop)
{
    ASTNode *func = function_in("fn main() {\n"
                                "    for i in 0..3 {\n"
                                "        break;\n"
                                "    }\n"
                                "}\n",
                                "main");
    ASSERT_TRUE(lsp_has_loop_before(func, 3));
}

UTEST(lsp_has_loop_before, a_repeat_loop)
{
    ASTNode *func = function_in("fn main() {\n"
                                "    repeat 3 {\n"
                                "        break;\n"
                                "    }\n"
                                "}\n",
                                "main");
    ASSERT_TRUE(lsp_has_loop_before(func, 3));
}

UTEST(lsp_has_loop_before, a_do_while_loop)
{
    ASTNode *func = function_in("fn main() {\n"
                                "    do {\n"
                                "        break;\n"
                                "    } while 1 > 0;\n"
                                "}\n",
                                "main");
    ASSERT_TRUE(lsp_has_loop_before(func, 3));
}
