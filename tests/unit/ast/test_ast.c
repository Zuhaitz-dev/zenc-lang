// SPDX-License-Identifier: MIT
// Unit tests of src/ast/ast.c.
#include "ast/ast.h"
#include "unit_test.h"

UTEST(ast_children, if_gives_condition_then_else_in_order)
{
    ASTNode *node = ast_create(NODE_IF);
    ASTChildren c = ast_children(node);
    ASSERT_EQ(3, c.count);
    ASSERT_TRUE(c.slots[0] == &node->if_stmt.condition);
    ASSERT_TRUE(c.slots[1] == &node->if_stmt.then_body);
    ASSERT_TRUE(c.slots[2] == &node->if_stmt.else_body);
}

UTEST(ast_children, a_slot_can_replace_the_child)
{
    ASTNode *node = ast_create(NODE_IF);
    ASTNode *cond = ast_create(NODE_EXPR_VAR);
    ASTChildren c = ast_children(node);
    *c.slots[0] = cond;
    ASSERT_TRUE(node->if_stmt.condition == cond);
}

UTEST(ast_children, import_does_not_enter_the_module)
{
    ASTNode *node = ast_create(NODE_IMPORT);
    node->import_stmt.module_root = ast_create(NODE_FUNCTION);
    ASSERT_EQ(0, ast_children(node).count);
}

UTEST(ast_children, comptime_gives_body_and_generated)
{
    ASTNode *node = ast_create(NODE_COMPTIME);
    ASTChildren c = ast_children(node);
    ASSERT_EQ(2, c.count);
    ASSERT_TRUE(c.slots[0] == &node->comptime.body);
    ASSERT_TRUE(c.slots[1] == &node->comptime.generated);
}

UTEST(ast_children, a_leaf_has_none)
{
    ASSERT_EQ(0, ast_children(ast_create(NODE_EXPR_LITERAL)).count);
}

UTEST(ast_children, null_has_none)
{
    ASSERT_EQ(0, ast_children(NULL).count);
}

UTEST(ast_children, no_kind_has_more_than_the_maximum)
{
    for (int kind = NODE_ROOT; kind <= NODE_ERRONEOUS; kind++)
    {
        ASTChildren c = ast_children(ast_create((NodeType)kind));
        ASSERT_LE(c.count, AST_MAX_CHILDREN);
    }
}
