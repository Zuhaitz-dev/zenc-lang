// SPDX-License-Identifier: MIT
#include "../constants.h"

#include "lsp_locals.h"
#include <string.h>

static void enqueue_node_children(ASTNode *curr, ASTNode **queue, int *q_tail, int q_limit)
{
    if (!curr)
    {
        return;
    }
    if (curr->kind == NODE_BLOCK)
    {
        ASTNode *stmt = curr->block.statements;
        while (stmt)
        {
            if (*q_tail < q_limit)
            {
                queue[(*q_tail)++] = stmt;
            }
            stmt = stmt->next;
        }
    }
    else if (curr->kind == NODE_IF)
    {
        if (curr->if_stmt.then_body && *q_tail < q_limit)
        {
            queue[(*q_tail)++] = curr->if_stmt.then_body;
        }
        if (curr->if_stmt.else_body && *q_tail < q_limit)
        {
            queue[(*q_tail)++] = curr->if_stmt.else_body;
        }
    }
    else if (curr->kind == NODE_WHILE)
    {
        if (curr->while_stmt.body && *q_tail < q_limit)
        {
            queue[(*q_tail)++] = curr->while_stmt.body;
        }
    }
    else if (curr->kind == NODE_FOR)
    {
        if (curr->for_stmt.init && *q_tail < q_limit)
        {
            queue[(*q_tail)++] = curr->for_stmt.init;
        }
        if (curr->for_stmt.body && *q_tail < q_limit)
        {
            queue[(*q_tail)++] = curr->for_stmt.body;
        }
    }
}

ASTNode *lsp_find_local_decl(ASTNode *func, const char *name)
{
    if (!func || !func->func.body)
    {
        return NULL;
    }

    ASTNode *queue[MAX_ERROR_MSG_LEN];
    int q_head = 0, q_tail = 0;
    queue[q_tail++] = func->func.body;

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

    while (q_head < q_tail && q_tail < 1024)
    {
        ASTNode *curr = queue[q_head++];
        if (curr->kind == NODE_VAR_DECL)
        {
            if (strcmp(curr->var_decl.name, name) == 0)
            {
                return curr;
            }
        }
        else
        {
            enqueue_node_children(curr, queue, &q_tail, 1024);
        }
    }
    return NULL;
}

int lsp_has_loop_before(ASTNode *func, int line)
{
    ASTNode *queue[1024];
    int q_head = 0, q_tail = 0;
    if (func->func.body)
    {
        queue[q_tail++] = func->func.body;
    }

    while (q_head < q_tail)
    {
        ASTNode *curr = queue[q_head++];
        if (!curr)
        {
            continue;
        }

        // Approximate line check (Zen C AST stores line numbers in tokens)
        if (curr->token.line > 0 && curr->token.line <= line)
        {
            if (curr->kind == NODE_WHILE || curr->kind == NODE_FOR || curr->kind == NODE_LOOP)
            {
                return 1;
            }
        }

        // Add children to queue
        enqueue_node_children(curr, queue, &q_tail, 1024);
    }

    return 0;
}
