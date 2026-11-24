// SPDX-License-Identifier: MIT
#include "codegen_shared.h"
#include "codegen_internal.h"
#include "../parser/parser.h"
#include "../ast/primitives.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

// String manipulation

char *strip_template_suffix(const char *name)
{
    if (!name)
    {
        return NULL;
    }
    char *lt = (char *)strchr(name, '<');
    if (lt)
    {
        ptrdiff_t len = lt - name;
        char *buf = xmalloc((size_t)(len + 1));
        strncpy(buf, name, (size_t)(len));
        buf[len] = 0;
        return buf;
    }
    return xstrdup(name);
}

char *extract_call_args(const char *args)
{
    if (!args || strlen(args) == 0)
    {
        return xstrdup("");
    }
    char *out = xmalloc(strlen(args) + 1);
    out[0] = 0;

    char *dup = xstrdup(args);
    char *p = strtok(dup, ",");
    while (p)
    {
        while (*p == ' ')
        {
            p++;
        }
        char *last_space = (char *)strrchr(p, ' ');
        char *ptr_star = (char *)strrchr(p, '*');

        char *name = p;
        if (last_space)
        {
            name = last_space + 1;
        }
        if (ptr_star && ptr_star > last_space)
        {
            name = ptr_star + 1;
        }

        if (strlen(out) > 0)
        {
            strcat(out, ", ");
        }
        strcat(out, name);

        p = strtok(NULL, ",");
    }
    zfree(dup);
    return out;
}

const char *parse_original_method_name(const char *mangled)
{
    const char *sep = (char *)strstr(mangled, "__");
    if (!sep)
    {
        return mangled;
    }

    const char *last_double = NULL;
    const char *p = mangled;
    while ((p = strstr(p, "__")))
    {
        last_double = p;
        p += 2;
    }

    return last_double ? last_double + 2 : mangled;
}

char *replace_string_type(const char *args)
{
    if (!args)
    {
        return NULL;
    }
    char *res = xmalloc(strlen(args) * 2 + 16);
    res[0] = 0;
    const char *p = args;
    while (*p)
    {
        const char *match = (char *)strstr(p, "string");
        if (match)
        {
            size_t prefix_len = (size_t)(match - p);
            strncat(res, p, (size_t)(prefix_len));
            p = match;

            int before_ok = (match == args || (!isalnum(*(match - 1)) && *(match - 1) != '_'));
            int after_ok = (!isalnum(match[6]) && match[6] != '_');

            if (before_ok && after_ok)
            {
                strcat(res, "const char*");
                p += 6;
            }
            else
            {
                strncat(res, p, 1);
                p += 1;
            }
        }
        else
        {
            strcat(res, p);
            break;
        }
    }
    return res;
}

// Type name mapping

const char *map_to_c_type(const char *t)
{
    if (strcmp(t, "c_int") == 0)
    {
        return "int";
    }
    if (strcmp(t, "c_uint") == 0)
    {
        return "unsigned int";
    }
    if (strcmp(t, "c_long") == 0)
    {
        return "long";
    }
    if (strcmp(t, "c_ulong") == 0)
    {
        return "unsigned long";
    }
    if (strcmp(t, "c_longlong") == 0)
    {
        return "long long";
    }
    if (strcmp(t, "c_ulonglong") == 0)
    {
        return "unsigned long long";
    }
    if (strcmp(t, "c_short") == 0)
    {
        return "short";
    }
    if (strcmp(t, "c_ushort") == 0)
    {
        return "unsigned short";
    }
    if (strcmp(t, "c_char") == 0)
    {
        return "char";
    }
    if (strcmp(t, "c_uchar") == 0)
    {
        return "unsigned char";
    }
    if (strcmp(t, "uint") == 0)
    {
        return "unsigned int";
    }

    return normalize_type_name(t);
}

// ** CodegenKeySet **

typedef struct CodegenKey
{
    int kind;
    const char *name; // NULL: an empty slot
    const char *extra;
} CodegenKey;

static unsigned codegen_key_hash(int kind, const char *name, const char *extra)
{
    unsigned h = 2166136261u ^ (unsigned)kind; // FNV-1a
    for (const unsigned char *c = (const unsigned char *)name; *c; c++)
    {
        h = (h ^ *c) * 16777619u;
    }
    h = (h ^ 0xFFu) * 16777619u; // between the two strings
    for (const unsigned char *c = (const unsigned char *)extra; c && *c; c++)
    {
        h = (h ^ *c) * 16777619u;
    }
    return h;
}

static int codegen_key_eq(const CodegenKey *k, int kind, const char *name, const char *extra)
{
    return k->kind == kind && strcmp(k->name, name) == 0 &&
           ((!k->extra && !extra) || (k->extra && extra && strcmp(k->extra, extra) == 0));
}

// The slot of the key, or the empty slot it would go in. The set is never full.
static CodegenKey *codegen_key_slot(CodegenKey *slots, unsigned cap, int kind, const char *name,
                                    const char *extra)
{
    unsigned i = codegen_key_hash(kind, name, extra) & (cap - 1);
    while (slots[i].name && !codegen_key_eq(&slots[i], kind, name, extra))
    {
        i = (i + 1) & (cap - 1);
    }
    return &slots[i];
}

int codegen_keyset_has(const CodegenKeySet *set, int kind, const char *name, const char *extra)
{
    if (!set->slots || !name)
    {
        return 0;
    }
    return codegen_key_slot(set->slots, set->cap, kind, name, extra)->name != NULL;
}

void codegen_keyset_add(CodegenKeySet *set, int kind, const char *name, const char *extra)
{
    if (!name)
    {
        return;
    }
    // Kept at most half full.
    if ((set->count + 1) * 2 > set->cap)
    {
        unsigned cap = set->cap ? set->cap * 2 : 256;
        CodegenKey *slots = xmalloc(cap * sizeof(CodegenKey));
        memset(slots, 0, cap * sizeof(CodegenKey));
        for (unsigned i = 0; i < set->cap; i++)
        {
            CodegenKey *k = &set->slots[i];
            if (k->name)
            {
                *codegen_key_slot(slots, cap, k->kind, k->name, k->extra) = *k;
            }
        }
        zfree(set->slots);
        set->slots = slots;
        set->cap = cap;
    }
    CodegenKey *slot = codegen_key_slot(set->slots, set->cap, kind, name, extra);
    if (!slot->name)
    {
        slot->kind = kind;
        slot->name = name;
        slot->extra = extra;
        set->count++;
    }
}

void codegen_keyset_free(CodegenKeySet *set)
{
    zfree(set->slots);
    set->slots = NULL;
    set->cap = 0;
    set->count = 0;
}
