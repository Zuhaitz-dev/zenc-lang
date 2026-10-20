// SPDX-License-Identifier: MIT

#ifndef ZC_ALLOW_INTERNAL
#error "repl/repl.h is internal to Zen C. Include the appropriate public header instead."
#endif

#ifndef REPL_H
#define REPL_H

struct ZenCompiler;

/**
 * @brief Starts the Read-Eval-Print Loop (REPL).
 *
 * @param compiler The compiler of the session (config, error counts).
 * @param self_path Path to the executable/ZC compiler itself.
 */
void run_repl(struct ZenCompiler *compiler, const char *self_path, int argc, char **argv);

#endif
