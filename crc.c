/* crc.c -- Compute CRCs
   Copyright (c) 1995-1996 by Eberhard Mattes

This file is part of fst.

fst is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

fst is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with fst; see the file COPYING.  If not, write to
the Free Software Foundation, 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */


#include <stdlib.h>
#include "crc.h"

#define CRC_POLYNOMIAL 0x4c11db7

static crc_t crc_table[256];

void crc_build_table (void)
{
  int i, j;
  crc_t t;

  crc_table[0] = 0;
  for (i = 0, j = 0; i < 128; ++i, j += 2)
    {
      t = crc_table[i] << 1;
      if (crc_table[i] & 0x80000000)
        {
          crc_table[j+0] = t ^ CRC_POLYNOMIAL;
          crc_table[j+1] = t;
        }
      else
        {
          crc_table[j+0] = t;
          crc_table[j+1] = t ^ CRC_POLYNOMIAL;
        }
    }
}


crc_t crc_compute (const unsigned char *src, size_t size)
{
  size_t i;
  crc_t crc;

  crc = ~0;
  for (i = 0; i < size; ++i)
    crc = (crc << 8) ^ crc_table[(crc >> 24) ^ src[i]];
  return ~crc;
}
