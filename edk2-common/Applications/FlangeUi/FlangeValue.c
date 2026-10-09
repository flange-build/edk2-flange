/* Bounded IFR numeric/date input validation, shared with host regression tests.
 * SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "FlangeValue.h"
bool FlangeParseNumber(const uint16_t *Text,size_t Base,bool Signed,size_t Width,uint64_t *Out)
{
  if(!Text || !Out || (Base!=10 && Base!=16) || (Width!=1 && Width!=2 && Width!=4 && Width!=8)) return false;
  bool Negative=false; uint64_t U=0; size_t I=0;
  if(Text[I]=='-' && Signed) { Negative=true; I++; }
  if(Base==16 && Text[I]=='0' && (Text[I+1]=='x'||Text[I+1]=='X')) I+=2;
  if(!Text[I]) return false;
  uint64_t Limit=Width==8?UINT64_MAX:((uint64_t)1<<(Width*8))-1;
  if(Signed) Limit=(Limit>>1)+(Negative?1:0);
  for(;Text[I];I++) {
    unsigned D=Text[I]>='0'&&Text[I]<='9'?Text[I]-'0':Text[I]>='a'&&Text[I]<='f'?Text[I]-'a'+10:Text[I]>='A'&&Text[I]<='F'?Text[I]-'A'+10:255;
    if(D>=Base || U>(Limit-D)/Base) return false;
    U=U*Base+D;
  }
  *Out=Negative?0-U:U; return true;
}
bool FlangeValidDate(unsigned Year,unsigned Month,unsigned Day)
{
  static const unsigned Days[]={31,28,31,30,31,30,31,31,30,31,30,31};
  if(Year<1900 || Year>9999 || Month<1 || Month>12 || Day<1) return false;
  unsigned Max=Days[Month-1];
  if(Month==2 && !(Year%4) && (Year%100 || !(Year%400))) Max++;
  return Day<=Max;
}
