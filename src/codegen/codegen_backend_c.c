// SPDX-License-Identifier: MIT
#include "codegen_backend.h"
#include "codegen.h"
#include "../parser/parser.h"
#include <string.h>

#define MAX_BACKENDS 12

static const CodegenBackend *registered_backends[MAX_BACKENDS];
static int backend_count = 0;

static const CodegenBackend c_backend = {
    .name = "c",
    .extension = ".c",
    .emit_program = codegen_c_program,
    .emit_preamble = emit_preamble,
    .needs_cc = 1,
};

void codegen_register_backend(const CodegenBackend *backend)
{
    if (!backend || !backend->name || backend_count >= MAX_BACKENDS)
    {
        return;
    }
    registered_backends[backend_count++] = backend;
}

const char *backend_opt(zvec_Str *opts, const char *key)
{
    if (!opts || !key)
    {
        return NULL;
    }
    size_t klen = strlen(key);
    for (size_t i = 0; i < opts->length; i++)
    {
        const char *opt = opts->data[i];
        if (strncmp(opt, key, (size_t)(klen)) == 0)
        {
            if (opt[klen] == '=')
            {
                return opt + klen + 1;
            }
            if (opt[klen] == '\0')
            {
                return "1";
            }
        }
    }
    return NULL;
}

const char *codegen_alias_lookup(const char *flag)
{
    if (!flag)
    {
        return NULL;
    }
    static char buf[512];
    for (int i = 0; i < backend_count; i++)
    {
        const BackendOptAlias *a = registered_backends[i]->aliases;
        if (!a)
        {
            continue;
        }
        for (; a->flag; a++)
        {
            if (strcmp(a->flag, flag) == 0)
            {
                const char *val = a->opt_val ? a->opt_val : "1";
                int n = snprintf(buf, sizeof(buf), "%s=%s", a->opt_key, val);
                if (n < (int)sizeof(buf))
                {
                    return buf;
                }
                return NULL;
            }
        }
    }
    return NULL;
}

const CodegenBackend *codegen_get_backend(const char *name)
{
    if (!name)
    {
        return &c_backend;
    }
    for (int i = 0; i < backend_count; i++)
    {
        if (strcmp(registered_backends[i]->name, name) == 0)
        {
            return registered_backends[i];
        }
    }
    return &c_backend;
}

const CodegenBackend *codegen_find_backend(const char *name)
{
    for (int i = 0; i < backend_count; i++)
    {
        if (strcmp(registered_backends[i]->name, name) == 0)
        {
            return registered_backends[i];
        }
    }
    return NULL;
}

const CodegenBackend *const *codegen_backends(int *count)
{
    *count = backend_count;
    return registered_backends;
}

void codegen_init_backends(void)
{
    if (backend_count > 0)
    {
        return;
    }
    codegen_register_backend(&c_backend);
#if ZC_HAS_CPP_BACKEND
    codegen_register_cpp_backend();
#endif
#if ZC_HAS_CUDA_BACKEND
    codegen_register_cuda_backend();
#endif
#if ZC_HAS_OBJC_BACKEND
    codegen_register_objc_backend();
#endif
#if ZC_HAS_JSON_BACKEND
    codegen_register_json_backend();
#endif
#if ZC_HAS_LISP_BACKEND
    codegen_register_lisp_backend();
#endif
#if ZC_HAS_DOT_BACKEND
    codegen_register_dot_backend();
#endif
#if ZC_HAS_ASTDUMP_BACKEND
    codegen_register_astdump_backend();
#endif
}

void codegen_node(ParserContext *ctx, ASTNode *root)
{
    const CodegenBackend *be = codegen_get_backend(ctx->config->backend_name);
    if (be && be->emit_program)
    {
        be->emit_program(ctx, root);
    }
}
