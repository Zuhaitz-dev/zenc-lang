// SPDX-License-Identifier: MIT
// Unit tests of src/diagnostics/diagnostics.c.
#include "diagnostics/diagnostics.h"
#include "parser/parser.h"
#include <stdio.h>
#include "unit_test.h"

// A consumer that records the last diagnostic it receives
typedef struct
{
    int calls;
    int is_error;
    int fatal;
    int line;
    char message[256];
    char first_hint[256];
} Recorded;

static void record(void *data, const ZenDiagnostic *d)
{
    Recorded *r = data;
    r->calls++;
    r->is_error = d->is_error;
    r->fatal = d->fatal;
    r->line = d->token.line;
    snprintf(r->message, sizeof(r->message), "%s", d->message);
    snprintf(r->first_hint, sizeof(r->first_hint), "%s",
             d->hints && d->hints[0] ? d->hints[0] : "");
}

// Registers `rec` as the consumer of `compiler`, and `ctx`, pointing to `compiler`, as the
// current parser context. Call diag_set_parser_ctx(NULL) at the end of the test.
static void start_recording(ZenCompiler *compiler, Recorded *rec, ParserContext *ctx)
{
    compiler->diag_consumer = record;
    compiler->diag_consumer_data = rec;
    ctx->compiler = compiler;
    ctx->config = &compiler->config;
    diag_set_parser_ctx(ctx);
}

static void render_json(const ZenDiagnostic *d, char *out, size_t size)
{
    FILE *f = tmpfile();
    diag_render_json(f, d);
    rewind(f);
    size_t n = fread(out, 1, size - 1, f);
    out[n] = '\0';
    fclose(f);
}

// Renders `d` with diag_render_text into a temporary file and copies what it wrote to
// `out`. A file is not a terminal, so the colour codes are left out.
static void render_text(const ZenDiagnostic *d, char *out, size_t size)
{
    FILE *f = tmpfile();
    diag_render_text(f, d);
    rewind(f);
    size_t n = fread(out, 1, size - 1, f);
    out[n] = '\0';
    fclose(f);
}

UTEST(diag_render_text, error_with_position_and_hints)
{
    const char *source = "fn main() {\n    let = 5;\n}\n";
    Token t = {0};
    t.start = source + 20; // the '=' in line 2
    t.len = 1;
    t.line = 2;
    t.col = 9;
    t.file = "demo.zc";
    const char *hints[] = {"first hint", "second hint", NULL};
    ZenDiagnostic d = {1, 1, DIAG_NONE, t, "Something went wrong", hints};

    char out[1024];
    render_text(&d, out, sizeof(out));

    ASSERT_STREQ("error: Something went wrong\n"
                 "  --> demo.zc:2:9\n"
                 "   |\n"
                 "2  |     let = 5;\n"
                 "   |         ^ here\n"
                 "   |\n"
                 "   = help: first hint\n"
                 "   = help: second hint\n",
                 out);
}

UTEST(diag_render_text, warning_without_position)
{
    Token t = {0};
    t.file = "demo.zc";
    const char *hints[] = {"a note", NULL};
    ZenDiagnostic d = {0, 0, DIAG_NONE, t, "Something looks odd", hints};

    char out[1024];
    render_text(&d, out, sizeof(out));

    ASSERT_STREQ("warning: Something looks odd\n"
                 "  --> demo.zc\n"
                 "   = note: a note\n",
                 out);
}

UTEST(diag_consumer, receives_an_error)
{
    ZenCompiler compiler = {0};
    Recorded rec = {0};
    ParserContext ctx = {0};
    start_recording(&compiler, &rec, &ctx);

    Token t = {0};
    t.line = 3;
    t.file = "demo.zc";
    zerror_at(t, "Something %s", "went wrong");

    EXPECT_EQ(1, rec.calls);
    EXPECT_EQ(1, rec.is_error);
    EXPECT_EQ(0, rec.fatal);
    EXPECT_EQ(3, rec.line);
    EXPECT_STREQ("Something went wrong", rec.message);
    EXPECT_EQ(1, compiler.error_count);
    diag_set_parser_ctx(NULL);
}

UTEST(diag_consumer, receives_a_warning_with_its_hint)
{
    ZenCompiler compiler = {0};
    Recorded rec = {0};
    ParserContext ctx = {0};
    start_recording(&compiler, &rec, &ctx);

    zwarn_with_suggestion(TOKEN_UNKNOWN, "Something looks odd", "a hint");

    EXPECT_EQ(1, rec.calls);
    EXPECT_EQ(0, rec.is_error);
    EXPECT_STREQ("Something looks odd", rec.message);
    EXPECT_STREQ("a hint", rec.first_hint);
    EXPECT_EQ(1, compiler.warning_count);
    diag_set_parser_ctx(NULL);
}

UTEST(diag_render_json, error_with_hints)
{
    Token t = {0};
    t.line = 2;
    t.col = 9;
    t.file = "demo.zc";
    const char *hints[] = {"first hint", "second hint", NULL};
    ZenDiagnostic d = {1, 1, DIAG_NONE, t, "Something went wrong", hints};

    char out[1024];
    render_json(&d, out, sizeof(out));

    ASSERT_STREQ("{\"file\":\"demo.zc\",\"line\":2,\"col\":9,\"level\":\"error\","
                 "\"message\":\"Something went wrong\","
                 "\"suggestion\":\"first hint\\nsecond hint\"}\n",
                 out);
}

UTEST(diag_render_json, warning_with_a_code)
{
    Token t = {0};
    t.line = 4;
    t.col = 1;
    t.file = "demo.zc";
    ZenDiagnostic d = {0, 0, DIAG_UNUSED_VAR, t, "Unused variable 'x'", NULL};

    char out[1024];
    render_json(&d, out, sizeof(out));

    char expected[256];
    snprintf(expected, sizeof(expected),
             "{\"file\":\"demo.zc\",\"line\":4,\"col\":1,\"level\":\"warning\","
             "\"message\":\"Unused variable 'x'\",\"code\":%d}\n",
             (int)DIAG_UNUSED_VAR);
    ASSERT_STREQ(expected, out);
}

UTEST(diag_consumer, fatal_error_does_not_exit_in_fault_tolerant_mode)
{
    ZenCompiler compiler = {0};
    Recorded rec = {0};
    ParserContext ctx = {0};
    ctx.is_fault_tolerant = 1;
    start_recording(&compiler, &rec, &ctx);

    zpanic_at(TOKEN_UNKNOWN, "Something %s", "fatal");

    EXPECT_EQ(1, rec.calls);
    EXPECT_EQ(1, rec.fatal);
    EXPECT_EQ(1, ctx.had_error);
    EXPECT_EQ(1, compiler.error_count);
    diag_set_parser_ctx(NULL);
}
