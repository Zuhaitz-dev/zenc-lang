// SPDX-License-Identifier: MIT

#include "../ast/ast.h"
#include "../constants.h"
#include "../parser/parser.h"
#include "../zprep.h"
#include "codegen.h"
#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../platform/misra.h"

static void emit_freestanding_preamble(ParserContext *ctx)
{
    EMIT(ctx, "%s",
         "#include <stddef.h>\n#include <stdint.h>\n#include <stdbool.h>\n#include <stdarg.h>\n");
    EMIT(ctx, "%s",
         "#ifdef __has_builtin\n#if __has_builtin(__builtin_pow)\n#define _zc_pow "
         "__builtin_pow\n#endif\n#endif\n#ifndef _zc_pow\nextern double pow(double, "
         "double);\n#define _zc_pow pow\n#endif\n");
    EMIT(ctx, "%s", ZC_TCC_COMPAT_STR);
    EMIT(ctx, "%s",
         "typedef size_t usize;\ntypedef char* string;\n"
         "#ifndef __CUDACC__\ntypedef intptr_t any;\n#else\ntypedef intptr_t zc_any;\n#define any "
         "zc_any\n#endif\n");
    EMIT(ctx, "%s",
         "#define U0 void\n#define I8 int8_t\n#define U8 uint8_t\n#define I16 int16_t\n#define U16 "
         "uint16_t\n");
    EMIT(ctx, "%s",
         "#define I32 int32_t\n#define U32 uint32_t\n#define I64 int64_t\n#define U64 uint64_t\n");
    EMIT(ctx, "%s", "#define F32 float\n#define F64 double\n");
    EMIT(ctx, "%s",
         "static inline const char* _z_bool_str(_Bool b) { return b ? \"true\" : \"false\"; }\n");
    EMIT(ctx, "%s", "#ifdef __SIZEOF_INT128__\n");
    EMIT(ctx, "%s",
         "static inline const char *_z_u128_str(unsigned __int128 val) {\n    static _Thread_local "
         "char buf[40];\n    if (val == 0) return \"0\";\n    int i = 38;\n    buf[39] = 0;\n    "
         "while (val > 0) { buf[i--] = (char)((val % 10) + '0'); val /= 10; }\n    return &buf[i + "
         "1];\n}\nstatic inline const char *_z_i128_str(__int128 val) {\n    static _Thread_local "
         "char buf[41];\n    if (val == 0) return \"0\";\n    int neg = val < 0;\n    "
         "unsigned __int128 uval = (unsigned __int128)(neg ? -val : val);\n    int i = 39;\n    "
         "buf[40] = 0;\n    while (uval > "
         "0) { buf[i--] = (char)((uval % 10) + '0'); uval /= 10; }\n    if (neg) buf[i--] = '-';\n "
         "   return &buf[i + 1];\n}\n#define _z_128_map ,__int128: \"%s\", unsigned __int128: "
         "\"%s\"\n#define _z_safe_i128(x) _Generic((x), __int128: (x), default: "
         "(__int128)0)\n#define _z_safe_u128(x) _Generic((x), unsigned __int128: (x), default: "
         "(unsigned __int128)0)\n#define _z_128_arg_map(x) ,__int128: "
         "_z_i128_str(_z_safe_i128(x)), unsigned __int128: _z_u128_str(_z_safe_u128(x))\n");
    EMIT(ctx, "%s", "#else\n");
    EMIT(ctx, "%s", "#define _z_128_map\n");
    EMIT(ctx, "%s", "#define _z_128_arg_map(x)\n");
    EMIT(ctx, "%s", "#endif\n");
    EMIT(ctx, "%s",
         "#define _z_str(x) _Generic((x), _Bool: \"%s\", char: \"%c\", signed char: \"%c\", "
         "unsigned char: \"%u\", short: \"%d\", unsigned short: \"%u\", int: \"%d\", unsigned int: "
         "\"%u\", long: \"%ld\", unsigned long: \"%lu\", long long: \"%lld\", unsigned long long: "
         "\"%llu\", float: \"%f\", double: \"%f\", char*: \"%s\", const char*: \"%s\", void*: "
         "\"%p\", default: \"%s\" _z_128_map)\n");
    EMIT(ctx, "%s",
         "#define _z_safe_bool(x) _Generic((x), _Bool: (x), default: (_Bool)0)\n#define _z_arg(x) "
         "_Generic((x), _Bool: _z_bool_str(_z_safe_bool(x)) _z_128_arg_map(x), default: (x))\n");
    EMIT(ctx, "%s",
         "typedef struct { void *func; void *ctx; void (*drop)(void*); } z_closure_T;\n");
    EMIT(ctx, "%s", "static void *_z_closure_ctx_stash[256];\n");

    // In true freestanding, explicit definitions of z_malloc/etc are removed.
    // The user must implement them if they use features requiring them.
    // Most primitives (integers, pointers) work without them.
}

void emit_preamble(ParserContext *ctx)
{
    if (ctx->config->misra_mode)
    {
        emit_misra_preamble(ctx->cg.emitter.file);
        return;
    }
    if (ctx->config->is_freestanding)
    {
        emit_freestanding_preamble(ctx);
        return;
    }
    else
    {
        // Standard hosted preamble.
        EMIT(ctx, "%s", "#ifndef _GNU_SOURCE\n#define _GNU_SOURCE\n#endif\n");
        EMIT(ctx, "%s",
             "#include <stdio.h>\n#include <stdlib.h>\n#include <stddef.h>\n#include <string.h>\n");
        EMIT(ctx, "%s", "#include <stdarg.h>\n#include <stdint.h>\n#include <stdbool.h>\n");
        EMIT(ctx, "%s", "#define null NULL\n");
        EMIT(ctx, "%s",
             "#ifdef __has_builtin\n#if __has_builtin(__builtin_pow)\n#define _zc_pow "
             "__builtin_pow\n#endif\n#endif\n#ifndef _zc_pow\nextern double pow(double, "
             "double);\n#define _zc_pow pow\n#endif\n");
        EMIT(ctx, "%s", "#include <unistd.h>\n#include <fcntl.h>\n"); // POSIX functions
        EMIT(ctx, "%s", "#define ZC_SIMD(T, N) T __attribute__((vector_size(N * sizeof(T))))\n");

        // Map C11 _Thread_local to C++11 thread_local (used in _z_{u}128_str)
        if (ctx->config->use_cpp ||
            (ctx->config->backend_name && strcmp(ctx->config->backend_name, "cpp") == 0))
        {
            EMIT(ctx, "%s", "#define _Thread_local thread_local\n");
        }

        // C++ compatibility
        if (ctx->config->use_cpp)
        {
            EMIT(ctx, "%s", "#define ZC_AUTO auto\n");
            EMIT(ctx, "%s", "#define ZC_AUTO_INIT(var, init) auto var = (init)\n");
            EMIT(ctx, "%s", "#define ZC_CAST(T, x) static_cast<T>(x)\n");
            EMIT(ctx, "%s", "#define null nullptr\n");
            // C++ _z_str via overloads
            EMIT(ctx, "%s",
                 "inline const char* _z_bool_str(bool b) { return b ? \"true\" : \"false\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(bool)               { return \"%s\"; }\n");
            EMIT(ctx, "%s",
                 "inline const char* _z_arg(bool b)             { return _z_bool_str(b); }\n");
            EMIT(ctx, "%s", "template<typename T> inline T _z_arg(T x)     { return x; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(char)               { return \"%c\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(signed char)        { return \"%d\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(unsigned char)      { return \"%u\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(short)               { return \"%d\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(unsigned short)      { return \"%u\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(int)                { return \"%d\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(unsigned int)       { return \"%u\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(long)               { return \"%ld\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(unsigned long)      { return \"%lu\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(long long)          { return \"%lld\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(unsigned long long) { return \"%llu\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(float)              { return \"%f\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(double)             { return \"%f\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(char*)              { return \"%s\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(const char*)        { return \"%s\"; }\n");
            EMIT(ctx, "%s", "inline const char* _z_str(void*)              { return \"%p\"; }\n");
        }
        else
        {
            // C mode
            EMIT(ctx, "%s", "#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202300L\n");
            EMIT(ctx, "%s", "#define ZC_AUTO auto\n");
            EMIT(ctx, "%s", "#define ZC_AUTO_INIT(var, init) auto var = (init)\n");
            EMIT(ctx, "%s", "#else\n");
            EMIT(ctx, "%s", "#define ZC_AUTO __auto_type\n");
            EMIT(ctx, "%s", "#define ZC_AUTO_INIT(var, init) __auto_type var = (init)\n");
            EMIT(ctx, "%s", "#endif\n");
            EMIT(ctx, "%s", "#define ZC_CAST(T, x) ((T)(x))\n");
            EMIT(ctx, "%s", ZC_TCC_COMPAT_STR);
            EMIT(ctx, "%s",
                 "static inline const char* _z_bool_str(_Bool b) { return b ? \"true\" : "
                 "\"false\"; }\n");
            EMIT(ctx, "%s", ZC_C_GENERIC_STR);
            EMIT(ctx, "%s", ZC_C_ARG_GENERIC_STR);
        }

        EMIT(ctx, "%s",
             "typedef size_t usize;\ntypedef char* string;\n"
             "#ifndef __CUDACC__\ntypedef intptr_t any;\n#else\ntypedef intptr_t zc_any;\n#define "
             "any zc_any\n#endif\n");
        if (ctx->cg.has_async)
        {
            EMIT(ctx, "%s", "typedef int (*PollFn)(void*);\n");
        }
        EMIT(ctx, "%s",
             "#ifdef ZC_STATIC_PLUGIN\n#define ZC_FUNC static\n#define ZC_GLOBAL "
             "static\n#else\n#define ZC_FUNC\n#define ZC_GLOBAL\n#endif\n");
        EMIT(ctx, "%s",
             "typedef struct { void *func; void *ctx; void (*drop)(void*); } z_closure_T;\n");
        EMIT(ctx, "%s", "static __attribute__((unused)) void *_z_closure_ctx_stash[256];\n");
        EMIT(ctx, "%s",
             "typedef void U0;\ntypedef int8_t I8;\ntypedef uint8_t U8;\ntypedef int16_t "
             "I16;\ntypedef uint16_t U16;\n");
        EMIT(ctx, "%s",
             "typedef int32_t I32;\ntypedef uint32_t U32;\ntypedef int64_t I64;\ntypedef uint64_t "
             "U64;\n");
        EMIT(ctx, "%s", "#define F32 float\n#define F64 double\n");

        // Memory Mapping.
        if (ctx->config->use_cpp)
        {
            // C++ needs explicit casts for void* conversions
            EMIT(ctx, "%s", "#define z_malloc(sz) static_cast<char*>(malloc(sz))\n");
            EMIT(ctx, "%s", "#define z_realloc(p, sz) static_cast<char*>(realloc(p, sz))\n");
        }
        else
        {
            EMIT(ctx, "%s", "#define z_malloc malloc\n#define z_realloc realloc\n");
        }
        EMIT(ctx, "%s", "#define z_free free\n#define z_print printf\n");
        EMIT(ctx, "%s",
             "static __attribute__((unused)) void __zenc_panic(const char* msg) { fprintf(stderr, "
             "\"Panic: %s\\n\", msg); "
             "exit(1); }\n");
        EMIT(ctx, "%s",
             "#if defined(__APPLE__)\n#define _ZC_SEC "
             "__attribute__((used,section(\"__DATA,__zarch\")))\n#elif defined(_WIN32)\n#define "
             "_ZC_SEC __attribute__((used))\n#else\n#define _ZC_SEC "
             "__attribute__((used,section(\".note.zarch\")))\n#endif\n");
        EMIT(ctx, "%s",
             "static const unsigned char _zc_abi_v1[] _ZC_SEC = "
             "{0x07,0xd5,0x59,0x30,0x7c,0x7f,0x66,0x75,0x30,0x69,0x7f,0x65,0x3c,0x30,0x59,0x7c,"
             "0x79,0x7e,0x73,0x71};\n");

        EMIT(ctx, "%s",
             "static __attribute__((unused)) void _z_autofree_impl(void *p) { void **pp = "
             "(void**)p; if(*pp) { "
             "z_free(*pp); *pp = NULL; } }\n");
        EMIT(ctx, "%s",
             "#define __zenc_assert(cond, ...) if (!(cond)) { fprintf(stderr, \"  Assertion "
             "failed: \" __VA_ARGS__); fprintf(stderr, \"\\n\"); _zc_test_failures++; }\n");
        EMIT(ctx, "%s",
             "#define __zenc_expect(cond, ...) if (!(cond)) { fprintf(stderr, \"  Expectation "
             "failed: \" __VA_ARGS__); fprintf(stderr, \"\\n\"); _zc_test_failures++; }\n");
        EMIT(ctx, "static __attribute__((unused)) int _zc_test_failures = 0;\n");

        // C++ compatible readln helper
        if (ctx->config->use_cpp)
        {
            EMIT(ctx, "%s",
                 "static __attribute__((unused)) string _z_readln_raw(void) { size_t cap = 64; "
                 "size_t len = 0; char *line = "
                 "static_cast<char*>(malloc(cap)); if(!line) return NULL; int c; while((c = "
                 "fgetc(stdin)) != EOF) { if(c == '\\n') break; if(len + 1 >= cap) { cap *= 2; "
                 "char *n = static_cast<char*>(realloc(line, cap)); if(!n) { z_free(line); return "
                 "NULL; } line = n; } line[len++] = (char)c; } if(len == 0 && c == EOF) { "
                 "z_free(line); "
                 "return NULL; } line[len] = 0; return line; }\n");
        }
        else
        {
            EMIT(
                ctx, "%s",
                "static __attribute__((unused)) string _z_readln_raw(void) { size_t cap = 64; "
                "size_t len = 0; char *line = "
                "z_malloc(cap); if(!line) return NULL; int c; while((c = fgetc(stdin)) != EOF) { "
                "if(c == '\\n') break; if(len + 1 >= cap) { cap *= 2; char *n = z_realloc(line, "
                "cap); if(!n) { z_free(line); return NULL; } line = n; } line[len++] = (char)c; } "
                "if(len "
                "== 0 && c == EOF) { z_free(line); return NULL; } line[len] = 0; return line; }\n");
        }
        // Scans one line with `fmt`; every argument is a pointer. A `%ms` (one word
        // into a new string, a glibc-only conversion) is done portably: `%s` into a
        // buffer as long as the line, handed to the `char**` argument on success.
        // The format passed to sscanf is built at run time, so -Wformat-nonliteral
        // is silenced around it (builds with -Werror, such as the APE one, fail).
        // It could be avoided by doing the `%ms` conversion at compile time: the
        // parser knows each argument's type, so the call site could read the line
        // and call sscanf with a literal format and a buffer per string.
        EMIT(ctx, "%s",
             "#if defined(__GNUC__) && !defined(__TINYC__)\n#pragma GCC diagnostic push\n"
             "#pragma GCC diagnostic ignored \"-Wformat-nonliteral\"\n#endif\n");
        EMIT(ctx, "%s",
             "static __attribute__((unused)) int _z_scan_helper(const char *fmt, ...) { char *l = "
             "_z_readln_raw(); if(!l) return 0; void *a[16] = {0}; void *p[16] = {0}; char "
             "*own[16] = {0}; void **pa = a; void **pp = p; char **po = own; char *f = "
             "(char*)z_malloc(strlen(fmt) + 1); if(!f) { z_free(l); return 0; } char *w = f; "
             "va_list ap; va_start(ap, fmt); for(const char *c = fmt; *c; c++) { *w++ = *c; "
             "if(*c == '%' && pa < a + 16) { *pa = va_arg(ap, void*); *pp = *pa; if(c[1] == 'm' "
             "&& c[2] == 's') { *po = (char*)z_malloc(strlen(l) + 1); *pp = *po; c++; } pa++; "
             "pp++; po++; } } va_end(ap); *w = 0; int r = sscanf(l, f, p[0], p[1], p[2], p[3], "
             "p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]); "
             "for(char **o = own; o < po; o++) { if(*o) { if(o - own < r) { *(char**)a[o - own] "
             "= *o; } else { z_free(*o); } } } z_free(f); z_free(l); return r; }\n");
        EMIT(ctx, "%s",
             "#if defined(__GNUC__) && !defined(__TINYC__)\n#pragma GCC diagnostic pop\n#endif\n");

        // REPL helpers: suppress/restore stdout.
        EMIT(ctx, "%s", "static int _z_orig_stdout = -1;\n");
        EMIT(ctx, "%s", "static __attribute__((unused)) void _z_suppress_stdout(void) {\n");
        emitter_indent(&ctx->cg.emitter);
        EMIT(ctx, "%s", "fflush(stdout);\n");
        EMIT(ctx, "%s", "if (_z_orig_stdout == -1) _z_orig_stdout = dup(STDOUT_FILENO);\n");
        EMIT(ctx, "%s", "int nullfd = open(\"/dev/null\", O_WRONLY);\n");
        EMIT(ctx, "%s", "dup2(nullfd, STDOUT_FILENO);\n");
        EMIT(ctx, "%s", "close(nullfd);\n");
        emitter_dedent(&ctx->cg.emitter);
        EMIT(ctx, "%s", "}\n");
        EMIT(ctx, "%s", "static __attribute__((unused)) void _z_restore_stdout(void) {\n");
        emitter_indent(&ctx->cg.emitter);
        EMIT(ctx, "%s", "fflush(stdout);\n");
        EMIT(ctx, "%s", "if (_z_orig_stdout != -1) {\n");
        emitter_indent(&ctx->cg.emitter);
        EMIT(ctx, "%s", "dup2(_z_orig_stdout, STDOUT_FILENO);\n");
        EMIT(ctx, "%s", "close(_z_orig_stdout);\n");
        EMIT(ctx, "%s", "_z_orig_stdout = -1;\n");
        emitter_dedent(&ctx->cg.emitter);
        EMIT(ctx, "%s", "}\n");
        emitter_dedent(&ctx->cg.emitter);
        EMIT(ctx, "%s", "}\n");
    }
}
