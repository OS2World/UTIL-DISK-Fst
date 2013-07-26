/* fat.h -- FAT definitions
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


#pragma pack(1)

typedef struct
{
  BYTE name[8+3];
  BYTE attr;
  BYTE reserved[8];
  USHORT ea;
  USHORT time;
  USHORT date;
  USHORT cluster;
  ULONG size;
} FAT_DIRENT;

typedef struct
{
  BYTE flag;
  USHORT name1[5];
  BYTE attr;
  BYTE reserved;
  BYTE checksum;
  USHORT name2[6];
  USHORT cluster;
  USHORT name3[2];
} VFAT_DIRENT;

typedef union
{
  BYTE raw[512];
  struct
    {
      BYTE   jump[3];
      BYTE   oem[8];
      USHORT bytes_per_sector;
      BYTE   sectors_per_cluster;
      USHORT reserved_sectors;
      BYTE   fats;
      USHORT root_entries;
      USHORT sectors;
      BYTE   media;
      USHORT sectors_per_fat;
      USHORT sectors_per_track;
      USHORT heads;
      USHORT hidden_sectors_lo;
      USHORT hidden_sectors_hi;
      ULONG  large_sectors;
      BYTE   drive_no;
      BYTE   reserved;
      BYTE   extended_sig;
      ULONG  vol_id;
      BYTE   vol_label[11];
      BYTE   vol_type[8];
    } boot;
  struct
    {
      BYTE   magic[2];          /* "ED" */
      USHORT unused[15];
      USHORT table[240];
    } ea1;
  struct
    {
      BYTE    magic[2];         /* "EA" */
      USHORT  rel_cluster;
      ULONG   need_eas;
      BYTE    name[14];
      BYTE    unknown[4];
      FEALIST fealist;
    } ea3;
  } FAT_SECTOR;
#pragma pack()
