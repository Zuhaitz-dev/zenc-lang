// SPDX-License-Identifier: MIT
// Standalone Zen C language server (JSON-RPC over stdio). Formerly `zc lsp`.
#include "tool_common.h"
#include "../compiler.h"
#include "../lsp/lsp_main.h"

int main(int argc, char **argv)
{
    ZenCompiler compiler = {0};
    z_compiler_setup(&compiler);
    return lsp_main(&compiler, argc, argv);
}
