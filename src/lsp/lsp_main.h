// SPDX-License-Identifier: MIT
#ifndef LSP_MAIN_H
#define LSP_MAIN_H

struct ZenCompiler;

// Entry point for the standalone `zc-lsp` binary (JSON-RPC over stdio). The server
// compiles with `compiler` for its whole lifetime.
int lsp_main(struct ZenCompiler *compiler, int argc, char **argv);

#endif // LSP_MAIN_H
