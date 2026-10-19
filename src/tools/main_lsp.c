// SPDX-License-Identifier: MIT
// Standalone Zen C language server (JSON-RPC over stdio). Formerly `zc lsp`.
#include "tool_common.h"
#include "../compiler.h"
#include "../lsp/lsp_main.h"

int main(int argc, char **argv)
{
    z_compiler_setup(&g_compiler);
    return lsp_main(argc, argv);
}
