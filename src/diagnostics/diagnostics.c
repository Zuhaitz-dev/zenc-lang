// SPDX-License-Identifier: MIT
#include "diagnostics.h"
#include "../utils/colors.h"
#include "constants.h"
#include "parser.h"
#include "utils/cJSON.h"
#include <stdio.h>
#include <stddef.h>

// Local diagnostic context — replaces g_parser_ctx dependency.
// Set via diag_set_parser_ctx() at the start of each compilation.
static struct
{
    ParserContext *parser_ctx;
} d_ctx = {NULL};

void diag_set_parser_ctx(ParserContext *ctx)
{
    d_ctx.parser_ctx = ctx;
}

// Diagnostics take no compiler argument: they use (config) and update (error and
// warning counts) the compiler of the registered parser context. Before a context is
// registered they fall back to a private compiler that nothing reads.
static ZenCompiler *diag_compiler(void)
{
    static ZenCompiler no_context;
    return d_ctx.parser_ctx ? d_ctx.parser_ctx->compiler : &no_context;
}

static CompilerConfig *diag_cfg(void)
{
    return &diag_compiler()->config;
}

static const char *diag_filename(Token t)
{
    if (t.file)
    {
        return t.file;
    }
    return d_ctx.parser_ctx ? d_ctx.parser_ctx->current_filename : "unknown";
}

static void diag_print_location(FILE *f, Token t)
{
    const char *file = diag_filename(t);
    if (t.line > 0 && t.col > 0)
    {
        fprintf(f, COLOR_BLUE "  --> " COLOR_RESET "%s:%d:%d\n", file, t.line, t.col);
    }
    else
    {
        fprintf(f, COLOR_BLUE "  --> " COLOR_RESET "%s\n", file);
    }
}

static void diag_print_context(FILE *f, Token t, const char *caret_color)
{
    if (!t.start || t.col <= 0)
    {
        return;
    }
    const char *line_start = t.start - (t.col - 1);
    const char *line_end = t.start;
    while (*line_end && *line_end != '\n')
    {
        line_end++;
    }
    ptrdiff_t line_len = line_end - line_start;
    fprintf(f, COLOR_BLUE "   |\n" COLOR_RESET);
    fprintf(f, COLOR_BLUE "%-3d| " COLOR_RESET "%.*s\n", t.line, (int)line_len, line_start);
    fprintf(f, COLOR_BLUE "   | " COLOR_RESET);
    for (int i = 0; i < t.col - 1; i++)
    {
        fprintf(f, " ");
    }
    fprintf(f, "%s^ here" COLOR_RESET "\n", caret_color);
    fprintf(f, COLOR_BLUE "   |\n" COLOR_RESET);
}

static void emit_json(const char *level, Token t, const char *msg, const char *suggestion,
                      int diag_id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "file", diag_filename(t));
    cJSON_AddNumberToObject(root, "line", t.line);
    cJSON_AddNumberToObject(root, "col", t.col);
    cJSON_AddStringToObject(root, "level", level);
    cJSON_AddStringToObject(root, "message", msg);
    if (diag_id != DIAG_NONE)
    {
        cJSON_AddNumberToObject(root, "code", diag_id);
    }
    if (suggestion)
    {
        cJSON_AddStringToObject(root, "suggestion", suggestion);
    }

    char *json = cJSON_PrintUnformatted(root);
    if (!diag_cfg()->mode_lsp)
    {
        fprintf(stderr, "%s\n", json);
    }
    zfree(json);
    cJSON_Delete(root);

    // Call LSP diagnostics hook
    if (d_ctx.parser_ctx && d_ctx.parser_ctx->is_fault_tolerant && d_ctx.parser_ctx->on_diagnostic)
    {
        int severity = 1; // Error
        if (strcmp(level, "warning") == 0)
        {
            severity = 2; // Warning
        }
        else if (strcmp(level, "info") == 0)
        {
            severity = 3; // Info
        }

        // Room for the message, the hints (diag_emit joins them in MAX_PATH_LEN) and the text.
        char full_msg[MAX_ERROR_MSG_LEN + MAX_PATH_LEN + 32];
        if (suggestion)
        {
            snprintf(full_msg, sizeof(full_msg), "%s (Suggestion: %s)", msg, suggestion);
        }
        else
        {
            snprintf(full_msg, sizeof(full_msg), "%s", msg);
        }
        d_ctx.parser_ctx->on_diagnostic(d_ctx.parser_ctx->error_callback_data, t, severity,
                                        full_msg, diag_id);
    }
}

// Prints a diagnostic as text to `data`, a FILE * (stderr when NULL). It has the
// DiagConsumer signature, so a tool can register it, and it is used when none is registered.
void diag_render_text(void *data, const ZenDiagnostic *d)
{
    FILE *out = data ? (FILE *)data : stderr;

    const char *label = d->is_error ? COLOR_RED "error: " : COLOR_YELLOW "warning: ";
    fprintf(out, "%s" COLOR_RESET COLOR_BOLD "%s" COLOR_RESET "\n", label, d->message);
    diag_print_location(out, d->token);
    diag_print_context(out, d->token, d->is_error ? COLOR_RED : COLOR_YELLOW);

    const char *kind = d->is_error ? "help" : "note";
    const char *const *h = d->hints;
    while (h && *h)
    {
        fprintf(out, COLOR_CYAN "   = %s: " COLOR_RESET "%s\n", kind, *h);
        h++;
    }
}

// The single path of every diagnostic: silence, count, print (text or JSON), notify the
// LSP/REPL and, for a fatal error, stop. `hints` is NULL or a NULL-terminated array.
static void diag_emit(int is_error, int fatal, int diag_id, Token t, const char *msg,
                      const char *const *hints)
{
    ParserContext *pctx = d_ctx.parser_ctx;
    if (is_error && pctx && pctx->suppress_errors)
    {
        if (fatal && pctx->is_fault_tolerant)
        {
            pctx->had_error = 1;
        }
        return;
    }
    if (!is_error && diag_cfg()->quiet)
    {
        return;
    }

    if (is_error)
    {
        diag_compiler()->error_count++;
    }
    else
    {
        diag_compiler()->warning_count++;
    }

    ZenDiagnostic d = {is_error, fatal, diag_id, t, msg, hints};

    ZenCompiler *compiler = diag_compiler();
    if (compiler->diag_consumer)
    {
        compiler->diag_consumer(compiler->diag_consumer_data, &d);
    }
    else if (diag_cfg()->json_output)
    {
        char joined_hints[MAX_PATH_LEN] = {0};
        const char *const *h = hints;
        while (h && *h)
        {
            if (joined_hints[0])
            {
                strncat(joined_hints, "\n", sizeof(joined_hints) - strlen(joined_hints) - 1);
            }
            strncat(joined_hints, *h, sizeof(joined_hints) - strlen(joined_hints) - 1);
            h++;
        }
        emit_json(is_error ? "error" : "warning", t, msg, joined_hints[0] ? joined_hints : NULL,
                  diag_id);
    }
    else
    {
        diag_render_text(NULL, &d);
    }

    if (is_error && pctx && pctx->on_error)
    {
        char full_msg[MAX_PATH_LEN * 2];
        snprintf(full_msg, sizeof(full_msg), "%s", msg);
        const char *const *h = hints;
        while (h && *h)
        {
            strncat(full_msg, "\nhelp: ", sizeof(full_msg) - strlen(full_msg) - 1);
            strncat(full_msg, *h, sizeof(full_msg) - strlen(full_msg) - 1);
            h++;
        }
        if (fatal && pctx->is_fault_tolerant)
        {
            pctx->had_error = 1;
        }
        pctx->on_error(pctx->error_callback_data, t, full_msg);
    }

    if (fatal && !(pctx && pctx->is_fault_tolerant && pctx->on_error) && !diag_cfg()->mode_lsp)
    {
        exit(1);
    }
}

void zfatal(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    fprintf(stderr, "Fatal: ");
    vfprintf(stderr, fmt, a);
    fprintf(stderr, "\n");
    va_end(a);
    if (d_ctx.parser_ctx && d_ctx.parser_ctx->is_fault_tolerant)
    {
        return;
    }
    if (diag_cfg()->mode_lsp)
    {
        return;
    }
    exit(1);
}

// Warning system (non-fatal).
void zwarn(const char *fmt, ...)
{
    char msg[MAX_ERROR_MSG_LEN];
    va_list a;
    va_start(a, fmt);
    vsnprintf(msg, sizeof(msg), fmt, a);
    va_end(a);
    diag_emit(0, 0, DIAG_NONE, TOKEN_UNKNOWN, msg, NULL);
}

void zwarn_at_diag(int diag_id, Token t, const char *fmt, ...)
{
    char msg[MAX_ERROR_MSG_LEN];
    va_list a;
    va_start(a, fmt);
    vsnprintf(msg, sizeof(msg), fmt, a);
    va_end(a);
    diag_emit(0, 0, diag_id, t, msg, NULL);
}

void zwarn_at(Token t, const char *fmt, ...)
{
    char msg[MAX_ERROR_MSG_LEN];
    va_list a;
    va_start(a, fmt);
    vsnprintf(msg, sizeof(msg), fmt, a);
    va_end(a);
    diag_emit(0, 0, DIAG_NONE, t, msg, NULL);
}

void zwarn_with_suggestion_diag(int diag_id, Token t, const char *msg, const char *suggestion)
{
    const char *hints[] = {suggestion, NULL};
    diag_emit(0, 0, diag_id, t, msg, suggestion ? hints : NULL);
}

void zwarn_with_suggestion(Token t, const char *msg, const char *suggestion)
{
    const char *hints[] = {suggestion, NULL};
    diag_emit(0, 0, DIAG_NONE, t, msg, suggestion ? hints : NULL);
}

void zpanic_at(Token t, const char *fmt, ...)
{
    char msg[MAX_ERROR_MSG_LEN];
    va_list a;
    va_start(a, fmt);
    vsnprintf(msg, sizeof(msg), fmt, a);
    va_end(a);
    diag_emit(1, 1, DIAG_NONE, t, msg, NULL);
}

// Enhanced error with suggestion.
void zpanic_with_suggestion(Token t, const char *msg, const char *suggestion)
{
    const char *hints[] = {suggestion, NULL};
    diag_emit(1, 1, DIAG_NONE, t, msg, suggestion ? hints : NULL);
}

void zpanic_with_hints(Token t, const char *msg, const char *const *hints)
{
    diag_emit(1, 1, DIAG_NONE, t, msg, hints);
}

void zerror_at(Token t, const char *fmt, ...)
{
    char msg[MAX_ERROR_MSG_LEN];
    va_list a;
    va_start(a, fmt);
    vsnprintf(msg, sizeof(msg), fmt, a);
    va_end(a);
    diag_emit(1, 0, DIAG_NONE, t, msg, NULL);
}

void zerror_with_hints(Token t, const char *msg, const char *const *hints)
{
    diag_emit(1, 0, DIAG_NONE, t, msg, hints);
}

// Specific error types with helpful messages.

void warn_unused_variable(Token t, const char *var_name)
{
    if (!is_diag_enabled(DIAG_UNUSED_VAR))
    {
        return;
    }
    char msg[MAX_SHORT_MSG_LEN];
    snprintf(msg, sizeof(msg), "Unused variable '%s'", var_name);
    zwarn_with_suggestion(t, msg, "Consider removing it or prefixing with '_'");
}

void warn_shadowing(Token t, const char *var_name)
{
    if (!is_diag_enabled(DIAG_STYLE_SHADOWING))
    {
        return;
    }
    char msg[MAX_SHORT_MSG_LEN];
    snprintf(msg, sizeof(msg), "Variable '%s' shadows a previous declaration", var_name);
    zwarn_with_suggestion(t, msg, "This can lead to confusion");
}

void warn_comparison_always_true(Token t, const char *reason)
{
    if (!is_diag_enabled(DIAG_LOGIC_ALWAYS_TRUE))
    {
        return;
    }
    zwarn_with_suggestion(t, "Comparison is always true", reason);
}

void warn_comparison_always_false(Token t, const char *reason)
{
    if (!is_diag_enabled(DIAG_LOGIC_ALWAYS_FALSE))
    {
        return;
    }
    zwarn_with_suggestion(t, "Comparison is always false", reason);
}

void warn_unused_parameter(Token t, const char *param_name, const char *func_name)
{
    if (!is_diag_enabled(DIAG_UNUSED_PARAM))
    {
        return;
    }
    char msg[MAX_SHORT_MSG_LEN];
    snprintf(msg, sizeof(msg), "Unused parameter '%s' in function '%s'", param_name, func_name);
    zwarn_with_suggestion(t, msg, "Consider prefixing with '_' if intentionally unused");
}

void warn_array_bounds(Token t, int index, int size)
{
    if (!is_diag_enabled(DIAG_SAFETY_ARRAY_BOUNDS))
    {
        return;
    }
    char msg[MAX_SHORT_MSG_LEN];
    snprintf(msg, sizeof(msg), "Array index %d is out of bounds for array of size %d", index, size);
    char note[MAX_SHORT_MSG_LEN];
    snprintf(note, sizeof(note), "Valid indices are 0 to %d", size - 1);
    zwarn_with_suggestion(t, msg, note);
}

void warn_format_string(Token t, int arg_num, const char *expected, const char *got)
{
    if (!is_diag_enabled(DIAG_STYLE_FORMAT))
    {
        return;
    }
    char msg[MAX_SHORT_MSG_LEN];
    snprintf(msg, sizeof(msg), "Format argument %d: expected '%s', got '%s'", arg_num, expected,
             got);
    zwarn_with_suggestion(t, msg, "Mismatched format specifier may cause undefined behavior");
}

void warn_void_main(Token t)
{
    if (!is_diag_enabled(DIAG_PEDANTIC_STRICT_TYPING))
    {
        return;
    }
    zwarn_with_suggestion(t, "'void main()' is non-standard and leads to undefined behavior",
                          "Consider using 'fn main()' or 'fn main() -> c_int' instead");
}

void warn_misra_violation(Token t, const char *msg)
{
    // Fire the MISRA warning if the explicit diag is enabled or if the global compiler flag is
    // active.
    if (!is_diag_enabled(DIAG_MISRA_VIOLATION) && !diag_cfg()->misra_mode)
    {
        return;
    }
    zwarn_with_suggestion(t, msg, "This pattern violates stringent MISRA C safety standards.");
}

int is_diag_enabled(DiagnosticID id)
{
    if (id == DIAG_NONE || id >= DIAG_MAX)
    {
        return 0;
    }
    // Check the bit of this diagnostic in the config's mask
    return (diag_cfg()->diag_mask & ((uint64_t)1 << id)) != 0;
}

void zwarn_diag(DiagnosticID id, Token t, const char *msg, const char *hint)
{
    if (!is_diag_enabled(id))
    {
        return;
    }

    char final_hint[MAX_MANGLED_NAME_LEN];
    if (id == DIAG_INTEROP_UNDEF_FUNC)
    {
        if (hint)
        {
            // Strip trailing period from hint if it exists to avoid double period
            char hint_copy[MAX_SHORT_MSG_LEN];
            strncpy(hint_copy, hint, sizeof(hint_copy) - 1);
            hint_copy[sizeof(hint_copy) - 1] = '\0';
            size_t len = strlen(hint_copy);
            if (len > 0 && hint_copy[len - 1] == '.')
            {
                hint_copy[len - 1] = '\0';
            }

            snprintf(final_hint, sizeof(final_hint),
                     "%s. If this is a C function, it might need to be whitelisted in 'zenc.json'",
                     hint_copy);
        }
        else
        {
            snprintf(final_hint, sizeof(final_hint),
                     "If this is a C function, it might need to be whitelisted in 'zenc.json'");
        }
        zwarn_with_suggestion_diag((int)(id), t, msg, final_hint);
    }
    else
    {
        zwarn_with_suggestion_diag((int)(id), t, msg, hint);
    }
}

int set_diag_by_name(CompilerConfig *cfg, const char *name, int enabled)
{
    if (strcmp(name, "interop") == 0)
    {
        if (enabled)
        {
            cfg->diag_mask |= ((uint64_t)1 << DIAG_INTEROP_UNDEF_FUNC);
        }
        else
        {
            cfg->diag_mask &= ~((uint64_t)1 << DIAG_INTEROP_UNDEF_FUNC);
        }
        return 1;
    }
    else if (strcmp(name, "pedantic") == 0)
    {
        if (enabled)
        {
            cfg->diag_mask |= ((uint64_t)1 << DIAG_PEDANTIC_STRICT_TYPING);
            cfg->diag_mask |= ((uint64_t)1 << DIAG_INTEROP_UNDEF_FUNC);
        }
        else
        {
            cfg->diag_mask &= ~((uint64_t)1 << DIAG_PEDANTIC_STRICT_TYPING);
            cfg->diag_mask &= ~((uint64_t)1 << DIAG_INTEROP_UNDEF_FUNC);
        }
        return 1;
    }
    else if (strcmp(name, "unused") == 0)
    {
        if (enabled)
        {
            cfg->diag_mask |= ((uint64_t)1 << DIAG_UNUSED_VAR);
            cfg->diag_mask |= ((uint64_t)1 << DIAG_UNUSED_PARAM);
        }
        else
        {
            cfg->diag_mask &= ~((uint64_t)1 << DIAG_UNUSED_VAR);
            cfg->diag_mask &= ~((uint64_t)1 << DIAG_UNUSED_PARAM);
        }
        return 1;
    }
    else if (strcmp(name, "safety") == 0)
    {
        if (enabled)
        {
            cfg->diag_mask |= ((uint64_t)1 << DIAG_SAFETY_NULL_PTR);
            cfg->diag_mask |= ((uint64_t)1 << DIAG_SAFETY_DIV_ZERO);
            cfg->diag_mask |= ((uint64_t)1 << DIAG_SAFETY_ARRAY_BOUNDS);
        }
        else
        {
            cfg->diag_mask &= ~((uint64_t)1 << DIAG_SAFETY_NULL_PTR);
            cfg->diag_mask &= ~((uint64_t)1 << DIAG_SAFETY_DIV_ZERO);
            cfg->diag_mask &= ~((uint64_t)1 << DIAG_SAFETY_ARRAY_BOUNDS);
        }
        return 1;
    }
    else if (strcmp(name, "logic") == 0)
    {
        if (enabled)
        {
            cfg->diag_mask |= ((uint64_t)1 << DIAG_LOGIC_UNREACHABLE);
            cfg->diag_mask |= ((uint64_t)1 << DIAG_LOGIC_ALWAYS_TRUE);
            cfg->diag_mask |= ((uint64_t)1 << DIAG_LOGIC_ALWAYS_FALSE);
        }
        else
        {
            cfg->diag_mask &= ~((uint64_t)1 << DIAG_LOGIC_UNREACHABLE);
            cfg->diag_mask &= ~((uint64_t)1 << DIAG_LOGIC_ALWAYS_TRUE);
            cfg->diag_mask &= ~((uint64_t)1 << DIAG_LOGIC_ALWAYS_FALSE);
        }
        return 1;
    }
    else if (strcmp(name, "conversion") == 0)
    {
        if (enabled)
        {
            cfg->diag_mask |= ((uint64_t)1 << DIAG_CONVERSION_IMPLICIT);
            cfg->diag_mask |= ((uint64_t)1 << DIAG_CONVERSION_NARROWING);
        }
        else
        {
            cfg->diag_mask &= ~((uint64_t)1 << DIAG_CONVERSION_IMPLICIT);
            cfg->diag_mask &= ~((uint64_t)1 << DIAG_CONVERSION_NARROWING);
        }
        return 1;
    }
    else if (strcmp(name, "style") == 0)
    {
        if (enabled)
        {
            cfg->diag_mask |= ((uint64_t)1 << DIAG_STYLE_SHADOWING);
            cfg->diag_mask |= ((uint64_t)1 << DIAG_STYLE_FORMAT);
        }
        else
        {
            cfg->diag_mask &= ~((uint64_t)1 << DIAG_STYLE_SHADOWING);
            cfg->diag_mask &= ~((uint64_t)1 << DIAG_STYLE_FORMAT);
        }
        return 1;
    }
    else if (strcmp(name, "all") == 0)
    {
        if (enabled)
        {
            cfg->diag_mask = 0xFFFFFFFFFFFFFFFF;
        }
        else
        {
            cfg->diag_mask = 0;
        }
        return 1;
    }
    return 0;
}
