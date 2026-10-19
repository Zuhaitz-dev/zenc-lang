// SPDX-License-Identifier: MIT
// Unit tests of src/diagnostics/diagnostics.c.
#include "utest.h"
#include "diagnostics/diagnostics.h"
#include <stdio.h>

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
