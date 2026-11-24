// SPDX-License-Identifier: MIT
#include "../parser/parser.h"

#include "../ast/ast.h"
#include "../constants.h"
#include "../zprep.h"
#include "codegen.h"
#include "codegen_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper: Check if a struct depends on another struct/enum by-value.
// ** What a struct or enum needs emitted before it **
//
// A field (or an enum payload) of a type that is not a pointer needs that type complete.
// What it names is matched against the other types by name, as a prefix: a field of type
// `Vec_int` needs a type `Vec` before it. An enum's match also needs the name to end
// there ('\0', '[', or a space) in its type as written.

// One field's or payload's type, as it is matched.
typedef struct SortKey
{
    char *mangled; // the type with `string` made `const char*`: a name is a prefix of it
    char *clean;   // an enum payload's type as written (NULL for a struct field)
    struct SortKey *next;
} SortKey;

static const char *sort_strip_tag(const char *type_str)
{
    if (strncmp(type_str, "struct ", 7) == 0)
    {
        return type_str + 7;
    }
    if (strncmp(type_str, "enum ", 5) == 0)
    {
        return type_str + 5;
    }
    if (strncmp(type_str, "union ", 6) == 0)
    {
        return type_str + 6;
    }
    return type_str;
}

static SortKey *sort_key_new(ParserContext *ctx, const char *type_str, int keep_clean,
                             SortKey *next)
{
    const char *clean = sort_strip_tag(type_str);
    if (ctx)
    {
        const char *alias = find_type_alias(ctx, clean);
        if (alias)
        {
            clean = alias;
        }
    }
    SortKey *k = xmalloc(sizeof(SortKey));
    k->mangled = replace_string_type(clean);
    k->clean = keep_clean ? xstrdup(clean) : NULL;
    k->next = next;
    return k;
}

// The keys of a struct's fields or an enum's payloads, which are not pointers.
static SortKey *sort_keys_of(ParserContext *ctx, ASTNode *s1)
{
    SortKey *keys = NULL;
    if (s1->kind == NODE_STRUCT)
    {
        for (ASTNode *field = s1->strct.fields; field; field = field->next)
        {
            if (field->kind == NODE_FIELD && field->field.type && !strchr(field->field.type, '*'))
            {
                keys = sort_key_new(ctx, field->field.type, 0, keys);
            }
        }
    }
    else if (s1->kind == NODE_ENUM)
    {
        for (ASTNode *variant = s1->enm.variants; variant; variant = variant->next)
        {
            if (variant->kind != NODE_ENUM_VARIANT || !variant->variant.payload)
            {
                continue;
            }
            char *type_str = type_to_c_string(variant->variant.payload, ctx->config->misra_mode);
            if (type_str && !strchr(type_str, '*'))
            {
                keys = sort_key_new(ctx, type_str, 1, keys);
            }
            zfree(type_str);
        }
    }
    return keys;
}

static void sort_keys_free(SortKey *k)
{
    while (k)
    {
        SortKey *next = k->next;
        zfree(k->mangled);
        zfree(k->clean);
        zfree(k);
        k = next;
    }
}

// The types by name: an open-addressed table of names, each with the indexes of the
// types of that name.
typedef struct SortName
{
    const char *name; // NULL: an empty slot
    size_t len;
    int *idx;
    int idx_count;
} SortName;

static unsigned sort_name_hash(const char *s, size_t len)
{
    unsigned h = 2166136261u; // FNV-1a
    for (size_t i = 0; i < len; i++)
    {
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    }
    return h;
}

// The slot of the name s[0..len), or the empty slot it would go in.
static SortName *sort_name_slot(SortName *table, unsigned cap, const char *s, size_t len)
{
    unsigned i = sort_name_hash(s, len) & (cap - 1);
    while (table[i].name && !(table[i].len == len && memcmp(table[i].name, s, len) == 0))
    {
        i = (i + 1) & (cap - 1);
    }
    return &table[i];
}

static int count_sortable_nodes_internal(ASTNode *head, VisitedModules **visited, int depth)
{
    if (depth > 1024)
    {
        zfatal("Infinite recursion detected in count_sortable_nodes (circular imports?)");
    }
    int count = 0;
    ASTNode *n = head;
    while (n)
    {
        if (n->kind == NODE_STRUCT || n->kind == NODE_ENUM || n->kind == NODE_TRAIT)
        {
            count++;
        }
        else if (n->kind == NODE_IMPORT)
        {
            if (!is_module_visited(*visited, n->import_stmt.path))
            {
                mark_module_visited(visited, n->import_stmt.path);
                count +=
                    count_sortable_nodes_internal(n->import_stmt.module_root, visited, depth + 1);
            }
        }
        else if (n->kind == NODE_ROOT)
        {
            // Avoid same-root recursion if children == head
            if (n->root.children != head)
            {
                count += count_sortable_nodes_internal(n->root.children, visited, depth + 1);
            }
        }
        n = n->next;
    }
    return count;
}

static int count_sortable_nodes(ASTNode *head)
{
    VisitedModules *visited = NULL;
    int count = count_sortable_nodes_internal(head, &visited, 0);
    free_visited_modules(visited);
    return count;
}

static void collect_sortable_nodes_internal(ASTNode *head, ASTNode **nodes, int *idx,
                                            VisitedModules **visited, int depth)
{
    if (depth > 1024)
    {
        zfatal("Infinite recursion detected in collect_sortable_nodes (circular imports?)");
    }
    ASTNode *n = head;
    while (n)
    {
        if (n->kind == NODE_STRUCT || n->kind == NODE_ENUM || n->kind == NODE_TRAIT)
        {
            nodes[(*idx)++] = n;
        }
        else if (n->kind == NODE_IMPORT)
        {
            if (!is_module_visited(*visited, n->import_stmt.path))
            {
                mark_module_visited(visited, n->import_stmt.path);
                collect_sortable_nodes_internal(n->import_stmt.module_root, nodes, idx, visited,
                                                depth + 1);
            }
        }
        else if (n->kind == NODE_ROOT)
        {
            // Avoid same-root recursion if children == head
            if (n->root.children != head)
            {
                collect_sortable_nodes_internal(n->root.children, nodes, idx, visited, depth + 1);
            }
        }
        n = n->next;
    }
}

static void collect_sortable_nodes(ASTNode *head, ASTNode **nodes, int *idx)
{
    VisitedModules *visited = NULL;
    collect_sortable_nodes_internal(head, nodes, idx, &visited, 0);
    free_visited_modules(visited);
}

// Topologically sort a list of struct/enum nodes.
//
// The order is that of a scan repeated until nothing changes: each pass goes through the
// nodes in their order, and emits a trait at once and a struct or enum when every other
// type it needs (see SortKey) has been emitted. What is left (a cycle) goes last, in order.
//
// That scan compares every node with every other on every pass. The same order comes from
// the pass each node is emitted in: a node goes in the first pass after each type it needs
// has gone, or in the same pass when that type comes before it in the list. So
//
//     pass(i) = max(1, pass(d) + (d > i ? 1 : 0) for each type d that i needs)
//
// worked out in dependency order (Kahn's algorithm); then the nodes in (pass, index) order.
typedef struct
{
    int pass;
    int index;
} SortSlot;

static int sort_slot_cmp(const void *a, const void *b)
{
    const SortSlot *x = a;
    const SortSlot *y = b;
    if (x->pass != y->pass)
    {
        return x->pass < y->pass ? -1 : 1;
    }
    return x->index < y->index ? -1 : (x->index > y->index ? 1 : 0);
}

static ASTNode *topo_sort_structs(ParserContext *ctx, ASTNode *head)
{
    if (!head)
    {
        return NULL;
    }

    // Count all nodes (structs + enums + traits).
    int count = count_sortable_nodes(head);
    if (count == 0)
    {
        return head;
    }

    ASTNode **nodes = xmalloc((size_t)count * sizeof(ASTNode *));
    int idx = 0;
    collect_sortable_nodes(head, nodes, &idx);

    // The types by name. A trait is no type a field can need.
    unsigned cap = 64;
    while (cap < (unsigned)count * 2)
    {
        cap *= 2;
    }
    SortName *names = xmalloc(cap * sizeof(SortName));
    memset(names, 0, cap * sizeof(SortName));
    for (int j = 0; j < count; j++)
    {
        const char *name = nodes[j]->kind == NODE_STRUCT ? nodes[j]->strct.name
                           : nodes[j]->kind == NODE_ENUM ? nodes[j]->enm.name
                                                         : NULL;
        if (!name)
        {
            continue;
        }
        size_t len = strlen(name);
        SortName *slot = sort_name_slot(names, cap, name, len);
        if (!slot->name)
        {
            slot->name = name;
            slot->len = len;
        }
        slot->idx = xrealloc(slot->idx, sizeof(int) * (size_t)(slot->idx_count + 1));
        slot->idx[slot->idx_count++] = j;
    }

    // Each node's needs, as edges d -> i (i needs d), without repeats.
    int *need_count = xmalloc((size_t)count * sizeof(int));
    int **needed_by = xmalloc((size_t)count * sizeof(int *));
    int *needed_by_count = xmalloc((size_t)count * sizeof(int));
    int *needed_by_cap = xmalloc((size_t)count * sizeof(int));
    int *seen = xmalloc((size_t)count * sizeof(int)); // the last i that needed d
    memset(need_count, 0, (size_t)count * sizeof(int));
    memset(needed_by, 0, (size_t)count * sizeof(int *));
    memset(needed_by_count, 0, (size_t)count * sizeof(int));
    memset(needed_by_cap, 0, (size_t)count * sizeof(int));
    for (int j = 0; j < count; j++)
    {
        seen[j] = -1;
    }

    for (int i = 0; i < count; i++)
    {
        if (nodes[i]->kind == NODE_TRAIT)
        {
            continue; // emitted at once, whatever it names
        }
        SortKey *keys = sort_keys_of(ctx, nodes[i]);
        for (SortKey *k = keys; k; k = k->next)
        {
            size_t mlen = strlen(k->mangled);
            size_t clen = k->clean ? strlen(k->clean) : 0;
            // Every name that is a prefix of the type, the empty one too.
            for (size_t len = 0; len <= mlen; len++)
            {
                if (k->clean)
                {
                    // An enum's: the name also ends where the type as written does, or at
                    // a '[' or a space in it.
                    if (len > clen)
                    {
                        break;
                    }
                    char next = k->clean[len];
                    if (!(next == 0 || next == '[' || isspace((unsigned char)next)))
                    {
                        continue;
                    }
                }
                SortName *slot = sort_name_slot(names, cap, k->mangled, len);
                if (!slot->name)
                {
                    continue;
                }
                for (int n = 0; n < slot->idx_count; n++)
                {
                    int d = slot->idx[n];
                    if (d == i || seen[d] == i)
                    {
                        continue;
                    }
                    seen[d] = i;
                    need_count[i]++;
                    if (needed_by_count[d] == needed_by_cap[d])
                    {
                        needed_by_cap[d] = needed_by_cap[d] ? needed_by_cap[d] * 2 : 4;
                        needed_by[d] =
                            xrealloc(needed_by[d], sizeof(int) * (size_t)(needed_by_cap[d]));
                    }
                    needed_by[d][needed_by_count[d]++] = i;
                }
            }
        }
        sort_keys_free(keys);
    }

    // Kahn's algorithm, working out each node's pass as its last need is met.
    int *pass = xmalloc((size_t)count * sizeof(int));
    int *ready = xmalloc((size_t)count * sizeof(int));
    int ready_head = 0;
    int ready_tail = 0;
    for (int i = 0; i < count; i++)
    {
        pass[i] = 1;
        if (need_count[i] == 0)
        {
            ready[ready_tail++] = i;
        }
    }
    while (ready_head < ready_tail)
    {
        int d = ready[ready_head++];
        for (int n = 0; n < needed_by_count[d]; n++)
        {
            int i = needed_by[d][n];
            int p = pass[d] + (d > i ? 1 : 0);
            if (p > pass[i])
            {
                pass[i] = p;
            }
            if (--need_count[i] == 0)
            {
                ready[ready_tail++] = i;
            }
        }
    }

    // The emitted nodes by (pass, index); then the rest (cycles), in order.
    SortSlot *slots = xmalloc((size_t)count * sizeof(SortSlot));
    int slot_count = 0;
    for (int i = 0; i < count; i++)
    {
        if (need_count[i] == 0)
        {
            slots[slot_count].pass = pass[i];
            slots[slot_count].index = i;
            slot_count++;
        }
    }
    qsort(slots, (size_t)slot_count, sizeof(SortSlot), sort_slot_cmp);
    for (int i = 0; i < count; i++)
    {
        if (need_count[i] != 0)
        {
            slots[slot_count].pass = 0;
            slots[slot_count].index = i;
            slot_count++;
        }
    }

    // Now build the linked list in the correct order.
    ASTNode *result = NULL;
    ASTNode *result_tail = NULL;
    for (int n = 0; n < slot_count; n++)
    {
        ASTNode *node = nodes[slots[n].index];
        if (!result)
        {
            result = node;
            result_tail = node;
        }
        else
        {
            result_tail->next = node;
            result_tail = node;
        }
    }
    if (result_tail)
    {
        result_tail->next = NULL;
    }

    for (unsigned c = 0; c < cap; c++)
    {
        zfree(names[c].idx);
    }
    zfree(names);
    for (int j = 0; j < count; j++)
    {
        zfree(needed_by[j]);
    }
    zfree(needed_by);
    zfree(needed_by_count);
    zfree(needed_by_cap);
    zfree(need_count);
    zfree(seen);
    zfree(pass);
    zfree(ready);
    zfree(slots);
    zfree(nodes);
    return result;
}

// Helper structure for tracking emitted content to prevent duplicates
typedef struct EmittedContent
{
    char *content;
    struct EmittedContent *next;
} EmittedContent;

// Check if content has already been emitted
static int is_content_emitted(EmittedContent *list, const char *content)
{
    while (list)
    {
        if (strcmp(list->content, content) == 0)
        {
            return 1;
        }
        list = list->next;
    }
    return 0;
}

// Mark content as emitted
static void mark_content_emitted(EmittedContent **list, const char *content)
{
    EmittedContent *node = xmalloc(sizeof(EmittedContent));
    node->content = xstrdup(content);
    node->next = *list;
    *list = node;
}

// Free emitted content list
static void free_emitted_list(EmittedContent *list)
{
    while (list)
    {
        EmittedContent *next = list->next;
        zfree(list->content);
        zfree(list);
        list = next;
    }
}

static void emit_raw_statements_internal(ParserContext *ctx, ASTNode *node,
                                         VisitedModules **visited, int depth, int preproc_only,
                                         EmittedContent **emitted_raw)
{
    if (depth > 1024)
    {
        zfatal(
            "Infinite recursion detected in emit_raw_statements_internal (ctx, circular imports?)");
    }
    while (node)
    {
        if (node->kind == NODE_IMPORT)
        {
            if (!is_module_visited(*visited, node->import_stmt.path))
            {
                mark_module_visited(visited, node->import_stmt.path);
                emit_raw_statements_internal(ctx, node->import_stmt.module_root, visited, depth + 1,
                                             preproc_only, emitted_raw);
            }
            node = node->next;
            continue;
        }
        else if (node->kind == NODE_ROOT)
        {
            emit_raw_statements_internal(ctx, node->root.children, visited, depth + 1, preproc_only,
                                         emitted_raw);
            node = node->next;
            continue;
        }

        if ((node->kind == NODE_RAW_STMT || node->kind == NODE_PREPROC_DIRECTIVE) &&
            node->raw_stmt.content)
        {
            const char *content = node->raw_stmt.content;
            while (*content == ' ' || *content == '\t' || *content == '\n')
            {
                content++;
            }

            int is_preproc = (*content == '#');

            if ((preproc_only && is_preproc) || (!preproc_only && !is_preproc))
            {
                if (!is_content_emitted(*emitted_raw, node->raw_stmt.content))
                {
                    EMIT(ctx, "%s\n", node->raw_stmt.content);
                    mark_content_emitted(emitted_raw, node->raw_stmt.content);
                }
            }
        }
        node = node->next;
    }
}

static void emit_auto_drop_glues(ParserContext *ctx, ASTNode *structs)
{
    ASTNode *s = structs;
    while (s)
    {
        if (s->kind == NODE_STRUCT && s->type_info && s->type_info->traits.has_drop &&
            !s->strct.is_template)
        {
            if (s->cfg_condition)
            {
                EMIT(ctx, "#if %s\n", s->cfg_condition);
            }

            char *sname = s->strct.name;
            EMIT(ctx, "// Auto-Generated RAII Glue for %s\n", sname);
            EMIT(ctx, "void %s__Drop__glue(%s *self) {\n", sname, sname);

            char glue_mangled[MAX_MANGLED_NAME_LEN];
            snprintf(glue_mangled, sizeof(glue_mangled), "%s__Drop__drop", sname);
            if (find_func(ctx, glue_mangled))
            {
                EMIT(ctx, "    %s__Drop__drop(self);\n", sname);
            }

            ASTNode *field = s->strct.fields;
            while (field)
            {
                Type *ft = field->type_info;
                if (ft && ft->kind == TYPE_STRUCT && ft->name)
                {
                    ASTNode *fdef = find_struct_def(ctx, ft->name);
                    if (fdef && fdef->type_info && fdef->type_info->traits.has_drop)
                    {
                        EMIT(ctx, "    %s__Drop__glue(&self->%s);\n", ft->name, field->field.name);
                    }
                }
                field = field->next;
            }
            EMIT(ctx, "}\n\n");
            if (s->cfg_condition)
            {
                EMIT(ctx, "#endif\n");
            }
            EMIT(ctx, "\n");
        }
        s = s->next;
    }
}

static void emit_generic_drop_macro(ParserContext *ctx, ASTNode *structs)
{
    (void)ctx;
    if (ctx->config->use_cpp && !ctx->config->use_cuda)
    {
        EMIT(ctx, "// Global Generic Drop Dispatch (C++ Overloads)\n");
        EMIT(ctx, "#ifdef __cplusplus\n");
        EMIT(ctx, "} // end extern \"C\"\n");
        EMIT(ctx, "template<typename T> inline void _z_drop(T& x) { (void)x; }\n");
        EMIT(ctx, "template<typename T> inline void _z_drop(T* x) { (void)x; }\n");

        ASTNode *s = structs;
        while (s)
        {
            if (s->kind == NODE_STRUCT && s->type_info && s->type_info->traits.has_drop &&
                !s->strct.is_template)
            {
                if (s->cfg_condition)
                {
                    EMIT(ctx, "#if %s\n", s->cfg_condition);
                }
                char *sname = s->strct.name;
                EMIT(ctx, "inline void _z_drop(%s& x) { %s__Drop__glue(&x); }\n", sname, sname);
                EMIT(ctx, "inline void _z_drop(%s* x) { if(x) %s__Drop__glue(x); }\n", sname,
                     sname);
                if (s->cfg_condition)
                {
                    EMIT(ctx, "#endif\n");
                }
            }
            s = s->next;
        }
        EMIT(ctx, "extern \"C\" {\n");
        EMIT(ctx, "#endif\n\n");
    }
    else
    {
        EMIT(ctx, "// Global Generic Drop Dispatch\n");
        EMIT(ctx, "#define _z_drop(x) _Generic((x)");

        ASTNode *s = structs;
        while (s)
        {
            if (s->kind == NODE_STRUCT && s->type_info && s->type_info->traits.has_drop &&
                !s->strct.is_template)
            {
                char *sname = s->strct.name;
                EMIT(ctx, ", \\\n    %s: %s__Drop__glue((void*)&(x))", sname, sname);
            }
            s = s->next;
        }

        EMIT(ctx, ", \\\n    default: (void)0)\n\n");
    }
}

// Walk the AST and replace NODE_COMPTIME nodes with their generated children.
// This must run before codegen so that emitted declarations (structs, functions)
// appear in the root children list.
static void flatten_comptime_nodes(ASTNode *node);

// In the list whose head is at `pp`, puts in place of each NODE_COMPTIME that generated
// code the code it generated, and flattens under every other node.
static void flatten_comptime_list(ASTNode **pp)
{
    while (*pp)
    {
        if ((*pp)->kind == NODE_COMPTIME && (*pp)->comptime.generated)
        {
            ASTNode *gen = (*pp)->comptime.generated;
            ASTNode *gen_last = gen;
            while (gen_last->next)
            {
                gen_last = gen_last->next;
            }
            gen_last->next = (*pp)->next;
            // Look at the same place again: the generated code may start with a comptime.
            *pp = gen;
        }
        else
        {
            flatten_comptime_nodes(*pp);
            pp = &(*pp)->next;
        }
    }
}

static void flatten_comptime_nodes(ASTNode *node)
{
    // The body of a comptime block is interpreted, not emitted.
    if (!node || node->kind == NODE_COMPTIME)
    {
        return;
    }

    ASTChildren children = ast_children(node);
    for (int i = 0; i < children.count; i++)
    {
        flatten_comptime_list(children.slots[i]);
    }
}

// Main entry point for code generation.
void codegen_c_program(ParserContext *ctx, ASTNode *node)
{
    // Flatten any NODE_COMPTIME blocks into their generated AST nodes
    if (node)
    {
        flatten_comptime_nodes(node);
    }

    if (!node)
    {
        return;
    }
    if (node->kind == NODE_ROOT)
    {
        ctx->current_scope = ctx->global_scope;
        ASTNode *kids = node->root.children;
        while (kids && kids->kind == NODE_ROOT)
        {
            kids = kids->root.children;
        }

        ctx->cg.current_func_ret_type = NULL;
        ctx->cg.current_lambda = NULL;
        ctx->cg.global_user_structs = kids;
        VisitedModules *visited = NULL;

        if (!ctx->cg.skip_preamble)
        {
            emit_preamble(ctx);
        }

        for (size_t i = 0; i < ctx->config->cfg_defines.length; i++)
        {
            EMIT(ctx, "#ifndef ZC_CFG_%s\n#define ZC_CFG_%s 1\n#endif\n",
                 ctx->config->cfg_defines.data[i], ctx->config->cfg_defines.data[i]);
        }

        emit_includes_and_aliases(ctx, kids, &visited);
        if (ctx->config->use_cpp && !ctx->config->use_cuda && !ctx->config->use_objc)
        {
            EMIT(ctx, "\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n");
        }

        if (ctx->cg.hoist_out)
        {
            long pos = ftell(ctx->cg.hoist_out);
            if (pos < 0)
            {
                pos = 0;
            }
            rewind(ctx->cg.hoist_out);
            char buf[4096];
            size_t n;
            while (ctx->cg.hoist_out && (n = fread(buf, 1, sizeof(buf), ctx->cg.hoist_out)) > 0)
            {
                emitter_write(&(ctx)->cg.emitter, buf, 1 * n);
            }
            fseek(ctx->cg.hoist_out, pos, SEEK_SET);
        }

        ASTNode *merged = NULL;
        ASTNode *merged_tail = NULL;

        ASTNode *s = ctx->instantiated_structs;
        while (s)
        {
            ASTNode *copy = xmalloc(sizeof(ASTNode));
            *copy = *s;
            copy->next = NULL;
            if (!merged)
            {
                merged = copy;
                merged_tail = copy;
            }
            else
            {
                merged_tail->next = copy;
                merged_tail = copy;
            }
            s = s->next;
        }

        StructRef *sr = ctx->parsed_structs_list;
        while (sr)
        {
            if (sr->node)
            {
                ASTNode *copy = xmalloc(sizeof(ASTNode));
                *copy = *sr->node;
                copy->next = NULL;
                if (!merged)
                {
                    merged = copy;
                    merged_tail = copy;
                }
                else
                {
                    merged_tail->next = copy;
                    merged_tail = copy;
                }
            }
            sr = sr->next;
        }

        StructRef *er = ctx->parsed_enums_list;
        while (er)
        {
            if (er->node)
            {
                ASTNode *copy = xmalloc(sizeof(ASTNode));
                *copy = *er->node;
                copy->next = NULL;
                if (!merged)
                {
                    merged = copy;
                    merged_tail = copy;
                }
                else
                {
                    merged_tail->next = copy;
                    merged_tail = copy;
                }
            }
            er = er->next;
        }

        // The structs and enums in `merged` by kind and name: a top-level one already there
        // is not added again.
        CodegenKeySet merged_types = {0};
        for (ASTNode *m = merged; m; m = m->next)
        {
            if (m->kind == NODE_STRUCT || m->kind == NODE_ENUM)
            {
                codegen_keyset_add(&merged_types, (int)m->kind,
                                   m->kind == NODE_STRUCT ? m->strct.name : m->enm.name, NULL);
            }
        }

        ASTNode *k = kids;
        while (k)
        {
            if (k->kind == NODE_STRUCT || k->kind == NODE_ENUM)
            {
                const char *k_name = (k->kind == NODE_STRUCT) ? k->strct.name : k->enm.name;
                int found = codegen_keyset_has(&merged_types, (int)k->kind, k_name, NULL);

                if (!found)
                {
                    codegen_keyset_add(&merged_types, (int)k->kind, k_name, NULL);
                    ASTNode *copy = xmalloc(sizeof(ASTNode));
                    *copy = *k;
                    copy->next = NULL;
                    if (!merged)
                    {
                        merged = copy;
                        merged_tail = copy;
                    }
                    else
                    {
                        merged_tail->next = copy;
                        merged_tail = copy;
                    }
                }
            }
            k = k->next;
        }

        // Tuple structs registered for tuple types and multi-value payloads take
        // part in the ordering: a tuple holding a struct by value must come after
        // it, and an enum carrying a tuple after the tuple.
        TupleType *tup = ctx->used_tuples;
        while (tup)
        {
            char *clean_sig = sanitize_mangled_name(tup->sig);
            size_t name_len = strlen(clean_sig) + sizeof("Tuple__");
            char *tuple_name = xmalloc(name_len);
            snprintf(tuple_name, name_len, "Tuple__%s", clean_sig);
            zfree(clean_sig);
            ASTNode *def = find_struct_def(ctx, tuple_name);
            zfree(tuple_name);
            if (def && def->kind == NODE_STRUCT)
            {
                ASTNode *copy = xmalloc(sizeof(ASTNode));
                *copy = *def;
                copy->next = NULL;
                if (!merged)
                {
                    merged = copy;
                    merged_tail = copy;
                }
                else
                {
                    merged_tail->next = copy;
                    merged_tail = copy;
                }
            }
            tup = tup->next;
        }

        codegen_keyset_free(&merged_types);

        // Topologically sort.
        ASTNode *sorted = topo_sort_structs(ctx, merged);

        print_type_defs(ctx, sorted);
        emit_global_aliases(ctx);

        visited = NULL;
        emit_type_aliases(ctx, kids, &visited);

        // Tagged-enum constructor prototypes name their payload types: they go
        // after the payload-less enums (print_type_defs) and the aliases.
        if (!ctx->config->use_cpp)
        {
            emit_enum_protos(ctx, sorted);
        }

        visited = NULL;
        emit_trait_defs(ctx, kids, &visited);

        // Track emitted raw statements to prevent duplicates
        EmittedContent *emitted_raw = NULL;

        // First pass: emit ONLY preprocessor directives before struct defs
        VisitedModules *raw_visited_1 = NULL;
        emit_raw_statements_internal(ctx, kids, &raw_visited_1, 0, 1, &emitted_raw);

        if (sorted)
        {
            emit_struct_defs(ctx, sorted, &visited);
        }

        // Second pass: emit non-preprocessor raw statements after struct defs
        VisitedModules *raw_visited_2 = NULL;
        emit_raw_statements_internal(ctx, kids, &raw_visited_2, 0, 0, &emitted_raw);

        // Emit type aliases was here (moved up)

        ASTNode *merged_globals = NULL;          // Head
        CodegenKeySet merged_global_names = {0}; // its globals and defs, by name

        if (ctx->parsed_globals_list)
        {
            StructRef *struct_ref = ctx->parsed_globals_list;
            while (struct_ref)
            {
                // Check if this global is already in the merged list (by name)
                int is_duplicate = 0;
                int is_var = struct_ref->node && (struct_ref->node->kind == NODE_VAR_DECL ||
                                                  struct_ref->node->kind == NODE_CONST);
                if (is_var)
                {
                    is_duplicate = codegen_keyset_has(&merged_global_names, 0,
                                                      struct_ref->node->var_decl.name, NULL);
                }

                if (!is_duplicate && struct_ref->node)
                {
                    if (is_var)
                    {
                        codegen_keyset_add(&merged_global_names, 0, struct_ref->node->var_decl.name,
                                           NULL);
                    }
                    ASTNode *copy = xmalloc(sizeof(ASTNode));
                    *copy = *struct_ref->node;
                    copy->next = merged_globals;
                    merged_globals = copy;
                }

                struct_ref = struct_ref->next;
            }
        }

        codegen_keyset_free(&merged_global_names);

        ASTNode *merged_funcs = NULL;
        ASTNode *merged_funcs_tail = NULL;

        if (ctx->instantiated_funcs)
        {
            ASTNode *fn_node = ctx->instantiated_funcs;
            while (fn_node)
            {
                ASTNode *copy = xmalloc(sizeof(ASTNode));
                *copy = *fn_node;
                copy->next = NULL;
                if (!merged_funcs)
                {
                    merged_funcs = copy;
                    merged_funcs_tail = copy;
                }
                else
                {
                    merged_funcs_tail->next = copy;
                    merged_funcs_tail = copy;
                }
                fn_node = fn_node->next;
            }
        }

        if (ctx->parsed_funcs_list)
        {
            StructRef *fn_ref = ctx->parsed_funcs_list;
            while (fn_ref)
            {
                ASTNode *copy = xmalloc(sizeof(ASTNode));
                *copy = *fn_ref->node;
                copy->next = NULL;
                if (!merged_funcs)
                {
                    merged_funcs = copy;
                    merged_funcs_tail = copy;
                }
                else
                {
                    merged_funcs_tail->next = copy;
                    merged_funcs_tail = copy;
                }
                fn_ref = fn_ref->next;
            }
        }

        if (ctx->parsed_impls_list)
        {
            StructRef *impl_ref = ctx->parsed_impls_list;
            while (impl_ref)
            {
                ASTNode *copy = xmalloc(sizeof(ASTNode));
                *copy = *impl_ref->node;
                copy->next = NULL;
                if (!merged_funcs)
                {
                    merged_funcs = copy;
                    merged_funcs_tail = copy;
                }
                else
                {
                    merged_funcs_tail->next = copy;
                    merged_funcs_tail = copy;
                }
                impl_ref = impl_ref->next;
            }
        }

        visited = NULL;
        emit_trait_wrappers(ctx, kids, &visited);

        visited = NULL;
        emit_protos(ctx, merged_funcs, &visited);

        visited = NULL;
        emit_globals(ctx, merged_globals, &visited);

        emit_impl_vtables(ctx);
        emit_auto_drop_glues(ctx, sorted);
        emit_generic_drop_macro(ctx, sorted);

        emit_lambda_defs(ctx);

        int test_count = emit_tests_and_runner(ctx, kids);

        ASTNode *iter = merged_funcs;
        while (iter)
        {
            if (iter->kind == NODE_IMPL)
            {
                char *sname = iter->impl.struct_name;
                if (!sname)
                {
                    iter = iter->next;
                    continue;
                }

                // Resolve opaque alias
                const char *resolved = find_type_alias(ctx, sname);

                char *mangled = replace_string_type(sname);
                ASTNode *def = find_struct_def(ctx, mangled);
                if (!def && resolved)
                {
                    zfree(mangled);
                    mangled = replace_string_type(resolved);
                    def = find_struct_def(ctx, mangled);
                }
                int skip = 0;
                if (def)
                {
                    if (def->kind == NODE_STRUCT && def->strct.is_template)
                    {
                        skip = 1;
                    }
                    else if (def->kind == NODE_ENUM && def->enm.is_template)
                    {
                        skip = 1;
                    }
                }
                else
                {
                    char *buf = strip_template_suffix(sname);
                    if (buf)
                    {
                        def = find_struct_def(ctx, buf);
                        if (def && def->strct.is_template)
                        {
                            skip = 1;
                        }
                        zfree(buf);
                    }
                }
                if (mangled)
                {
                    zfree(mangled);
                }
                if (skip)
                {
                    iter = iter->next;
                    continue;
                }
            }
            if (iter->kind == NODE_IMPL_TRAIT)
            {
                char *sname = iter->impl_trait.target_type;
                if (!sname)
                {
                    iter = iter->next;
                    continue;
                }

                char *mangled = replace_string_type(sname);
                ASTNode *def = find_struct_def(ctx, mangled);
                int skip = 0;
                if (def)
                {
                    if (def->strct.is_template)
                    {
                        skip = 1;
                    }
                }
                else
                {
                    char *buf = strip_template_suffix(sname);
                    if (buf)
                    {
                        def = find_struct_def(ctx, buf);
                        if (def && def->strct.is_template)
                        {
                            skip = 1;
                        }
                        zfree(buf);
                    }
                }
                if (mangled)
                {
                    zfree(mangled);
                }
                if (skip)
                {
                    iter = iter->next;
                    continue;
                }
            }
            if (iter->cfg_condition)
            {
                EMIT(ctx, "#if %s\n", iter->cfg_condition);
            }
            codegen_node_single(ctx, iter);
            if (iter->cfg_condition)
            {
                EMIT(ctx, "#endif\n");
            }
            iter = iter->next;
        }

        int has_user_main = 0;
        ASTNode *chk = merged_funcs;
        while (chk)
        {
            if (chk->kind == NODE_FUNCTION && strcmp(chk->func.name, "main") == 0)
            {
                has_user_main = 1;
                break;
            }
            chk = chk->next;
        }

        if (!has_user_main && test_count > 0)
        {
            EMIT(ctx, "\nint main() { return _z_run_tests(); }\n");
        }

        if (ctx->config->use_cpp && !ctx->config->use_cuda && !ctx->config->use_objc)
        {
            EMIT(ctx, "\n#ifdef __cplusplus\n}\n#endif\n");
        }

        // Clean up emitted content tracking list
        free_emitted_list(emitted_raw);
        free_visited_modules(visited);
    }
}
