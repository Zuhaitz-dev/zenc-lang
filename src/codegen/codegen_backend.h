// SPDX-License-Identifier: MIT
#ifndef CODEGEN_BACKEND_H
#define CODEGEN_BACKEND_H

#include "../ast/ast.h"
#include "../compiler_config.h"

struct ParserContext;

/**
 * @brief A CLI alias that maps a --flag to a backend option.
 */
typedef struct BackendOptAlias
{
    const char *flag;    ///< "--flag-name" including the -- prefix.
    const char *opt_key; ///< Option key pushed to backend_opts.
    const char *opt_val; ///< Option value (NULL defaults to "1").
} BackendOptAlias;

/**
 * @brief A code generation backend.
 *
 * Each backend provides an entry point to emit the full program.
 * The default "c" backend wraps the existing C codegen.
 * A new backend (e.g. x64, wasm, LLVM IR) implements emit_program from scratch.
 */
typedef struct CodegenBackend
{
    /// Backend identifier (e.g. "c", "llvm", "x64").
    const char *name;

    /// Default output file extension (e.g. ".c", ".ll", ".s").
    const char *extension;

    /// Emit the entire program to the current emitter output.
    void (*emit_program)(struct ParserContext *ctx, struct ASTNode *root);

    /// Emit the language preamble (type aliases, runtime, includes).
    void (*emit_preamble)(struct ParserContext *ctx);

    /// Whether this backend needs a C/C++/CUDA/ObjC compiler step after emission.
    int needs_cc;

    /// NULL-terminated array of CLI aliases, or NULL if none.
    const BackendOptAlias *aliases;
} CodegenBackend;

/// Register a backend implementation.
void codegen_register_backend(const CodegenBackend *backend);

/// Get a registered backend by name. Returns the default "c" backend if name is NULL or no
/// backend has that name.
const CodegenBackend *codegen_get_backend(const char *name);

/// Get a registered backend by name, or NULL if no backend has that name.
const CodegenBackend *codegen_find_backend(const char *name);

/// The registered backends (`*count` of them), to list them.
const CodegenBackend *const *codegen_backends(int *count);

/// Initialize the default backends (called once at startup).
void codegen_init_backends(void);

/// Look up a backend option by key. Returns the value ("1" for flag-style opts) or NULL.
const char *backend_opt(zvec_Str *opts, const char *key);

/// Look up a --flag across all registered backends' aliases.
/// Returns "key=value" string if found, or NULL.
const char *codegen_alias_lookup(const char *flag);

void codegen_register_c_backend(void);
void codegen_register_cpp_backend(void);
void codegen_register_cuda_backend(void);
void codegen_register_objc_backend(void);
void codegen_register_json_backend(void);
void codegen_register_lisp_backend(void);
void codegen_register_dot_backend(void);
void codegen_register_astdump_backend(void);

#endif // CODEGEN_BACKEND_H
