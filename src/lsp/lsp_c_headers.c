// SPDX-License-Identifier: MIT
#include "lsp_c_headers.h"
#include "lsp_project.h"
#include "../utils/cJSON.h"
#include "../utils/string_list.h"
#include "../utils/utils.h"
#include "../constants.h"
#include "../platform/os.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// libc heap, not the arena: lookups run in read-only requests, which rewind the arena.

#define MAX_INCLUDE_DEPTH 16
#define MAX_HOVER_LINES 24
#define MAX_DOC_LENGTH 2048
#define COMPILER_OUTPUT_SIZE 65536

/* --- libc-backed helpers --- */

static char *copy_text(const char *text, size_t length)
{
    char *copy = libc_malloc(length + 1);
    if (copy)
    {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

// Copies `text[0..length)` into `out` without surrounding whitespace.
static void copy_trimmed(const char *text, size_t length, char *out, size_t out_size)
{
    while (length > 0 && isspace((unsigned char)*text))
    {
        text++;
        length--;
    }
    while (length > 0 && isspace((unsigned char)text[length - 1]))
    {
        length--;
    }
    if (length >= out_size)
    {
        length = out_size - 1;
    }
    memcpy(out, text, length);
    out[length] = '\0';
}

static int is_identifier_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static int is_regular_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// Directory part of `path`, or an empty string when it has none.
static void directory_of(const char *path, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s", path);
    char *separator = z_path_last_sep(out);
    if (separator)
    {
        *separator = '\0';
    }
    else
    {
        out[0] = '\0';
    }
}

static void join_path(const char *directory, const char *name, char *out, size_t out_size)
{
    if (!directory[0] || z_is_abs_path(name))
    {
        snprintf(out, out_size, "%s", name);
    }
    else
    {
        snprintf(out, out_size, "%s/%s", directory, name);
    }
}

/* --- Where to look for headers --- */

static StringList g_compiler_dirs;
static int g_compiler_dirs_probed = 0;

// Asks the configured C compiler for its default `#include <...>` directories: the ones it
// will search when compiling the generated C. Done once per process.
static void probe_compiler_dirs(void)
{
    if (g_compiler_dirs_probed)
    {
        return;
    }
    g_compiler_dirs_probed = 1;

    // The configured compiler may carry its own arguments (e.g. "zig cc").
    char compiler[sizeof(g_config.cc)];
    snprintf(compiler, sizeof(compiler), "%s", g_config.cc[0] ? g_config.cc : "cc");
    char *argv[24];
    int argc = 0;
    for (char *word = strtok(compiler, " \t"); word && argc < 16; word = strtok(NULL, " \t"))
    {
        argv[argc++] = word;
    }
    if (argc == 0)
    {
        return;
    }
#if ZC_OS_WINDOWS
    char null_device[] = "NUL";
#else
    char null_device[] = "/dev/null";
#endif
    char preprocess_only[] = "-E";
    char language_flag[] = "-x";
    char language[] = "c";
    char verbose[] = "-v";
    argv[argc++] = preprocess_only;
    argv[argc++] = language_flag;
    argv[argc++] = language;
    argv[argc++] = verbose;
    argv[argc++] = null_device;
    argv[argc] = NULL;

    char *output = libc_malloc(COMPILER_OUTPUT_SIZE);
    if (!output)
    {
        return;
    }
    int status = z_run_command_capture_ex(argv, output, COMPILER_OUTPUT_SIZE, 1);

    const char *marker = strstr(output, "#include <...> search starts here:");
    const char *line_start = marker ? strchr(marker, '\n') : NULL;
    while (line_start)
    {
        line_start++;
        const char *line_end = strchr(line_start, '\n');
        size_t length = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        char entry[MAX_PATH_LEN];
        copy_trimmed(line_start, length, entry, sizeof(entry));
        if (strncmp(entry, "End of search list.", 19) == 0)
        {
            break;
        }
        if (!strstr(entry, "(framework directory)"))
        {
            string_list_add(&g_compiler_dirs, entry);
        }
        line_start = line_end;
    }

    fprintf(stderr, "zls: C compiler '%s' (exit %d) searches %d include directories\n", g_config.cc,
            status, g_compiler_dirs.count);
    for (int i = 0; i < g_compiler_dirs.count; i++)
    {
        fprintf(stderr, "zls:   %s\n", g_compiler_dirs.items[i]);
    }
    libc_free(output);
}

typedef struct PkgConfigEntry
{
    char *spec;
    StringList dirs;
    struct PkgConfigEntry *next;
} PkgConfigEntry;

static PkgConfigEntry *g_pkg_config_entries = NULL;

// Include directories reported by `pkg-config --cflags-only-I`, cached per argument list.
static const StringList *pkg_config_dirs(const char *spec)
{
    for (PkgConfigEntry *entry = g_pkg_config_entries; entry; entry = entry->next)
    {
        if (strcmp(entry->spec, spec) == 0)
        {
            return &entry->dirs;
        }
    }

    PkgConfigEntry *entry = libc_malloc(sizeof(PkgConfigEntry));
    char *spec_copy = copy_text(spec, strlen(spec));
    if (!entry || !spec_copy)
    {
        libc_free(entry);
        libc_free(spec_copy);
        return NULL;
    }
    memset(entry, 0, sizeof(PkgConfigEntry));
    entry->spec = spec_copy;
    entry->next = g_pkg_config_entries;
    g_pkg_config_entries = entry;

    if (!is_safe_pkg_config_spec(spec))
    {
        fprintf(stderr, "zls: pkg-config '%s' ignored: invalid characters\n", spec);
        return &entry->dirs;
    }

    char arguments[1024];
    snprintf(arguments, sizeof(arguments), "%s", spec);
    char program[] = "pkg-config";
    char only_include_flags[] = "--cflags-only-I";
    char *argv[40];
    int argc = 0;
    argv[argc++] = program;
    argv[argc++] = only_include_flags;
    for (char *word = strtok(arguments, " "); word && argc < 38; word = strtok(NULL, " "))
    {
        argv[argc++] = word;
    }
    argv[argc] = NULL;

    char output[4096];
    if (z_run_command_capture_ex(argv, output, sizeof(output), 0) == 0)
    {
        for (char *word = strtok(output, " \t\r\n"); word; word = strtok(NULL, " \t\r\n"))
        {
            if (strncmp(word, "-I", 2) == 0)
            {
                string_list_add(&entry->dirs, word + 2);
            }
        }
    }
    fprintf(stderr, "zls: pkg-config %s: %d include directories\n", spec, entry->dirs.count);
    return &entry->dirs;
}

/* --- Search directories --- */

#define MAX_MODULE_DEPTH 8

static char *read_file(const char *path);

typedef struct
{
    char *path;
    char *origin; ///< What made it a search directory, for hover.
} SearchDir;

typedef struct
{
    SearchDir *items;
    int count;
    int capacity;
} SearchDirList;

static void search_dirs_add(SearchDirList *list, const char *path, const char *origin)
{
    if (!path[0])
    {
        return;
    }
    for (int i = 0; i < list->count; i++)
    {
        if (strcmp(list->items[i].path, path) == 0)
        {
            return;
        }
    }
    if (list->count == list->capacity)
    {
        int capacity = list->capacity ? list->capacity * 2 : 8;
        SearchDir *items = libc_realloc(list->items, (size_t)capacity * sizeof(SearchDir));
        if (!items)
        {
            return;
        }
        list->items = items;
        list->capacity = capacity;
    }
    char *path_copy = copy_text(path, strlen(path));
    char *origin_copy = copy_text(origin, strlen(origin));
    if (!path_copy || !origin_copy)
    {
        libc_free(path_copy);
        libc_free(origin_copy);
        return;
    }
    list->items[list->count].path = path_copy;
    list->items[list->count].origin = origin_copy;
    list->count++;
}

static void search_dirs_append(SearchDirList *list, const SearchDirList *other)
{
    for (int i = 0; i < other->count; i++)
    {
        search_dirs_add(list, other->items[i].path, other->items[i].origin);
    }
}

static void search_dirs_free(SearchDirList *list)
{
    for (int i = 0; i < list->count; i++)
    {
        libc_free(list->items[i].path);
        libc_free(list->items[i].origin);
    }
    libc_free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

// Name of a file without its directories, for messages.
static const char *file_label(const char *path)
{
    const char *separator = z_path_last_sep(path);
    return separator ? separator + 1 : path;
}

// Directories named by `//> include:` and `//> pkg-config:` directives in `source`.
// Relative include paths are taken from the project root, which is where `zc build`
// normally runs.
static void collect_directive_dirs(const char *source, const char *file_path, SearchDirList *dirs)
{
    char file_dir[MAX_PATH_LEN];
    directory_of(file_path, file_dir, sizeof(file_dir));
    const char *base_dir = (g_project && g_project->root_path) ? g_project->root_path : file_dir;

    const char *line_start = source;
    while (*line_start)
    {
        const char *line_end = strchr(line_start, '\n');
        size_t length = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        const char *p = line_start;
        while (p < line_start + length && isspace((unsigned char)*p))
        {
            p++;
        }

        if ((size_t)(line_start + length - p) >= 3 && strncmp(p, "//>", 3) == 0)
        {
            char raw[2048];
            copy_trimmed(p + 3, (size_t)(line_start + length - (p + 3)), raw, sizeof(raw));
            char directive[2048];
            if (resolve_build_directive(raw, directive, sizeof(directive)))
            {
                char origin[MAX_PATH_LEN];
                snprintf(origin, sizeof(origin), "`//> %s` in %s", directive,
                         file_label(file_path));
                if (strncmp(directive, "include:", 8) == 0)
                {
                    for (char *word = strtok(directive + 8, " \t"); word;
                         word = strtok(NULL, " \t"))
                    {
                        char path[MAX_PATH_LEN];
                        join_path(base_dir, word, path, sizeof(path));
                        search_dirs_add(dirs, path, origin);
                    }
                }
                else if (strncmp(directive, "pkg-config:", 11) == 0)
                {
                    char spec[1024];
                    copy_trimmed(directive + 11, strlen(directive + 11), spec, sizeof(spec));
                    const StringList *found = pkg_config_dirs(spec);
                    for (int i = 0; found && i < found->count; i++)
                    {
                        search_dirs_add(dirs, found->items[i], origin);
                    }
                }
            }
        }

        if (!line_end)
        {
            break;
        }
        line_start = line_end + 1;
    }
}

// Where the C compiler will look for headers when building `file_path`: its directives,
// the configured include paths and the compiler's own directories, in the order the
// compiler searches them.
static void collect_search_dirs(const char *source, const char *file_path, SearchDirList *dirs)
{
    collect_directive_dirs(source, file_path, dirs);
    for (size_t i = 0; i < g_config.include_paths.length; i++)
    {
        search_dirs_add(dirs, g_config.include_paths.data[i], "the configured include paths");
    }
    probe_compiler_dirs();
    char origin[MAX_PATH_LEN];
    snprintf(origin, sizeof(origin), "the default directories of the C compiler (`%s`)",
             g_config.cc);
    for (int i = 0; i < g_compiler_dirs.count; i++)
    {
        search_dirs_add(dirs, g_compiler_dirs.items[i], origin);
    }
}

// Resolves a header like the C preprocessor: `local_dir` first for `"x.h"` (the directory
// of the file that includes it, NULL for `<x.h>`), then the search directories. Returns a
// libc string or NULL, and what found it in `found_by` when given.
static char *find_header(const char *name, const char *local_dir, const SearchDirList *dirs,
                         char *found_by, size_t found_by_size)
{
    char path[MAX_PATH_LEN];
    const char *origin = NULL;
    if (z_is_abs_path(name))
    {
        if (is_regular_file(name))
        {
            snprintf(path, sizeof(path), "%s", name);
            origin = "its absolute path";
        }
    }
    else
    {
        if (local_dir && local_dir[0])
        {
            join_path(local_dir, name, path, sizeof(path));
            if (is_regular_file(path))
            {
                origin = "the directory of the file that imports it";
            }
        }
        for (int i = 0; !origin && i < dirs->count; i++)
        {
            join_path(dirs->items[i].path, name, path, sizeof(path));
            if (is_regular_file(path))
            {
                origin = dirs->items[i].origin;
            }
        }
    }
    if (!origin)
    {
        return NULL;
    }
    if (found_by)
    {
        snprintf(found_by, found_by_size, "%s", origin);
    }
    return copy_text(path, strlen(path));
}

/* --- Imports of a file --- */

typedef struct
{
    char name[MAX_PATH_LEN];
    char alias[MAX_VAR_NAME_LEN]; ///< Empty when imported without `as` or with `as *`.
    int is_system;                ///< `include <x.h>`.
    int is_export;                ///< `export import`.
    int line;                     ///< 0-based line of the import.
    int alias_column;             ///< Column of the alias, -1 without alias.
    int name_column;              ///< Column of the header name.
} HeaderImport;

typedef struct
{
    char name[MAX_PATH_LEN];
    int is_export;
    int line;
    int name_column;
} ModuleImport;

typedef struct
{
    HeaderImport *headers;
    int header_count;
    int header_capacity;
    ModuleImport *modules;
    int module_count;
    int module_capacity;
} FileImports;

static HeaderImport *add_header_import(FileImports *imports)
{
    if (imports->header_count == imports->header_capacity)
    {
        int capacity = imports->header_capacity ? imports->header_capacity * 2 : 4;
        HeaderImport *headers =
            libc_realloc(imports->headers, (size_t)capacity * sizeof(HeaderImport));
        if (!headers)
        {
            return NULL;
        }
        imports->headers = headers;
        imports->header_capacity = capacity;
    }
    HeaderImport *header = &imports->headers[imports->header_count];
    memset(header, 0, sizeof(HeaderImport));
    header->alias_column = -1;
    return header;
}

static ModuleImport *add_module_import(FileImports *imports)
{
    if (imports->module_count == imports->module_capacity)
    {
        int capacity = imports->module_capacity ? imports->module_capacity * 2 : 4;
        ModuleImport *modules =
            libc_realloc(imports->modules, (size_t)capacity * sizeof(ModuleImport));
        if (!modules)
        {
            return NULL;
        }
        imports->modules = modules;
        imports->module_capacity = capacity;
    }
    ModuleImport *module = &imports->modules[imports->module_count];
    memset(module, 0, sizeof(ModuleImport));
    return module;
}

static void free_imports(FileImports *imports)
{
    libc_free(imports->headers);
    libc_free(imports->modules);
    memset(imports, 0, sizeof(FileImports));
}

static const char *skip_blanks(const char *p)
{
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    return p;
}

static int starts_with_word(const char *p, const char *word)
{
    size_t length = strlen(word);
    return strncmp(p, word, length) == 0 && (p[length] == ' ' || p[length] == '\t');
}

// Copies the text up to `close` (on the same line) into `out`, and returns the position
// after `close`, or NULL when it is missing.
static const char *read_until(const char *p, char close, char *out, size_t out_size)
{
    size_t length = 0;
    while (p[length] && p[length] != close && p[length] != '\n')
    {
        length++;
    }
    if (p[length] != close || length == 0 || length >= out_size)
    {
        return NULL;
    }
    memcpy(out, p, length);
    out[length] = '\0';
    return p + length + 1;
}

static int has_extension(const char *name, const char *extension)
{
    size_t length = strlen(name);
    size_t extension_length = strlen(extension);
    return length > extension_length && strcmp(name + length - extension_length, extension) == 0;
}

// The `import "x.h" [as alias]`, `include <x.h>`/`include "x.h"` and `import "x.zc"` lines
// of a file, with their positions.
static void collect_imports(const char *source, FileImports *imports)
{
    int line = 0;
    const char *line_start = source;
    while (*line_start)
    {
        const char *p = skip_blanks(line_start);
        int is_export = starts_with_word(p, "export");
        if (is_export)
        {
            p = skip_blanks(p + 6);
        }

        char name[MAX_PATH_LEN];
        if (starts_with_word(p, "import"))
        {
            p = skip_blanks(p + 6);
            const char *name_start = p + 1;
            const char *after = *p == '"' ? read_until(name_start, '"', name, sizeof(name)) : NULL;
            if (after && has_extension(name, ".h"))
            {
                HeaderImport *header = add_header_import(imports);
                if (header)
                {
                    snprintf(header->name, sizeof(header->name), "%s", name);
                    header->is_export = is_export;
                    header->line = line;
                    header->name_column = (int)(name_start - line_start);
                    const char *q = skip_blanks(after);
                    if (starts_with_word(q, "as"))
                    {
                        q = skip_blanks(q + 2);
                        size_t length = 0;
                        while (is_identifier_char(q[length]))
                        {
                            length++;
                        }
                        if (length > 0 && length < sizeof(header->alias))
                        {
                            memcpy(header->alias, q, length);
                            header->alias[length] = '\0';
                            header->alias_column = (int)(q - line_start);
                        }
                    }
                    imports->header_count++;
                }
            }
            else if (after && has_extension(name, ".zc"))
            {
                ModuleImport *module = add_module_import(imports);
                if (module)
                {
                    snprintf(module->name, sizeof(module->name), "%s", name);
                    module->is_export = is_export;
                    module->line = line;
                    module->name_column = (int)(name_start - line_start);
                    imports->module_count++;
                }
            }
        }
        else if (!is_export && starts_with_word(p, "include"))
        {
            p = skip_blanks(p + 7);
            if (*p == '<' || *p == '"')
            {
                char close = *p == '<' ? '>' : '"';
                if (read_until(p + 1, close, name, sizeof(name)))
                {
                    HeaderImport *header = add_header_import(imports);
                    if (header)
                    {
                        snprintf(header->name, sizeof(header->name), "%s", name);
                        header->is_system = close == '>';
                        header->line = line;
                        header->name_column = (int)(p + 1 - line_start);
                        imports->header_count++;
                    }
                }
            }
        }

        const char *line_end = strchr(line_start, '\n');
        if (!line_end)
        {
            break;
        }
        line_start = line_end + 1;
        line++;
    }
}

// The text of `line` in `source`, trimmed, into `out`.
static void source_line(const char *source, int line, char *out, size_t out_size)
{
    const char *p = source;
    for (int i = 0; i < line && p; i++)
    {
        p = strchr(p, '\n');
        if (p)
        {
            p++;
        }
    }
    if (!p)
    {
        out[0] = '\0';
        return;
    }
    const char *end = strchr(p, '\n');
    copy_trimmed(p, end ? (size_t)(end - p) : strlen(p), out, out_size);
}

/* --- Modules imported by a file --- */

// Resolves `import "x.zc"` next to the importing file, then from the project root.
static char *find_module_file(const char *name, const char *from_dir)
{
    char path[MAX_PATH_LEN];
    if (z_is_abs_path(name))
    {
        return is_regular_file(name) ? copy_text(name, strlen(name)) : NULL;
    }
    join_path(from_dir, name, path, sizeof(path));
    if (is_regular_file(path))
    {
        return copy_text(path, strlen(path));
    }
    if (g_project && g_project->root_path)
    {
        join_path(g_project->root_path, name, path, sizeof(path));
        if (is_regular_file(path))
        {
            return copy_text(path, strlen(path));
        }
    }
    return NULL;
}

// Text of a module: the client's version when it is open, otherwise the file on disk.
static char *module_text(const char *path)
{
    char uri[MAX_PATH_LEN + 8];
    snprintf(uri, sizeof(uri), "file://%s", path);
    ProjectFile *file = lsp_project_get_file(uri);
    if (file && file->source)
    {
        return copy_text(file->source, strlen(file->source));
    }
    return read_file(path);
}

typedef struct
{
    const ModuleImport *top; ///< The document's own import that leads to the module.
    const char *module_path;
    const char *module_source;
    const HeaderImport *header;
} ModuleHeader;

typedef int (*ModuleHeaderVisitor)(const ModuleHeader *entry, void *data);

// Visits the C headers imported by the modules that `source` imports, depth first. With
// `only_reexports`, only what they re-export: `export import "x.h"`, through modules they
// `export import` themselves. Stops when `visit` returns non-zero, and returns that value.
static int walk_module_headers(const char *source, const char *file_path, const ModuleImport *top,
                               int only_reexports, int depth, StringList *visited,
                               ModuleHeaderVisitor visit, void *data)
{
    if (depth > MAX_MODULE_DEPTH)
    {
        return 0;
    }
    FileImports imports = {0};
    collect_imports(source, &imports);
    char file_dir[MAX_PATH_LEN];
    directory_of(file_path, file_dir, sizeof(file_dir));

    int stop = 0;
    for (int i = 0; i < imports.module_count && !stop; i++)
    {
        const ModuleImport *module = &imports.modules[i];
        if (only_reexports && depth > 0 && !module->is_export)
        {
            continue;
        }
        char *path = find_module_file(module->name, file_dir);
        if (!path || string_list_contains(visited, path))
        {
            libc_free(path);
            continue;
        }
        string_list_add(visited, path);

        char *text = module_text(path);
        if (text)
        {
            const ModuleImport *entry_top = top ? top : module;
            FileImports module_imports = {0};
            collect_imports(text, &module_imports);
            for (int h = 0; h < module_imports.header_count && !stop; h++)
            {
                if (only_reexports && !module_imports.headers[h].is_export)
                {
                    continue;
                }
                ModuleHeader entry = {entry_top, path, text, &module_imports.headers[h]};
                stop = visit(&entry, data);
            }
            free_imports(&module_imports);
            if (!stop)
            {
                stop = walk_module_headers(text, path, entry_top, only_reexports, depth + 1,
                                           visited, visit, data);
            }
            libc_free(text);
        }
        libc_free(path);
    }
    free_imports(&imports);
    return stop;
}

/* --- Symbol under the cursor --- */

typedef struct
{
    char name[MAX_VAR_NAME_LEN];
    char qualifier[MAX_VAR_NAME_LEN]; ///< `alias` of `alias::name`, or empty.
    int is_qualifier;                 ///< The cursor is on `alias` itself.
    int column;                       ///< Column where the identifier starts.
    const char *line_start;
} CursorSymbol;

static int symbol_at(const char *source, int line, int col, CursorSymbol *cursor)
{
    const char *p = source;
    for (int i = 0; i < line && p; i++)
    {
        p = strchr(p, '\n');
        if (p)
        {
            p++;
        }
    }
    if (!p || col < 0)
    {
        return 0;
    }
    const char *line_end = strchr(p, '\n');
    int length = line_end ? (int)(line_end - p) : (int)strlen(p);
    if (col > length)
    {
        col = length;
    }

    int start = col;
    int end = col;
    while (start > 0 && is_identifier_char(p[start - 1]))
    {
        start--;
    }
    while (end < length && is_identifier_char(p[end]))
    {
        end++;
    }
    if (start == end || isdigit((unsigned char)p[start]) ||
        (size_t)(end - start) >= sizeof(cursor->name))
    {
        return 0;
    }
    memcpy(cursor->name, p + start, (size_t)(end - start));
    cursor->name[end - start] = '\0';
    cursor->column = start;
    cursor->line_start = p;
    cursor->is_qualifier = end + 1 < length && p[end] == ':' && p[end + 1] == ':';

    cursor->qualifier[0] = '\0';
    if (start >= 2 && p[start - 1] == ':' && p[start - 2] == ':')
    {
        int qualifier_end = start - 2;
        int qualifier_start = qualifier_end;
        while (qualifier_start > 0 && is_identifier_char(p[qualifier_start - 1]))
        {
            qualifier_start--;
        }
        size_t qualifier_length = (size_t)(qualifier_end - qualifier_start);
        if (qualifier_length > 0 && qualifier_length < sizeof(cursor->qualifier))
        {
            memcpy(cursor->qualifier, p + qualifier_start, qualifier_length);
            cursor->qualifier[qualifier_length] = '\0';
        }
    }
    return 1;
}

/* --- Header cache --- */

typedef struct
{
    CHeaderSymbol symbol;
    char *name;
    int is_forward; ///< `struct X;` without a body.
} HeaderSymbol;

typedef struct
{
    char *name;
    int is_system;
} HeaderInclude;

typedef struct HeaderFile
{
    char *path;
    long long modified;
    long long size;
    char *text;
    int *line_offsets;
    int line_count;
    HeaderSymbol *symbols;
    int symbol_count;
    int symbol_capacity;
    HeaderInclude *includes;
    int include_count;
    int include_capacity;
    struct HeaderFile *next;
} HeaderFile;

static HeaderFile *g_headers = NULL;

static int line_of(const HeaderFile *header, int offset)
{
    int low = 0;
    int high = header->line_count - 1;
    while (low < high)
    {
        int middle = (low + high + 1) / 2;
        if (header->line_offsets[middle] <= offset)
        {
            low = middle;
        }
        else
        {
            high = middle - 1;
        }
    }
    return low;
}

static void add_header_symbol(HeaderFile *header, const char *name, int name_length, int offset,
                              int start_line, int end_line, int is_forward)
{
    if (header->symbol_count == header->symbol_capacity)
    {
        int capacity = header->symbol_capacity ? header->symbol_capacity * 2 : 64;
        HeaderSymbol *symbols =
            libc_realloc(header->symbols, (size_t)capacity * sizeof(HeaderSymbol));
        if (!symbols)
        {
            return;
        }
        header->symbols = symbols;
        header->symbol_capacity = capacity;
    }
    char *copy = copy_text(name, (size_t)name_length);
    if (!copy)
    {
        return;
    }
    HeaderSymbol *entry = &header->symbols[header->symbol_count++];
    entry->name = copy;
    entry->is_forward = is_forward;
    entry->symbol.path = header->path;
    entry->symbol.line = line_of(header, offset);
    entry->symbol.column = offset - header->line_offsets[entry->symbol.line];
    entry->symbol.length = name_length;
    entry->symbol.start_line = start_line;
    entry->symbol.end_line = end_line;
}

static void add_include(HeaderFile *header, const char *name, int is_system)
{
    if (header->include_count == header->include_capacity)
    {
        int capacity = header->include_capacity ? header->include_capacity * 2 : 8;
        HeaderInclude *includes =
            libc_realloc(header->includes, (size_t)capacity * sizeof(HeaderInclude));
        if (!includes)
        {
            return;
        }
        header->includes = includes;
        header->include_capacity = capacity;
    }
    char *copy = copy_text(name, strlen(name));
    if (copy)
    {
        header->includes[header->include_count].name = copy;
        header->includes[header->include_count].is_system = is_system;
        header->include_count++;
    }
}

/* --- Declaration scanner ---
 *
 * Not a C preprocessor: it reads the header line by line, skipping comments, and splits
 * the code into top-level statements (up to `;`, or up to the closing brace of a function
 * body). Every `#if` branch is read, so a name can be declared more than once. */

typedef struct
{
    char *chars;
    int *offsets; ///< Offset in the header text of each character.
    int length;
    int capacity;
} Statement;

static void statement_push(Statement *statement, char c, int offset)
{
    if (c == ' ' && (statement->length == 0 || statement->chars[statement->length - 1] == ' '))
    {
        return;
    }
    if (statement->length == statement->capacity)
    {
        int capacity = statement->capacity ? statement->capacity * 2 : 256;
        char *chars = libc_realloc(statement->chars, (size_t)capacity);
        if (!chars)
        {
            return;
        }
        statement->chars = chars;
        int *offsets = libc_realloc(statement->offsets, (size_t)capacity * sizeof(int));
        if (!offsets)
        {
            return;
        }
        statement->offsets = offsets;
        statement->capacity = capacity;
    }
    statement->chars[statement->length] = c;
    statement->offsets[statement->length] = offset;
    statement->length++;
}

typedef struct
{
    int start; ///< Index in the statement.
    int length;
    char kind; ///< 'w' word, 'n' number, 's' string literal, or the punctuation character.
    int brace_depth;
    int paren_depth;
} CToken;

static int tokenize(const Statement *statement, CToken *tokens)
{
    int count = 0;
    int brace_depth = 0;
    int paren_depth = 0;
    int i = 0;
    while (i < statement->length)
    {
        char c = statement->chars[i];
        if (c == ' ')
        {
            i++;
            continue;
        }

        CToken *token = &tokens[count++];
        token->start = i;
        token->brace_depth = brace_depth;
        token->paren_depth = paren_depth;
        if (isalpha((unsigned char)c) || c == '_')
        {
            int end = i;
            while (end < statement->length && is_identifier_char(statement->chars[end]))
            {
                end++;
            }
            token->kind = 'w';
            token->length = end - i;
            i = end;
        }
        else if (isdigit((unsigned char)c))
        {
            int end = i;
            while (end < statement->length &&
                   (is_identifier_char(statement->chars[end]) || statement->chars[end] == '.'))
            {
                end++;
            }
            token->kind = 'n';
            token->length = end - i;
            i = end;
        }
        else if (c == '"' || c == '\'')
        {
            int end = i + 1;
            while (end < statement->length && statement->chars[end] != c)
            {
                if (statement->chars[end] == '\\')
                {
                    end++;
                }
                end++;
            }
            token->kind = 's';
            token->length = (end < statement->length ? end + 1 : end) - i;
            i += token->length;
        }
        else
        {
            token->kind = c;
            token->length = 1;
            i++;
            if (c == '{')
            {
                brace_depth++;
            }
            else if (c == '}' && brace_depth > 0)
            {
                brace_depth--;
                token->brace_depth = brace_depth;
            }
            else if (c == '(')
            {
                paren_depth++;
            }
            else if (c == ')' && paren_depth > 0)
            {
                paren_depth--;
                token->paren_depth = paren_depth;
            }
        }
    }
    return count;
}

static int token_is(const Statement *statement, const CToken *token, const char *word)
{
    size_t length = strlen(word);
    return token->kind == 'w' && (size_t)token->length == length &&
           strncmp(statement->chars + token->start, word, length) == 0;
}

static int token_in(const Statement *statement, const CToken *token, const char *const *words)
{
    for (int i = 0; words[i]; i++)
    {
        if (token_is(statement, token, words[i]))
        {
            return 1;
        }
    }
    return 0;
}

static const char *const C_KEYWORDS[] = {
    "auto",          "break",     "case",       "char",
    "const",         "continue",  "default",    "do",
    "double",        "else",      "enum",       "extern",
    "float",         "for",       "goto",       "if",
    "inline",        "int",       "long",       "register",
    "restrict",      "return",    "short",      "signed",
    "sizeof",        "static",    "struct",     "switch",
    "typedef",       "union",     "unsigned",   "void",
    "volatile",      "while",     "_Bool",      "_Complex",
    "_Atomic",       "bool",      "__restrict", "__inline",
    "__extension__", "_Nullable", "_Nonnull",   "_Null_unspecified",
    "__nullable",    "__nonnull", NULL};

// Words followed by parentheses that are not the declared name.
static const char *const ATTRIBUTE_WORDS[] = {
    "__attribute__", "__attribute", "__declspec", "__asm__", "__asm",          "asm",
    "_Alignas",      "alignas",     "__typeof__", "typeof",  "_Static_assert", "static_assert",
    "_Pragma",       "__pragma",    "sizeof",     NULL};

static int is_name_token(const Statement *statement, const CToken *token)
{
    return token->kind == 'w' && !token_in(statement, token, C_KEYWORDS) &&
           !token_in(statement, token, ATTRIBUTE_WORDS);
}

static void record(HeaderFile *header, const Statement *statement, const CToken *token,
                   int start_line, int end_line, int is_forward)
{
    add_header_symbol(header, statement->chars + token->start, token->length,
                      statement->offsets[token->start], start_line, end_line, is_forward);
}

// First token of `kind` at the statement's top level (outside braces and parentheses).
static int find_top_level(const CToken *tokens, int count, int from, char kind)
{
    for (int i = from; i < count; i++)
    {
        if (tokens[i].kind == kind && tokens[i].brace_depth == 0 && tokens[i].paren_depth == 0)
        {
            return i;
        }
    }
    return -1;
}

// `enum { A, B = 2, C }`: every name right after the opening brace or a comma.
static void record_enumerators(HeaderFile *header, const Statement *statement, const CToken *tokens,
                               int count, int body)
{
    for (int i = body + 1; i < count; i++)
    {
        if (tokens[i].kind == '}' && tokens[i].brace_depth == 0)
        {
            return;
        }
        if (tokens[i].kind == 'w' && tokens[i].brace_depth == 1 && tokens[i].paren_depth == 0 &&
            (i == body + 1 || tokens[i - 1].kind == ','))
        {
            int line = line_of(header, statement->offsets[tokens[i].start]);
            record(header, statement, &tokens[i], line, line, 0);
        }
    }
}

// A function pointer `(*name)`: the name after the opening parenthesis at `open`.
static int function_pointer_name(const Statement *statement, const CToken *tokens, int count,
                                 int open)
{
    int i = open + 1;
    if (i >= count || (tokens[i].kind != '*' && tokens[i].kind != '^'))
    {
        return -1;
    }
    while (i < count && (tokens[i].kind == '*' || tokens[i].kind == '^' ||
                         token_in(statement, &tokens[i], C_KEYWORDS)))
    {
        i++;
    }
    return (i < count && tokens[i].kind == 'w') ? i : -1;
}

static void process_typedef(HeaderFile *header, const Statement *statement, const CToken *tokens,
                            int count, int start_line, int end_line)
{
    if (count < 2)
    {
        return;
    }
    int body = find_top_level(tokens, count, 0, '{');
    if (body >= 0)
    {
        // typedef struct Tag { ... } Name, *Pointer;
        int is_enum = token_is(statement, &tokens[1], "enum");
        if (body >= 3 && tokens[body - 1].kind == 'w' &&
            (is_enum || token_is(statement, &tokens[1], "struct") ||
             token_is(statement, &tokens[1], "union")))
        {
            record(header, statement, &tokens[body - 1], start_line, end_line, 0);
        }
        if (is_enum)
        {
            record_enumerators(header, statement, tokens, count, body);
        }
        int close = find_top_level(tokens, count, body + 1, '}');
        for (int i = close + 1; close >= 0 && i < count; i++)
        {
            if (tokens[i].paren_depth == 0 && is_name_token(statement, &tokens[i]))
            {
                record(header, statement, &tokens[i], start_line, end_line, 0);
            }
        }
        return;
    }

    // typedef void (*Name)(int);
    for (int i = 1; i < count; i++)
    {
        if (tokens[i].kind == '(' && tokens[i].paren_depth == 0)
        {
            int name = function_pointer_name(statement, tokens, count, i);
            if (name >= 0)
            {
                record(header, statement, &tokens[name], start_line, end_line, 0);
                return;
            }
        }
    }

    // typedef int Name(int);   typedef unsigned int Name;   typedef int Name[4];
    int last_name = -1;
    for (int i = 1; i < count; i++)
    {
        if (tokens[i].paren_depth > 0)
        {
            continue;
        }
        if (tokens[i].kind == '(' || tokens[i].kind == '[' || tokens[i].kind == ';')
        {
            break;
        }
        if (is_name_token(statement, &tokens[i]))
        {
            last_name = i;
        }
    }
    if (last_name >= 0)
    {
        record(header, statement, &tokens[last_name], start_line, end_line, 0);
    }
}

static void process_function_or_variable(HeaderFile *header, const Statement *statement,
                                         const CToken *tokens, int count, int start_line,
                                         int end_line)
{
    for (int i = 0; i < count; i++)
    {
        if (tokens[i].kind != '(' || tokens[i].brace_depth != 0 || tokens[i].paren_depth != 0)
        {
            continue;
        }
        // void (*callback)(int);
        int pointer_name = function_pointer_name(statement, tokens, count, i);
        if (pointer_name >= 0)
        {
            record(header, statement, &tokens[pointer_name], start_line, end_line, 0);
            return;
        }
        // RLAPI void InitWindow(int width, int height, const char *title);
        if (i > 0 && is_name_token(statement, &tokens[i - 1]))
        {
            record(header, statement, &tokens[i - 1], start_line, end_line, 0);
            return;
        }
        // `__attribute__((...))` and the like: the name comes later.
    }

    // Only `extern` variables are declarations worth recording.
    int is_extern = 0;
    int last_name = -1;
    for (int i = 0; i < count; i++)
    {
        if (tokens[i].brace_depth != 0 || tokens[i].paren_depth != 0)
        {
            continue;
        }
        if (token_is(statement, &tokens[i], "extern"))
        {
            is_extern = 1;
        }
        if (tokens[i].kind == '=' || tokens[i].kind == '[' || tokens[i].kind == ':' ||
            tokens[i].kind == ';')
        {
            break;
        }
        if (is_name_token(statement, &tokens[i]))
        {
            last_name = i;
        }
    }
    if (is_extern && last_name >= 0)
    {
        record(header, statement, &tokens[last_name], start_line, end_line, 0);
    }
}

// `__X` or `_X`: names reserved for the implementation, used by system header macros.
static int is_reserved_identifier(const char *name)
{
    return name[0] == '_' && (name[1] == '_' || isupper((unsigned char)name[1]));
}

// Skips what may precede the declaration itself: macro invocations without a semicolon,
// like `_LIBC_SINGLE_BY_DEFAULT()` (a declaration never starts with its name), and bare
// macros like `__BEGIN_DECLS` in front of `typedef`, `struct`, `union` or `enum`.
static int declaration_start(const Statement *statement, const CToken *tokens, int count)
{
    int start = 0;
    while (start + 1 < count && is_name_token(statement, &tokens[start]) &&
           tokens[start + 1].kind == '(')
    {
        int close = -1;
        for (int i = start + 2; i < count; i++)
        {
            if (tokens[i].kind == ')' && tokens[i].paren_depth == tokens[start + 1].paren_depth)
            {
                close = i;
                break;
            }
        }
        if (close < 0)
        {
            break;
        }
        start = close + 1;
    }

    for (int i = start; i < count && tokens[i].kind == 'w'; i++)
    {
        if (token_is(statement, &tokens[i], "typedef") ||
            token_is(statement, &tokens[i], "struct") || token_is(statement, &tokens[i], "union") ||
            token_is(statement, &tokens[i], "enum"))
        {
            return i;
        }
        if (!is_name_token(statement, &tokens[i]))
        {
            break;
        }
    }
    return start;
}

static void process_statement(HeaderFile *header, const Statement *statement)
{
    if (statement->length == 0)
    {
        return;
    }
    CToken *tokens = libc_malloc((size_t)statement->length * sizeof(CToken));
    if (!tokens)
    {
        return;
    }
    int all_count = tokenize(statement, tokens);
    int first_token = declaration_start(statement, tokens, all_count);
    const CToken *declaration = tokens + first_token;
    int count = all_count - first_token;
    if (count > 0)
    {
        // A reserved-name macro alone on a line above the declaration (`__BEGIN_DECLS`) is
        // not shown as part of it.
        int shown = 0;
        while (shown + 1 < count && declaration[shown].kind == 'w' &&
               is_reserved_identifier(statement->chars + declaration[shown].start) &&
               line_of(header, statement->offsets[declaration[shown].start]) <
                   line_of(header, statement->offsets[declaration[shown + 1].start]))
        {
            shown++;
        }
        int start_line = line_of(header, statement->offsets[declaration[shown].start]);
        int end_line = line_of(header, statement->offsets[statement->length - 1]);

        int first_paren = find_top_level(declaration, count, 0, '(');
        int body = find_top_level(declaration, count, 0, '{');
        if (token_is(statement, &declaration[0], "typedef"))
        {
            process_typedef(header, statement, declaration, count, start_line, end_line);
        }
        else if ((token_is(statement, &declaration[0], "struct") ||
                  token_is(statement, &declaration[0], "union") ||
                  token_is(statement, &declaration[0], "enum")) &&
                 (first_paren < 0 || (body >= 0 && body < first_paren)))
        {
            // struct Tag { ... };   enum { A, B };   struct Tag;
            if (count >= 2 && declaration[1].kind == 'w')
            {
                record(header, statement, &declaration[1], start_line, end_line, body < 0);
            }
            if (body >= 0 && token_is(statement, &declaration[0], "enum"))
            {
                record_enumerators(header, statement, declaration, count, body);
            }
        }
        else
        {
            process_function_or_variable(header, statement, declaration, count, start_line,
                                         end_line);
        }
    }
    libc_free(tokens);
}

// `extern "C" {`: a C++ linkage block, whose contents are top-level declarations.
static int is_extern_c_block(const Statement *statement)
{
    char compact[16];
    int length = 0;
    for (int i = 0; i < statement->length; i++)
    {
        if (statement->chars[i] == ' ')
        {
            continue;
        }
        if (length == (int)sizeof(compact) - 1)
        {
            return 0;
        }
        compact[length++] = statement->chars[i];
    }
    compact[length] = '\0';
    return strcmp(compact, "extern\"C\"{") == 0;
}

// Whether the body that just closed belongs to a function: `... )  { ... }`.
static int is_function_body(const Statement *statement)
{
    for (int i = 0; i < statement->length; i++)
    {
        if (statement->chars[i] == '{')
        {
            int j = i - 1;
            while (j >= 0 && statement->chars[j] == ' ')
            {
                j--;
            }
            return j >= 0 && statement->chars[j] == ')';
        }
    }
    return 0;
}

// Handles the preprocessor line starting at `offset` (`#define NAME`, `#include`) and
// returns the offset of the newline that ends it, continuation lines included.
static int scan_preprocessor_line(HeaderFile *header, int offset)
{
    const char *text = header->text;
    int end = offset;
    while (text[end] && !(text[end] == '\n' && (end == 0 || text[end - 1] != '\\')))
    {
        end++;
    }

    const char *p = skip_blanks(text + offset + 1);
    if (strncmp(p, "define", 6) == 0 && (p[6] == ' ' || p[6] == '\t'))
    {
        const char *name = skip_blanks(p + 6);
        int length = 0;
        while (is_identifier_char(name[length]))
        {
            length++;
        }
        if (length > 0)
        {
            int start_line = line_of(header, offset);
            add_header_symbol(header, name, length, (int)(name - text), start_line,
                              line_of(header, end > offset ? end - 1 : offset), 0);
        }
    }
    else if (strncmp(p, "include", 7) == 0)
    {
        p = skip_blanks(p + 7);
        if (*p == '<' || *p == '"')
        {
            char name[MAX_PATH_LEN];
            if (read_until(p + 1, *p == '<' ? '>' : '"', name, sizeof(name)))
            {
                add_include(header, name, *p == '<');
            }
        }
    }
    return end;
}

static void scan_header(HeaderFile *header)
{
    const char *text = header->text;
    Statement statement = {0};
    int brace_depth = 0;
    int paren_depth = 0;
    int at_line_start = 1;
    int i = 0;
    while (text[i])
    {
        char c = text[i];
        if (c == '\n')
        {
            at_line_start = 1;
            statement_push(&statement, ' ', i);
            i++;
            continue;
        }
        if (isspace((unsigned char)c))
        {
            statement_push(&statement, ' ', i);
            i++;
            continue;
        }
        if (at_line_start && c == '#')
        {
            i = scan_preprocessor_line(header, i);
            continue;
        }
        at_line_start = 0;

        if (c == '/' && text[i + 1] == '/')
        {
            while (text[i] && text[i] != '\n')
            {
                i++;
            }
            continue;
        }
        if (c == '/' && text[i + 1] == '*')
        {
            i += 2;
            while (text[i] && !(text[i] == '*' && text[i + 1] == '/'))
            {
                i++;
            }
            if (text[i])
            {
                i += 2;
            }
            statement_push(&statement, ' ', i);
            continue;
        }
        if (c == '"' || c == '\'')
        {
            statement_push(&statement, c, i);
            i++;
            while (text[i] && text[i] != c && text[i] != '\n')
            {
                if (text[i] == '\\' && text[i + 1])
                {
                    statement_push(&statement, text[i], i);
                    i++;
                }
                statement_push(&statement, text[i], i);
                i++;
            }
            if (text[i] == c)
            {
                statement_push(&statement, c, i);
                i++;
            }
            continue;
        }

        statement_push(&statement, c, i);
        if (c == '(')
        {
            paren_depth++;
        }
        else if (c == ')')
        {
            if (paren_depth > 0)
            {
                paren_depth--;
            }
        }
        else if (paren_depth == 0 && c == '{')
        {
            if (brace_depth == 0 && is_extern_c_block(&statement))
            {
                statement.length = 0;
            }
            else
            {
                brace_depth++;
            }
        }
        else if (paren_depth == 0 && c == '}')
        {
            if (brace_depth == 0)
            {
                // Closes an `extern "C"` block.
                statement.length = 0;
            }
            else
            {
                brace_depth--;
                if (brace_depth == 0 && is_function_body(&statement))
                {
                    process_statement(header, &statement);
                    statement.length = 0;
                }
            }
        }
        else if (paren_depth == 0 && brace_depth == 0 && c == ';')
        {
            process_statement(header, &statement);
            statement.length = 0;
        }
        i++;
    }
    libc_free(statement.chars);
    libc_free(statement.offsets);
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
    {
        return NULL;
    }
    char *text = NULL;
    if (fseek(file, 0, SEEK_END) == 0)
    {
        long size = ftell(file);
        if (size >= 0 && fseek(file, 0, SEEK_SET) == 0)
        {
            text = libc_malloc((size_t)size + 1);
            if (text)
            {
                size_t read_size = fread(text, 1, (size_t)size, file);
                text[read_size] = '\0';
            }
        }
    }
    fclose(file);
    return text;
}

static void clear_header(HeaderFile *header)
{
    for (int i = 0; i < header->symbol_count; i++)
    {
        libc_free(header->symbols[i].name);
    }
    for (int i = 0; i < header->include_count; i++)
    {
        libc_free(header->includes[i].name);
    }
    libc_free(header->symbols);
    libc_free(header->includes);
    libc_free(header->text);
    libc_free(header->line_offsets);
    header->symbols = NULL;
    header->includes = NULL;
    header->text = NULL;
    header->line_offsets = NULL;
    header->symbol_count = 0;
    header->symbol_capacity = 0;
    header->include_count = 0;
    header->include_capacity = 0;
    header->line_count = 0;
}

static HeaderFile *find_cached_header(const char *path)
{
    for (HeaderFile *header = g_headers; header; header = header->next)
    {
        if (strcmp(header->path, path) == 0)
        {
            return header;
        }
    }
    return NULL;
}

// The scanned header at `path`, scanned again when it has changed on disk.
static HeaderFile *get_header(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
    {
        return NULL;
    }

    HeaderFile *header = find_cached_header(path);
    if (header && header->modified == (long long)st.st_mtime &&
        header->size == (long long)st.st_size)
    {
        return header;
    }
    if (header)
    {
        clear_header(header);
    }
    else
    {
        header = libc_malloc(sizeof(HeaderFile));
        char *path_copy = copy_text(path, strlen(path));
        if (!header || !path_copy)
        {
            libc_free(header);
            libc_free(path_copy);
            return NULL;
        }
        memset(header, 0, sizeof(HeaderFile));
        header->path = path_copy;
        header->next = g_headers;
        g_headers = header;
    }
    header->modified = (long long)st.st_mtime;
    header->size = (long long)st.st_size;

    header->text = read_file(path);
    if (!header->text)
    {
        return header;
    }
    int lines = 1;
    for (const char *p = header->text; *p; p++)
    {
        lines += *p == '\n';
    }
    header->line_offsets = libc_malloc((size_t)lines * sizeof(int));
    if (!header->line_offsets)
    {
        return header;
    }
    header->line_offsets[0] = 0;
    header->line_count = 1;
    for (int i = 0; header->text[i]; i++)
    {
        if (header->text[i] == '\n')
        {
            header->line_offsets[header->line_count++] = i + 1;
        }
    }
    scan_header(header);
    return header;
}

// Declarations of `name` in `header`; forward declarations only when nothing else exists.
// A name declared twice by the same declaration (`typedef struct Color {...} Color;`)
// counts once.
static int collect_matches(HeaderFile *header, const char *name, const CHeaderSymbol **results,
                           int max_results)
{
    int count = 0;
    for (int forward = 0; forward < 2 && count == 0; forward++)
    {
        for (int i = 0; i < header->symbol_count && count < max_results; i++)
        {
            const HeaderSymbol *candidate = &header->symbols[i];
            if (candidate->is_forward != forward || strcmp(candidate->name, name) != 0)
            {
                continue;
            }
            int same_declaration = 0;
            for (int j = 0; j < count; j++)
            {
                if (results[j]->start_line == candidate->symbol.start_line)
                {
                    same_declaration = 1;
                }
            }
            if (!same_declaration)
            {
                results[count++] = &candidate->symbol;
            }
        }
    }
    return count;
}

// Searches `path` and then, depth first, the headers it includes. Returns the matches of the
// first header that declares `name`.
static int search_header_tree(const char *path, const char *name, const SearchDirList *dirs,
                              StringList *visited, const CHeaderSymbol **results, int max_results,
                              int depth)
{
    if (depth > MAX_INCLUDE_DEPTH || string_list_contains(visited, path))
    {
        return 0;
    }
    string_list_add(visited, path);

    HeaderFile *header = get_header(path);
    if (!header)
    {
        return 0;
    }
    int count = collect_matches(header, name, results, max_results);
    if (count > 0)
    {
        return count;
    }

    char header_dir[MAX_PATH_LEN];
    directory_of(path, header_dir, sizeof(header_dir));
    for (int i = 0; i < header->include_count; i++)
    {
        char *child = find_header(header->includes[i].name,
                                  header->includes[i].is_system ? NULL : header_dir, dirs, NULL, 0);
        if (!child)
        {
            continue;
        }
        count = search_header_tree(child, name, dirs, visited, results, max_results, depth + 1);
        libc_free(child);
        if (count > 0)
        {
            return count;
        }
    }
    return 0;
}

static void fill_origin(CHeaderOrigin *origin, const HeaderImport *header, const char *file_path,
                        const char *file_source, int is_re_export, const SearchDirList *dirs)
{
    snprintf(origin->header, sizeof(origin->header), "%s", header->name);
    snprintf(origin->alias, sizeof(origin->alias), "%s", header->alias);
    snprintf(origin->declared_in, sizeof(origin->declared_in), "%s", file_path);
    source_line(file_source, header->line, origin->statement, sizeof(origin->statement));
    origin->line = header->line;
    origin->alias_column = header->alias_column;
    origin->alias_length = (int)strlen(header->alias);
    origin->name_column = header->name_column;
    origin->is_re_export = is_re_export;

    char file_dir[MAX_PATH_LEN];
    directory_of(file_path, file_dir, sizeof(file_dir));
    origin->found_by[0] = '\0';
    char *path = find_header(header->name, header->is_system ? NULL : file_dir, dirs,
                             origin->found_by, sizeof(origin->found_by));
    snprintf(origin->path, sizeof(origin->path), "%s", path ? path : "");
    libc_free(path);
}

// Where to look for a header that a module imports: where the document looks, plus the
// module's own directives. `zc build` ignores those (see lsp_c_headers_add_diagnostics()),
// but the module may well be compiled on its own or with them copied over.
static void module_search_dirs(const SearchDirList *document_dirs, const ModuleHeader *entry,
                               SearchDirList *dirs)
{
    search_dirs_append(dirs, document_dirs);
    collect_directive_dirs(entry->module_source, entry->module_path, dirs);
}

typedef struct
{
    const char *qualifier;
    const char *name;
    const SearchDirList *document_dirs;
    StringList *visited_headers;
    const CHeaderSymbol **results;
    int max_results;
    CHeaderOrigin *via;
    int found;
} ReexportSearch;

static int search_reexport(const ModuleHeader *entry, void *data)
{
    ReexportSearch *search = data;
    if (search->qualifier[0] && strcmp(entry->header->alias, search->qualifier) != 0)
    {
        return 0;
    }
    SearchDirList dirs = {0};
    module_search_dirs(search->document_dirs, entry, &dirs);
    CHeaderOrigin origin;
    fill_origin(&origin, entry->header, entry->module_path, entry->module_source, 1, &dirs);
    if (origin.path[0])
    {
        search->found =
            search_header_tree(origin.path, search->name, &dirs, search->visited_headers,
                               search->results, search->max_results, 0);
    }
    search_dirs_free(&dirs);
    if (search->found > 0 && search->via)
    {
        *search->via = origin;
    }
    // A qualified name stops at its alias, whether the header declares it or not.
    return search->found > 0 || search->qualifier[0];
}

int lsp_c_headers_find_at(const char *document_path, const char *source, int line, int col,
                          const CHeaderSymbol **results, int max_results, CHeaderOrigin *via)
{
    if (via)
    {
        via->is_re_export = 0;
        via->declared_in[0] = '\0';
    }
    CursorSymbol cursor;
    if (!document_path || !source || max_results <= 0 || !symbol_at(source, line, col, &cursor) ||
        cursor.is_qualifier)
    {
        return 0;
    }

    FileImports imports = {0};
    collect_imports(source, &imports);
    SearchDirList dirs = {0};
    collect_search_dirs(source, document_path, &dirs);
    char document_dir[MAX_PATH_LEN];
    directory_of(document_path, document_dir, sizeof(document_dir));

    StringList visited = {0};
    int found = 0;
    for (int i = 0; i < imports.header_count && found == 0; i++)
    {
        const HeaderImport *header = &imports.headers[i];
        if (cursor.qualifier[0] && strcmp(header->alias, cursor.qualifier) != 0)
        {
            continue;
        }
        char *path =
            find_header(header->name, header->is_system ? NULL : document_dir, &dirs, NULL, 0);
        if (!path)
        {
            fprintf(stderr, "zls: C header '%s' not found\n", header->name);
            continue;
        }
        found = search_header_tree(path, cursor.name, &dirs, &visited, results, max_results, 0);
        libc_free(path);
    }

    if (found == 0)
    {
        // Headers that imported modules re-export (`export import "x.h" as alias`).
        ReexportSearch search = {cursor.qualifier, cursor.name, &dirs, &visited,
                                 results,          max_results, via,   0};
        StringList visited_modules = {0};
        walk_module_headers(source, document_path, NULL, 1, 0, &visited_modules, search_reexport,
                            &search);
        string_list_free(&visited_modules);
        found = search.found;
    }

    string_list_free(&visited);
    search_dirs_free(&dirs);
    free_imports(&imports);
    return found;
}

int lsp_c_headers_is_extern_name(const char *source, int line, int col)
{
    CursorSymbol cursor;
    if (!source || !symbol_at(source, line, col, &cursor) || cursor.qualifier[0] ||
        cursor.is_qualifier)
    {
        return 0;
    }
    // `extern fn name`: walk back over `fn` and `extern`.
    const char *p = cursor.line_start + cursor.column;
    const char *const keywords[] = {"fn", "extern"};
    for (int k = 0; k < 2; k++)
    {
        while (p > cursor.line_start && (p[-1] == ' ' || p[-1] == '\t'))
        {
            p--;
        }
        size_t length = strlen(keywords[k]);
        if ((size_t)(p - cursor.line_start) < length ||
            strncmp(p - length, keywords[k], length) != 0)
        {
            return 0;
        }
        p -= length;
        if (p > cursor.line_start && is_identifier_char(p[-1]))
        {
            return 0;
        }
    }
    return 1;
}

typedef struct
{
    const char *alias;
    const SearchDirList *document_dirs;
    CHeaderOrigin *origin;
} AliasSearch;

static int find_reexported_alias(const ModuleHeader *entry, void *data)
{
    AliasSearch *search = data;
    if (strcmp(entry->header->alias, search->alias) != 0)
    {
        return 0;
    }
    SearchDirList dirs = {0};
    module_search_dirs(search->document_dirs, entry, &dirs);
    fill_origin(search->origin, entry->header, entry->module_path, entry->module_source, 1, &dirs);
    search_dirs_free(&dirs);
    return 1;
}

CHeaderOriginKind lsp_c_headers_origin_at(const char *document_path, const char *source, int line,
                                          int col, CHeaderOrigin *origin)
{
    if (!document_path || !source)
    {
        return C_HEADER_NONE;
    }
    FileImports imports = {0};
    collect_imports(source, &imports);
    SearchDirList dirs = {0};
    CHeaderOriginKind kind = C_HEADER_NONE;

    // On an import line of the document: its alias or its header name.
    for (int i = 0; i < imports.header_count && kind == C_HEADER_NONE; i++)
    {
        const HeaderImport *header = &imports.headers[i];
        if (header->line != line)
        {
            continue;
        }
        int alias_end = header->alias_column + (int)strlen(header->alias);
        int name_end = header->name_column + (int)strlen(header->name);
        if (header->alias_column >= 0 && col >= header->alias_column && col <= alias_end)
        {
            kind = C_HEADER_ALIAS_DECLARATION;
        }
        else if (col >= header->name_column && col <= name_end)
        {
            kind = C_HEADER_NAME;
        }
        if (kind != C_HEADER_NONE)
        {
            collect_search_dirs(source, document_path, &dirs);
            fill_origin(origin, header, document_path, source, 0, &dirs);
        }
    }

    // On `alias` of `alias::name`: imported here, or re-exported by an imported module.
    CursorSymbol cursor;
    if (kind == C_HEADER_NONE && symbol_at(source, line, col, &cursor) && cursor.is_qualifier)
    {
        collect_search_dirs(source, document_path, &dirs);
        for (int i = 0; i < imports.header_count && kind == C_HEADER_NONE; i++)
        {
            if (strcmp(imports.headers[i].alias, cursor.name) == 0)
            {
                fill_origin(origin, &imports.headers[i], document_path, source, 0, &dirs);
                kind = C_HEADER_ALIAS_USE;
            }
        }
        if (kind == C_HEADER_NONE)
        {
            AliasSearch search = {cursor.name, &dirs, origin};
            StringList visited = {0};
            if (walk_module_headers(source, document_path, NULL, 1, 0, &visited,
                                    find_reexported_alias, &search))
            {
                kind = C_HEADER_ALIAS_USE;
            }
            string_list_free(&visited);
        }
    }

    search_dirs_free(&dirs);
    free_imports(&imports);
    return kind;
}

/* --- Hover --- */

// The text of `line` in `header`, without its line break.
static const char *line_text(const HeaderFile *header, int line, int *length)
{
    const char *start = header->text + header->line_offsets[line];
    const char *end = strchr(start, '\n');
    int size = end ? (int)(end - start) : (int)strlen(start);
    while (size > 0 && start[size - 1] == '\r')
    {
        size--;
    }
    *length = size;
    return start;
}

// Column where a comment starts on the line, outside string literals, or -1.
static int comment_column(const char *text, int length)
{
    char quote = 0;
    for (int i = 0; i < length; i++)
    {
        if (quote)
        {
            if (text[i] == '\\')
            {
                i++;
            }
            else if (text[i] == quote)
            {
                quote = 0;
            }
        }
        else if (text[i] == '"' || text[i] == '\'')
        {
            quote = text[i];
        }
        else if (text[i] == '/' && i + 1 < length && (text[i + 1] == '/' || text[i + 1] == '*'))
        {
            return i;
        }
    }
    return -1;
}

// Appends the text of a comment line without its markers (`//`, `/*`, `*/`, leading `*`).
static void append_comment_text(char *doc, size_t doc_size, const char *text, int length)
{
    char line[MAX_DOC_LENGTH];
    copy_trimmed(text, (size_t)length, line, sizeof(line));
    char *p = line;
    if (strncmp(p, "/*", 2) == 0)
    {
        p += 2;
    }
    while (*p == '/' || *p == '*' || *p == '!' || *p == '<')
    {
        p++;
    }
    char *end = strstr(p, "*/");
    if (end)
    {
        *end = '\0';
    }
    char cleaned[MAX_DOC_LENGTH];
    copy_trimmed(p, strlen(p), cleaned, sizeof(cleaned));
    if (!cleaned[0])
    {
        return;
    }
    size_t used = strlen(doc);
    snprintf(doc + used, doc_size - used, "%s%s", used ? "\n" : "", cleaned);
}

// The comment block right above `line`, or nothing.
static void leading_comment(const HeaderFile *header, int line, char *doc, size_t doc_size)
{
    int first = line;
    int length;
    for (int k = line - 1; k >= 0 && line - k <= 40; k--)
    {
        const char *text = line_text(header, k, &length);
        char trimmed[MAX_DOC_LENGTH];
        copy_trimmed(text, (size_t)length, trimmed, sizeof(trimmed));
        size_t trimmed_length = strlen(trimmed);
        int is_comment = strncmp(trimmed, "//", 2) == 0 || strncmp(trimmed, "/*", 2) == 0 ||
                         trimmed[0] == '*' ||
                         (trimmed_length >= 2 && strcmp(trimmed + trimmed_length - 2, "*/") == 0);
        if (!is_comment)
        {
            break;
        }
        first = k;
    }
    for (int k = first; k < line; k++)
    {
        const char *text = line_text(header, k, &length);
        append_comment_text(doc, doc_size, text, length);
    }
}

// Appends `text` to the growing libc buffer `*out`.
static void append(char **out, size_t *used, size_t *capacity, const char *text, size_t length)
{
    if (!*out)
    {
        return;
    }
    if (*used + length + 1 > *capacity)
    {
        size_t new_capacity = (*used + length + 1) * 2;
        char *grown = libc_realloc(*out, new_capacity);
        if (!grown)
        {
            return;
        }
        *out = grown;
        *capacity = new_capacity;
    }
    memcpy(*out + *used, text, length);
    *used += length;
    (*out)[*used] = '\0';
}

char *lsp_c_headers_hover(const CHeaderSymbol *symbol, const CHeaderOrigin *via)
{
    HeaderFile *header = symbol ? find_cached_header(symbol->path) : NULL;
    if (!header || !header->text || symbol->end_line >= header->line_count)
    {
        return NULL;
    }

    int first = symbol->start_line;
    int last = symbol->end_line;
    int truncated = last - first >= MAX_HOVER_LINES;
    if (truncated)
    {
        last = first + MAX_HOVER_LINES - 1;
    }

    // A one-line declaration usually carries its documentation as a trailing comment.
    char doc[MAX_DOC_LENGTH];
    doc[0] = '\0';
    int last_length;
    const char *last_text = line_text(header, last, &last_length);
    if (first == last)
    {
        int comment = comment_column(last_text, last_length);
        if (comment >= 0)
        {
            append_comment_text(doc, sizeof(doc), last_text + comment, last_length - comment);
            last_length = comment;
        }
    }
    if (!doc[0])
    {
        leading_comment(header, first, doc, sizeof(doc));
    }

    // Indentation shared by every line of the declaration.
    int indent = -1;
    for (int k = first; k <= last; k++)
    {
        int length;
        const char *text = line_text(header, k, &length);
        int spaces = 0;
        while (spaces < length && (text[spaces] == ' ' || text[spaces] == '\t'))
        {
            spaces++;
        }
        if (spaces < length && (indent < 0 || spaces < indent))
        {
            indent = spaces;
        }
    }
    if (indent < 0)
    {
        indent = 0;
    }

    size_t used = 0;
    size_t capacity = 512;
    char *out = libc_malloc(capacity);
    if (!out)
    {
        return NULL;
    }
    out[0] = '\0';
    append(&out, &used, &capacity, "```c\n", 5);
    for (int k = first; k <= last; k++)
    {
        int length;
        const char *text = line_text(header, k, &length);
        if (k == last)
        {
            length = last_length;
        }
        while (length > 0 && isspace((unsigned char)text[length - 1]))
        {
            length--;
        }
        int skip = indent < length ? indent : length;
        append(&out, &used, &capacity, text + skip, (size_t)(length - skip));
        append(&out, &used, &capacity, "\n", 1);
    }
    if (truncated)
    {
        append(&out, &used, &capacity, "...\n", 4);
    }
    append(&out, &used, &capacity, "```\n", 4);
    if (doc[0])
    {
        append(&out, &used, &capacity, doc, strlen(doc));
        append(&out, &used, &capacity, "\n\n", 2);
    }
    char location[MAX_PATH_LEN + 32];
    snprintf(location, sizeof(location), "`%s:%d`", symbol->path, symbol->line + 1);
    append(&out, &used, &capacity, location, strlen(location));
    if (via && via->is_re_export)
    {
        char reexport[MAX_PATH_LEN + 600];
        snprintf(reexport, sizeof(reexport), "\n\nRe-exported by `%s`:\n```zc\n%s\n```",
                 file_label(via->declared_in), via->statement);
        append(&out, &used, &capacity, reexport, strlen(reexport));
    }
    return out;
}

char *lsp_c_headers_origin_hover(const CHeaderOrigin *origin, CHeaderOriginKind kind)
{
    char text[MAX_PATH_LEN * 3];
    size_t used = 0;
    if (kind == C_HEADER_NAME)
    {
        used += (size_t)snprintf(text, sizeof(text), "C header `%s`", origin->header);
    }
    else
    {
        used += (size_t)snprintf(text, sizeof(text), "`%s`: alias of the C header `%s`",
                                 origin->alias, origin->header);
    }
    if (origin->is_re_export)
    {
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "\n\nRe-exported by `%s`:\n```zc\n%s\n```",
                                 file_label(origin->declared_in), origin->statement);
    }
    else if (kind == C_HEADER_ALIAS_USE)
    {
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "\n\nImported at line %d:\n```zc\n%s\n```", origin->line + 1,
                                 origin->statement);
    }
    if (used < sizeof(text))
    {
        if (origin->path[0])
        {
            snprintf(text + used, sizeof(text) - used, "\n\n`%s`\n\nFound through %s.",
                     origin->path, origin->found_by);
        }
        else
        {
            snprintf(text + used, sizeof(text) - used,
                     "\n\nNot found where the C compiler will look for it.");
        }
    }
    return copy_text(text, strlen(text));
}

/* --- Locations --- */

static char *path_to_uri(const char *path)
{
    size_t length = strlen(path);
    char *uri = libc_malloc(length * 3 + 16);
    if (!uri)
    {
        return NULL;
    }
    size_t capacity = length * 3 + 16;
    size_t used = (size_t)snprintf(uri, capacity, "file://");
    if (isalpha((unsigned char)path[0]) && path[1] == ':')
    {
        uri[used++] = '/';
    }
    for (size_t i = 0; i < length; i++)
    {
        unsigned char c = (unsigned char)path[i];
        if (c == '\\')
        {
            uri[used++] = '/';
        }
        else if (isalnum(c) || strchr("/-._~:", c))
        {
            uri[used++] = (char)c;
        }
        else
        {
            used += (size_t)snprintf(uri + used, capacity - used, "%%%02X", c);
        }
    }
    uri[used] = '\0';
    return uri;
}

static cJSON *make_range(int line, int column, int length)
{
    cJSON *range = cJSON_CreateObject();
    cJSON *start = cJSON_CreateObject();
    cJSON_AddNumberToObject(start, "line", line);
    cJSON_AddNumberToObject(start, "character", column);
    cJSON *end = cJSON_CreateObject();
    cJSON_AddNumberToObject(end, "line", line);
    cJSON_AddNumberToObject(end, "character", column + length);
    cJSON_AddItemToObject(range, "start", start);
    cJSON_AddItemToObject(range, "end", end);
    return range;
}

static cJSON *make_location(const char *path, int line, int column, int length)
{
    char *uri = path_to_uri(path);
    if (!uri)
    {
        return NULL;
    }
    cJSON *location = cJSON_CreateObject();
    cJSON_AddStringToObject(location, "uri", uri);
    libc_free(uri);
    cJSON_AddItemToObject(location, "range", make_range(line, column, length));
    return location;
}

struct cJSON *lsp_c_headers_locations(const CHeaderSymbol **symbols, int count)
{
    cJSON *locations = cJSON_CreateArray();
    for (int i = 0; i < count; i++)
    {
        cJSON *location = make_location(symbols[i]->path, symbols[i]->line, symbols[i]->column,
                                        symbols[i]->length);
        if (location)
        {
            cJSON_AddItemToArray(locations, location);
        }
    }
    return locations;
}

struct cJSON *lsp_c_headers_origin_location(const CHeaderOrigin *origin, CHeaderOriginKind kind)
{
    if (kind == C_HEADER_ALIAS_USE)
    {
        return make_location(origin->declared_in, origin->line, origin->alias_column,
                             origin->alias_length);
    }
    return origin->path[0] ? make_location(origin->path, 0, 0, 0) : NULL;
}

/* --- Diagnostics --- */

typedef struct
{
    const SearchDirList *document_dirs;
    struct cJSON *diagnostics;
    StringList warned;
} DirectiveCheck;

// `zc build` only applies the `//>` directives of the file it compiles, but every header
// of every imported module ends up in the generated C. Warns about the headers that only a
// module's own directive makes findable.
static int check_module_header(const ModuleHeader *entry, void *data)
{
    DirectiveCheck *check = data;
    const HeaderImport *header = entry->header;
    if (string_list_contains(&check->warned, header->name))
    {
        return 0;
    }
    char module_dir[MAX_PATH_LEN];
    directory_of(entry->module_path, module_dir, sizeof(module_dir));
    char *path = find_header(header->name, header->is_system ? NULL : module_dir,
                             check->document_dirs, NULL, 0);
    if (path)
    {
        libc_free(path);
        return 0;
    }

    SearchDirList module_dirs = {0};
    collect_directive_dirs(entry->module_source, entry->module_path, &module_dirs);
    char found_by[MAX_PATH_LEN];
    path = find_header(header->name, NULL, &module_dirs, found_by, sizeof(found_by));
    search_dirs_free(&module_dirs);
    if (!path)
    {
        return 0;
    }
    libc_free(path);
    string_list_add(&check->warned, header->name);

    char message[MAX_PATH_LEN * 2];
    snprintf(message, sizeof(message),
             "C header '%s' (imported by %s) is only found through %s, but zc build only "
             "applies the directives of the file it compiles: add that directive here.",
             header->name, file_label(entry->module_path), found_by);
    cJSON *diagnostic = cJSON_CreateObject();
    cJSON_AddItemToObject(
        diagnostic, "range",
        make_range(entry->top->line, entry->top->name_column, (int)strlen(entry->top->name)));
    cJSON_AddNumberToObject(diagnostic, "severity", 2);
    cJSON_AddStringToObject(diagnostic, "message", message);
    cJSON_AddItemToArray(check->diagnostics, diagnostic);
    return 0;
}

void lsp_c_headers_add_diagnostics(const char *document_path, const char *source,
                                   struct cJSON *diagnostics)
{
    if (!document_path || !source || !diagnostics)
    {
        return;
    }
    FileImports imports = {0};
    collect_imports(source, &imports);
    int module_count = imports.module_count;
    free_imports(&imports);
    if (module_count == 0)
    {
        return;
    }

    SearchDirList dirs = {0};
    collect_search_dirs(source, document_path, &dirs);
    DirectiveCheck check = {&dirs, diagnostics, {0}};
    StringList visited = {0};
    walk_module_headers(source, document_path, NULL, 0, 0, &visited, check_module_header, &check);
    string_list_free(&visited);
    string_list_free(&check.warned);
    search_dirs_free(&dirs);
}
