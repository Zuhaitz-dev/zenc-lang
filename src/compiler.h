// SPDX-License-Identifier: MIT
#ifndef COMPILER_H
#define COMPILER_H

/**
 * @file compiler.h
 * @brief Compiler configuration + global state + OS functions.
 *
 * Includes compiler_config.h for struct definitions plus platform/os.h
 * for OS detection/utility functions.
 * For files that only need CompilerConfig/ZenCompiler without OS deps,
 * include compiler_config.h instead.
 */

#include "compiler_config.h"
#include "arena.h"
#include "platform/os.h"

extern ZenCompiler g_compiler;
extern zarena g_arena;

#define g_config g_compiler.config
#define g_link_flags g_compiler.link_flags
#define g_cflags g_compiler.cflags
#define g_error_count g_compiler.error_count
#define g_warning_count g_compiler.warning_count
#define g_start_time g_compiler.start_time

#endif // COMPILER_H
