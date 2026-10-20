// SPDX-License-Identifier: MIT
#include "../parser/parser.h"

#include "codegen.h"
#include "zprep.h"
#include "../constants.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../plugins/plugin_manager.h"
#include "ast.h"
#include "zprep_plugin.h"
#include "codegen_internal.h"

void handle_expr_sizeof(ParserContext *ctx, ASTNode *node)
{
    if (node->size_of.target_type_info)
    {
        char *mapped = type_to_c_string(node->size_of.target_type_info, ctx->config->misra_mode);
        EMIT(ctx, "sizeof(%s)", mapped);
        zfree(mapped);
    }
    else if (node->size_of.target_type)
    {
        const char *t = node->size_of.target_type;
        const char *mapped = map_to_c_type(t);
        EMIT(ctx, "sizeof(%s)", mapped);
    }
    else
    {
        EMIT(ctx, "sizeof(");
        codegen_expression(ctx, node->size_of.expr);
        EMIT(ctx, ")");
    }
}

void handle_typeof(ParserContext *ctx, ASTNode *node)
{
    if (node->size_of.target_type_info)
    {
        char *mapped = type_to_c_string(node->size_of.target_type_info, ctx->config->misra_mode);
        EMIT(ctx, "typeof(%s)", mapped);
        zfree(mapped);
    }
    else if (node->size_of.target_type)
    {
        EMIT(ctx, "typeof(%s)", node->size_of.target_type);
    }
    else
    {
        EMIT(ctx, "typeof(");
        codegen_expression(ctx, node->size_of.expr);
        EMIT(ctx, ")");
    }
}

void handle_expr_unary(ParserContext *ctx, ASTNode *node)
{
    if (node->unary.temp_id > 0)
    {
        // A temporary that the enclosing call materialised (see handle_expr_call).
        EMIT(ctx, "(&_z_temp_%d)", node->unary.temp_id - 1);
    }
    else if (node->unary.op && strcmp(node->unary.op, "&_rval") == 0)
    {
        if (ctx->config->use_cpp)
        {
            EMIT(ctx, "({ __typeof__((");
            codegen_expression(ctx, node->unary.operand);
            EMIT(ctx, ")) _tmp = ");
            codegen_expression(ctx, node->unary.operand);
            EMIT(ctx, "; &_tmp; })");
        }
        else
        {
            EMIT(ctx, "(__typeof__((");
            codegen_expression(ctx, node->unary.operand);
            EMIT(ctx, "))[]){");
            codegen_expression(ctx, node->unary.operand);
            EMIT(ctx, "}");
        }
    }
    else if (node->unary.op && strcmp(node->unary.op, "?") == 0)
    {
        EMIT(ctx, "({ ");
        emit_auto_type(ctx, node->unary.operand, node->token);
        EMIT(ctx, " _t = (");
        codegen_expression(ctx, node->unary.operand);
        EMIT(ctx, "); if (_t.tag != 0) return _t; _t.data.Ok; })");
    }
    else if (node->unary.op && strcmp(node->unary.op, "_post++") == 0)
    {
        EMIT(ctx, "(");
        codegen_expression(ctx, node->unary.operand);
        EMIT(ctx, "++)");
    }
    else if (node->unary.op && strcmp(node->unary.op, "_post--") == 0)
    {
        EMIT(ctx, "(");
        codegen_expression(ctx, node->unary.operand);
        EMIT(ctx, "--)");
    }
    else
    {
        EMIT(ctx, "(%s", node->unary.op);
        codegen_expression(ctx, node->unary.operand);
        EMIT(ctx, ")");
    }
}

void handle_expr_array_literal(ParserContext *ctx, ASTNode *node)
{
    EMIT(ctx, "{");
    ASTNode *elem = node->array_literal.elements;
    int first_arr = 1;
    while (elem)
    {
        if (!first_arr)
        {
            EMIT(ctx, ", ");
        }
        codegen_expression(ctx, elem);
        elem = elem->next;
        first_arr = 0;
    }
    EMIT(ctx, "}");
}

void handle_expr_tuple_literal(ParserContext *ctx, ASTNode *node)
{
    char *type = node->resolved_type ? node->resolved_type
                 : node->type_info   ? type_to_string(node->type_info)
                                     : "unknown";
    EMIT(ctx, "(%s){", type);
    ASTNode *tup_elem = node->tuple_literal.elements;
    int first_tup = 1;
    while (tup_elem)
    {
        if (!first_tup)
        {
            EMIT(ctx, ", ");
        }
        codegen_expression(ctx, tup_elem);
        tup_elem = tup_elem->next;
        first_tup = 0;
    }
    EMIT(ctx, "}");
}
