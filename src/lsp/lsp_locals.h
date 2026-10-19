// SPDX-License-Identifier: MIT

#ifndef ZC_ALLOW_INTERNAL
#error "lsp/lsp_locals.h is internal to Zen C. Include the appropriate public header instead."
#endif

#ifndef LSP_LOCALS_H
#define LSP_LOCALS_H

#include "parser.h"

// Searches in the body of a function, used by completion. They only read the AST.

/**
 * @brief The `let` that declares `name` in the body of `func`, or NULL.
 *
 * Also NULL when `name` is one of the parameters of `func`: the caller resolves those.
 */
ASTNode *lsp_find_local_decl(ASTNode *func, const char *name);

/**
 * @brief Whether a loop (while, for, for-in, loop, repeat, do-while) starts in the body of
 *        `func` at or before `line`.
 *
 * `line` is compared with the line of the loop's token as it is.
 */
int lsp_has_loop_before(ASTNode *func, int line);

#endif
