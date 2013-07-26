/* diskio.h -- Header file for diskio.c
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


/* Bits for diskio_open()'s FLAG argument and diskio_type()'s return
   value. */

#define DIO_DISK        0x01    /* Direct disk access */
#define DIO_SNAPSHOT    0x02    /* Snapshot file */
#define DIO_CRC         0x04    /* CRC file */

/* Hide the implementation of DISKIO. */

struct diskio;
typedef struct diskio DISKIO;

/* Type of the save file. */

enum save_type
{
  SAVE_RAW,
  SAVE_SNAPSHOT,
  SAVE_CRC
};

/* Method for accessing the disk. */

enum access_type
{
  ACCESS_DASD,                  /* DosRead, DosWrite */
  ACCESS_LOG_TRACK              /* Logical: DSK_READTRACK, DSK_WRITETRACK */
};

/* Magic numbers for various file types. */

#define CRC_MAGIC               0xac994df4
#define SNAPSHOT_MAGIC          0xaf974803

/* XOR the first 32-bit word of a save file with this value. */

#define SNAPSHOT_SCRAMBLE       0x551234af


/* This header is used for snapshot files and CRC files. */

typedef union
{
  BYTE raw[512];                /* Force the header size to 512 bytes */
  ULONG magic;                  /* Magic number */
  struct
    {
      ULONG magic;              /* Magic number */
      ULONG sector_count;       /* Number of sectors in the snapshot */
      ULONG map_pos;            /* Relative byte address of the sector table */
      ULONG version;            /* Format version number */
    } s;                        /* Header for snapshot file */
  struct
    {
      ULONG magic;              /* Magic number */
      ULONG sector_count;       /* Number of sectors (CRCs) in the snapshot */
      ULONG version;            /* Format version number */
    } c;                        /* Header for CRC file */
} header;

typedef struct
{
  ULONG cyl;
  ULONG head;
  ULONG sec;
} cyl_head_sec;

/* See diskio.c */
extern enum access_type diskio_access;
extern char write_enable;
extern char removable_allowed;
extern char ignore_lock_error;
extern char dont_lock;

extern enum save_type save_type;
extern FILE *save_file;
extern const char *save_fname;
extern ULONG save_sector_count;
extern ULONG save_sector_alloc;
extern ULONG *save_sector_map;


/* See diskio.c */
DISKIO *diskio_open (PCSZ fname, unsigned flags, int for_write);
void diskio_close (DISKIO *d);
unsigned diskio_type (DISKIO *d);
ULONG diskio_total_sectors (DISKIO *d);
ULONG diskio_snapshot_sectors (DISKIO *d);
ULONG *diskio_snapshot_sort (DISKIO *d);
void diskio_crc_load (DISKIO *d);
int diskio_cyl_head_sec (DISKIO *d, cyl_head_sec *dst, ULONG secno);
void save_sec (const void *src, ULONG sec, ULONG count);
void save_create (const char *avoid_fname, enum save_type type);
void save_error (void);
void save_close (void);
ULONG find_sec_in_snapshot (DISKIO *d, ULONG n);
void read_sec (DISKIO *d, void *dst, ULONG sec, ULONG count, int save);
int crc_sec (DISKIO *d, crc_t *pcrc, ULONG secno);
int write_sec (DISKIO *d, const void *src, ULONG sec);
