// SPDX-License-Identifier: MIT
#include "lsp_locals.h"
#include <string.h>

// The first `let` called `name` in the list starting at `node`, children included, in source
// order.
static ASTNode *find_decl(ASTNode *node, const char *name)
{
    for (; node; node = node->next)
    {
        if (node->kind == NODE_VAR_DECL && node->var_decl.name &&
            strcmp(node->var_decl.name, name) == 0)
        {
            return node;
        }

        ASTChildren children = ast_children(node);
        for (int i = 0; i < children.count; i++)
        {
            ASTNode *found = find_decl(*children.slots[i], name);
            if (found)
            {
                return found;
            }
        }
    }
    return NULL;
}

ASTNode *lsp_find_local_decl(ASTNode *func, const char *name)
{
    if (!func || !func->func.body)
    {
        return NULL;
    }

    // Also check params!
    if (func->func.param_names && func->func.arg_types)
    {
        for (int i = 0; i < func->func.count; i++)
        {
            if (strcmp(func->func.param_names[i], name) == 0)
            {
                return NULL; // Args handled by the caller's symbol resolution path
            }
        }
    }

    return find_decl(func->func.body, name);
}

// Every loop statement, where break and continue make sense.
static int is_loop(NodeType kind)
{
    return kind == NODE_WHILE || kind == NODE_FOR || kind == NODE_FOR_RANGE || kind == NODE_LOOP ||
           kind == NODE_LOOP;
}

static int has_loop_before(ASTNode *node, int line)
{
    for (; node; node = node->next)
    {
        // Approximate line check (Zen C AST stores line numbers in tokens)
        if (node->token.line > 0 && node->token.line <= line && is_loop(node->kind))
        {
            return 1;
        }

        ASTChildren children = ast_children(node);
        for (int i = 0; i < children.count; i++)
        {
            if (has_loop_before(*children.slots[i], line))
            {
                return 1;
            }
        }
    }
    return 0;
}

int lsp_has_loop_before(ASTNode *func, int line)
{
    return func && has_loop_before(func->func.body, line);
}
