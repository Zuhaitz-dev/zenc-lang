// SPDX-License-Identifier: MIT

/**
 * @file symbols.h
 * @brief Symbol table -- scopes, symbol lookup, registration.
 */

#ifndef SYMBOLS_H
#ifndef ZC_ALLOW_INTERNAL
#error "ast/symbols.h is internal to Zen C. Include the appropriate public header instead."
#endif

#define SYMBOLS_H

#include <stddef.h>
#include "ast.h"

struct FuncSig;
typedef struct FuncSig FuncSig;

/**
 * @brief Kind of symbol in the unified symbol table.
 */
typedef enum
{
    SYM_VARIABLE,  ///< Local or global variable.
    SYM_FUNCTION,  ///< Function or method.
    SYM_STRUCT,    ///< Struct definition.
    SYM_ENUM,      ///< Enum definition.
    SYM_ALIAS,     ///< Type alias (typedef).
    SYM_CONSTANT,  ///< Constant value.
    SYM_MODULE,    ///< Module/Namespace.
    SYM_TRAIT,     ///< Trait definition.
    SYM_PRIMITIVE, ///< Built-in primitive type.
    SYM_LABEL      ///< Goto label.
} SymbolKind;

/**
 * @brief Represents a symbol in the unified symbol table.
 */
typedef struct ZenSymbol
{
    char *name; ///< Symbol name.

    // --- MISRA tracking ---
    struct ZenSymbol *original; ///< Pointer to the original symbol (for clones).
    ASTNode *first_using_func;  ///< First function that referenced this global.
    int multi_func_use;         ///< Flag set if referenced by multiple functions.

    SymbolKind kind;         ///< Kind of symbol.
    char *type_name;         ///< String representation of the type (legacy).
    Type *type_info;         ///< Formal type definition.
    Token decl_token;        ///< Token where the symbol was declared.
    int is_used;             ///< 1 if reference count > 0.
    int is_immutable;        ///< 1 if value cannot be changed.
    int is_static;           ///< 1 if static storage.
    int is_export;           ///< 1 if visible outside module.
    int is_const_value;      ///< 1 if compile-time constant.
    int is_def;              ///< 1 if definition vs declaration.
    long long const_int_val; ///< Integer value if constant.
    int is_moved;            ///< 1 if ownership transferred.
    int is_local;            ///< 1 if this is a local variable/symbol.
    int is_written_to;       ///< 1 if the value (or dereferenced memory) was modified.
    int is_dereferenced;     ///< 1 if struct fields were explicitly accessed.
    char *cfg_condition;     ///< Optional @cfg condition.
    int scope_depth; ///< Nesting level where the symbol was defined (0 = global, 1+ = local).
    char *link_name; ///< Overriding C identifier.

    union
    {
        ASTNode *node; ///< AST node for the definition (Struct, Enum, Func).
        FuncSig *sig;  ///< Function signature metadata.
        struct
        {
            char *original_type;
            Type *resolved_type;
        } alias;
        struct
        {
            int int_val;
            double float_val;
            char *str_val;
        } constant;
        struct
        {
            char *path;
            char *alias_name;
        } module;
    } data;

    struct ZenSymbol *next;            ///< Next symbol in the same scope.
    struct ZenSymbol *bucket_next;     ///< Next in its scope's name index (SymbolIndex).
    struct ZenSymbol *all_bucket_next; ///< Next in the name index of the list of all symbols.
    struct ZenSymbol *type_next;       ///< Next struct or enum in the list of all symbols.
} ZenSymbol;

/**
 * @brief A name index over a list of symbols that only grows at its head.
 *
 * The list stays the truth; the index finds a name in it without walking it all. A
 * bucket's chain is in the list's order (newest first), so the first match in a chain
 * is the first match in the list. Symbols' names never change once added.
 *
 * The chain link is a field of ZenSymbol, given as its offset (`link`), so a symbol can
 * be in two indexes: its scope's (bucket_next) and the all-symbols one (all_bucket_next).
 * There are no buckets until the list is worth indexing: then the list is walked.
 */
typedef struct SymbolIndex
{
    struct ZenSymbol **buckets; ///< NULL while the list is short.
    unsigned bucket_count;      ///< A power of two.
    unsigned count;             ///< Symbols in the list.
} SymbolIndex;

/** @brief Offsets of the two chain links, for the `link` arguments below. */
#define SYMBOL_LINK_SCOPE offsetof(ZenSymbol, bucket_next)
#define SYMBOL_LINK_ALL offsetof(ZenSymbol, all_bucket_next)

/**
 * @brief Tells the index that `sym` was just put at the head of the list `head`.
 */
void symbol_index_added(SymbolIndex *ix, ZenSymbol *head, ZenSymbol *sym, size_t link);

/**
 * @brief Whether the index has buckets; if not, the caller walks the list.
 */
int symbol_index_ready(const SymbolIndex *ix);

/**
 * @brief The first symbol named `name` in the index, or after `prev` in its chain (NULL
 * for the first). Only valid when symbol_index_ready().
 */
ZenSymbol *symbol_index_find(const SymbolIndex *ix, const char *name, const ZenSymbol *prev,
                             size_t link);

/**
 * @brief Represents a lexical scope (block).
 */
typedef struct Scope
{
    ZenSymbol *symbols;   ///< Linked list of symbols in this scope.
    SymbolIndex index;    ///< Name index of `symbols`.
    struct Scope *parent; ///< Pointer to the parent scope (NULL for global).
    char *name;           ///< Optional name for the scope (e.g. "Module::Func").
    int is_loop;          ///< 1 if this is a loop scope (for break/continue).
} Scope;

// ** Symbol Table Utilities **

/**
 * @brief Creates a new scope as a child of the given parent.
 */
Scope *symbol_scope_create(Scope *parent, const char *name);

/**
 * @brief Adds a symbol to the given scope.
 */
ZenSymbol *symbol_add(Scope *s, const char *name, SymbolKind kind);

/**
 * @brief Performs a local lookup in the given scope only.
 */
ZenSymbol *symbol_lookup_local(Scope *s, const char *name);

/**
 * @brief Performs a recursive lookup across the scope hierarchy.
 */
ZenSymbol *symbol_lookup(Scope *s, const char *name);

/**
 * @brief Performs a recursive lookup of a specific symbol kind.
 */
ZenSymbol *symbol_lookup_kind(Scope *s, const char *name, SymbolKind kind);

#endif // SYMBOLS_H
