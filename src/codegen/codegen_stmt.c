// SPDX-License-Identifier: MIT
#include "../parser/parser.h"

#include "codegen.h"
#include "codegen_internal.h"
#include "zprep.h"
#include "../constants.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../plugins/plugin_manager.h"
#include "ast.h"
#include "codegen_internal.h"
#include "zprep_plugin.h"

// Whether `node` names a stable storage location (a variable, a field of one, an element of
// one, or a dereference). Only these can be moved out of and then zeroed: a field or index
// of a temporary has no address to clear and would be evaluated twice.
static int is_place_expression(ASTNode *node, int depth)
{
    if (!node || depth > 32)
    {
        return 0;
    }
    switch (node->kind)
    {
    case NODE_EXPR_VAR:
        return 1;
    case NODE_EXPR_MEMBER:
        return is_place_expression(node->member.target, depth + 1);
    case NODE_EXPR_INDEX:
        return is_place_expression(node->index.array, depth + 1);
    case NODE_EXPR_UNARY:
        return strcmp(node->unary.op, "*") == 0;
    default:
        return 0;
    }
}

// Enters a loop: emits its label, if any, and records where its defers start, so `break` and
// `continue` know how many to run.
void codegen_loop_enter(ParserContext *ctx, const char *label)
{
    if (label)
    {
        EMIT(ctx, "%s:;\n", label);
    }
    if (ctx->cg.loop_depth >= ctx->cg.loop_defer_capacity)
    {
        int capacity = ctx->cg.loop_defer_capacity ? ctx->cg.loop_defer_capacity * 2 : 16;
        ctx->cg.loop_defer_boundary =
            xrealloc(ctx->cg.loop_defer_boundary, sizeof(int) * (size_t)capacity);
        ctx->cg.loop_defer_capacity = capacity;
    }
    ctx->cg.loop_defer_boundary[ctx->cg.loop_depth] = ctx->cg.defer_count;
    ctx->cg.loop_depth++;
}

// Emits a loop body. A labelled loop gets a block that ends in its `continue` target. With
// `misra_wrap`, an unlabelled body that is not a block is braced in MISRA mode (rule 15.6).
void codegen_loop_body(ParserContext *ctx, ASTNode *body, const char *label, int misra_wrap)
{
    if (label)
    {
        EMIT(ctx, "{\n");
        emitter_indent(&ctx->cg.emitter);
        codegen_node_single(ctx, body);
        emitter_dedent(&ctx->cg.emitter);
        EMIT(ctx, "__continue_%s:;\n", label);
        EMIT(ctx, "}\n");
        return;
    }

    int wrap = misra_wrap && ctx->config->misra_mode && body->kind != NODE_BLOCK;
    if (wrap)
    {
        EMIT(ctx, "{\n");
        emitter_indent(&ctx->cg.emitter);
    }
    codegen_node_single(ctx, body);
    if (wrap)
    {
        emitter_dedent(&ctx->cg.emitter);
        EMIT(ctx, "\n}");
    }
}

// Leaves a loop entered with codegen_loop_enter: emits its `break` target, if labelled.
void codegen_loop_exit(ParserContext *ctx, const char *label)
{
    ctx->cg.loop_depth--;
    if (label)
    {
        EMIT(ctx, "__break_%s:;\n", label);
    }
}

// Emits the pending defers, innermost first, down to stack index `boundary`.
void emit_defers_down_to(ParserContext *ctx, int boundary)
{
    for (int i = ctx->cg.defer_count - 1; i >= boundary; i--)
    {
        emit_source_mapping_duplicate(ctx, ctx->cg.defer_stack[i]);
        codegen_node_single(ctx, ctx->cg.defer_stack[i]);
    }
}

// Name of the struct with Drop that a call statement produces and discards, or NULL.
static char *discarded_drop_type(ParserContext *ctx, ASTNode *node)
{
    if (node->kind != NODE_EXPR_CALL)
    {
        return NULL;
    }

    char *tname = NULL;
    if (node->type_info && node->type_info->kind == TYPE_STRUCT && node->type_info->name)
    {
        tname = node->type_info->name;
    }
    else
    {
        tname = infer_type(ctx, node);
    }
    if (!tname || strchr(tname, '*'))
    {
        return NULL;
    }
    if (strncmp(tname, "struct ", 7) == 0)
    {
        tname += 7;
    }

    ASTNode *def = find_struct_def(ctx, tname);
    if (def && def->kind == NODE_STRUCT && def->type_info && def->type_info->traits.has_drop)
    {
        return tname;
    }
    return NULL;
}

void codegen_node_single(ParserContext *ctx, ASTNode *node)
{
    if (!node)
    {
        return;
    }

    static const CodegenHandler handlers[256] = {
        [NODE_AST_COMMENT] = handle_node_ast_comment,
        [NODE_MATCH] = handle_node_match,
        [NODE_ASSERT] = handle_node_assert,
        [NODE_EXPECT] = handle_node_expect,
        [NODE_DEFER] = handle_node_defer,
        [NODE_BLOCK] = handle_node_block,
        [NODE_IMPL] = handle_node_impl,
        [NODE_FUNCTION] = handle_node_function,
        [NODE_IMPL_TRAIT] = handle_node_impl_trait,
        [NODE_DESTRUCT_VAR] = handle_node_destruct_var,
        [NODE_VAR_DECL] = handle_node_var_decl,
        [NODE_CONST] = handle_node_const,
        [NODE_FIELD] = handle_node_field,
        [NODE_IF] = handle_node_if,
        [NODE_WHILE] = handle_node_while,
        [NODE_COMPTIME] = handle_node_comptime,
    };

    if (node->kind < 256 && handlers[node->kind])
    {
        handlers[node->kind](ctx, node);
        return;
    }

    switch (node->kind)
    {
    case NODE_FOR:
    {
        codegen_loop_enter(ctx, node->for_stmt.loop_label);
        EMIT(ctx, "for (");
        if (node->for_stmt.init)
        {
            if (node->for_stmt.init->kind == NODE_VAR_DECL)
            {
                ASTNode *v = node->for_stmt.init;
                if (v->var_decl.type_str && strcmp(v->var_decl.type_str, "__auto_type") != 0)
                {
                    EMIT(ctx, "%s %s = (%s)(", v->var_decl.type_str, v->var_decl.name,
                         v->var_decl.type_str);
                    codegen_expression(ctx, v->var_decl.init_expr);
                    EMIT(ctx, ")");
                }
                else
                {
                    emit_auto_type(ctx, v->var_decl.init_expr, v->token);
                    EMIT(ctx, " %s = ", v->var_decl.name);
                    codegen_expression(ctx, v->var_decl.init_expr);
                }
            }
            else
            {
                codegen_expression(ctx, node->for_stmt.init);
            }
        }
        EMIT(ctx, "; ");
        if (node->for_stmt.condition)
        {
            codegen_expression_bare(ctx, node->for_stmt.condition);
        }
        EMIT(ctx, "; ");
        if (node->for_stmt.step)
        {
            codegen_expression_bare(ctx, node->for_stmt.step);
        }
        EMIT(ctx, ") ");
        codegen_loop_body(ctx, node->for_stmt.body, node->for_stmt.loop_label, 1);
        codegen_loop_exit(ctx, node->for_stmt.loop_label);
        break;
    }
    case NODE_BREAK:
        // Run defers from current scope down to loop boundary before breaking
        if (ctx->cg.loop_depth > 0)
        {
            int boundary = ctx->cg.loop_defer_boundary[ctx->cg.loop_depth - 1];
            emit_defers_down_to(ctx, boundary);
        }
        if (node->break_stmt.target_label)
        {
            EMIT(ctx, "goto __break_%s;\n", node->break_stmt.target_label);
        }
        else
        {
            EMIT(ctx, "break;\n");
        }
        break;
    case NODE_CONTINUE:
        // Run defers from current scope down to loop boundary before continuing
        if (ctx->cg.loop_depth > 0)
        {
            int boundary = ctx->cg.loop_defer_boundary[ctx->cg.loop_depth - 1];
            emit_defers_down_to(ctx, boundary);
        }
        if (node->continue_stmt.target_label)
        {
            EMIT(ctx, "goto __continue_%s;\n", node->continue_stmt.target_label);
        }
        else
        {
            EMIT(ctx, "continue;\n");
        }
        break;
    case NODE_GOTO:
        if (node->goto_stmt.goto_expr)
        {
            // Computed goto: goto *expr;
            EMIT(ctx, "goto *(");
            codegen_expression(ctx, node->goto_stmt.goto_expr);
            EMIT(ctx, ");\n");
        }
        else
        {
            EMIT(ctx, "goto %s;\n", node->goto_stmt.label_name);
        }
        break;
    case NODE_LABEL:
        EMIT(ctx, "%s:;\n", node->label_stmt.label_name);
        break;
    // Loop constructs: loop, for-in
    case NODE_LOOP:
    {
        // loop { ... } => while (1) { ... }
        codegen_loop_enter(ctx, node->loop_stmt.loop_label);
        EMIT(ctx, "while (1) ");
        codegen_loop_body(ctx, node->loop_stmt.body, node->loop_stmt.loop_label, 0);
        codegen_loop_exit(ctx, node->loop_stmt.loop_label);
        break;
    }
    case NODE_FOR_RANGE:
    {
        codegen_loop_enter(ctx, node->for_range.loop_label);

        EMIT(ctx, "for (");
        if (ctx->config->misra_output)
        {
            // The minimal MISRA preamble has no ZC_AUTO/typeof macro.
            char *range_type = NULL;
            if (node->for_range.start && node->for_range.start->type_info &&
                node->for_range.start->type_info->kind != TYPE_UNKNOWN)
            {
                range_type =
                    type_to_c_string(node->for_range.start->type_info, ctx->config->misra_mode);
            }
            EMIT(ctx, "%s %s = ", range_type ? range_type : "int32_t", node->for_range.var_name);
            zfree(range_type);
        }
        else if (z_path_match_compiler(ctx->config->cc, "tcc"))
        {
            EMIT(ctx, "__typeof__((");
            codegen_expression(ctx, node->for_range.start);
            EMIT(ctx, ")) %s = ", node->for_range.var_name);
        }
        else
        {
            EMIT(ctx, "ZC_AUTO %s = ", node->for_range.var_name);
        }
        codegen_expression(ctx, node->for_range.start);
        if (node->for_range.step && node->for_range.step[0] == '-')
        {
            if (node->for_range.is_inclusive)
            {
                EMIT(ctx, "; %s >= ", node->for_range.var_name);
            }
            else
            {
                EMIT(ctx, "; %s > ", node->for_range.var_name);
            }
        }
        else
        {
            if (node->for_range.is_inclusive)
            {
                EMIT(ctx, "; %s <= ", node->for_range.var_name);
            }
            else
            {
                EMIT(ctx, "; %s < ", node->for_range.var_name);
            }
        }
        codegen_expression(ctx, node->for_range.end);
        EMIT(ctx, "; %s", node->for_range.var_name);
        if (node->for_range.step)
        {
            EMIT(ctx, " += %s) ", node->for_range.step);
        }
        else
        {
            EMIT(ctx, "++) ");
        }
        codegen_loop_body(ctx, node->for_range.body, node->for_range.loop_label, 0);
        codegen_loop_exit(ctx, node->for_range.loop_label);
        break;
    }

    case NODE_ASM:
    {
        int is_extended = (node->asm_stmt.output_count > 0 || node->asm_stmt.input_count > 0 ||
                           node->asm_stmt.clobber_count > 0);

        if (node->asm_stmt.is_volatile)
        {
            EMIT(ctx, "__asm__ __volatile__(");
        }
        else
        {
            EMIT(ctx, "__asm__(");
        }

        char *code = node->asm_stmt.code;
        size_t transformed_sz = strlen(code) * 3 + 128; // Extra buffer for expansions
        char *transformed = xmalloc(transformed_sz);
        char *dst = transformed;

        for (char *p = code; *p; p++)
        {
            if (*p == '{')
            {
                // Find matching }
                char *end = (char *)strchr(p + 1, '}');
                if (end)
                {
                    // Extract variable name
                    ptrdiff_t var_len = end - p - 1;
                    char var_name[64];
                    strncpy(var_name, p + 1, (size_t)(var_len));
                    var_name[var_len] = 0;

                    // Find variable index
                    int idx = -1;

                    // Check outputs first
                    for (int i = 0; i < node->asm_stmt.output_count; i++)
                    {
                        if (strcmp(node->asm_stmt.outputs[i], var_name) == 0)
                        {
                            idx = i;
                            break;
                        }
                    }

                    // Then check inputs
                    if (idx == -1)
                    {
                        for (int i = 0; i < node->asm_stmt.input_count; i++)
                        {
                            if (strcmp(node->asm_stmt.inputs[i], var_name) == 0)
                            {
                                idx = node->asm_stmt.output_count + i;
                                break;
                            }
                        }
                    }

                    size_t rem = transformed_sz - (size_t)(dst - transformed);
                    if (idx >= 0)
                    {
                        // Replace with %N
#if defined(ZC_ARCH_ARM64)
                        // Use most optimal register size on arm architectures
                        if (node->asm_stmt.register_size <= 32)
                        {
                            int _n = snprintf(dst, rem, "%%w%d", idx);
                            dst += (_n > 0 && (size_t)_n < rem) ? (size_t)_n
                                   : rem > 1                    ? rem - 1
                                                                : 0;
                        }
                        else
#endif
                        {
                            int _n = snprintf(dst, rem, "%%%d", idx);
                            dst += (_n > 0 && (size_t)_n < rem) ? (size_t)_n
                                   : rem > 1                    ? rem - 1
                                                                : 0;
                        }
                    }
                    else
                    {
                        // Variable not found - error or keep as-is?
                        int _n = snprintf(dst, rem, "{%s}", var_name);
                        dst += (_n > 0 && (size_t)_n < rem) ? (size_t)_n : rem > 1 ? rem - 1 : 0;
                    }

                    p = end; // Skip past }
                }
                else
                {
                    *dst++ = *p;
                }
            }
            else if (*p == '%')
            {
                if (is_extended)
                {
                    *dst++ = '%';
                    *dst++ = '%';
                }
                else
                {
                    *dst++ = '%';
                }
            }
            else
            {
                *dst++ = *p;
            }
        }
        *dst = 0;

        EMIT(ctx, "\"");
        for (char *p = transformed; *p; p++)
        {
            if (*p == '\n')
            {
                EMIT(ctx, "\\n\"\n        \"");
            }
            else if (*p == '"')
            {
                EMIT(ctx, "\\\"");
            }
            else if (*p == '\\')
            {
                EMIT(ctx, "\\\\");
            }
            else
            {
                EMIT(ctx, "%c", *p);
            }
        }
        EMIT(ctx, "\\n\"");

        if (node->asm_stmt.output_count > 0)
        {
            EMIT(ctx, "\n        : ");
            for (int i = 0; i < node->asm_stmt.output_count; i++)
            {
                if (i > 0)
                {
                    EMIT(ctx, ", ");
                }

                // Determine constraint
                char *mode = node->asm_stmt.output_modes[i];
                if (strcmp(mode, "out") == 0)
                {
                    EMIT(ctx, "\"=r\"(%s)", node->asm_stmt.outputs[i]);
                }
                else if (strcmp(mode, "inout") == 0)
                {
                    EMIT(ctx, "\"+r\"(%s)", node->asm_stmt.outputs[i]);
                }
                else
                {
                    EMIT(ctx, "\"=r\"(%s)", node->asm_stmt.outputs[i]);
                }
            }
        }

        if (node->asm_stmt.input_count > 0)
        {
            EMIT(ctx, "\n        : ");
            for (int i = 0; i < node->asm_stmt.input_count; i++)
            {
                if (i > 0)
                {
                    EMIT(ctx, ", ");
                }
                EMIT(ctx, "\"r\"(%s)", node->asm_stmt.inputs[i]);
            }
        }
        else if (node->asm_stmt.output_count > 0)
        {
            EMIT(ctx, "\n        : ");
        }

        if (node->asm_stmt.clobber_count > 0)
        {
            EMIT(ctx, "\n        : ");
            for (int i = 0; i < node->asm_stmt.clobber_count; i++)
            {
                if (i > 0)
                {
                    EMIT(ctx, ", ");
                }
                EMIT(ctx, "\"%s\"", node->asm_stmt.clobbers[i]);
            }
        }

        EMIT(ctx, ");\n");
        break;
    }
    case NODE_RETURN:
    {
        // Suppress pending closure frees — returned closures escape the scope
        ctx->cg.pending_closure_free_count = 0;
        int has_defers = (ctx->cg.defer_count > ctx->cg.func_defer_boundary);
        int handled = 0;

        if (node->ret.value && node->ret.value->kind == NODE_EXPR_ARRAY_LITERAL &&
            ctx->cg.current_func_ret_type &&
            strncmp(ctx->cg.current_func_ret_type, "Slice__", 7) == 0)
        {
            // Heap allocation for slice literals to prevent use-after-return
            ASTNode *arr = node->ret.value;
            int count = arr->array_literal.count;
            char *elem_type = "void*"; // fallback

            // Prioritize the function return type (Slice_T) to determine the pointer type
            // This prevents "incompatible pointer type" errors in C when returning literals of
            // different types
            if (ctx->cg.current_func_ret_type &&
                strncmp(ctx->cg.current_func_ret_type, "Slice__", 7) == 0)
            {
                elem_type = xstrdup(ctx->cg.current_func_ret_type + 7);
            }
            else if (arr->array_literal.elements && arr->array_literal.elements->type_info)
            {
                elem_type = type_to_c_string(arr->array_literal.elements->type_info,
                                             ctx->config->misra_mode);
            }
            else if (arr->type_info && arr->type_info->inner)
            {
                elem_type = type_to_c_string(arr->type_info->inner, ctx->config->misra_mode);
            }
            else
            {
                elem_type = xstrdup("void*");
            }

            EMIT(ctx, "{ %s *_tmp_arr = (%s*)malloc(%d * sizeof(%s));\n", elem_type, elem_type,
                 count, elem_type);
            emitter_indent(&ctx->cg.emitter);

            ASTNode *elem = arr->array_literal.elements;
            int idx = 0;
            while (elem)
            {
                EMIT(ctx, "_tmp_arr[%d] = ", idx++);
                codegen_expression(ctx, elem);
                EMIT(ctx, ";\n");
                elem = elem->next;
            }

            if (ctx->config->use_cpp)
            {
                // Traditional initializer: (Slice){data, len, cap}
                EMIT(ctx, "return (%s){_tmp_arr, %d, %d};\n", ctx->cg.current_func_ret_type, count,
                     count);
            }
            else
            {
                EMIT(ctx, "return (%s){.data = _tmp_arr, .len = %d, .cap = %d};\n",
                     ctx->cg.current_func_ret_type, count, count);
            }
            emitter_dedent(&ctx->cg.emitter);
            EMIT(ctx, "}\n");
            handled = 1;
        }

        // Returning a place expression (`x`, `self.field`, `ptr[i]`, `*p`) of
        // an owning type moves the value out of that place. Without this the
        // caller receives a bitwise copy while the source keeps ownership, so
        // both are dropped: the element is freed twice.
        if (node->ret.value && is_place_expression(node->ret.value, 0))
        {
            char *tname = infer_type(ctx, node->ret.value);
            if (tname)
            {
                char *clean = tname;
                if (strncmp(clean, "struct ", 7) == 0)
                {
                    clean += 7;
                }

                ASTNode *def = find_struct_def(ctx, clean);
                if (def && def->type_info && def->type_info->traits.has_drop)
                {
                    EMIT(ctx, "return ({ ");

                    int ret_is_self = node->ret.value->kind == NODE_EXPR_VAR &&
                                      ctx->self_is_pointer &&
                                      strcmp(node->ret.value->var_ref.name, "self") == 0;
                    if (node->ret.value->kind != NODE_EXPR_VAR)
                    {
                        // Member, index or dereference: bind the place once (an
                        // index may have side effects), copy from it, then zero it
                        // so its owner does not drop the value a second time.
                        EMIT(ctx, "__typeof__(&(");
                        codegen_expression(ctx, node->ret.value);
                        EMIT(ctx, ")) _z_ret_place = &(");
                        codegen_expression(ctx, node->ret.value);
                        EMIT(ctx, "); ZC_AUTO_INIT(_z_ret_mv, *_z_ret_place); ");
                        EMIT(ctx, "memset(_z_ret_place, 0, sizeof(_z_ret_mv)); ");
                    }
                    else
                    {
                        if (z_path_match_compiler(ctx->config->cc, "tcc"))
                        {
                            EMIT(ctx, "__typeof__(");
                            codegen_expression(ctx, node->ret.value);
                            EMIT(ctx, ")");
                        }
                        else
                        {
                            EMIT(ctx, "ZC_AUTO");
                        }
                        EMIT(ctx, " _z_ret_mv = ");
                        if (ret_is_self)
                        {
                            EMIT(ctx, "*");
                        }
                        codegen_expression(ctx, node->ret.value);
                        EMIT(ctx, "; memset(&");
                        if (ret_is_self)
                        {
                            EMIT(ctx, "*");
                        }
                        codegen_expression(ctx, node->ret.value);
                        EMIT(ctx, ", 0, sizeof(_z_ret_mv)); ");
                        if (!ret_is_self)
                        {
                            EMIT(ctx, "__z_drop_flag_%s = 0; ", node->ret.value->var_ref.name);
                        }
                    }
                    emit_defers_down_to(ctx, ctx->cg.func_defer_boundary);
                    EMIT(ctx, "_z_ret_mv; });\n");
                    handled = 1;
                }
                zfree(tname);
            }
        }

        if (!handled)
        {
            if (has_defers && node->ret.value)
            {
                // MISRA output keeps the return in the enclosing block so the
                // function has a single exit at its end. A unique temporary
                // avoids a scope around the return.
                int inline_ret = ctx->config->misra_output;
                char ret_name[32] = "_z_ret";
                if (inline_ret)
                {
                    snprintf(ret_name, sizeof(ret_name), "_z_ret_%d", ctx->cg.tmp_counter++);
                }
                else
                {
                    EMIT(ctx, "{ ");
                    emitter_indent(&ctx->cg.emitter);
                }
                if (ctx->cg.current_func_ret_type_info)
                {
                    char *tstr = type_to_c_string(ctx->cg.current_func_ret_type_info,
                                                  ctx->config->misra_mode);
                    EMIT(ctx, "%s", tstr);
                    zfree(tstr);
                }
                else if (ctx->cg.current_func_ret_type &&
                         strcmp(ctx->cg.current_func_ret_type, "void") != 0 &&
                         strcmp(ctx->cg.current_func_ret_type, "unknown") != 0)
                {
                    EMIT(ctx, "%s", ctx->cg.current_func_ret_type);
                }
                else
                {
                    emit_auto_type(ctx, node->ret.value, node->token);
                }
                EMIT(ctx, " %s = ", ret_name);
                if (node->ret.value->kind == NODE_EXPR_VAR && ctx->self_is_pointer &&
                    strcmp(node->ret.value->var_ref.name, "self") == 0)
                {
                    EMIT(ctx, "*");
                }
                codegen_expression(ctx, node->ret.value);
                EMIT(ctx, "; ");
                emit_defers_down_to(ctx, ctx->cg.func_defer_boundary);
                if (!inline_ret)
                {
                    emitter_dedent(&ctx->cg.emitter);
                    EMIT(ctx, "return %s; }\n", ret_name);
                }
                else
                {
                    EMIT(ctx, "return %s;\n", ret_name);
                }
            }
            else if (has_defers)
            {
                emit_defers_down_to(ctx, ctx->cg.func_defer_boundary);
                EMIT(ctx, "return;\n");
            }
            else
            {
                EMIT(ctx, "return ");

                if (node->ret.value && node->ret.value->kind == NODE_EXPR_VAR &&
                    ctx->self_is_pointer && strcmp(node->ret.value->var_ref.name, "self") == 0)
                {
                    // return self; -> return *self; (if returns by value)
                    EMIT(ctx, "*");
                }
                else if (node->ret.value && node->ret.value->kind == NODE_EXPR_UNARY &&
                         strcmp(node->ret.value->unary.op, "&") == 0 &&
                         node->ret.value->unary.operand->kind == NODE_EXPR_VAR &&
                         strcmp(node->ret.value->unary.operand->var_ref.name, "self") == 0)
                {
                    // return &self; -> return self; (since self is already a pointer in C)
                    codegen_expression(ctx, node->ret.value->unary.operand);
                    EMIT(ctx, ";\n");
                    break;
                }
                if (node->ret.value)
                {
                    codegen_expression(ctx, node->ret.value);
                }
                EMIT(ctx, ";\n");
            }
        }
        break;
    }
    case NODE_EXPR_MEMBER:
    {
        codegen_expression(ctx, node->member.target);
        char *lt = infer_type(ctx, node->member.target);
        if (lt && (lt[strlen(lt) - 1] == '*' || strstr(lt, "*")))
        {
            EMIT(ctx, "->%s", node->member.field);
        }
        else
        {
            EMIT(ctx, ".%s", node->member.field);
        }
        if (lt)
        {
            zfree(lt);
        }
        break;
    }
    case NODE_REPL_PRINT:
    {
        EMIT(ctx, "{ ");
        emit_auto_type(ctx, node->repl_print.expr, node->token);
        EMIT(ctx, " _zval = (");
        codegen_expression(ctx, node->repl_print.expr);
        EMIT(ctx,
             "); fprintf(stdout, _z_str(_zval), _z_arg(_zval)); fprintf(stdout, \"\\n\"); }\n");
        break;
    }
    case NODE_EXPR_LITERAL:
        // String literal statement should auto-print
        if (node->literal.kind == LITERAL_STRING)
        {
            EMIT(ctx, "printf(\"%%s\\n\", ");
            codegen_expression(ctx, node);
            EMIT(ctx, ");\n");
        }
        else
        {
            // Non-string literals as statements - just evaluate
            codegen_expression(ctx, node);
            EMIT(ctx, ";\n");
        }
        break;
    case NODE_CUDA_LAUNCH:
    {
        // Emit CUDA kernel launch: kernel<<<grid, block, shared, stream>>>(args);
        ASTNode *call = node->cuda_launch.call;

        // Get kernel name from callee
        if (call->call.callee->kind == NODE_EXPR_VAR)
        {
            EMIT(ctx, "%s<<<", call->call.callee->var_ref.name);
        }
        else
        {
            codegen_expression(ctx, call->call.callee);
            EMIT(ctx, "<<<");
        }

        // Grid dimension
        codegen_expression(ctx, node->cuda_launch.grid);
        EMIT(ctx, ", ");

        // Block dimension
        codegen_expression(ctx, node->cuda_launch.block);

        // Optional shared memory size
        if (node->cuda_launch.shared_mem || node->cuda_launch.stream)
        {
            EMIT(ctx, ", ");
            if (node->cuda_launch.shared_mem)
            {
                codegen_expression(ctx, node->cuda_launch.shared_mem);
            }
            else
            {
                EMIT(ctx, "0");
            }
        }

        // Optional CUDA stream
        if (node->cuda_launch.stream)
        {
            EMIT(ctx, ", ");
            codegen_expression(ctx, node->cuda_launch.stream);
        }

        EMIT(ctx, ">>>(");

        // Arguments
        ASTNode *arg = call->call.args;
        int first = 1;
        while (arg)
        {
            if (!first)
            {
                EMIT(ctx, ", ");
            }
            codegen_expression(ctx, arg);
            first = 0;
            arg = arg->next;
        }

        EMIT(ctx, ");\n");
        break;
    }
    case NODE_PREPROC_DIRECTIVE:
    {
        EMIT(ctx, "%s\n", node->raw_stmt.content);
        break;
    }
    case NODE_RAW_STMT:
        emit_raw_with_captures(ctx, node->raw_stmt.content);
        EMIT(ctx, "\n");
        break;

    case NODE_LINK:
        // @link handled during compilation setup — nothing to emit
        break;

    default:
    {
        char *discard_type = discarded_drop_type(ctx, node);
        if (discard_type)
        {
            // The result is never bound to a name: drop it right away.
            int id = ctx->cg.tmp_counter++;
            EMIT(ctx, "{ %s _z_discard_%d = ", discard_type, id);
            codegen_expression(ctx, node);
            EMIT(ctx, "; %s__Drop__glue(&_z_discard_%d); }\n", discard_type, id);
        }
        else
        {
            codegen_expression(ctx, node);
            EMIT(ctx, ";\n");
        }
        if (node->kind == NODE_EXPR_CALL && node->call.callee &&
            ctx->cg.pending_closure_free_count > 0)
        {
            int is_thread_spawn = 0;
            if (node->call.callee->kind == NODE_EXPR_VAR && node->call.callee->var_ref.name &&
                strstr(node->call.callee->var_ref.name, "Thread::spawn"))
            {
                is_thread_spawn = 1;
            }
            else if (node->call.callee->kind == NODE_EXPR_MEMBER &&
                     node->call.callee->member.target &&
                     node->call.callee->member.target->kind == NODE_EXPR_VAR &&
                     strcmp(node->call.callee->member.target->var_ref.name, "Thread") == 0 &&
                     strcmp(node->call.callee->member.field, "spawn") == 0)
            {
                is_thread_spawn = 1;
            }
            if (is_thread_spawn)
            {
                ctx->cg.pending_closure_free_count = 0;
            }
        }
        emit_pending_closure_frees(ctx);
    }
    }
}

// Walks AST nodes and generates code.
void codegen_walker(ParserContext *ctx, ASTNode *node)
{
    while (node)
    {
        emit_source_mapping(ctx, node); // Step to this expression
        codegen_node_single(ctx, node);
        node = node->next;
    }
}
