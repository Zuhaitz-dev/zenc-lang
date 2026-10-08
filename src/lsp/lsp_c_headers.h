// SPDX-License-Identifier: MIT
#ifndef LSP_C_HEADERS_H
#ifndef ZC_ALLOW_INTERNAL
#error "lsp/lsp_c_headers.h is internal to Zen C. Include the appropriate public header instead."
#endif

#define LSP_C_HEADERS_H

#include "../constants.h"

struct cJSON;

/**
 * @brief A declaration found in a C header: function, macro, type, tag, enum value or
 * extern variable.
 *
 * Owned by the header cache and valid until the header changes on disk, so it must not be
 * kept across requests.
 */
typedef struct CHeaderSymbol
{
    const char *path; ///< Path of the header.
    int line;         ///< 0-based line of the name.
    int column;       ///< 0-based column of the name.
    int length;       ///< Length of the name.
    int start_line;   ///< First line of the whole declaration.
    int end_line;     ///< Last line of the whole declaration.
} CHeaderSymbol;

/**
 * @brief The `import "x.h" [as alias]` (or `include`) a C header comes from.
 */
typedef struct CHeaderOrigin
{
    char header[MAX_PATH_LEN];      ///< Header as written (`raylib.h`).
    char alias[MAX_VAR_NAME_LEN];   ///< Alias of the import, empty without one.
    char path[MAX_PATH_LEN];        ///< Resolved path, empty when it is not found.
    char found_by[MAX_PATH_LEN];    ///< What made the search find it (e.g. a directive).
    char declared_in[MAX_PATH_LEN]; ///< File with the import line.
    char statement[512];            ///< The import line itself.
    int line;                       ///< 0-based line of the import.
    int alias_column;               ///< Column of the alias, -1 without alias.
    int alias_length;
    int name_column;  ///< Column of the header name.
    int is_re_export; ///< Re-exported by an imported module with `export import`.
} CHeaderOrigin;

typedef enum
{
    C_HEADER_NONE = 0,
    C_HEADER_ALIAS_USE,         ///< `alias` in `alias::name`.
    C_HEADER_ALIAS_DECLARATION, ///< The alias in its own `import "x.h" as alias` line.
    C_HEADER_NAME               ///< The header name in an import or include line.
} CHeaderOriginKind;

/**
 * @brief Finds the C declarations of the symbol at (`line`, `col`) of a document.
 *
 * Searches the headers the document imports (`import "x.h" [as alias]`, `include <x.h>`,
 * `include "x.h"`) and then the ones its imported modules re-export
 * (`export import "x.h" as alias`); `alias::Name` only searches the headers imported with
 * that alias. Headers are looked up where the C compiler will look for them when building
 * the generated code: `//> include:` and `//> pkg-config:` directives, the configured
 * include paths and the compiler's default directories.
 *
 * @param via When the header is re-exported by another module, receives that `export
 *        import`; otherwise its `is_re_export` is 0. May be NULL.
 * @return Number of declarations written to `results`, at most `max_results`.
 */
int lsp_c_headers_find_at(const char *document_path, const char *source, int line, int col,
                          const CHeaderSymbol **results, int max_results, CHeaderOrigin *via);

/**
 * @brief Whether (`line`, `col`) is on the name of an `extern fn` declaration, whose C
 * declaration is more relevant there than the Zen C one.
 */
int lsp_c_headers_is_extern_name(const char *source, int line, int col);

/**
 * @brief Recognizes a C header alias or header name at (`line`, `col`) and fills `origin`.
 */
CHeaderOriginKind lsp_c_headers_origin_at(const char *document_path, const char *source, int line,
                                          int col, CHeaderOrigin *origin);

/**
 * @brief Markdown for hovering a C declaration: its source, its documentation comment, where
 * it is and, when re-exported, through which module. Allocated with libc_malloc(); release
 * it with libc_free().
 */
char *lsp_c_headers_hover(const CHeaderSymbol *symbol, const CHeaderOrigin *via);

/**
 * @brief Markdown for hovering a header alias or name. Release it with libc_free().
 */
char *lsp_c_headers_origin_hover(const CHeaderOrigin *origin, CHeaderOriginKind kind);

/**
 * @brief LSP `Location[]` pointing at the names of the given declarations.
 */
struct cJSON *lsp_c_headers_locations(const CHeaderSymbol **symbols, int count);

/**
 * @brief Go to definition on a header alias or name: the alias goes to the import that
 * declares it, the declaration and the name go to the header. With `link_support`, a
 * `LocationLink[]` whose origin is the whole header name, which the client's notion of a word
 * would split at the dot; otherwise a `Location`. NULL when the header is not found.
 */
struct cJSON *lsp_c_headers_origin_location(const CHeaderOrigin *origin, CHeaderOriginKind kind,
                                            int link_support);

/**
 * @brief Range of the header name or alias under the cursor, for the hover to highlight it
 * whole. NULL for an alias in `alias::name`, which is a plain word.
 */
struct cJSON *lsp_c_headers_origin_range(const CHeaderOrigin *origin, CHeaderOriginKind kind);

/**
 * @brief Adds a warning for every C header that the document's imported modules need and
 * that is only found through a directive of one of those modules: `zc build` only applies
 * the directives of the file it compiles.
 */
void lsp_c_headers_add_diagnostics(const char *document_path, const char *source,
                                   struct cJSON *diagnostics);

#endif
