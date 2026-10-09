/* Freestanding C types for LVGL; no host C runtime is linked into firmware.
 * SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef FLANGE_STD_H
#define FLANGE_STD_H
#include <Base.h>
typedef __INT8_TYPE__ int8_t;
typedef __UINT8_TYPE__ uint8_t;
typedef __INT16_TYPE__ int16_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __INT32_TYPE__ int32_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __INT64_TYPE__ int64_t;
typedef __UINT64_TYPE__ uint64_t;
typedef __INTMAX_TYPE__ intmax_t;
typedef __UINTMAX_TYPE__ uintmax_t;
typedef __INTPTR_TYPE__ intptr_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef _Bool bool;
#define true 1
#define false 0
#define INT8_MIN (-127 - 1)
#define INT8_MAX 127
#define UINT8_MAX 255U
#define INT16_MIN (-32767 - 1)
#define INT16_MAX 32767
#define UINT16_MAX 65535U
#define INT32_MIN (-2147483647 - 1)
#define INT32_MAX 2147483647
#define UINT32_MAX 4294967295U
#define INT64_MAX __INT64_MAX__
#define UINT64_MAX __UINT64_MAX__
#define INT_MAX __INT_MAX__
#define INT_MIN (-INT_MAX - 1)
#define UINT_MAX (INT_MAX * 2U + 1U)
#define LONG_MAX __LONG_MAX__
#define ULONG_MAX (LONG_MAX * 2UL + 1UL)
#define SIZE_MAX __SIZE_MAX__
#define UINTPTR_MAX __UINTPTR_MAX__
#define CHAR_BIT 8
#define PRId32 "d"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRId64 "lld"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define offsetof(type, member) __builtin_offsetof(type, member)
typedef __builtin_va_list va_list;
#define va_start(v, p) __builtin_va_start(v, p)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v, t) __builtin_va_arg(v, t)
#define va_copy(d, s) __builtin_va_copy(d, s)
#endif
