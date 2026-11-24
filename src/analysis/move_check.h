// SPDX-License-Identifier: MIT
#ifndef MOVE_CHECK_H
#ifndef ZC_ALLOW_INTERNAL
#error "analysis/move_check.h is internal to Zen C. Include the appropriate public header instead."
#endif

#define MOVE_CHECK_H

#include "ast/ast.h"
#include "ast/symbols.h"
#include "token.h"

typedef struct ParserContext ParserContext;
typedef struct TypeChecker TypeChecker;

/**
 * @brief Status of a moved variable in a specific flow path.
 */
typedef enum
{
    MOVE_STATE_VALID,
    MOVE_STATE_MOVED,
    MOVE_STATE_MAYBE_MOVED // Used when merging diverging paths (e.g., if/else)
} MoveStatus;

/**
 * @brief Linked list of tracked moved symbols in the current state.
 */
typedef struct MoveEntry
{
    char *symbol_name;
    MoveStatus status;
    Token moved_at; // For error reporting
    struct MoveEntry *next;
} MoveEntry;

/**
 * @brief Represents the state of moves at a specific point in control flow.
 */
typedef struct MoveState
{
    struct MoveEntry *entries; // List of tracked symbols
    struct MoveState *parent;  // Parent state (for scoping/forking)
    // A name index of `entries` (a name is in it once), made when the list is long enough
    // to be worth it: a state can track every global of a program.
    struct MoveEntry **index; // open-addressed by symbol_name; NULL: walk `entries`
    unsigned index_cap;       // a power of two
    unsigned count;           // entries in the list
} MoveState;

/**
 * @brief Creates a new move state, optionally inheriting from a parent.
 */
MoveState *move_state_create(MoveState *parent);

/**
 * @brief Deep clones a move state (for branching).
 */
MoveState *move_state_clone(MoveState *src);

/**
 * @brief Merges two branches into a target state.
 *
 * Logic:
 * - Valid + Valid -> Valid
 * - Moved + Moved -> Moved
 * - Moved + Valid -> Maybe Moved (Error on use)
 */
void move_state_merge(MoveState *target, MoveState *a, MoveState *b);

/**
 * @brief Merges a source state into a target state, initializing target if NULL.
 * Used for merging states from multiple break/continue points.
 */
void move_state_merge_into(MoveState **target, MoveState *src);

/**
 * @brief Frees a move state.
 */
void move_state_free(MoveState *state);

/**
 * @brief Check if a symbol or path is moved in the given state (or parents).
 */
MoveStatus get_move_status(MoveState *state, const char *path);

/**
 * @brief Returns a heap-allocated path string for a node (e.g. "var.field"), or NULL.
 */
char *get_node_path(ASTNode *node, int depth, ParserContext *ctx);

/**
 * @brief Determines if a type is safe to copy (implements Copy or is a primitive).
 *
 * @param ctx Parser context for type lookups.
 * @param t The type to check.
 * @return 1 if the type is Copy, 0 if it is Move.
 */
int is_type_copy(ParserContext *ctx, Type *t);

/**
 * @brief Checks if a path use is valid (not moved).
 *
 * @param tc Type checker context for error reporting.
 * @param path The path string to check.
 * @param t Token for error location.
 */
void check_path_validity(TypeChecker *tc, const char *path, Token t);

/**
 * @brief Checks if a node use is valid (not moved).
 *
 * Reports an error if the node (or its parent path) has been moved.
 *
 * @param tc Type checker context for error reporting.
 * @param node The AST node where the use occurs.
 */
void check_use_validity(TypeChecker *tc, ASTNode *node);

/**
 * @brief Marks a symbol (or path) as moved if its type is not Copy.
 *
 * @param ctx Parser context (for checking Copy trait).
 * @param sym The symbol to mark (may be NULL if node is a member).
 * @param context_node The AST node causing the move (assignment, call, etc.).
 */
void mark_symbol_moved(ParserContext *ctx, ZenSymbol *sym, ASTNode *context_node);

/**
 * @brief Marks a symbol (or path) as valid (initialized or re-assigned).
 *
 * @param sym The symbol to mark.
 * @param context_node The AST node where it's assigned.
 */
void mark_symbol_valid(ParserContext *ctx, ZenSymbol *sym, ASTNode *context_node);

#endif // MOVE_CHECK_H
