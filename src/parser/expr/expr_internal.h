// SPDX-License-Identifier: MIT
#ifndef EXPR_INTERNAL_H
#define EXPR_INTERNAL_H

typedef struct
{
    ASTNode *head;
    ASTNode *tail;
    char **arg_names;
    int count;
    int has_named;
} CallArgs;

// From expr_helpers.c
int token_is_field_name(Token t);
/**
 * Cheap token scan run (with the lexer on a `<`) before speculatively parsing
 * `name<...>` as generic arguments. Returns 0 when the tokens cannot form a
 * type list closed by `>`, so the `<` is a comparison: `count < limits[i] {`,
 * `k < *j`, `k < (n + 1)`. Speculative type parsing is not free of side
 * effects: it reports errors (array sizes, prefix pointers) and registers
 * tuple types.
 */
int tokens_could_be_generic_args(Lexer *l);
void check_move_usage(ParserContext *ctx, ASTNode *node, Token t);
CallArgs parse_call_args(ParserContext *ctx, Lexer *l, FuncSig *sig);
/**
 * Rejects an argument whose type cannot stand for the parameter's, where the C
 * compiler would not notice or would only report it in the generated code:
 * - an opaque alias is a distinct type outside the file that defines it:
 *   another alias where one is expected, or the underlying type, or an alias
 *   where its underlying type is expected, as `let` does;
 * - two instances of one generic with different arguments (`Handle<A>` for
 *   `Handle<B>`).
 */
void check_argument_type(ParserContext *ctx, Type *expected, ASTNode *arg);
void check_format_string(ASTNode *call, Token t);

// From expr_entry.c
void analyze_lambda_captures(ParserContext *ctx, ASTNode *lambda);

// From expr_literal.c
ASTNode *parse_int_literal(ParserContext *ctx, Token t);
ASTNode *parse_float_literal(Token t);
ASTNode *parse_string_literal(ParserContext *ctx, Token t);
ASTNode *parse_fstring_literal(ParserContext *ctx, Token t);
ASTNode *parse_char_literal(Token t);
ASTNode *parse_size_or_typeof(ParserContext *ctx, Lexer *l, Token tk, int is_typeof);
ASTNode *parse_intrinsic(ParserContext *ctx, Lexer *l);
ASTNode *parse_sizeof_expr(ParserContext *ctx, Lexer *l, Token sizeof_tk);
ASTNode *parse_typeof_expr(ParserContext *ctx, Lexer *l);

void get_struct_name(ParserContext *ctx, ASTNode *node, char **out_struct_name,
                     char **out_var_name);
ASTNode *find_function_definition(ParserContext *ctx, const char *name);
int type_is_unsigned(Type *t);
char *infer_printf_format(ParserContext *ctx, ASTNode **args, int ac);

// From expr_field.c
int is_comparison_op(const char *op);
Type *get_field_type(ParserContext *ctx, Type *struct_type, const char *field_name);
const char *get_operator_method(const char *op);

// From expr_prec.c references
void validate_named_arguments(Token call_token, const char *func_name, char **arg_names,
                              int args_provided, ASTNode *def);

ASTNode *parse_lambda(ParserContext *ctx, Lexer *l);
ASTNode *parse_arrow_lambda_single(ParserContext *ctx, Lexer *l, char *param_name,
                                   int default_capture_mode);
ASTNode *parse_arrow_lambda_multi(ParserContext *ctx, Lexer *l, char **param_names,
                                  Type **param_types, int count, int default_capture_mode);

#endif
