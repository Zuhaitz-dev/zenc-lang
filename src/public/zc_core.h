// SPDX-License-Identifier: MIT
#ifndef ZC_CORE_H
#define ZC_CORE_H

/**
 * @file zc_core.h
 * @brief Public API for libzc-core: lexer, parser, AST, and core types.
 *
 * This is the stable interface for consuming the Zen C frontend.
 * Internal headers (parser.h, ast.h internals) are not part of this API.
 */

#include "token.h"
#include "arena.h"

// Compiler Configuration

typedef struct CompilerConfig CompilerConfig;
typedef struct ZenCompiler ZenCompiler;

// AST Types

typedef struct ASTNode ASTNode;
typedef struct Type Type;
typedef struct ParserContext ParserContext;

// Type introspection helpers (defined in ast.c)

/** @brief Check if a type is an integer type (signed or unsigned). */
int is_integer_type(Type *t);
/** @brief Check if a type is an unsigned integer type. */
int is_unsigned_type(Type *t);
/** @brief Check if a type is a signed integer type. */
int is_signed_type(Type *t);
/** @brief Check if a type is a boolean type. */
int is_boolean_type(Type *t);
/** @brief Check if a type is a floating point type. */
int is_float_type(Type *t);
/** @brief Check if a type is incomplete (forward declaration or opaque). */
int is_incomplete_type(ParserContext *ctx, Type *t);
/** @brief Check if an AST node represents a composite expression (struct/array/tuple literal). */
int is_composite_expression(ASTNode *node);

// Type to string conversions

/** @brief Convert a type to a human-readable string. Caller must free the result. */
char *type_to_string(Type *t);
/** @brief Convert a type to a C-compatible string, respecting MISRA rules. Caller must free. */
char *type_to_c_string(Type *t, int misra_mode);

// Parser — Entry Points

struct ParserContext;

/**
 * @brief Parse a complete Zen C source file into an AST.
 * The caller must have initialized the ParserContext and Lexer.
 */
ASTNode *parse_program(ParserContext *ctx, Lexer *l);

/**
 * @brief Parse top-level nodes (program nodes).
 */
ASTNode *parse_program_nodes(ParserContext *ctx, Lexer *l);

/**
 * @brief Parse a single expression.
 */
ASTNode *parse_expression(ParserContext *ctx, Lexer *l);

/**
 * @brief Parse a single statement.
 */
ASTNode *parse_statement(ParserContext *ctx, Lexer *l);

/**
 * @brief Parse a block statement { ... }.
 */
ASTNode *parse_block(ParserContext *ctx, Lexer *l);

// Parser — Context and Utilities

/**
 * @brief Create a minimal ParserContext for parsing.
 * Sets compiler and config fields. Does NOT initialize codegen state.
 *
 * @param compiler The compiler instance to associate.
 * @return An initialized ParserContext (on stack, or heap depending on caller).
 */
void parser_context_init(ParserContext *ctx, struct ZenCompiler *compiler);

/**
 * @brief Scan build directives from source before lexing.
 */
void scan_build_directives(ParserContext *ctx, const char *src);

/**
 * @brief Resolve an import path using include paths and root.
 */
char *z_resolve_path(const char *fn, const char *relative_to, CompilerConfig *cfg);

/**
 * @brief Load a file into memory.
 */
char *load_file(const char *fn, const char *relative_to, CompilerConfig *cfg);

// Diagnostics (minimal)

/** @brief Report a compilation error at a source location. */
void zerror_at(Token t, const char *fmt, ...);
/** @brief Report a warning at a source location. */
void zwarn_at(Token t, const char *fmt, ...);
/** @brief Report a fatal error and abort compilation. */
void zpanic_at(Token t, const char *fmt, ...);

#endif // ZC_CORE_H
