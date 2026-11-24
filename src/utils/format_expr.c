// SPDX-License-Identifier: MIT
#include "../parser/parser.h"
#include "../codegen/codegen.h"
#include "../utils/emitter.h"
#include "../ast/ast.h"
#include "../utils/format_expr.h"

char *format_expression_as_c(struct ParserContext *ctx, struct ASTNode *node)
{
    if (!ctx || !node)
    {
        return NULL;
    }

    // Into a buffer of its own, then back to where the emitter was. (emitter_init_buffer
    // would also clear the saved states, so emitter_pop couldn't restore it, and anything
    // emitted after this call during code generation would be lost.)
    Emitter *e = &ctx->cg.emitter;
    emitter_push(e);
    e->mode = EMITTER_BUFFER;
    e->buffer.buf = NULL;
    e->buffer.len = 0;
    e->buffer.cap = 0;
    e->indent_level = 0;
    e->pending_indent = 0;
    codegen_expression(ctx, node);
    char *result = emitter_take_string(&ctx->cg.emitter);
    emitter_pop(&ctx->cg.emitter);
    // Copy to arena so callers don't need system heap management
    char *arena_copy = result ? xstrdup(result) : NULL;
    zfree(result);
    return arena_copy;
}
