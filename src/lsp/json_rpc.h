// SPDX-License-Identifier: MIT

#ifndef ZC_ALLOW_INTERNAL
#error "lsp/json_rpc.h is internal to Zen C. Include the appropriate public header instead."
#endif

#ifndef JSON_RPC_H
#define JSON_RPC_H

/**
 * @brief Handle a raw JSON-RPC request string.
 *
 * Parses the request, routes it to the appropriate handler (initialize, textDocument/didChange,
 * etc.), and sends back the response to stdout.
 *
 * @param json_str Null-terminated JSON request string.
 */
void handle_request(const char *json_str);

/// Set to 0 before calling handlers that modify persistent project state (didOpen, didChange).
/// When 1 (default), the caller (lsp_main.c) may restore the arena after the request.
extern int g_lsp_request_is_readonly;

/// The client accepts `LocationLink` results for textDocument/definition
/// (`textDocument.definition.linkSupport`), read at initialize.
extern int g_lsp_definition_link_support;

#endif
int lsp_main(int argc, char **argv);
