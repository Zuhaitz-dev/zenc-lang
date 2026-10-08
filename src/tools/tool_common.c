// SPDX-License-Identifier: MIT
#include "tool_common.h"
#include "../compiler.h"
#include "../codegen/codegen.h"
#include "../diagnostics/diagnostics.h"
#include "../platform/os.h"
#include "../utils/utils.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void z_tool_crash(int sig)
{
    // Only async-signal-safe calls belong in a signal handler.
    static const char hdr[] = "\nCRITICAL: Compiler crashed with signal ";
    static const char tail[] = "\nThis is likely a bug in the Zen compiler.\n";
    char sigbuf[16];
    int n = 0;
    if (sig == 0)
    {
        sigbuf[n++] = '0';
    }
    else
    {
        char tmp[16];
        int tn = 0;
        int s = sig;
        while (s > 0 && tn < 15)
        {
            tmp[tn++] = (char)('0' + (s % 10));
            s /= 10;
        }
        while (tn > 0)
        {
            sigbuf[n++] = tmp[--tn];
        }
    }
    sigbuf[n++] = '\n';
    ssize_t r = write(STDERR_FILENO, hdr, sizeof(hdr) - 1);
    (void)r;
    r = write(STDERR_FILENO, sigbuf, (size_t)n);
    (void)r;
    r = write(STDERR_FILENO, tail, sizeof(tail) - 1);
    (void)r;
    _exit(139);
}

// Shared initialization: signal handlers, terminal/config defaults, diagnostics
// and std/ root-path discovery. Mirrors what `zc`'s main() used to do inline so
// every binary (compiler and tools) boots the same way.
void z_compiler_setup(ZenCompiler *compiler)
{
    CompilerConfig *cfg = &compiler->config;

    signal(SIGSEGV, z_tool_crash);
    signal(SIGABRT, z_tool_crash);
    signal(SIGFPE, z_tool_crash);

    z_setup_terminal();
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode_debug = 1;
    if (z_is_windows())
    {
        strncpy(cfg->cc, "gcc.exe", sizeof(cfg->cc) - 1);
        cfg->cc[sizeof(cfg->cc) - 1] = '\0';
    }
    else
    {
        strncpy(cfg->cc, "gcc", sizeof(cfg->cc) - 1);
        cfg->cc[sizeof(cfg->cc) - 1] = '\0';
    }

    // Default diagnostics: Enable standard Zen C diagnostics
    set_diag_by_name(cfg, "unused", 1);
    set_diag_by_name(cfg, "safety", 1);
    set_diag_by_name(cfg, "logic", 1);
    set_diag_by_name(cfg, "conversion", 1);
    set_diag_by_name(cfg, "style", 1);

    codegen_init_backends();

    char self_path[MAX_PATH_SIZE];
    z_get_executable_path(self_path, sizeof(self_path));
    if (self_path[0])
    {
        cfg->root_path = xstrdup(self_path);

        // Improve root_path discovery: look for std.zc in root_path or its parents, and in
        // their share/zenc (where `make install` puts it: PREFIX/bin/zc finds PREFIX/share/zenc).
        char current_root[MAX_PATH_SIZE];
        strncpy(current_root, self_path, sizeof(current_root) - 1);
        current_root[sizeof(current_root) - 1] = '\0';

        while (current_root[0])
        {
            char check_path[MAX_PATH_SIZE + 32];
            snprintf(check_path, sizeof(check_path), "%s/std.zc", current_root);
            if (access(check_path, F_OK) == 0)
            {
                // Found it!
                zfree(cfg->root_path);
                cfg->root_path = xstrdup(current_root);
                break;
            }
            snprintf(check_path, sizeof(check_path), "%s/share/zenc/std.zc", current_root);
            if (access(check_path, F_OK) == 0)
            {
                zfree(cfg->root_path);
                snprintf(check_path, sizeof(check_path), "%s/share/zenc", current_root);
                cfg->root_path = xstrdup(check_path);
                break;
            }

            // Try parent
            char *last_slash = (char *)strrchr(current_root, '/');
            if (last_slash && last_slash != current_root)
            {
                *last_slash = '\0';
            }
            else
            {
                break; // Reached root or no more slashes
            }
        }
    }
    else
    {
        // Fallback to std_root in config if self path fails
        if (cfg->std_root[0])
        {
            zfree(cfg->root_path);
            cfg->root_path = xstrdup(cfg->std_root);
        }
    }
}
