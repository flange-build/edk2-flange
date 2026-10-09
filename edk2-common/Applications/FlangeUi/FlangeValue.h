/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef FLANGE_VALUE_H
#define FLANGE_VALUE_H
#ifdef FLANGE_UEFI
#include <FlangeStd.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif
bool FlangeParseNumber(const uint16_t *Text, size_t Base, bool Signed, size_t Width, uint64_t *Out);
bool FlangeValidDate(unsigned Year, unsigned Month, unsigned Day);
#endif
