// SPDX-License-Identifier: MIT
// Unit tests of src/driver/driver.c.
#include "driver/driver.h"
#include "diagnostics/diagnostics.h"
#include "unit_test.h"

UTEST(driver_compile, leaves_no_parser_context_registered)
{
    // A compilation that stops early: the input file does not exist.
    ZenCompiler compiler = {0};
    compiler.config.input_file = "no/such/file.zc";
    compiler.config.quiet = 1;
    ASSERT_EQ(1, driver_compile(&compiler));

    // Its parser context lived on the stack of the finished call: it must not stay registered.
    ASSERT_TRUE(diag_parser_ctx() == NULL);
}
