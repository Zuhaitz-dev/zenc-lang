// SPDX-License-Identifier: MIT
#ifndef LSP_C_HEADERS_H
#ifndef ZC_ALLOW_INTERNAL
#error "lsp/lsp_c_headers.h is internal to Zen C. Include the appropriate public header instead."
#endif

#define LSP_C_HEADERS_H

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
 * @brief Finds the C declarations of the symbol at (`line`, `col`) of a document.
 *
 * Only the headers the document imports are searched (`import "x.h" [as alias]`,
 * `include <x.h>`, `include "x.h"`), and `alias::Name` only searches the headers imported
 * with that alias. Headers are looked up where the C compiler will look for them when
 * building the generated code: the document's `//> include:` and `//> pkg-config:`
 * directives, the configured include paths and the compiler's default directories.
 *
 * @return Number of declarations written to `results`, at most `max_results`.
 */
int lsp_c_headers_find_at(const char *document_path, const char *source, int line, int col,
                          const CHeaderSymbol **results, int max_results);

/**
 * @brief Markdown for hovering a C declaration: its source, its documentation comment and
 * where it is. Allocated with libc_malloc(); release it with libc_free().
 */
char *lsp_c_headers_hover(const CHeaderSymbol *symbol);

/**
 * @brief LSP `Location[]` pointing at the names of the given declarations.
 */
struct cJSON *lsp_c_headers_locations(const CHeaderSymbol **symbols, int count);

#endif
