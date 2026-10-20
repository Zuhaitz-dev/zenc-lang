// SPDX-License-Identifier: MIT
#include "codegen/codegen.h"
#include "parser/parser.h"
#include "constants.h"
#include "tools/tool_common.h"
#if ZC_HAS_PLUGINS
#include "plugins/plugin_manager.h"
#endif
#include "zprep.h"
#include "analysis/typecheck.h"
#include "codegen/compat.h"
#include "driver/driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "utils/cmd.h"
#include "diagnostics/diagnostics.h"
#include <signal.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

// The standalone tools (zc-lsp, zc-repl, zc-doc, zc-format) ship alongside
// zc. `zc lsp|repl|doc|format` are deprecated wrappers that dispatch to them.
static int exec_tool(const char *cmd, const char *tool, int argc, char **argv)
{
    fprintf(stderr, "note: 'zc %s' is deprecated; use '%s' directly\n", cmd, tool);
    fflush(stderr);

    // The tools are standalone binaries and do not expect the `zc` subcommand
    // in argv, so forward everything after it.
    char *forward[128];
    int n = 0;
    forward[n++] = NULL; // argv[0] placeholder (replaced with the tool path)
    for (int i = 2; i < argc && n < 127; i++)
    {
        forward[n++] = argv[i];
    }
    forward[n] = NULL;

    // z_get_executable_path returns the directory of the executable.
    char self_path[MAX_PATH_SIZE];
    z_get_executable_path(self_path, sizeof(self_path));

    char path[MAX_PATH_SIZE + 32];
    if (self_path[0])
    {
        snprintf(path, sizeof(path), "%s/%s", self_path, tool);
    }
    else
    {
        snprintf(path, sizeof(path), "%s", tool);
    }

    forward[0] = path;
    execv(path, forward);
    execvp(tool, forward);

    fprintf(stderr, "error: could not launch '%s' (is it installed?)\n", tool);
    return 127;
}

int main(int argc, char **argv)
{
    ZenCompiler compiler = {0};
    CompilerConfig *cfg = &compiler.config;
    z_compiler_setup(&compiler);

    int i;
    const char *optimization_level = NULL;
    char *env_root;
    char *input_file_copy;

    i = 0;
    (void)i;
    (void)optimization_level;
    (void)env_root;
    (void)input_file_copy;

    if (argc < 2)
    {
        print_usage();
        return 1;
    }

    // Parse command
    char *command = argv[1];
    int arg_start = 2;

    if (strcmp(command, "lsp") == 0)
    {
        return exec_tool("lsp", "zc-lsp", argc, argv);
    }
    else if (strcmp(command, "repl") == 0)
    {
        return exec_tool("repl", "zc-repl", argc, argv);
    }
    else if (strcmp(command, "doc") == 0)
    {
        return exec_tool("doc", "zc-doc", argc, argv);
    }
    else if (strcmp(command, "format") == 0)
    {
        return exec_tool("format", "zc-format", argc, argv);
    }
    else if (strcmp(command, "transpile") == 0 || strcmp(command, "-c") == 0)
    {
        cfg->mode_transpile = 1;
        cfg->emit_c = 1; // Transpile implies emitting C
    }
    else if (strcmp(command, "run") == 0)
    {
        cfg->mode_run = 1;
    }
    else if (strcmp(command, "debug") == 0)
    {
        cfg->mode_debug = 1;
        cfg->mode_run = 1;
    }
    else if (strcmp(command, "check") == 0)
    {
        cfg->mode_check = 1;
    }
    else if (strcmp(command, "help") == 0)
    {
        if (argc > 2)
        {
            print_command_help(argv[2]);
        }
        else
        {
            print_usage();
        }
        return 0;
    }
    else if (strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0)
    {
        print_usage();
        return 0;
    }
    else if (command[0] == '-')
    {
        // implicit build or run? assume build if starts with flag, but usually
        // command first If file provided directly: "zc file.zc" -> build
        if (strchr(command, '.'))
        {
            // treat as filename
            cfg->input_file = command;
            arg_start = 2; // already consumed
        }
        else
        {
            // Flags
            arg_start = 1;
        }
    }
    else if (strcmp(command, "build") == 0)
    {
        // Building is the default mode.
    }
    else if (strchr(command, '.'))
    {
        // "zc file.zc": build it
        cfg->input_file = command;
        arg_start = 2;
    }
    else
    {
        fprintf(stderr, "error: unknown command '%s'\n\n", command);
        print_usage();
        return 1;
    }

    // Parse args
    for (i = arg_start; i < argc; i++)
    {
        char *arg = argv[i];
        if (strcmp(arg, "--emit-c") == 0)
        {
            cfg->emit_c = 1;
        }
        else if (strcmp(arg, "--json") == 0)
        {
            compiler.diag_consumer = diag_render_json;
        }
        else if (strcmp(arg, "--keep-comments") == 0)
        {
            cfg->keep_comments = 1;
        }
        else if (strcmp(arg, "--recursive-doc") == 0 || strcmp(arg, "--no-recursive-doc") == 0)
        {
            fprintf(stderr, "note: --recursive-doc only applies to 'zc doc' (now 'zc-doc')\n");
        }
        else if (strcmp(arg, "--version") == 0 || strcmp(arg, "-V") == 0)
        {
            // Handled later
        }
        else if (strcmp(arg, "--paths") == 0)
        {
            // Handled later
        }
        else if (strcmp(arg, "--verbose") == 0 || strcmp(arg, "-v") == 0)
        {
            cfg->verbose = 1;
        }
        else if (strcmp(arg, "--quiet") == 0 || strcmp(arg, "-q") == 0)
        {
            cfg->quiet = 1;
        }
        else if (strcmp(arg, "--check") == 0 || strcmp(arg, "-c") == 0)
        {
            cfg->mode_check = 1;
            cfg->use_typecheck = 1;
        }
        else if (strcmp(arg, "--no-check") == 0)
        {
            cfg->mode_check = 0;
            cfg->use_typecheck = 0;
        }
        else if (strcmp(arg, "--misra") == 0)
        {
            cfg->misra_mode = 1;
            cfg->use_typecheck = 1;
            zvec_push_Str(&cfg->cfg_defines, xstrdup("misra"));
            zvec_push_Str(&cfg->cfg_defines, xstrdup("ZC_MISRA"));
        }
        else if (strcmp(arg, "--backend") == 0 && i + 1 < argc)
        {
            cfg->backend_name = argv[++i];
            if (strcmp(cfg->backend_name, "cpp") == 0)
            {
                cfg->use_cpp = 1;
            }
        }
        else if (strcmp(arg, "--backend-opt") == 0 && i + 1 < argc)
        {
            zvec_push_Str(&cfg->backend_opts, xstrdup(argv[++i]));
        }
        else if (strcmp(arg, "--freestanding") == 0)
        {
            cfg->is_freestanding = 1;
            zvec_push_Str(&cfg->cfg_defines, xstrdup("freestanding"));
            zvec_push_Str(&cfg->cfg_defines, xstrdup("ZC_FREESTANDING"));
        }
        else if (strcmp(arg, "--warn-errors") == 0)
        {
            cfg->warn_as_errors = 1;
        }
        else if (strcmp(arg, "--no-suppress-warnings") == 0)
        {
            cfg->no_suppress_warnings = 1;
        }
        else if (strcmp(arg, "--cpp") == 0)
        {
            if (z_is_windows())
            {
                strncpy(cfg->cc, "g++.exe", sizeof(cfg->cc) - 1);
                cfg->cc[sizeof(cfg->cc) - 1] = '\0';
            }
            else
            {
                strncpy(cfg->cc, "g++", sizeof(cfg->cc) - 1);
                cfg->cc[sizeof(cfg->cc) - 1] = '\0';
            }
            cfg->use_cpp = 1;
            cfg->backend_name = "cpp";
        }
        else if (strcmp(arg, "--cuda") == 0)
        {
            if (z_is_windows())
            {
                strncpy(cfg->cc, "nvcc.exe", sizeof(cfg->cc) - 1);
                cfg->cc[sizeof(cfg->cc) - 1] = '\0';
            }
            else
            {
                strncpy(cfg->cc, "nvcc", sizeof(cfg->cc) - 1);
                cfg->cc[sizeof(cfg->cc) - 1] = '\0';
            }
            cfg->use_cuda = 1;
            cfg->use_cpp = 1; // CUDA implies C++ mode.
            cfg->backend_name = "cuda";
        }
        else if (strcmp(arg, "--objc") == 0 || strcmp(arg, "--objective-c") == 0)
        {
            cfg->use_objc = 1;
            cfg->backend_name = "objc";
            if (!cfg->cc[0] || strcmp(cfg->cc, "gcc") == 0)
            {
                if (z_is_windows())
                {
                    snprintf(cfg->cc, sizeof(cfg->cc), "gcc");
                }
                else if (strcmp(z_get_system_name(), "macos") == 0)
                {
                    snprintf(cfg->cc, sizeof(cfg->cc), "clang");
                }
                else
                {
                    snprintf(cfg->cc, sizeof(cfg->cc), "gcc");
                }
            }
        }
        else if (strcmp(arg, "--filcc") == 0)
        {
            // Auto-discover bundled Fil-C compiler relative to known paths
            const char *search_paths[] = {cfg->root_path, NULL};
            int found = 0;
            for (int pi = 0; search_paths[pi]; pi++)
            {
                if (!search_paths[pi] || !search_paths[pi][0])
                {
                    continue;
                }
                char path[MAX_PATH_SIZE];
                snprintf(path, sizeof(path), "%s/filc-0.678-linux-x86_64/build/bin/filcc",
                         search_paths[pi]);
                if (access(path, X_OK) == 0)
                {
                    size_t plen = strlen(path);
                    if (plen >= sizeof(cfg->cc))
                    {
                        plen = sizeof(cfg->cc) - 1;
                    }
                    memcpy(cfg->cc, path, (size_t)(plen));
                    cfg->cc[plen] = '\0';
                    char libpath[MAX_PATH_SIZE + 32];
                    snprintf(libpath, sizeof(libpath), "%s/filc-0.678-linux-x86_64/pizfix/lib",
                             search_paths[pi]);
#if ZC_OS_WINDOWS
                    SetEnvironmentVariableA("FILC_LIBRARY_PATH", libpath);
#else
                    setenv("FILC_LIBRARY_PATH", libpath, 1);
#endif
                    found = 1;
                    break;
                }
            }
            if (!found)
            {
                snprintf(cfg->cc, sizeof(cfg->cc), "filcc");
            }
        }
        else if (strcmp(arg, "--cc") == 0)
        {
            if (i + 1 < argc)
            {
                char *cc_arg = argv[++i];
                // Handle "zig" shorthand for "zig cc"
                if (strcmp(cc_arg, "zig") == 0)
                {
                    if (z_is_windows())
                    {
                        strncpy(cfg->cc, "zig.exe cc", sizeof(cfg->cc) - 1);
                        cfg->cc[sizeof(cfg->cc) - 1] = '\0';
                    }
                    else
                    {
                        strncpy(cfg->cc, "zig cc", sizeof(cfg->cc) - 1);
                        cfg->cc[sizeof(cfg->cc) - 1] = '\0';
                    }
                }
                else
                {
                    snprintf(cfg->cc, sizeof(cfg->cc), "%s", cc_arg);
                    if (z_is_windows() && !z_path_has_extension(cfg->cc, ".exe"))
                    {
                        strcat(cfg->cc, ".exe");
                    }
                }
            }
        }
        else if (strncmp(arg, "-o", 2) == 0)
        {
            if (strlen(arg) > 2)
            {
                cfg->output_file = arg + 2;
            }
            else if (i + 1 < argc)
            {
                cfg->output_file = argv[++i];
            }
            else
            {
                fprintf(stderr, COLOR_BOLD COLOR_RED "error" COLOR_RESET
                                                     ": missing output filename after '-o'\n");
                return 1;
            }
        }
        else if (strncmp(arg, "-I", 2) == 0)
        {
            char *i_path = NULL;
            if (strlen(arg) > 2)
            {
                i_path = arg + 2;
            }
            else if (i + 1 < argc)
            {
                i_path = argv[++i];
            }
            if (i_path)
            {
                append_flag(cfg->gcc_flags, sizeof(cfg->gcc_flags), arg, NULL);
                zvec_push_Str(&cfg->include_paths, xstrdup(i_path));
            }
        }
        else if (strncmp(arg, "-L", 2) == 0 || strncmp(arg, "-l", 2) == 0)
        {
            char prefix[3] = {arg[0], arg[1], '\0'};
            if (strlen(arg) > 2)
            {
                append_flag(compiler.link_flags, MAX_FLAGS_SIZE, prefix, arg + 2);
            }
            else if (i + 1 < argc)
            {
                append_flag(compiler.link_flags, MAX_FLAGS_SIZE, prefix, argv[++i]);
            }
        }
        else if (strncmp(arg, "-O", 2) == 0)
        {
            if (strlen(arg) > 2)
            {
                optimization_level = arg + 2;
                append_flag(cfg->gcc_flags, sizeof(cfg->gcc_flags), "-O", optimization_level);
            }
            else if (i + 1 < argc)
            {
                optimization_level = argv[++i];
                append_flag(cfg->gcc_flags, sizeof(cfg->gcc_flags), "-O", optimization_level);
            }
        }
        else if (strcmp(arg, "-g") == 0)
        {
            cfg->mode_debug = 1;
        }
        else if (strcmp(arg, "-g0") == 0 || strcmp(arg, "--no-debug") == 0)
        {
            cfg->mode_debug = 0;
        }
        else if (strcmp(arg, "--release") == 0)
        {
            cfg->mode_debug = 0;
            append_flag(cfg->gcc_flags, sizeof(cfg->gcc_flags), "-O3", NULL);
        }
        else if (strncmp(arg, "-D", 2) == 0)
        {
            const char *def = (strlen(arg) > 2) ? arg + 2 : NULL;
            if (!def && i + 1 < argc)
            {
                i++;
                def = argv[i];
            }
            if (def)
            {
                char *name = xstrdup(def);
                char *eq = (char *)strchr(name, '=');
                if (eq)
                {
                    *eq = '\0';
                }
                zvec_push_Str(&cfg->cfg_defines, name);
            }
            append_flag(cfg->gcc_flags, sizeof(cfg->gcc_flags), "-D", def);
        }
        else if (strncmp(arg, "-Wno-", 5) == 0)
        {
            if (!set_diag_by_name(cfg, arg + 5, 0))
            {
                append_flag(cfg->gcc_flags, sizeof(cfg->gcc_flags), arg, NULL);
            }
        }
        else if (strncmp(arg, "-W", 2) == 0)
        {
            if (!set_diag_by_name(cfg, arg + 2, 1))
            {
                append_flag(cfg->gcc_flags, sizeof(cfg->gcc_flags), arg, NULL);
                if (!cfg->warn_as_errors && strcmp(arg, "-Werror") == 0)
                {
                    cfg->warn_as_errors = 1;
                }
            }
        }
        else if (strncmp(arg, "-f", 2) == 0 || strncmp(arg, "-m", 2) == 0 ||
                 strncmp(arg, "-x", 2) == 0 || strcmp(arg, "-S") == 0 || strcmp(arg, "-E") == 0 ||
                 strcmp(arg, "-shared") == 0 || strcmp(arg, "--shared") == 0)
        {
            // Standard C compiler flags that we want to pass directly to the backend
            append_flag(cfg->gcc_flags, sizeof(cfg->gcc_flags), arg, NULL);
            if (strcmp(arg, "-shared") == 0 || strcmp(arg, "--shared") == 0)
            {
                append_flag(cfg->gcc_flags, sizeof(cfg->gcc_flags), "-fPIC", NULL);
            }
        }
        else if (arg[0] == '-')
        {
            // Check if this is a backend alias
            const char *alias_opt = codegen_alias_lookup(arg);
            if (alias_opt)
            {
                zvec_push_Str(&cfg->backend_opts, xstrdup(alias_opt));
            }
            else
            {
                // Unknown flag, pass to C compiler just in case
                append_flag(cfg->gcc_flags, sizeof(cfg->gcc_flags), arg, NULL);
            }
        }
        else if (arg[0] != '\0')
        {
            if (!cfg->input_file)
            {
                cfg->input_file = arg;
            }
            else
            {
                zvec_push_Str(&cfg->extra_files, arg);
            }
        }
    }

    for (i = arg_start; i < argc; i++)
    {
        char *arg = argv[i];
        if (arg && (strcmp(arg, "--version") == 0 || strcmp(arg, "-V") == 0))
        {
            print_version();
            return 0;
        }
        else if (strcmp(arg, "--paths") == 0)
        {
            print_search_paths(cfg);
            return 0;
        }
    }

    if (!cfg->input_file)
    {
        fprintf(stderr, COLOR_BOLD COLOR_RED "error" COLOR_RESET ": no input file specified\n");
        return 1;
    }

    // Compute input directory
    input_file_copy = xstrdup(cfg->input_file);
    char *last_slash = z_path_last_sep(input_file_copy);
    if (last_slash)
    {
        *last_slash = 0;
        cfg->input_dir = xstrdup(input_file_copy);
    }
    else
    {
        cfg->input_dir = xstrdup(".");
    }
    zfree(input_file_copy);

    return driver_run(&compiler);
}
