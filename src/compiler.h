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

extern zarena g_arena;

#endif // COMPILER_H
