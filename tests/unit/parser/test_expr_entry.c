// SPDX-License-Identifier: MIT
// Unit tests of src/parser/expr/expr_entry.c: the capture analysis of lambdas, which the
// parser runs when it finishes each lambda.
#include "unit_parse.h"
#include <string.h>
#include "unit_test.h"

// The first lambda under `node`, outermost first.
static ASTNode *first_lambda(ASTNode *node)
{
    for (; node; node = node->next)
    {
        if (node->kind == NODE_LAMBDA)
        {
            return node;
        }
        ASTChildren children = ast_children(node);
        for (int i = 0; i < children.count; i++)
        {
            ASTNode *found = first_lambda(*children.slots[i]);
            if (found)
            {
                return found;
            }
        }
    }
    return NULL;
}

static int captures(ASTNode *lambda, const char *name)
{
    for (int i = 0; i < lambda->lambda.capture_count; i++)
    {
        if (strcmp(lambda->lambda.captured_vars[i], name) == 0)
        {
            return 1;
        }
    }
    return 0;
}

UTEST(lambda_captures, a_variable_used_in_an_fstring_expression)
{
    ASTNode *lambda =
        first_lambda(parse_source("fn main() {\n"
                                  "    let k = 7;\n"
                                  "    let f = fn(x: int) -> int { let s = \"{k}\"; return x; };\n"
                                  "}\n"));
    ASSERT_TRUE(lambda != NULL);
    ASSERT_TRUE(captures(lambda, "k"));
    ASSERT_FALSE(captures(lambda, "x"));
    ASSERT_FALSE(captures(lambda, "s"));
}

UTEST(lambda_captures, an_outer_lambda_takes_what_the_inner_one_captures)
{
    ASTNode *lambda =
        first_lambda(parse_source("fn main() {\n"
                                  "    let k = 7;\n"
                                  "    let f = fn(x: int) -> int {\n"
                                  "        let g = fn(y: int) -> int { return y + k; };\n"
                                  "        return g(x);\n"
                                  "    };\n"
                                  "}\n"));
    ASSERT_TRUE(lambda != NULL);
    ASSERT_TRUE(captures(lambda, "k"));
    ASSERT_FALSE(captures(lambda, "y"));
}

UTEST(lambda_captures, not_its_own_loop_variable)
{
    ASTNode *lambda = first_lambda(parse_source("fn main() {\n"
                                                "    let k = 7;\n"
                                                "    let f = fn() -> int {\n"
                                                "        let r = 0;\n"
                                                "        for i in 0..3 { r = r + i + k; }\n"
                                                "        return r;\n"
                                                "    };\n"
                                                "}\n"));
    ASSERT_TRUE(lambda != NULL);
    ASSERT_TRUE(captures(lambda, "k"));
    ASSERT_FALSE(captures(lambda, "i"));
}

UTEST(lambda_captures, not_its_own_destructured_names)
{
    ASTNode *lambda = first_lambda(parse_source("fn main() {\n"
                                                "    let k = 7;\n"
                                                "    let f = fn(x: int) -> int {\n"
                                                "        let (a, b) = (x, k);\n"
                                                "        return a + b;\n"
                                                "    };\n"
                                                "}\n"));
    ASSERT_TRUE(lambda != NULL);
    ASSERT_TRUE(captures(lambda, "k"));
    ASSERT_FALSE(captures(lambda, "a"));
    ASSERT_FALSE(captures(lambda, "b"));
}
