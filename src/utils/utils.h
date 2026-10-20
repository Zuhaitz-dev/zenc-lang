// SPDX-License-Identifier: MIT
#ifndef UTILS_UTILS_H
#define UTILS_UTILS_H

/**
 * @file utils/utils.h
 * @brief Utility functions declared in zprep.h but defined in utils.c.
 *
 * Files that need these functions should include this header directly
 * instead of relying on the transitive include through zprep.h.
 */

#include <stddef.h>

struct ParserContext;
struct CompilerConfig;
struct zarena;
struct ASTNode;

void register_trait(const char *name);
void clear_registered_traits(void);
int is_trait(const char *name);
char *z_resolve_path(const char *fn, const char *relative_to, struct CompilerConfig *cfg);
char *load_file(const char *fn, const char *relative_to, struct CompilerConfig *cfg);
char *sanitize_path_for_c_string(const char *path);
char *z_basename(const char *path);
char *z_strip_ext(const char *filename);
void append_flag(char *dest, size_t max_size, const char *prefix, const char *val);
/** Joins the NULL-terminated `parts` (or none, if NULL) with `sep` into `out`, which
 *  holds `size` bytes (size > 0). The result is cut to fit and always terminated. */
void str_join(char *out, size_t size, const char *const *parts, const char *sep);
void scan_build_directives(struct ParserContext *ctx, const char *src);
int resolve_build_directive(const char *raw, char *out, size_t out_size);
int is_safe_pkg_config_spec(const char *libs);
int levenshtein(const char *s1, const char *s2);
void load_all_configs(struct CompilerConfig *cfg);

#endif // UTILS_UTILS_H
