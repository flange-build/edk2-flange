/* Bounded UTF-16 conversion, preserving complete code points at truncation.
 * SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "FlangeText.h"
void FlangeUtf16ToUtf8(const uint16_t *Source, char *Output, size_t Size)
{
  size_t Used = 0;
  if(!Size) return;
  if(Source) while(*Source) {
    uint32_t Code = *Source++;
    if(Code == 0xfff0 || Code == 0xfff1) continue; /* HII width directives. */
    if(Code >= 0xd800 && Code <= 0xdbff) {
      if(*Source >= 0xdc00 && *Source <= 0xdfff) Code = 0x10000 + ((Code - 0xd800) << 10) + (*Source++ - 0xdc00);
      else Code = 0xfffd;
    } else if(Code >= 0xdc00 && Code <= 0xdfff) Code = 0xfffd;
    /* Strip control codes in labels supplied by boot-option variables. */
    if(Code < 0x20 || Code == 0x7f) Code = ' ';
    size_t Bytes = Code < 0x80 ? 1 : Code < 0x800 ? 2 : Code < 0x10000 ? 3 : 4;
    if(Used + Bytes >= Size) break;
    if(Bytes == 1) Output[Used++] = (char)Code;
    else {
      Output[Used++] = (char)((Bytes == 2 ? 0xc0 : Bytes == 3 ? 0xe0 : 0xf0) | (Code >> (6 * (Bytes - 1))));
      for(size_t Index=Bytes-1; Index>0; Index--) Output[Used++] = (char)(0x80 | ((Code >> (6 * (Index-1))) & 0x3f));
    }
  }
  Output[Used] = 0;
}
