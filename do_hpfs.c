/* do_hpfs.c -- HPFS-specific code for fst
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


#define INCL_DOSDEVIOCTL
#define INCL_DOSNLS
#define INCL_DOSERRORS
#include <os2.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "fst.h"
#include "crc.h"
#include "diskio.h"
#include "hpfs.h"

/* Return a non-zero value if sector X is allocated. */

#define ALLOCATED(x)    !BITSETP (alloc_vector, (x))


/* Our own representation of a code page. */

typedef struct
{
  CPINFOENTRY info;
  char hit;
  BYTE case_map[256];
  BYTE case_map_changed[256];
} MYCP;

/* This structure is used for checking whether DIRENTs are arranged in
   ascending order by file name. */

typedef struct
{
  BYTE name[256];               /* Previous file name */
  ULONG cpindex;                /* Code page index for previous file name */
} SORT;

/* This structure is used for counting extents. */

typedef struct
{
  ULONG size;                   /* Number of elements in `counts' */
  ULONG *counts;                /* counts[i] = # of objects with i extents */
} EXTENTS;

static ULONG total_sectors;     /* Total number of sectors of HPFS volume */
static ULONG total_alloc;       /* # of bytes allocated for ïalloc_vector' */
static BYTE *usage_vector;      /* One byte per sector, indicating usage */
static BYTE *seen_vector;       /* One byte per sector, to avoid loops */
static BYTE *alloc_vector;      /* One bit per sector, indicating allocation */
static const path_chain **path_vector; /* One path name chain per sector */
static BYTE alloc_ready;        /* TRUE if alloc_vector contents valid */
static ULONG code_page_count;   /* Number of code pages */
static MYCP *code_pages;        /* All code pages of the HPFS volume */
static ULONG cpdata_count;      /* Number of code page data sectors */
static ULONG *cpdata_visited;   /* Sector numbers of visited cp data sectors */
static ULONG min_time;          /* Minimum valid time stamp */
static ULONG dirband_start;     /* First sector of DIRBLK band */
static ULONG dirband_end;       /* Last sector of DIRBLK band */
static ULONG dirblk_total;      /* Total number of DIRBLKs */
static ULONG dirblk_outside;    /* Number of DIRBLKs outside DIRBLK band */
static ULONG alsec_count;       /* Number of ALSECs */
static ULONG file_count;        /* Number of files */
static ULONG dir_count;         /* Number of directories */
static ULONG sectors_per_block; /* Block size (in sectors) for `multimedia' */
static EXTENTS file_extents;    /* Number of extents for files */
static EXTENTS ea_extents;      /* Number of extents for EAs */
static char no_country_sys;     /* COUNTRY.SYS not available */
static char alsec_number[100];  /* Formatted ALSEC number */
static char find_comp[256];     /* Current component of `find_path' */
static char copy_buf[512];      /* Buffer for `copy' action */

static void dirblk_warning (int level, const char *fmt, ULONG secno,
                            const path_chain *path, ...) ATTR_PRINTF (2, 5);
static void dirent_warning (int level, const char *fmt, ULONG secno,
                            const path_chain *path, int dirent_no,
                            const char *fname, ...) ATTR_PRINTF (2, 7);
static void alsec_warning (int level, const char *fmt, ULONG secno,
                           const path_chain *path, ...) ATTR_PRINTF (2, 5);
static void alloc_warning (int level, const char *fmt, ULONG secno,
                           const path_chain *path,
                           int fnode_flag, ...) ATTR_PRINTF (2, 6);
static void fnode_warning (int level, const char *fmt, ULONG secno,
                           const path_chain *path, ...) ATTR_PRINTF (2, 5);


/* Compute the checksum for SIZE bytes at P. */

static ULONG chksum (const BYTE *p, size_t size)
{
  ULONG sum;

  sum = 0;
  while (size != 0)
    {
      sum += *p++;
      sum = (sum << 7) | (sum >> 25); /* Rotate left by 7 bits */
      --size;
    }
  return sum;
}


/* Return a pointer to a string containing a formatted time stamp.
   Note that the pointer points to static memory; do not use
   format_time() more than once in one expression! */

static const char *format_time (ULONG x)
{
  static char buf[100];
  time_t t;
  struct tm *tm;

  if (x == 0)
    strcpy (buf, "never");
  else if (x < min_time)
    sprintf (buf, "0x%lx", x);
  else
    {
      t = (time_t)x;
      tm = gmtime (&t);
      sprintf (buf, "0x%lx (%d-%.2d-%.2d %.2d:%.2d:%.2d)",
               x, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
               tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
  return buf;
}


/* Return a pointer to a string containing a formatted time stamp for
   the `dir' action.  Note that the pointer points to static memory;
   do not use format_dir_time() more than once in one expression! */

static const char *format_dir_time (ULONG x)
{
  static char buf[100];
  time_t t;
  struct tm *tm;

  if (x < min_time)
    return "????-??-?? ??:??:??";
  else
    {
      t = (time_t)x;
      tm = gmtime (&t);
      sprintf (buf, "%d-%.2d-%.2d %.2d:%.2d:%.2d",
               tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
               tm->tm_hour, tm->tm_min, tm->tm_sec);
      return buf;
    }
}


/* Return TRUE iff the file name pointed to by NAME would be a valid
   file name on a FAT file system.  HPFS.IFS seems to treat the space
   character (0x20) as being valid on FAT. */

static int is_fat_name (const BYTE *name)
{
  int n, len;
  const BYTE *p;

  if (name[0] == '.')
    return name[1] == 0 || (name[1] == '.' && name[2] == 0);
  len = strlen ((const char *)name);
  p = (const BYTE *)strchr ((const char *)name, '.');
  if (p == NULL)
    n = len;
  else
    n = p - name;
  if (n > 8)
    return FALSE;
  if (p != NULL)
    {
      if (p[-1] == ' ')         /* (p > name) is guaranteed */
        return FALSE;
      if (len - (n + 1) > 3)
        return FALSE;
      if (strchr ((const char *)p + 1, '.') != NULL)
        return FALSE;
    }
  for (p = name; *p != 0; ++p)
    if (*p < 0x20 || strchr ("\"*+,/;:<=>?[\\]|", *p) != NULL)
      return FALSE;
  return TRUE;
}


/* Return TRUE iff the file name pointed to by NAME is a valid HPFS
   file name.  "." and ".." are considered invalid by this
   function. */

static int is_hpfs_name (const BYTE *name)
{
  const BYTE *p;

  for (p = name; *p != 0; ++p)
    if (*p < 0x20 || strchr ("\"*/:<>?\\|", *p) != NULL)
      return FALSE;
  if (p == name)
    return FALSE;
  if (p[-1] == '.' || p[-1] == ' ')
    return FALSE;
  return TRUE;
}


/* Initialize an EXTENTS structure. */

static void extents_init (EXTENTS *e)
{
  e->size = 0;
  e->counts = NULL;
}


/* Free any resources of an EXTENTS structure. */

static void extents_exit (EXTENTS *e)
{
  e->size = 0;
  free (e->counts);
  e->counts = NULL;
}


/* Add an object to an EXTENTS structure.  The object has COUNT
   extents. */

static void extents_stat (EXTENTS *e, ULONG count)
{
  if (count >= e->size)
    {
      ULONG new_size, i;

      new_size = (count | 0xff) + 1;
      e->counts = realloc (e->counts, new_size * sizeof (*e->counts));
      for (i = e->size; i < new_size; ++i)
        e->counts[i] = 0;
      e->size = new_size;
    }
  e->counts[count] += 1;
}


/* Show the statistics collected in an EXTENTS structure. */

static void extents_show (const EXTENTS *e, const char *msg)
{
  ULONG i;

  info ("\nFragmentation of %s:\n", msg);
  info ("Extents | Number\n");
  info ("--------+-------\n");
  for (i = 0; i < e->size; ++i)
    if (e->counts[i] != 0)
      info ("%7lu | %lu\n", i, e->counts[i]);
}


/* Bits in seen_vector[]. */

#define SEEN_FNODE      0x01
#define SEEN_DIRBLK     0x02
#define SEEN_ALSEC      0x04
#define SEEN_BADLIST    0x08
#define SEEN_CPINFOSEC  0x10


/* Set and check a `have seen' bit.  Return TRUE if any of the COUNT
   sectors starting at SECNO has been interpreted as WHAT.  MSG is a
   string describing the type of sector.  This function is used to
   avoid infinite loops. */

static int have_seen (ULONG secno, ULONG count, BYTE what, const char *msg)
{
  int seen;
  ULONG i;

  seen = FALSE;
  for (i = 0; i < count && secno < total_sectors; ++i, ++secno)
    if (seen_vector[secno] & what)
      {
        seen = TRUE;
        warning (1, "Sector #%lu already used for %s", secno, msg);
      }
    else
      seen_vector[secno] |= what;
  return seen;
}


/* Types of sectors. */

#define USE_EMPTY       0
#define USE_SUPER       1
#define USE_SPARE       2
#define USE_BITMAPIND   3
#define USE_BITMAP      4
#define USE_DIRBLKBITMAP 5
#define USE_SPAREDIRBLK 6
#define USE_BANDDIRBLK  7
#define USE_DIRBLK      8
#define USE_FNODE       9
#define USE_SID         10
#define USE_CPINFOSEC   11
#define USE_CPDATASEC   12
#define USE_BAD         13
#define USE_HOTFIXLIST  14
#define USE_HOTFIX      15
#define USE_BADLIST     16
#define USE_FILE        17
#define USE_ALSEC       18
#define USE_EA          19
#define USE_BOOT        20
#define USE_LOADER      21
#define USE_ACL         22

/* Return a pointer to a string containing a description of a sector
   type. */

static const char *sec_usage (BYTE what)
{
  switch (what)
    {
    case USE_EMPTY:
      return "empty";
    case USE_SUPER:
      return "super block";
    case USE_SPARE:
      return "spare block";
    case USE_BITMAPIND:
      return "bitmap indirect block";
    case USE_BITMAP:
      return "bitmap";
    case USE_DIRBLKBITMAP:
      return "DIRBLK band bitmap";
    case USE_SPAREDIRBLK:
      return "spare DIRBLK";
    case USE_BANDDIRBLK:
      return "DIRBLK band";
    case USE_DIRBLK:
      return "DIRBLK";
    case USE_FNODE:
      return "FNODE";
    case USE_SID:
      return "SID";
    case USE_CPINFOSEC:
      return "code page info";
    case USE_CPDATASEC:
      return "code page data";
    case USE_BAD:
      return "bad sector";
    case USE_HOTFIXLIST:
      return "hotfix list";
    case USE_HOTFIX:
      return "hotfix sector";
    case USE_BADLIST:
      return "bad block list";
    case USE_FILE:
      return "file data";
    case USE_ALSEC:
      return "allocation sector";
    case USE_EA:
      return "extended attributes";
    case USE_BOOT:
      return "boot sector";
    case USE_LOADER:
      return "loader";
    case USE_ACL:
      return "ACL";
    default:
      return "INTERNAL_ERROR";
    }
}


/* Use COUNT sectors starting at SECNO for WHAT.  PATH points to the
   path name chain for the file or directory and can be NULL for
   sectors not used by a file or directory. */

static void use_sectors (ULONG secno, ULONG count, BYTE what,
                         const path_chain *path)
{
  BYTE old;

  /* Handle the sectors one by one. */

  while (count > 0)
    {
      /* Before indexing any array with SECNO, check if SECNO is
         valid. */

      if (secno >= total_sectors)
        {
          if (path == NULL)
            warning (1, "Sector number #%lu (%s) is too big",
                     secno, sec_usage (what));
          else
            warning (1, "Sector number #%lu (%s for \"%s\") is too big",
                     secno, sec_usage (what), format_path_chain (path, NULL));
        }
      else
        {
          /* Check usage of the sector.  A previously unused sector
             (USE_EMPTY) can be turned into any type of sector, Spare
             DIRBLKs and sectors in the DIRBLK band can be turned into
             DIRBLK sectors.  Note that reusing a code page data
             sector as code page data sector is ignored here as
             do_cpdatasec() can be called more than once for a single
             sector. */

          old = usage_vector[secno];
          if (old != USE_EMPTY
              && !(what == USE_DIRBLK
                   && (old == USE_SPAREDIRBLK || old == USE_BANDDIRBLK))
              && !(what == USE_CPDATASEC && old == USE_CPDATASEC))
            {
              warning (1, "Sector #%lu usage conflict: %s vs. %s",
                       secno, sec_usage (usage_vector[secno]),
                       sec_usage (what));

              /* Display path names, if possible. */

              if (path_vector != NULL && path_vector[secno] != NULL)
                warning_cont ("File 1: \"%s\"",
                              format_path_chain (path_vector[secno], NULL));
              if (path != NULL)
                warning_cont ("File 2: \"%s\"",
                              format_path_chain (path, NULL));
            }
          else
            {
              usage_vector[secno] = what;
              if (path_vector != NULL)
                path_vector[secno] = path;
            }

          /* Check if the sector is marked as allocated.  Don't check
             if the allocation bitmap has not yet been read completely
             into memory. */

          if (alloc_ready && !ALLOCATED (secno))
            {
              warning (1, "Sector #%lu used (%s) but not marked as allocated",
                       secno, sec_usage (what));
              if (path != NULL)
                warning_cont ("File: \"%s\"",
                              format_path_chain (path, NULL));
            }
        }
      ++secno; --count;
    }
}


/* Process the bad block list starting in sector SECNO.  The list has
   TOTAL entries. */

static void do_bad (DISKIO *d, ULONG secno, ULONG total)
{
  ULONG list[512];
  ULONG rest, used, i, what_index = 0;

  /* The bad block list consists of one or more 4-sector blocks.  The
     first 32-bit word of such a block points to the next 4-sector
     block.  The end of the chain is marked by a zero sector number.
     The remaining 511 32-bit words of the blocks contain the sector
     numbers of bad sectors.  Entries which are zero do not point to
     bad sectors. */

  used = 0; rest = total;
  while (secno != 0)
    {
      if (a_info)
        {
          info ("Sectors #%lu-#%lu: Bad block list\n", secno, secno+3);
          info ("  Sector number of next bad block: #%lu\n", list[0]);
        }
      else if (a_what && IN_RANGE (what_sector, secno, 4))
        {
          info ("Sector #%lu: Bad block list (+%lu)\n",
                what_sector, what_sector - secno);
          if (secno + 0 == what_sector)
            info ("  Next sector in list: #%lu\n", list[0]);
          what_index = (secno - what_sector) / 4;
        }
      if (have_seen (secno, 4, SEEN_BADLIST, "bad block list"))
        break;
      use_sectors (secno, 4, USE_BADLIST, NULL);
      read_sec (d, list, secno, 4, TRUE);
      for (i = 1; i < 512 && i <= rest; ++i)
        if (list[i] != 0)
          {
            ++used;
            if (a_info || (a_what && IN_RANGE (i, what_index, 512/4)))
              info ("  Bad sector: #%lu\n", list[i]);
            else if (a_what && list[i] == what_sector)
              info ("Sector #%lu: Bad sector\n", list[i]);
            use_sectors (list[i], 1, USE_BAD, NULL);
          }
      secno = list[0];
      if (rest > 511)
        rest -= 511;
      else
        rest = 0;
    }
  if (rest != 0 || secno != 0)
    warning (1, "Wrong length of bad block list");
  if (used != total)
    warning (1, "Wrong number of bad blocks");
}


/* Process the hotfix list starting at sector SECNO.  The list has
   TOTAL entries. */

static void do_hotfix_list (DISKIO *d, ULONG secno, ULONG total)
{
  ULONG list[512];              /* 4 Sectors */
  ULONG i, hsecno;

  if (total > 512 / 3)
    {
      warning (1, "Maximum number of hotfixes is too big");
      total = 512 / 3;
    }
  if (a_info)
    info ("Sectors #%lu-#%lu: Hotfix list\n", secno, secno + 3);
  else if (a_what && IN_RANGE (what_sector, secno, 4))
    info ("Sector #%lu: Hotfix list (+%lu)\n",
          what_sector, what_sector - secno);
  use_sectors (secno, 4, USE_HOTFIXLIST, NULL);
  read_sec (d, list, secno, 4, TRUE);
  for (i = 0; i < total; ++i)
    {
      hsecno = list[i+total];
      if (hsecno == 0)
        warning (1, "Hotfix sector number is zero");
      else if (hsecno >= total_sectors)
        warning (1, "Hotfix sector number #%lu is too big", hsecno);
      else if (usage_vector[hsecno] == USE_EMPTY)
        {
          if (a_info)
            info ("  Hotfix sector: #%lu for #%lu, FNODE #%lu\n",
                  hsecno, list[i], list[i+2*total]);
          if (a_what && hsecno == what_sector)
            info ("Sector #%lu: Hotfix sector for #%lu, FNODE #%lu\n",
                  hsecno, list[i], list[i+2*total]);
          use_sectors (hsecno, 1, USE_HOTFIX, NULL);
          if (alloc_vector != NULL && !ALLOCATED (hsecno))
            warning (1, "Hotfix sector #%lu not marked as allocated", hsecno);
        }
    }
  if (a_what && IN_RANGE (what_sector, secno, 4))
    {
      for (i = 0; i < total; ++i)
        if (list[i] != 0 && what_sector == secno + i / (512 / 4))
          info ("  Bad sector: #%lu\n", list[i]);
      for (i = 0; i < total; ++i)
        if (what_sector == secno + (i + total) / (512 / 4))
          info ("  Hotfix sector: #%lu\n", list[i+total]);
    }
}


/* One band has 8 MBytes, a run of free sectors may not span more than
   two bands.  Therefore, there are at most 32768 successive free
   sectors. */

#define MAX_FREE_SIZE   32768

/* Show fragmentation of free space. */

static void do_free_frag (void)
{
  ULONG *counts, count, start, end, j, k;

  counts = xmalloc (MAX_FREE_SIZE * sizeof (*counts));
  for (j = 0; j < MAX_FREE_SIZE; ++j)
    counts[j] = 0;
  count = 0;
  for (j = 0; j < total_sectors; ++j)
    if (!ALLOCATED (j))
      ++count;
    else if (count != 0)
      {
        if (count < MAX_FREE_SIZE)
          counts[count] += 1;
        count = 0;
      }
  if (count != 0)
    {
      if (count < MAX_FREE_SIZE)
        counts[count] += 1;
    }
  info ("\nFragmentation of free space:\n");
  info ("Fragment size | Number of fragments of that size\n");
  info ("--------------+---------------------------------\n");
  for (j = 0; (1 << j) < MAX_FREE_SIZE; ++j)
    {
      start = 1 << j;
      end = 2 * start;
      if (end > MAX_FREE_SIZE)
        end = MAX_FREE_SIZE;
      count = 0;
      for (k = start; k < end; ++k)
        count += counts[k];
      info (" %5lu-%-5lu  | %lu\n", start, end - 1, count);
    }
  info ("\n");
}


/* Show a range of COUNT free sectors starting at sector START. */

static void do_bitmap_show (ULONG start, ULONG count)
{
  if (count == 1)
    info ("  Unallocated: 1 sector #%lu\n", start);
  else
    info ("  Unallocated: %lu sectors #%lu-#%lu\n",
          count, start, start + count - 1);
}


/* Show free sectors in the bitmap pointed to by BITMAP.  BASE is the
   number of the first sector mapped by the bitmap.  SIZE is the
   number of bits in the bitmap.  Return the number of free
   sectors. */

static ULONG do_bitmap2 (const BYTE *bitmap, ULONG base, ULONG size)
{
  ULONG start, count, j, total;

  start = count = total = 0;
  for (j = 0; j < size; ++j)
    if (BITSETP (bitmap, j))
      {
        if (count == 0)
          start = j + base;
        ++count;
      }
    else if (count != 0)
      {
        do_bitmap_show (start, count);
        total += count;
        count = 0;
      }
  if (count != 0)
    {
      do_bitmap_show (start, count);
      total += count;
    }
  return total;
}


/* Process the bitmap block starting in sector SECNO for band BAND. */

static void do_bitmap (DISKIO *d, ULONG secno, ULONG band, int show)
{
  BYTE bitmap[2048];
  ULONG pos, total, first_sec, rel_sec;

  if (a_info || show)
    info ("Bitmap for band %lu is in sectors #%lu-#%lu\n",
          band, secno, secno + 3);
  if (a_what && IN_RANGE (what_sector, secno, 4))
    info ("Sector #%lu: Bitmap for band %lu (+%lu)\n",
          what_sector, band, what_sector - secno);
  use_sectors (secno, 4, USE_BITMAP, NULL);
  read_sec (d, bitmap, secno, 4, TRUE);
  pos = band * 2048;
  first_sec = band * 2048 * 8;
  if (a_info || a_check || a_what)
    {
      if (pos + 2048 <= total_alloc)
        memcpy (alloc_vector + pos, bitmap, 2048);
      else if (pos < total_alloc)
        memcpy (alloc_vector + pos, bitmap, total_alloc - pos);
    }
  if (a_info && show_unused)
    {
      total = do_bitmap2 (bitmap, first_sec, 2048 * 8);
      info ("  Unallocated sectors in band %lu: %lu\n", band, total);
    }
  else if (a_what && IN_RANGE (what_sector, secno, 4))
    do_bitmap2 (bitmap + (what_sector - secno) * 512,
                (band * 2048 + (what_sector - secno) * 512) * 8,
                512 * 8);
  if (a_what && IN_RANGE (what_sector, first_sec, 2048 * 8))
    {
      rel_sec = what_sector - first_sec;
      info ("Allocation bit for sector #%lu (%s) is in sector #%lu,\n"
            "  byte 0x%lx, bit %lu\n",
            what_sector,
            (BITSETP (bitmap, rel_sec) ? "unallocated" : "allocated"),
            secno + rel_sec / (512 * 8),
            (rel_sec % (512 * 8)) / 8, rel_sec % 8);
    }
}


/* Process the bitmap indirect block starting in sector SECNO. */

static void do_bitmap_indirect (DISKIO *d, ULONG secno)
{
  BYTE bit_count[256];
  ULONG *list;
  ULONG i, bsecno, nfree, resvd, bands, blocks;

  bands = DIVIDE_UP (total_sectors, 2048 * 8);
  blocks = DIVIDE_UP (bands, 512);
  if (a_info)
    info ("Sectors #%lu-#%lu: Bitmap indirect block\n",
          secno, secno + 4 * blocks - 1);
  else if (a_what && IN_RANGE (what_sector, secno, 4 * blocks))
    info ("Sector #%lu: Bitmap indirect block (+%lu)\n",
          what_sector, what_sector - secno);
  use_sectors (secno, 4 * blocks, USE_BITMAPIND, NULL);
  list = xmalloc (2048 * blocks);
  read_sec (d, list, secno, 4 * blocks, TRUE);
  for (i = 0; i < bands; ++i)
    {
      bsecno = list[i];
      if (bsecno == 0)
        {
          warning (1, "Bitmap indirect block starting at #%lu: "
                   "Entry %lu is zero", secno, i);
          break;
        }
      do_bitmap (d, bsecno, i,
                 (a_what && what_sector == secno + i / (512 / 4)));
    }
  if (a_check)
    {
      for (i = bands; i < blocks * 512; ++i)
        if (list[i] != 0)
          {
            warning (1, "Bitmap indirect block starting at #%lu: "
                     "Too many entries", secno);
            break;
          }
    }
  free (list); list = NULL;

  if (a_check || a_info)
    {
      for (i = 0; i < 256; ++i)
        {
          bit_count[i] = 0;
          if (i & 0x01) ++bit_count[i];
          if (i & 0x02) ++bit_count[i];
          if (i & 0x04) ++bit_count[i];
          if (i & 0x08) ++bit_count[i];
          if (i & 0x10) ++bit_count[i];
          if (i & 0x20) ++bit_count[i];
          if (i & 0x40) ++bit_count[i];
          if (i & 0x80) ++bit_count[i];
        }
      nfree = 0;
      for (i = 0; i < total_alloc; ++i)
        nfree += bit_count[alloc_vector[i]];
      resvd = total_sectors / 50;
      if (resvd > 4096) resvd = 4096;
      if (a_info)
        info ("Number of reserved sectors:    %lu (%lu used)\n",
              resvd, resvd > nfree ? resvd - nfree : 0);
      if (resvd > nfree)
        {
          if (a_check)
            warning (0, "Reserved sectors are in used (%lu)", resvd - nfree);
          resvd = 0;
        }
      if (a_info)
        info ("Number of unallocated sectors: %lu (%lu available)\n",
              nfree, nfree - resvd);
    }
  if (alloc_vector != NULL)
    alloc_ready = TRUE;
}


/* Compare two file names pointed to by P1 and P2.  CPIDX1 and CPIDX2
   are the code page indices of the files.  If a code page index is
   out of range, the current code page will be used. */

static int compare_fname (const BYTE *p1, const BYTE *p2,
                          ULONG cpidx1, ULONG cpidx2)
{
  const BYTE *map1, *map2;

  map1 = (cpidx1 >= code_page_count
          ? cur_case_map : code_pages[cpidx1].case_map);
  map2 = (cpidx2 >= code_page_count
          ? cur_case_map : code_pages[cpidx2].case_map);
  for (;;)
    {
      if (*p1 == 0 && *p2 == 0)
        return 0;
      if (*p2 == 0)
        return 1;
      if (*p1 == 0)
        return -1;
      if (map1[*p1] > map2[*p2])
        return 1;
      if (map1[*p1] < map2[*p2])
        return -1;
      ++p1; ++p2;
    }
}


/* Process one entry of a code page data sector.  The entry is in
   sector SECNO (used for messages, only).  PD points to the code page
   entry.  The length of the entry is LEN bytes.  CS is the expected
   checksum for the entry.  Update our code page entry pointed to by
   DST. */

static void do_cpdata (ULONG secno, const CPDATAENTRY *pd, ULONG len,
                       ULONG cs, MYCP *dst)
{
  ULONG rc, i, diffs, cs2;
  COUNTRYCODE cc;
  BYTE map[128], c;
  BYTE dbcs[12];

  memcpy (dst->case_map + 128, pd->bCaseMapTable, 128);
  for (c = 0; c < 128; ++c)
    map[c] = (BYTE)(c + 128);
  cc.country = USHORT_FROM_FS (pd->usCountryCode);
  cc.codepage = USHORT_FROM_FS (pd->usCodePageID);
  rc = DosMapCase (128, &cc, (PCHAR)map);
  if (rc == ERROR_NLS_NO_COUNTRY_FILE)
    {
      if (!no_country_sys)
        warning (0, "COUNTRY.SYS not found -- "
                 "cannot check case mapping tables");
      no_country_sys = TRUE;
    }
  if (!no_country_sys)
    {
      if (rc != 0)
        warning (1, "DosMapCase failed for %lu/%lu, rc=%lu",
                 cc.country, cc.codepage, rc);
      else
        {
          diffs = 0;
          for (i = 0; i < 128; ++i)
            if (map[i] != pd->bCaseMapTable[i])
              {
                ++diffs;
                dst->case_map_changed[i+128] = TRUE;
              }

          /* Don't complain about up to 2 differences in a case
             mapping table unless -p is given. */

          if (diffs != 0 && (check_pedantic || diffs > 2))
            warning (diffs > 2 ? 1 : 0,
                     "CPDATASEC #%lu: Case mapping table does not match "
                     "DosMapCase for %lu/%lu (%lu difference%s)",
                     secno, cc.country, cc.codepage, diffs,
                     diffs == 1 ? "" : "s");
        }
      rc = DosQueryDBCSEnv (sizeof (dbcs), &cc, (PCHAR)&dbcs);
      if (rc != 0)
        warning (1, "DosQueryDBCSEnv failed for %lu/%lu, rc=%lu",
                 cc.country, cc.codepage, rc);
      else
        {
          for (i = 0; i < 6; ++i)
            if (dbcs[2*i+0] == 0 && dbcs[2*i+1] == 0)
              break;
          if (i != USHORT_FROM_FS (pd->cDBCSRange))
            warning (1, "CPDATASEC #%lu: Number of DBCS ranges does not match "
                     "DosQueryDBCSEnv for %lu/%lu",
                     secno, cc.country, cc.codepage);
          else if (memcmp (dbcs, (const char *)pd->DBCSRange, 2 * i) != 0)
            warning (1, "CPDATASEC #%lu: DBCS ranges does not match "
                     "DosQueryDBCSEnv for %lu/%lu",
                     secno, cc.country, cc.codepage);
        }
    }
  cs2 = chksum ((const BYTE *)pd, len);
  if (cs != cs2)
    warning (1, "CPDATASEC #%lu: Incorrect checksum for %lu/%lu",
             secno, cc.country, cc.codepage);
}


/* Process a code page data sector.  DI is an index into
   code_pages[]. */

static void do_cpdatasec (DISKIO *d, ULONG di)
{
  ULONG secno, dcount, index, offset, len, j, c;
  HPFS_SECTOR cpdatasec;
  char used[512];
  const CPDATAENTRY *pd;

  secno = ULONG_FROM_FS (code_pages[di].info.lsnCPData);
  for (j = 0; j < cpdata_count; ++j)
    if (cpdata_visited[j] == secno)
      return;
  cpdata_visited[cpdata_count++] = secno;
  if (a_info || (a_what && secno == what_sector))
    info ("Sector #%lu: Code page data sector\n", secno);
  use_sectors (secno, 1, USE_CPDATASEC, NULL);
  read_sec (d, &cpdatasec, secno, 1, TRUE);
  if (ULONG_FROM_FS (cpdatasec.cpdatasec.sig) != CPDATA_SIG1)
    {
      warning (1, "CPDATASEC #%lu: Bad signature", secno);
      return;
    }
  dcount = USHORT_FROM_FS (cpdatasec.cpdatasec.cCodePage);
  if (dcount > 3)
    {
      warning (1, "CPDATASEC #%lu: Too many code pages", secno);
      dcount = 3;
    }
  memset (used, FALSE, 512);
  for (j = 0; j < dcount; ++j)
    {
      index = USHORT_FROM_FS (cpdatasec.cpdatasec.iFirstCP) + j;
      if (index >= code_page_count)
        warning (1, "CPDATASEC #%lu: Index too big", secno);
      else
        {
          code_pages[index].hit = TRUE;
          for (c = 0; c < 256; ++c)
            {
              code_pages[index].case_map[c] = (BYTE)c;
              code_pages[index].case_map_changed[c] = FALSE;
            }
          for (c = 'a'; c <= 'z'; ++c)
            code_pages[index].case_map[c] = (BYTE)toupper (c);
          if (ULONG_FROM_FS (cpdatasec.cpdatasec.cksCP[j])
              != ULONG_FROM_FS (code_pages[index].info.cksCP))
            warning (1, "CPDATASEC #%lu: Wrong checksum for code page %lu",
                     secno, index);
          offset = USHORT_FROM_FS (cpdatasec.cpdatasec.offCPData[j]);
          len = sizeof (CPDATAENTRY) - sizeof (DBCSRG);
          if (offset < sizeof (cpdatasec.cpdatasec) || offset + len > 512)
            warning (1, "CPDATASEC #%lu: Invalid offset: %lu", secno, offset);
          else
            {
              pd = (const CPDATAENTRY *)(cpdatasec.raw + offset);
              if (USHORT_FROM_FS (pd->cDBCSRange)
                  != USHORT_FROM_FS (code_pages[index].info.cDBCSRange))
                warning (1, "CPDATASEC #%lu: Incorrect number of DBCS ranges",
                         secno);
              else
                {
                  len += ((USHORT_FROM_FS (pd->cDBCSRange) + 1)
                          * sizeof (DBCSRG));
                  if (offset + len > 512)
                    warning (1, "CPDATASEC #%lu: Invalid offset: %lu",
                             secno, offset);
                  else if (memchr (used + offset, TRUE, len) != NULL)
                    warning (1, "CPDATASEC #%lu: Overlapping data", secno);
                  else
                    {
                      memset (used + offset, TRUE, len);
                      do_cpdata (secno, pd, len,
                                 ULONG_FROM_FS (cpdatasec.cpdatasec.cksCP[j]),
                                 &code_pages[index]);
                    }
                }
            }
        }
    }
}


/* Process one code page information sector.  PSECNO points to an
   object containing the sector number of the code page information
   sector.  The object will be modified to contain the sector number
   of the next code page information sector.  The object pointed to by
   PCOUNT contains the number of code pages.  The object will be
   updated.  Return TRUE iff successful. */

static int do_one_cpinfosec (DISKIO *d, ULONG *psecno, ULONG *pcount)
{
  ULONG secno, i, n, next;
  HPFS_SECTOR cpinfosec;
  const CPINFOENTRY *pi;

  secno = *psecno;
  if (a_info || (a_what && secno == what_sector))
    info ("Sector #%lu: Code page information sector\n", secno);
  if (have_seen (secno, 1, SEEN_CPINFOSEC, "code page information"))
    return FALSE;
  use_sectors (secno, 1, USE_CPINFOSEC, NULL);
  read_sec (d, &cpinfosec, secno, 1, TRUE);
  if (ULONG_FROM_FS (cpinfosec.cpinfosec.sig) != CPINFO_SIG1)
    {
      warning (1, "CPINFOSEC #%lu: Bad signature", secno);
      return FALSE;
    }
  if (ULONG_FROM_FS (cpinfosec.cpinfosec.iFirstCP) != *pcount)
    warning (1, "CPINFOSEC #%lu: Wrong code page index", secno);
  n = ULONG_FROM_FS (cpinfosec.cpinfosec.cCodePage);
  if (n > 31)
    {
      warning (1, "CPINFOSEC #%lu: Too many code pages", secno);
      n = 31;
    }
  for (i = 0; i < n; ++i)
    {
      pi = &cpinfosec.cpinfosec.CPInfoEnt[i];
      code_pages[*pcount].info = *pi;
      code_pages[*pcount].hit = FALSE;
      if (a_info || (a_what && what_sector == secno))
        info ("  Code page index %lu: code page %u, country %u\n",
              i, USHORT_FROM_FS (pi->usCodePageID),
              USHORT_FROM_FS (pi->usCountryCode));
      if (USHORT_FROM_FS (pi->iCPVol) != *pcount)
        warning (1, "CPINFOSEC #%lu: Incorrect index", secno);
      ++*pcount;
    }
  next = ULONG_FROM_FS (cpinfosec.cpinfosec.lsnNext);
  if (next == 0)
    return FALSE;
  *psecno = next;
  return TRUE;
}


/* Process the list of code page information sectors starting in
   sector SECNO. */

static void do_cpinfosec (DISKIO *d, ULONG secno)
{
  ULONG count, i;

  code_pages = xmalloc (code_page_count * sizeof (MYCP));
  count = 0;
  while (do_one_cpinfosec (d, &secno, &count))
    ;
  if (count != code_page_count)
    {
      warning (1, "Wrong number of code pages in code page information "
               "sectors");
      if (count < code_page_count)
        code_page_count = count;
    }

  cpdata_count = 0;
  cpdata_visited = xmalloc (code_page_count * sizeof (*cpdata_visited));
  for (i = 0; i < code_page_count; ++i)
    do_cpdatasec (d, i);
  free (cpdata_visited); cpdata_visited = NULL;

  for (i = 0; i < code_page_count; ++i)
    if (!code_pages[i].hit)
      warning (1, "No code page data for code page index %lu", i);
  /* TODO: check for duplicate code pages */
}


static void do_fnode (DISKIO *d, ULONG secno, const path_chain *path,
                      int dir_flag, ULONG parent_fnode, ULONG file_size,
                      ULONG ea_size, int check_ea_size, int need_eas,
                      int list);
static int do_storage (DISKIO *d, ULONG secno, const ALBLK *header,
                       ULONG leaf_count, const path_chain *path,
                       ULONG *pexp_file_sec, ULONG *pnext_disk_sec,
                       ULONG total_sectors, ULONG parent_fnode,
                       int alsec_level, BYTE what, ULONG *pextents,
                       int show, ULONG copy_size, BYTE *buf, ULONG buf_size);
static void do_dirblk (DISKIO *d, ULONG secno, const path_chain *path,
                       ULONG parent_fnode, ULONG parent, SORT *psort,
                       int *down_ptr, int level, int *pglobal_dirent_index,
                       int *pdotdot, int list);


/* Display a warning message for the DIRBLK starting in sector SECNO.
   PATH points to the path name chain of the directory. */

static void dirblk_warning (int level, const char *fmt, ULONG secno,
                            const path_chain *path, ...)
{
  va_list arg_ptr;

  warning_prolog (level);
  my_fprintf (diag_file, "DIRBLK #%lu (\"%s\"): ",
              secno, format_path_chain (path, NULL));
  va_start (arg_ptr, path);
  my_vfprintf (diag_file, fmt, arg_ptr);
  va_end (arg_ptr);
  fputc ('\n', diag_file);
  warning_epilog ();
}


/* Display a warning message for directory entry number DIRENT_NO of
   the DIRBLK starting in sector SECNO.  PATH points to the path name
   chain of the directory, FNAME is the file name in the directory
   entry. */

static void dirent_warning (int level, const char *fmt, ULONG secno,
                            const path_chain *path, int dirent_no,
                            const char *fname, ...)
{
  va_list arg_ptr;

  warning_prolog (level);
  my_fprintf (diag_file, "DIRBLK #%lu (\"%s\"): ",
              secno, format_path_chain (path, NULL));
  if (fname == NULL)
    my_fprintf (diag_file, "DIRENT %d: ", dirent_no);
  else
    my_fprintf (diag_file, "DIRENT %d (\"%s\"): ", dirent_no, fname);
  va_start (arg_ptr, fname);
  my_vfprintf (diag_file, fmt, arg_ptr);
  va_end (arg_ptr);
  fputc ('\n', diag_file);
  warning_epilog ();
}


/* Show the directory entry pointed to by P.  Indent with INDENT
   spaces. */

static void show_dirent (const DIRENT *p, int indent)
{
  ULONG cpindex, length, gap_size, ace_size, i;
  const BYTE *pb;

  length = USHORT_FROM_FS (p->cchThisEntry);
  infoi ("Length:                      %lu\n", indent, length);
  infoi ("Flags:                       0x%.2x", indent, p->bFlags);
  if (p->bFlags & DF_SPEC) info (" ..");
  if (p->bFlags & DF_END) info (" end");
  if (p->bFlags & DF_ATTR) info (" EA");
  if (p->bFlags & DF_NEEDEAS) info (" need-EA");
  if (p->bFlags & DF_PERM) info (" perms");
  if (p->bFlags & DF_ACL) info (" ACL");
  if (p->bFlags & DF_XACL) info (" explicit-ACL");
  info ("\n");
  if (!(p->bFlags & DF_END))
    {
      infoi ("Attributes:                  0x%.2x", indent, p->bAttr);
      if (p->bAttr & ATTR_DIR)
        info (" dir");
      if (p->bAttr & ATTR_READONLY)
        info (" r/o");
      if (p->bAttr & ATTR_HIDDEN)
        info (" hidden");
      if (p->bAttr & ATTR_SYSTEM)
        info (" system");
      if (p->bAttr & ATTR_LABEL)
        info (" label");
      if (p->bAttr & ATTR_ARCHIVED)
        info (" arch");
      if (p->bAttr & ATTR_NONFAT)
        info (" non-FAT");
      info ("\n");
      infoi ("FNODE:                       #%lu\n", indent,
             ULONG_FROM_FS (p->lsnFNode));
      infoi ("Time of creation:            %s\n", indent,
             format_time (ULONG_FROM_FS (p->timCreate)));
      infoi ("Time of last modification:   %s\n", indent,
             format_time (ULONG_FROM_FS (p->timLastMod)));
      infoi ("Time of last access:         %s\n", indent,
             format_time (ULONG_FROM_FS (p->timLastAccess)));
      infoi ("Size of file:                %lu\n", indent,
             ULONG_FROM_FS (p->cchFSize));
      infoi ("Size of extended attributes: %lu\n", indent,
             ULONG_FROM_FS (p->ulEALen));
      infoi ("Number of ACEs:              %u\n", indent, p->bFlex & 7);
      cpindex = p->bCodePage & 0x7f;
      if (cpindex >= code_page_count)
        infoi ("Code page index:             %lu\n", indent, cpindex);
      else
        infoi ("Code page:                   %u\n", indent,
               USHORT_FROM_FS (code_pages[cpindex].info.usCodePageID));
      if (p->bCodePage & 0x80)
        infoi ("Name contains DBCS characters\n", indent);
      if (p->bFlags & DF_ACL)
        {
          gap_size = length - (sizeof (DIRENT) + p->cchName - 1);
          if (p->bFlags & DF_BTP)
            gap_size -= sizeof (ULONG);
          ace_size = (p->bFlex & 7) * 4;
          pb = p->bName + p->cchName;
          for (i = 0; i < gap_size; ++i)
            {
              if (i % 16 == 0)
                {
                  if (i != 0)
                    info ("\n");
                  infoi ("ACE data:                   ", indent);
                }
              info ("%c%.2x",
                    i == ace_size ? '|' : i > 0 && i % 4 == 0 ? '.' : ' ',
                    pb[i]);
            }
          if (gap_size != 0)
            info ("\n");
        }
    }
  if (p->bFlags & DF_BTP)
    infoi ("Down pointer:                #%lu\n", indent,
           ((ULONG *)((const char *)p + length))[-1]);
}


/* Show the directory entry pointed to by P, formatted for the `dir'
   action. */

static void show_dir (const DIRENT *p, const char *name)
{
  info ("%s ", format_dir_time (ULONG_FROM_FS (p->timLastMod)));
  if (p->bAttr & ATTR_DIR)
    info ("     <DIR>      ");
  else
    info ("%10lu %c%c%c%c%c",
          ULONG_FROM_FS (p->cchFSize),
          (p->bAttr & ATTR_READONLY) ? 'R' : '-',
          (p->bAttr & ATTR_HIDDEN)   ? 'H' : '-',
          (p->bAttr & ATTR_SYSTEM)   ? 'S' : '-',
          (p->bAttr & ATTR_LABEL)    ? 'V' : '-',
          (p->bAttr & ATTR_ARCHIVED) ? 'A' : '-');
  info (" \"%s\"\n", name);
}


/* Perform basic checks for the DIRENT at offset POS of the DIRBLK
   pointed to by PDIR.  Return a pointer to the DIRENT if it looks
   reasonably good.  Return NULL if it looks bad.  Store the name to
   NAME, which points to an array of 256 characters.  Print warnings
   if WARN is TRUE.  The remaining arguments are passed to
   dirent_warning(). */

const DIRENT *check_dirent (const DIRBLK *pdir, size_t pos, char *name,
                            int warn, ULONG secno, const path_chain *path,
                            int index)
{
  const DIRENT *p;
  ULONG length;

  *name = 0;
  p = (const DIRENT *)((const char *)pdir + pos);
  if (pos + sizeof (DIRENT) > 2048)
    {
      if (warn)
        dirent_warning (1, "Extends beyond end of DIRBLK",
                        secno, path, index, NULL);
      return NULL;
    }
  length = USHORT_FROM_FS (p->cchThisEntry);
  if (pos + length > 2048)
    {
      if (warn)
        dirent_warning (1, "Extends beyond end of DIRBLK",
                        secno, path, index, NULL);
      return NULL;
    }
  if (length < sizeof (DIRENT))
    {
      if (warn)
        dirent_warning (1, "Length too small (case 1)",
                        secno, path, index, NULL);
      return NULL;
    }
  if (length < (ROUND_UP (sizeof (DIRENT) + p->cchName - 1, 4)
                + ((p->bFlags & DF_BTP) ? sizeof (ULONG) : 0)))
    {
      if (warn)
        dirent_warning (1, "Length too small (case 2)",
                        secno, path, index, NULL);
      return NULL;
    }
  if (length & 3)
    {
      if (warn)
        dirent_warning (1, "Length is not a multiple of 4",
                        secno, path, index, NULL);
      return NULL;
    }

  if (p->bFlags & DF_END)
    {
      strcpy (name, "[END]");
      if (a_check && (p->cchName != 1 || memcmp (p->bName, "\377", 1) != 0))
        dirent_warning (0, "Wrong name for end entry",
                        secno, path, index, NULL);
#if 0
      /* This error(?) seems to be quite normal with HPFS386. */
      if (a_check && (p->bFlags & DF_SPEC))
        dirent_warning (1, "Both DF_END and DF_SPEC are set",
                        secno, path, index, NULL);
#endif
    }
  else if (p->bFlags & DF_SPEC)
    {
      strcpy (name, "..");
      if (a_check && (p->cchName != 2 || memcmp (p->bName, "\1\1", 2) != 0))
        dirent_warning (0, "Wrong name for \"..\" entry",
                        secno, path, index, NULL);
    }
  else
    {
      memcpy (name, p->bName, p->cchName);
      name[p->cchName] = 0;
    }

  return p;
}


/* Show DIRENTs for the `info <number>' action. */

static void do_dirblk_what (DISKIO *d, const DIRBLK *pdir, ULONG secno,
                            const path_chain *path)
{
  const DIRENT *p;
  char name[256];
  int dirent_index;
  size_t pos;
  ULONG length;

  if (what_sector == secno)
    {
      info ("  Change count(?):           %lu\n",
            ULONG_FROM_FS (pdir->dirblk.culChange) >> 1);
      info ("  Offset to first free byte: 0x%lx\n",
            ULONG_FROM_FS (pdir->dirblk.offulFirstFree));
      info ("  Pointer to parent:         #%lu\n",
            ULONG_FROM_FS (pdir->dirblk.lsnParent));
      info ("  Pointer to this directory: #%lu\n",
            ULONG_FROM_FS (pdir->dirblk.lsnThisDir));
    }

  pos = offsetof (DIRBLK, dirblk.dirent);
  for (dirent_index = 0;; ++dirent_index)
    {
      p = check_dirent (pdir, pos, name, FALSE, secno, path, dirent_index);
      if (p == NULL)
        break;
      length = USHORT_FROM_FS (p->cchThisEntry);
      if (secno + pos / 512 <= what_sector
          && secno + (pos + length - 1) / 512 >= what_sector)
        {
          info ("  ");
          if (secno + pos / 512 != what_sector
              || secno + (pos + length - 1) / 512 != what_sector)
            info ("Partial ");
          info ("DIRENT %d (offset 0x%lx):\n", dirent_index, pos);
          info ("    Name: \"%s\"\n", name);
          show_dirent (p, 4);
        }
      pos += length;
      if (p->bFlags & DF_END)
        break;
    }
}


/* Handle actions which search for a file: Search the DIRBLK for the
   current path name component.  If found, either show information
   about the path name (for `info <path>') or recurse. */

static void do_dirblk_find (DISKIO *d, const DIRBLK *pdir, ULONG secno,
                            const path_chain *path, ULONG parent_fnode)
{
  const DIRENT *p;
  char name[256];
  int dirent_index, cmp, list;
  ULONG cpindex, length;
  size_t pos;
  path_chain link, *plink;

  pos = offsetof (DIRBLK, dirblk.dirent);
  for (dirent_index = 0;; ++dirent_index)
    {
      p = check_dirent (pdir, pos, name, TRUE, secno, path, dirent_index);
      if (p == NULL)
        break;

      length = USHORT_FROM_FS (p->cchThisEntry);
      if (p->bFlags & DF_END)
        cmp = 1;
      else if (p->bFlags & DF_SPEC)
        cmp = -1;
      else
        {
          cpindex = p->bCodePage & 0x7f;
          cmp = compare_fname ((const BYTE *)name,
                               (const BYTE *)find_comp,
                               cpindex, code_page_count);
        }

      if (cmp < 0)
        {
          /* The DIRENT's name is less than the name we are looking
             for.  Examine the next DIRENT.  This is the only case in
             which the loop is continued. */

          pos += length;
        }
      else if (cmp == 0)
        {
          /* We found the name.  If it's the last component of the
             path name we are looking for, show information about the
             DIRENT. */

          if (*find_path == 0)
            {
              if (a_where)
                {
                  info ("Directory entry %d of DIRBLK #%lu+%lu (#%lu)\n",
                        dirent_index, secno, pos / 512, secno + pos / 512);
                  show_dirent (p, 2);
                }
              list = FALSE;
              if (a_dir)
                {
                  if (p->bAttr & ATTR_DIR)
                    list = TRUE;
                  else
                    {
                      show_dir (p, name);
                      quit (0, FALSE);
                    }
                }
              if (!(p->bFlags & DF_SPEC))
                {
                  plink = PATH_CHAIN_NEW (&link, path, name);
                  do_fnode (d, ULONG_FROM_FS (p->lsnFNode), plink,
                            (p->bAttr & ATTR_DIR), parent_fnode,
                            ULONG_FROM_FS (p->cchFSize),
                            ULONG_FROM_FS (p->ulEALen), TRUE,
                            p->bFlags & DF_NEEDEAS, list);
                }
              quit (0, FALSE);
            }

          /* There is at least one more component in the path name, so
             this DIRENT must be a directory. */

          if (!(p->bAttr & ATTR_DIR))
            error ("\"%s\" is not a directory",
                   format_path_chain (path, name));

          /* Recurse into the directory. */

          plink = PATH_CHAIN_NEW (&link, path, name);
          do_fnode (d, ULONG_FROM_FS (p->lsnFNode), plink, TRUE,
                    parent_fnode, 0, ULONG_FROM_FS (p->ulEALen), TRUE,
                    p->bFlags & DF_NEEDEAS, FALSE);
          return;
        }
      else
        {
          /* The DIRENT's name is greater than the name we are looking
             for.  If there is a down pointer, recurse into the next
             level of the B-tree.  Otherwise, there is no such
             name. */

          if (!(p->bFlags & DF_BTP))
            break;
          do_dirblk (d, ((ULONG *)((const char *)p + length))[-1],
                     path, parent_fnode, secno,
                     NULL, NULL, 0, NULL, NULL, FALSE);
          return;
        }
    }
  error ("\"%s\" not found in \"%s\"",
         find_comp, format_path_chain (path, NULL));
}


#define MAX_DIRBLK_LEVELS       32

/* Check the down pointer of a DIRENT.  INDEX is the index of the
   DIRENT, FLAG is zero for a leaf, one for a node.  See do_dirblk()
   for the remaining arguments. */

static void check_dirent_down (int *down_ptr, int level, ULONG secno,
                               const path_chain *path, int index, int flag)
{
  if (level < MAX_DIRBLK_LEVELS)
    {
      if (down_ptr[level] == -1)
        down_ptr[level] = flag;
      else if (down_ptr[level] != flag)
        dirent_warning (1, "%s down pointer",
                        secno, path, index, NULL,
                        flag == 0 ? "Undesired" : "Missing");
    }
}


/* Recurse into the next DIRBLK level for `check' action etc.  See
   do_dirbkl() for a description of the arguments. */

static void do_dirblk_recurse (DISKIO *d, const DIRBLK *pdir, ULONG secno,
                               const path_chain *path, ULONG parent_fnode,
                               ULONG parent, SORT *psort, int *down_ptr,
                               int level, int *pglobal_dirent_index,
                               int *pdotdot, int list)
{
  const DIRENT *p;
  const char *pname;
  char name[256];
  int dirent_index;
  ULONG cpindex, length;
  size_t pos;
  path_chain link, *plink;

  pos = offsetof (DIRBLK, dirblk.dirent);
  for (dirent_index = 0;; ++dirent_index)
    {
      p = check_dirent (pdir, pos, name, TRUE, secno, path, dirent_index);
      if (p == NULL)
        break;
      length = USHORT_FROM_FS (p->cchThisEntry);
      if (p->bFlags & DF_BTP)
        {
          do_dirblk (d, ((ULONG *)((const char *)p + length))[-1],
                     path, parent_fnode, secno, psort,
                     down_ptr, level + 1, pglobal_dirent_index, pdotdot,
                     list);
          check_dirent_down (down_ptr, level, secno, path,
                             dirent_index, 1);
        }
      else
        check_dirent_down (down_ptr, level, secno, path, dirent_index, 0);
      if (!(p->bFlags & DF_END))
        {
          if (p->bFlags & DF_SPEC)
            {
              pname = "";
              if (*pdotdot)
                dirent_warning (1, "More than one \"..\" entry",
                                secno, path, dirent_index, name);
              else if (*pglobal_dirent_index != 0)
                dirent_warning (1, "\"..\" entry is not the first entry",
                                secno, path, dirent_index, name);
              *pdotdot = TRUE;
            }
          else
              pname = name;
          if (verbose)
            my_fprintf (prog_file, "%s\n", format_path_chain (path, name));
          if (a_check && strlen (name) + path_chain_len (path) > 255)
            dirent_warning (1, "Path name too long",
                            secno, path, dirent_index, name);
          cpindex = p->bCodePage & 0x7f;
          if (cpindex >= code_page_count)
            dirent_warning (1, "Code page index too big",
                            secno, path, dirent_index, name);
          else if (check_pedantic)
            {
              const BYTE *s;
              int changed;

              changed = FALSE;
              for (s = (const BYTE *)pname; *s != 0; ++s)
                if (code_pages[cpindex].case_map_changed[*s])
                  changed = TRUE;
              if (changed)
                dirent_warning (0, "Case mapping changed",
                                secno, path, dirent_index, name);
            }
          if (compare_fname (psort->name, (const BYTE *)pname,
                             psort->cpindex, cpindex) > 0)
            dirent_warning (1, "File names are not in ascending order "
                            "(\"%s\" vs \"%s\")",
                            secno, path, dirent_index, NULL,
                            psort->name, pname);
          strcpy ((char *)psort->name, pname);
          psort->cpindex = cpindex;
          if (a_check)
            {
              ULONG t, gap, ace_size, temp_size;

              t = ULONG_FROM_FS (p->timLastMod);
              if (t != 0 && t < min_time)
                dirent_warning (1, "Modification time is out of range (%lu)",
                                secno, path, dirent_index, name, t);
              t = ULONG_FROM_FS (p->timLastAccess);
              if (t != 0 && t < min_time)
                dirent_warning (1, "Access time is out of range (%lu)",
                                secno, path, dirent_index, name, t);
              t = ULONG_FROM_FS (p->timCreate);
              if (t != 0 && t < min_time)
                dirent_warning (1, "Creation time is out of range (%lu)",
                                secno, path, dirent_index, name, t);

              if (!(p->bFlags & DF_SPEC))
                {
                  if (!is_hpfs_name ((const BYTE *)name))
                    dirent_warning (1, "Invalid character in file name",
                                    secno, path, dirent_index, name);
                  else if (!is_fat_name ((const BYTE *)name)
                           != !!(p->bAttr & ATTR_NONFAT))
                    dirent_warning (1, "Incorrect FAT compatibility bit",
                                    secno, path, dirent_index, name);
                }
              if (p->bAttr & (0x80 | ATTR_LABEL))
                dirent_warning (0, "Undefined attribute bit is set",
                                secno, path, dirent_index, name);

              /* The following ACL stuff is based on a few examples;
                 the meaning of all this is quite unknown. */

              if (p->bFlags & DF_PERM)
                dirent_warning (0, "DF_PERM bit is set -- meaning unknown",
                                secno, path, dirent_index, name);
              if ((p->bFlags & (DF_ACL|DF_XACL)) == DF_XACL)
                dirent_warning (0, "DF_XACL is set without DF_ACL",
                                secno, path, dirent_index, name);
              gap = USHORT_FROM_FS (p->cchThisEntry);
              gap -= (sizeof (DIRENT) + p->cchName - 1);
              if (p->bFlags & DF_BTP)
                gap -= sizeof (ULONG);
              if (gap > 3 && !(p->bFlags & DF_ACL))
                dirent_warning (0, "DF_ACL should be set "
                                "(up to %lu bytes of ACEs)",
                                secno, path, dirent_index, name, gap);
              if ((p->bFlex & 7) && !(p->bFlags & DF_ACL))
                dirent_warning (0, "DF_ACL should be set (ACE count: %u)",
                                secno, path, dirent_index, name, p->bFlex & 7);
              ace_size = (p->bFlex & 7) * 4;
              temp_size = ROUND_UP (sizeof (DIRENT) + p->cchName - 1
                                    + ace_size, 4);
              if (p->bFlags & DF_BTP)
                temp_size += sizeof (ULONG);
              if (temp_size != USHORT_FROM_FS (p->cchThisEntry))
                dirent_warning (0, "ACE count/size mismatch (%u/%lu)",
                                secno, path, dirent_index, name,
                                p->bFlex & 7, gap);
              if (p->bFlex & ~7)
                dirent_warning (0, "Bits with unknown meaning are set "
                                "in bFlex (0x%.2x)",
                                secno, path, dirent_index, name,
                                p->bFlex & ~7);
            }

          if (list)
            show_dir (p, name);
          else if (!(p->bFlags & DF_SPEC))
            {
              plink = PATH_CHAIN_NEW (&link, path, name);
              do_fnode (d, ULONG_FROM_FS (p->lsnFNode), plink,
                        (p->bAttr & ATTR_DIR), parent_fnode,
                        ULONG_FROM_FS (p->cchFSize),
                        ULONG_FROM_FS (p->ulEALen), TRUE,
                        p->bFlags & DF_NEEDEAS, list);
            }
        }

      pos += length;
      if (p->bFlags & DF_END)
        break;
      *pglobal_dirent_index += 1;
    }
  if (pos != ULONG_FROM_FS (pdir->dirblk.offulFirstFree))
    dirblk_warning (1, "Wrong offset to first free byte", secno, path);
}


/* Process the DIRBLK starting in sector SECNO.  PATH points to the
   path name chain of the file or directory.  PARENT_FNODE is the
   sector number of the FNODE of the directory containing the DIRBLK.
   PARENT is the sector number of the parent DIRBLK or (for a DIRBLK
   in the top level) of the directory's FNODE.  PATH points to the
   path name chain of the directory.  PSORT points to a structure
   containing a file name which must compare less than (or equal to,
   for "") the first DIRENT of the current DIRBLK sub-tree.  The
   structure will be updated to contain the file name of the last
   DIRENT of this DIRBLK.  DOWN_PTR points to an array recording
   down-pointer usage.  The array has LEVEL entries.
   PGLOBAL_DIRENT_INDEX points to an object containing the index of
   the first DIRENT entry of this DIRBLK sub-tree, relative to the
   directory.  The object will be updated.  PDOTDOT points to an
   object recording presence of the ".." entry.  The object will be
   updated.  List the directory if LIST is true. */

static void do_dirblk (DISKIO *d, ULONG secno, const path_chain *path,
                       ULONG parent_fnode, ULONG parent, SORT *psort,
                       int *down_ptr, int level, int *pglobal_dirent_index,
                       int *pdotdot, int list)
{
  DIRBLK dir;

  if (a_what && IN_RANGE (what_sector, secno, 4))
    info ("Sector #%lu: DIRBLK of \"%s\" (+%lu)\n",
          what_sector, format_path_chain (path, NULL), what_sector - secno);
  if (have_seen (secno, 4, SEEN_DIRBLK, "DIRBLK"))
    return;
  use_sectors (secno, 4, USE_DIRBLK, path);
  if (secno & 3)
    dirblk_warning (1, "Sector number is not a multiple of 4", secno, path);
  read_sec (d, &dir, secno, 4, TRUE);
  if (ULONG_FROM_FS (dir.dirblk.sig) != DIRBLK_SIG1)
    {
      dirblk_warning (1, "Bad signature", secno, path);
      return;
    }
  ++dirblk_total;
  if (secno < dirband_start || secno > dirband_end)
    ++dirblk_outside;
  if (ULONG_FROM_FS (dir.dirblk.lsnThisDir) != secno)
    dirblk_warning (1, "Wrong self pointer", secno, path);
  if (ULONG_FROM_FS (dir.dirblk.lsnParent) != parent)
    dirblk_warning (1, "Wrong parent pointer", secno, path);
  if (a_check)
    {
      if (!(ULONG_FROM_FS (dir.dirblk.culChange) & 1) != (level != 0))
        dirblk_warning (1, "`top-most' bit is incorrect", secno, path);
    }

  /* Show DIRENTs for the `info <number>' action. */

  if (a_what && IN_RANGE (what_sector, secno, 4))
    do_dirblk_what (d, &dir, secno, path);

  /* Handle action which search for a file. */

  if (a_find && !list)
    {
      do_dirblk_find (d, &dir, secno, path, parent_fnode);
      return;
    }

  /* Recurse into the next level of the B-tree. */

  do_dirblk_recurse (d, &dir, secno, path, parent_fnode, parent, psort,
                     down_ptr, level, pglobal_dirent_index, pdotdot, list);
}


/* Display a warning message for the ALSEC in sector SECNO.  PATH
   points to the path name chain of the file or directory. */

static void alsec_warning (int level, const char *fmt, ULONG secno,
                           const path_chain *path, ...)
{
  va_list arg_ptr;

  warning_prolog (level);
  my_fprintf (diag_file, "ALSEC #%lu (\"%s\"): ",
              secno, format_path_chain (path, NULL));
  va_start (arg_ptr, path);
  my_vfprintf (diag_file, fmt, arg_ptr);
  va_end (arg_ptr);
  fputc ('\n', diag_file);
  warning_epilog ();
}


/* Process the ALSEC in sector SECNO.  PATH points to the path name
   chain of the file or directory.  PEXP_FILE_SEC points to an object
   containing the first logical sector number mapped by the ALSEC.
   The object's value will be incremented by the number of sectors
   mapped by the ALSEC.  The sector number of the sector following the
   previous run is passed in the object pointed to by PNEXT_DISK_SEC.
   The object will be updated to contain the sector number of the
   sector following the last run.  TOTAL_SECTORS is the number of
   sectors expected for the object.  PARENT_FNODE is the sector number
   of the FNODE of the directory containing the object.  PARENT_ALBLK
   is the sector number of the parent ALSEC or FNODE.  ALSEC_LEVEL
   contains the current level in the ALSEC tree.  WHAT is USE_FILE,
   USE_EA, or USE_ACL for file data, extended attributes, or ACL,
   respectively.  PEXTENTS points to an object counting the number of
   extents.  The value of that object will be updated.  Show
   information if SHOW is non-zero.  Copy up to COPY_SIZE bytes of
   data to the save file.  Read sectors to BUF (of BUF_SIZE bytes) if
   BUF is non-NULL.

   Return the height of the ALSEC sub-tree. */

static int do_alsec (DISKIO *d, ULONG secno, const path_chain *path,
                     ULONG *pexp_file_sec, ULONG *pnext_disk_sec,
                     ULONG total_sectors, ULONG parent_fnode,
                     ULONG parent_alblk, int alsec_level, BYTE what,
                     ULONG *pextents, int show, ULONG copy_size,
                     BYTE *buf, ULONG buf_size)
{
  HPFS_SECTOR alsec;
  int height;

  if (show)
    info ("ALSEC(%s): #%lu\n", alsec_number, secno);
  if (a_what && secno == what_sector)
    info ("Sector #%lu: Allocation sector (ALSEC) for \"%s\"\n",
          secno, format_path_chain (path, NULL));
  if (have_seen (secno, 1, SEEN_ALSEC, "ALSEC"))
    return 1;
  use_sectors (secno, 1, USE_ALSEC, path);
  read_sec (d, &alsec, secno, 1, TRUE);
  if (ULONG_FROM_FS (alsec.alsec.sig) != ALSEC_SIG1)
    {
      alsec_warning (1, "Bad signature", secno, path);
      return 1;
    }
  ++alsec_count;
  if (ULONG_FROM_FS (alsec.alsec.lsnSelf) != secno)
    alsec_warning (1, "Incorrect self pointer", secno, path);
  if (ULONG_FROM_FS (alsec.alsec.lsnRent) != parent_alblk)
    alsec_warning (1, "Incorrect parent pointer", secno, path);

  height = do_storage (d, secno, &alsec.alsec.alb, 40, path,
                       pexp_file_sec, pnext_disk_sec, total_sectors,
                       parent_fnode, alsec_level + 1, what, pextents,
                       show, copy_size, buf, buf_size);
  return height + 1;
}


/* Display a warning message for the allocation structure in sector
   SECNO.  PATH points to the path name chain of the file or
   directory. */

static void alloc_warning (int level, const char *fmt, ULONG secno,
                           const path_chain *path, int fnode_flag, ...)
{
  va_list arg_ptr;

  warning_prolog (level);
  my_fprintf (diag_file, "%s #%lu (\"%s\"): ",
              fnode_flag ? "FNODE" : "ALSEC",
              secno, format_path_chain (path, NULL));
  va_start (arg_ptr, path);
  my_vfprintf (diag_file, fmt, arg_ptr);
  va_end (arg_ptr);
  fputc ('\n', diag_file);
  warning_epilog ();
}


/* Process the allocation structure pointed to by HEADER.  It is in
   sector SECNO (used for messages only).  The structure contains
   LEAF_COUNT leaves (the number of nodes is computed from
   LEAF_COUNT).  PATH points to the path name chain of the file or
   directory.  PEXP_FILE_SEC points to an object containing the first
   logical sector number mapped by the allocation structure.  The
   object's value will be incremented by the number of sectors mapped
   by the allocation strcuture.  The sector number of the sector
   following the previous run is passed in the object pointed to by
   PNEXT_DISK_SEC.  The object will be updated to contain the sector
   number of the sector following the last run.  TOTAL_SECTORS is the
   number of sectors expected for the object.  PARENT_FNODE is the
   sector number of the FNODE of the directory containing the object.
   ALSEC_LEVEL contains the current level in the ALSEC tree.  WHAT is
   USE_FILE, USE_EA, or USE_ACL for file data, extended attributes, or
   ACL, respectively.  PEXTENTS points to an object counting the
   number of extents.  The value of that object will be updated.  Show
   information if SHOW is non-zero.  Copy up to COPY_SIZE bytes of
   data to the save file.  Read sectors to BUF (of BUF_SIZE bytes) if
   BUF is non-NULL.

   Return the height of the ALSEC tree. */

static int do_storage (DISKIO *d, ULONG secno, const ALBLK *header,
                       ULONG leaf_count, const path_chain *path,
                       ULONG *pexp_file_sec, ULONG *pnext_disk_sec,
                       ULONG total_sectors, ULONG parent_fnode,
                       int alsec_level, BYTE what, ULONG *pextents,
                       int show, ULONG copy_size, BYTE *buf, ULONG buf_size)
{
  const ALLEAF *pleaf = (ALLEAF *)((char *)header + sizeof (ALBLK));
  const ALNODE *pnode = (ALNODE *)((char *)header + sizeof (ALBLK));
  ULONG i, j, n, start, count, pos;
  const char *what_text;
  int fnode_flag = (leaf_count == 8);

  if (show)
    info ("  %s count:                  %u\n",
          (header->bFlag & ABF_NODE) ? "Node" : "Leaf", header->cUsed);

  /* Note: Do not check for underflow -- as the root node has a
     smaller maximum node/leaf count than the other nodes of the tree,
     it's impossible to make all (non-root) nodes at least half
     filled. */

  switch (what)
    {
    case USE_EA:
      what_text = "EA data";
      break;
    case USE_FILE:
      what_text = "file data";
      break;
    case USE_ACL:
      what_text = "ACL";
      break;
    default:
      what_text = "???";
      break;
    }
  if (!(header->bFlag & ABF_FNP) != !(alsec_level == 1))
    alloc_warning (1, "ABF_FNP bit is wrong (%d)", secno, path, fnode_flag,
                   !!(header->bFlag & ABF_FNP));
  n = header->cUsed;
  if (header->bFlag & ABF_NODE)
    {
      ULONG node_count = leaf_count + leaf_count / 2;
      size_t nlen;
      int height, max_height;

      if ((ULONG)header->cFree + (ULONG)header->cUsed != node_count)
        {
          alloc_warning (1, "Wrong number of ALNODEs",
                         secno, path, fnode_flag);
          if (n > node_count)
            n = node_count;
        }
      if (n * sizeof (ALNODE) + sizeof (ALBLK)
          != USHORT_FROM_FS (header->oFree))
        alloc_warning (1, "Offset to free entry is wrong",
                       secno, path, fnode_flag);
      nlen = strlen (alsec_number);
      max_height = 0;
      for (i = 0; i < n; ++i)
        {
          sprintf (alsec_number + nlen, ".%lu", i);
          height = do_alsec (d, ULONG_FROM_FS (pnode[i].lsnPhys), path,
                             pexp_file_sec, pnext_disk_sec, total_sectors,
                             parent_fnode, secno, alsec_level, what,
                             pextents, show, copy_size, buf, buf_size);
          if (ULONG_FROM_FS (pnode[i].lsnLog)
              != (i + 1 == n ? 0xffffffff : *pexp_file_sec))
            alloc_warning (1, "Wrong file sector in ALNODE (%lu vs. %lu)",
                           secno, path, fnode_flag,
                           ULONG_FROM_FS (pnode[i].lsnLog),
                           (i + 1 == n ? 0xffffffff : *pexp_file_sec));
          if (i == 0)
            max_height = height;
          else
            {
              if (height != max_height)
                alloc_warning (1, "Unbalanced allocation tree",
                               secno, path, fnode_flag);
              if (height > max_height)
                max_height = height;
            }
        }
      alsec_number[nlen] = 0;
      return max_height;
    }
  else
    {
      if ((ULONG)header->cFree + (ULONG)header->cUsed != leaf_count)
        {
          alloc_warning (1, "Wrong number of ALLEAFs",
                         secno, path, fnode_flag);
          if (n > leaf_count)
            n = leaf_count;
        }
      if (n * sizeof (ALLEAF) + sizeof (ALBLK)
          != USHORT_FROM_FS (header->oFree))
        alloc_warning (1, "Offset to free entry is wrong",
                       secno, path, fnode_flag);
      *pextents += n;
      for (i = 0; i < n; ++i)
        {
          start = ULONG_FROM_FS (pleaf[i].lsnPhys);
          count = ULONG_FROM_FS (pleaf[i].csecRun);
          if (ULONG_FROM_FS (pleaf[i].lsnLog) != *pexp_file_sec)
            alloc_warning (1, "Wrong file sector (%lu vs. %lu)",
                           secno, path, fnode_flag,
                           ULONG_FROM_FS (pleaf[i].lsnLog), *pexp_file_sec);
          if (check_pedantic && *pnext_disk_sec != 0
              && start == *pnext_disk_sec)
            alloc_warning (0, "Contiguous runs of disk sectors",
                           secno, path, fnode_flag);
          *pnext_disk_sec = start + count;
          if (show)
            info ("  %c%s in %s (file sector %lu)\n",
                  toupper (what_text[0]), what_text+1,
                  format_sector_range (start, count),
                  ULONG_FROM_FS (pleaf[i].lsnLog));
          if (a_what && IN_RANGE (what_sector, start, count))
            info ("Sector #%lu: Sector %lu of %s for \"%s\" (+%lu)\n",
                  what_sector,
                  *pexp_file_sec + what_sector - start,
                  what_text, format_path_chain (path, NULL),
                  what_sector - start);
          if (a_check && sectors_per_block > 1 && what == USE_FILE)
            {
              /* These criterions have been guessed... */
              if (count < sectors_per_block
                  && *pexp_file_sec + count < total_sectors)
                alloc_warning (1, "Too fragmented for the `multimedia format'",
                               secno, path, fnode_flag);
              if (start & 3)
                alloc_warning (1, "Run not properly aligned for the "
                               "`multimedia format'", secno, path, fnode_flag);
            }
          use_sectors (start, count, what, path);
          pos = *pexp_file_sec * 512;
          if (buf != NULL)
            {
              if ((*pexp_file_sec + count) * 512 > buf_size)
                abort ();
              read_sec (d, buf + pos, start, count, TRUE);
            }
          for (j = 0; j < count && pos < copy_size; ++j)
            {
              read_sec (d, copy_buf, start, 1, FALSE);
              fwrite (copy_buf, MIN (copy_size - pos, 512), 1, save_file);
              if (ferror (save_file))
                save_error ();
              ++start; pos += 512;
            }
          *pexp_file_sec += count;
        }
      return 0;
    }
}


/* Display a warning message for the FNODE in sector SECNO.  PATH
   points to the path name chain of the file or directory. */

static void fnode_warning (int level, const char *fmt, ULONG secno,
                           const path_chain *path, ...)
{
  va_list arg_ptr;

  warning_prolog (level);
  my_fprintf (diag_file, "FNODE #%lu (\"%s\"): ",
              secno, format_path_chain (path, NULL));
  va_start (arg_ptr, path);
  my_vfprintf (diag_file, fmt, arg_ptr);
  va_end (arg_ptr);
  fputc ('\n', diag_file);
  warning_epilog ();
}


/* Process extended attributes.  BUF points to a buffer containing
   BUF_SIZE bytes of data.  SECNO is the sector number of the FNODE.
   PATH is the path name chain for the file or directory.  EA_SIZE is
   the expected size of the extended attributes.  Ignore EA_SIZE if
   CHECK_EA_SIZE is FALSE.  EA_NEED is the expected number of `need'
   EAs.  Display information if SHOW is true. */

static void do_auxinfo_ea (DISKIO *d, const BYTE *buf, ULONG buf_size,
                           ULONG secno, const path_chain *path,
                           ULONG ea_size, int check_ea_size, ULONG ea_need,
                           int show)
{
  ULONG pos, size, need_ea_count, start, count, bytes;
  ULONG value_size, file_sec, disk_sec, extents;
  const FEA *pfea;
  const SPTR *psptr;

  /* BUF points to a list of FEA structures.  Process them one by
     one. */

  pos = 0; size = 0; need_ea_count = 0;
  while (pos < buf_size)
    {
      /* Stop if the FEA structure is not completely inside the
         buffer. */

      if (pos + sizeof (FEA) > buf_size)
        {
          fnode_warning (1, "Truncated FEA structure", secno, path);
          break;
        }
      pfea = (FEA *)(buf + pos);
      value_size = USHORT_FROM_FS (pfea->cbValue);
      if (pos + sizeof (FEA) + pfea->cbName + 1 + value_size > buf_size)
        {
          fnode_warning (1, "Truncated FEA structure", secno, path);
          break;
        }

      /* The name of an EA must be terminated by a null character. */

      if (buf[pos + sizeof (FEA) + pfea->cbName] != 0)
        fnode_warning (1, "Name of extended attribute not terminated by "
                       "a null character", secno, path);

      /* Count the number of `need' EAs.  After processing all FEA
         structures, we'll compare the actual count to the expected
         count (from the FNODE). */

      if (pfea->fEA & FEA_NEEDEA)
        ++need_ea_count;

      /* There are three methods for storing the value of an extended
         attribute.  The fEA member of the FEA structure tells us
         which one is used for the current EA.  Note that bit 7
         (FEA_NEEDEA) is used to mark `need' EAs. */

      if ((pfea->fEA & 0x7f) == 0x00)
        {
          /* Method 1: The EA value is stored inline, directly
             following the FEA structure. */

          if (show_eas >= 1)
            info ("  Extended attribute %s (%lu bytes) is stored inline\n",
                  format_ea_name (pfea), value_size);
          size += sizeof (FEA) + pfea->cbName + 1 + value_size;
          if (show_frag)
            extents_stat (&ea_extents, 0);
        }
      else if ((pfea->fEA & 0x7f) == 0x01)
        {
          /* Method 2: The EA value is stored externally, in one run
             of sectors pointed to by a SPTR structure directly
             following the FEA structure. */

          if (value_size != sizeof (SPTR))
            fnode_warning (1, "Incorrect size of FEA structure", secno, path);
          else
            {
              psptr = (const SPTR *)(buf + pos + sizeof (FEA)
                                     + pfea->cbName + 1);
              start = ULONG_FROM_FS (psptr->lsn);
              bytes = ULONG_FROM_FS (psptr->cbRun);
              count = DIVIDE_UP (bytes, 512);
              if (show_eas >= 1)
                info ("  Extended attribute %s (%lu bytes) is stored "
                      "in %s\n",
                      format_ea_name (pfea), bytes,
                      format_sector_range (start, count));
              else if (show)
                info ("  Extended attributes in %s\n",
                      format_sector_range (start, count));
              if (a_what && IN_RANGE (what_sector, start, count))
                info ("Sector #%lu: EA data for \"%s\"\n",
                      what_sector, format_path_chain (path, NULL));
              use_sectors (start, count, USE_EA, path);
              size += sizeof (FEA) + pfea->cbName + 1 + bytes;
              if (show_frag)
                extents_stat (&ea_extents, 1);
            }
        }
      else if ((pfea->fEA & 0x7f) == 0x03)
        {
          /* Method 3: The EA value is stored externally, in several
             runs of sectors, mapped by an ALSEC which is pointed to
             by a SPTR structure directly following the FEA
             structure. */

          if (value_size != sizeof (SPTR))
            fnode_warning (1, "Incorrect size of FEA structure", secno, path);
          else
            {
              psptr = (const SPTR *)(buf + pos + sizeof (FEA)
                                     + pfea->cbName + 1);
              start = ULONG_FROM_FS (psptr->lsn);
              bytes = ULONG_FROM_FS (psptr->cbRun);
              if (show_eas >= 1)
                info ("  Extended attribute %s (%lu bytes) is stored in "
                      "sectors mapped by ALSEC #%lu\n",
                      format_ea_name (pfea), bytes, start);

              /* Process the ALSEC. */

              file_sec = 0; disk_sec = 0; extents = 0;
              strcpy (alsec_number, "0");
              do_alsec (d, start, path, &file_sec, &disk_sec,
                        DIVIDE_UP (bytes, 512), secno, secno,
                        0, USE_EA, &extents, show, 0, NULL, 0);
              if (show_eas >= 1)
                info ("  Number of sectors for this EA: %lu\n", file_sec);

              /* Check the number of sectors.  There is a bug in
                 HPFS.IFS: It never assigns more than 40 extents (that
                 is, one ALSEC), truncating EAs. */

              if (file_sec * 512 < bytes)
                fnode_warning (1, "Not enough sectors allocated for EA %s",
                               secno, path, format_ea_name (pfea));
              if (file_sec > DIVIDE_UP (bytes, 512))
                fnode_warning (1, "Too many sectors allocated for EA %s (%lu)",
                               secno, path, format_ea_name (pfea),
                               file_sec - DIVIDE_UP (bytes, 512));

              size += sizeof (FEA) + pfea->cbName + 1 + bytes;
              if (show_frag)
                extents_stat (&ea_extents, extents);
            }
        }
      else
        {
          /* No other methods are known. */

          fnode_warning (1, "Invalid FEA flag 0x%.2x for extended "
                         "attribute %s", secno, path, pfea->fEA,
                         format_ea_name (pfea));
          break;
        }
      pos += sizeof (FEA) + pfea->cbName + 1 + value_size;
    }

  /* Compare the actual EA size and `need' EA count to the values
     stored in the FNODE. */

  if (check_ea_size && size != ea_size)
    fnode_warning (1, "Incorrect EA size (%lu vs. %lu)",
                   secno, path, size, ea_size);
  if (need_ea_count != ea_need)
    fnode_warning (1, "Incorrect number of `need' EAs (%lu vs. %lu)",
                   secno, path, need_ea_count, ea_need);
}


/* Process the data pointed to by an AUXINFO structure (for EAs or
   ACL).  BUF points to a buffer containing BUF_SIZE bytes of data.
   WHAT indicates whether the AUXINFO structure is for EAs or an ACL.
   SECNO is the sector number of the FNODE.  PATH is the path name
   chain for the file or directory.  EA_SIZE is the expected size of
   the extended attributes.  Ignore EA_SIZE if CHECK_EA_SIZE is FALSE.
   EA_NEED is the expected number of `need' EAs.  Display information
   if SHOW is true. */

static void do_auxinfo_buf (DISKIO *d, const BYTE *buf, ULONG buf_size,
                            BYTE what, ULONG secno, const path_chain *path,
                            ULONG ea_size, int check_ea_size, ULONG ea_need,
                            int show)
{
  switch (what)
    {
    case USE_EA:
      do_auxinfo_ea (d, buf, buf_size, secno, path, ea_size, check_ea_size,
                     ea_need, show);
      break;
    case USE_ACL:
      /* I do not yet know anything about ACLs. */
      break;
    }
}


/* Process the AUXINFO structure (for EAs or ACL) pointed to by PAI.
   If the structure indicates that the data is stored inside the
   FNODE, take the data from PFNODE, starting at offset BASE.  SECNO
   is the sector number of the FNODE.  PATH is the path name chain for
   the file or directory.  WHAT indicates whether the AUXINFO
   structure is for EAs or an ACL.  EA_SIZE is the expected size of
   the extended attributes, EA_NEED is the expected number of `need'
   EAs.  Display information if SHOW is true. */

static void do_auxinfo (DISKIO *d, const FNODE *pfnode, const AUXINFO *pai,
                        unsigned base, ULONG secno, const path_chain *path,
                        BYTE what, ULONG ea_size, int check_ea_size,
                        ULONG ea_need, int show)
{
  BYTE *buf;
  ULONG buf_size, file_sec, disk_sec, extents, run_length, start, fnode_length;

  /* Fetch some values from the AUXINFO structure. */

  run_length = ULONG_FROM_FS (pai->sp.cbRun);
  start = ULONG_FROM_FS (pai->sp.lsn);
  fnode_length = USHORT_FROM_FS (pai->usFNL);

  /* EAs and ACLs are either stored entirely inside the FNODE or
     entirely externally.  Having data both inside the FNODE and
     externally is an error. */

  if (run_length != 0 && fnode_length != 0)
    fnode_warning (1, "Both internal and external %s",
                   secno, path, (what == USE_EA ? "EA" : "ACL"));

  /* No buffer has been allocated so far. */

  buf = NULL; buf_size = 0;

  /* Data is stored outside the FNODE if pai->sp.cbRun is non-zero. */

  if (run_length != 0)
    {
      buf_size = run_length;

      /* If pai->bDat is non-zero, pai->sp.lsn points to an ALSEC.
         Otherwise it directly points to the first data sector. */

      if (pai->bDat)
        {
          /* The data is mapped by an ALSEC. */

          if (a_where)
            {
              if (what == USE_EA)
                info ("  Extended attributes (FEA structures, %lu bytes) "
                      "in sectors mapped by ALSEC #%lu\n",
                      run_length, start);
              else
                info ("  ACL (%lu bytes) in sectors mapped by ALSEC #%lu\n",
                      run_length, start);
            }
          if (buf_size <= 0x100000)
            buf = malloc (ROUND_UP (buf_size, 512));
          if (buf != NULL)
            memset (buf, 0, buf_size);
          file_sec = 0; disk_sec = 0; extents = 0;
          strcpy (alsec_number, "0");
          do_alsec (d, start, path, &file_sec, &disk_sec,
                    DIVIDE_UP (run_length, 512), secno, secno, 0,
                    what, &extents, show, 0, buf, ROUND_UP (buf_size, 512));
          if (file_sec * 512 < run_length)
            fnode_warning (1, "Not enough sectors allocated for %s",
                           secno, path, (what == USE_EA ? "EAs" : "ACLs"));
          if (file_sec > DIVIDE_UP (run_length, 512))
            fnode_warning (1, "Too many sectors allocated for %s (%lu)",
                           secno, path, (what == USE_EA ? "EAs" : "ACLs"),
                           file_sec - DIVIDE_UP (run_length, 512));
        }
      else
        {
          /* The data is stored in one run of sectors. */

          ULONG count;

          count = DIVIDE_UP (run_length, 512);
          if (a_where)
            {
              if (what == USE_EA)
                info ("  Extended attributes (FEA structures, %lu bytes) "
                      "in %s\n",
                      run_length, format_sector_range (start, count));
              else
                info ("  ACL (%lu bytes) in %s\n",
                      run_length, format_sector_range (start, count));
            }

          if (a_what && IN_RANGE (what_sector, start, count))
            {
              if (what == USE_EA)
                info ("Sector #%lu: Extended attributes (FEA structures) for "
                      "\"%s\" (+%lu)\n",
                      what_sector, format_path_chain (path, NULL),
                      what_sector - start);
              else
                info ("Sector #%lu: ACL for \"%s\" (+%lu)\n",
                      what_sector, format_path_chain (path, NULL),
                      what_sector - start);
            }
          use_sectors (start, count, what, path);
          if (buf_size <= 0x100000)
            {
              buf = xmalloc (count * 512);
              read_sec (d, buf, start, count, TRUE);
            }
        }
    }
  else if (fnode_length != 0)
    {
      /* The data is stored inside the FNODE. */

      if (a_where)
        {
          if (what == USE_EA)
            info ("  Extended attributes (FEA structures, %lu bytes at 0x%x) "
                  "in FNODE #%lu\n", fnode_length, base, secno);
          else
            info ("  ACL (%lu bytes at 0x%x) in FNODE #%lu\n",
                  fnode_length, base, secno);
        }
      if (base < offsetof (FNODE, abFree))
        fnode_warning (1, "%s offset invalid", secno, path,
                       (what == USE_EA ? "EA" : "ACL"));
      else if (base + fnode_length > 512)
        fnode_warning (1, "%s beyond end of FNODE", secno, path,
                       (what == USE_EA ? "EA list" : "ACL"));
      else
        do_auxinfo_buf (d, (const BYTE *)pfnode + base, fnode_length,
                        what, secno, path, ea_size, check_ea_size, ea_need,
                        show);
    }

  /* If we have copied the data to a buffer, call do_auxinfo_buf() on
     that buffer.  (For data stored inside the FNODE, do_auxinfo_buf()
     has already been called above, without allocating a buffer.) */

  if (buf_size != 0)
    {
      if (buf == NULL)
        fnode_warning (1, "%s too big for examination", secno, path,
                       (what == USE_EA ? "EAs" : "ACL"));
      else
        {
          do_auxinfo_buf (d, buf, buf_size, what, secno, path,
                          ea_size, check_ea_size, ea_need, show);
          free (buf);
        }
    }
}


/* Process the FNODE in sector SECNO.  PATH points to the path name
   chain for the file or directory.  DIR_FLAG is true if the FNODE
   belongs to a directory.  PARENT_FNODE is the sector number of the
   FNODE of the directory containing the object.  FILE_SIZE is the
   size of the file.  EA_SIZE is the total size of the extended
   attributes of the file or directory.  Ignore EA_SIZE if
   CHECK_EA_SIZE is FALSE (the root directory is the only object for
   which EA_SIZE is unknown).  NEED_EAS is non-zero if the object has
   `need' EAs.  List the directory if LIST is true. */

static void do_fnode (DISKIO *d, ULONG secno, const path_chain *path,
                      int dir_flag, ULONG parent_fnode, ULONG file_size,
                      ULONG ea_size, int check_ea_size, int need_eas, int list)
{
  HPFS_SECTOR fnode;
  ULONG file_sec, disk_sec, extents, fn_fsize, i;
  size_t name_len;
  int show, found, height;

  found = (a_find && *find_path == 0);
  show = (found && a_where);
  if (show)
    info ("FNODE: #%lu\n", secno);
  if (a_what && secno == what_sector)
    {
      info ("Sector #%lu: FNODE for \"%s\"\n",
            secno, format_path_chain (path, NULL));
      show = TRUE;
    }
  if (have_seen (secno, 1, SEEN_FNODE, "FNODE"))
    return;
  use_sectors (secno, 1, USE_FNODE, path);
  read_sec (d, &fnode, secno, 1, TRUE);
  if (ULONG_FROM_FS (fnode.fnode.sig) != FNODE_SIG1)
    {
      fnode_warning (1, "Bad signature", secno, path);
      if (found)
        quit (0, FALSE);
      return;
    }
  if (dir_flag)
    ++dir_count;
  else
    ++file_count;
  fn_fsize = ULONG_FROM_FS (fnode.fnode.fst.ulVLen);
  if (!(fnode.fnode.bFlag & FNF_DIR) != !dir_flag)
    fnode_warning (1, "Incorrect directory bit", secno, path);
  if (ULONG_FROM_FS (fnode.fnode.lsnContDir) != parent_fnode)
    fnode_warning (1, "Wrong pointer to containing directory", secno, path);
  if (a_check)
    {
      if ((ULONG_FROM_FS (fnode.fnode.ulRefCount) == 0) != !need_eas)
        fnode_warning (1, "Need-EA bit of DIRENT is wrong", secno, path);
      name_len = strlen (path->name);
      if (fnode.fnode.achName[0] != name_len
          && memcmp (fnode.fnode.achName, path->name, MIN (name_len, 16)) == 0)
        fnode_warning (0, "Truncated name mangled by OS/2 2.0 bug",
                       secno, path);
      else if (fnode.fnode.achName[0] != name_len)
        fnode_warning (1, "Wrong full name length (%lu vs. %lu)",
                       secno, path, (ULONG)fnode.fnode.achName[0],
                       (ULONG)name_len);
      else if (memcmp (fnode.fnode.achName+1, path->name,
                       MIN (name_len, 15)) != 0)
        fnode_warning (1, "Wrong truncated name", secno, path);
      if (!dir_flag && file_size != fn_fsize)
        fnode_warning (1, "File size does not match DIRENT", secno, path);
      if (check_pedantic)
        {
          for (i = 0; i < sizeof (fnode.fnode.abSpare); ++i)
            if (fnode.fnode.abSpare[i] != 0)
              fnode_warning (0, "abSpare[%lu] is 0x%.2x", secno, path,
                             i, fnode.fnode.abSpare[i]);
        }
    }

  if (show)
    {
      info ("  Flags:                       0x%.2x", fnode.fnode.bFlag);
      if (fnode.fnode.bFlag & FNF_DIR)
        info (" dir");
      info ("\n");
      info ("  Size of file:                %lu\n", fn_fsize);
      info ("  Number of `need' EAs:        %lu\n",
            ULONG_FROM_FS (fnode.fnode.ulRefCount));
      info ("  Offset of first ACE:         %u\n",
            USHORT_FROM_FS (fnode.fnode.usACLBase));
      info ("  ACL size in FNODE:           %u\n",
            USHORT_FROM_FS (fnode.fnode.aiACL.usFNL));
      info ("  External ACL size:           %lu\n",
            ULONG_FROM_FS (fnode.fnode.aiACL.sp.cbRun));
    }

  if (dir_flag)
    {
      if (show)
        info ("  Root DIRBLK sector:          #%lu\n",
              ULONG_FROM_FS (fnode.fnode.fst.a.aall[0].lsnPhys));
      if (a_copy && found)
        error ("Directories cannot be copied");
      if (a_find && !found && !list)
        {
          const char *pdelim;
          size_t len;

          pdelim = strchr (find_path, '\\');
          if (pdelim == NULL)
            len = strlen (find_path);
          else
            len = pdelim - find_path;
          if (len > 255)
            error ("Path name component too long");
          memcpy (find_comp, find_path, len);
          find_comp[len] = 0;
          find_path += len;
          if (*find_path == '\\')
            {
              ++find_path;
              if (*find_path == 0)
                error ("Trailing backslash");
            }
        }
      if (!found || list)
        {
          SORT sort;
          int index, dotdot, i;
          int down_ptr[MAX_DIRBLK_LEVELS];

          sort.name[0] = 0;
          sort.cpindex = code_page_count;
          index = 0; dotdot = FALSE;
          for (i = 0; i < MAX_DIRBLK_LEVELS; ++i)
            down_ptr[i] = -1; /* Existence of down pointer is unknown */
          do_dirblk (d,
                     ULONG_FROM_FS (fnode.fnode.fst.a.aall[0].lsnPhys),
                     path, secno, secno, &sort, down_ptr, 0, &index, &dotdot,
                     list);
          if (!dotdot)
            warning (1, "Missing \"..\" entry in directory \"%s\"",
                     format_path_chain (path, NULL));
        }
      if (a_find && !found)
        error ("\"%s\" not found in \"%s\"",
               find_comp, format_path_chain (path, NULL));
    }
  else
    {
      file_sec = 0; disk_sec = 0; extents = 0;
      alsec_number[0] = 0;
      height = do_storage (d, secno, &fnode.fnode.fst.alb, 8, path,
                           &file_sec, &disk_sec, DIVIDE_UP (fn_fsize, 512),
                           secno, 0, USE_FILE, &extents,
                           show, found && a_copy ? fn_fsize : 0, NULL, 0);
      if (show)
        {
          info ("  Allocation tree height:      %d\n", height);
          info ("  Number of sectors:           %lu\n", file_sec);
          info ("  Number of extents:           %lu\n", extents);
        }
      if (show_frag)
        extents_stat (&file_extents, extents);
      if (file_sec * 512 < fn_fsize)
        fnode_warning (1, "Not enough sectors allocated", secno, path);
      if (file_sec > DIVIDE_UP (fn_fsize, 512))
        fnode_warning (1, "Too many sectors allocated (%lu)",
                       secno, path, file_sec - DIVIDE_UP (fn_fsize, 512));
    }

  /* Extended attributes are stored in the FNODE.  ACL entries are
     stored first (at offset usACLBase), followed by the extended
     attributes. */

  do_auxinfo (d, &fnode.fnode, &fnode.fnode.aiEA,
              (USHORT_FROM_FS (fnode.fnode.usACLBase)
               + USHORT_FROM_FS (fnode.fnode.aiACL.usFNL)),
              secno, path, USE_EA, ea_size, check_ea_size,
              ULONG_FROM_FS (fnode.fnode.ulRefCount), show);

  do_auxinfo (d, &fnode.fnode, &fnode.fnode.aiACL,
              USHORT_FROM_FS (fnode.fnode.usACLBase),
              secno, path, USE_ACL, 0, FALSE, 0, show);

  if (found)
    {
      if (a_copy)
        save_close ();
      quit (0, TRUE);
    }
}


/* Complain about sectors which are used but marked unallocated.
   Optionally complain about sectors which are not in use but marked
   allocated. */

static void check_alloc (void)
{
  ULONG i, start, count;
  BYTE start_what, first;
  const path_chain *start_path;

  /* List used sectors not marked as allocated. */

  i = 0; first = TRUE;
  while (i < total_sectors)
    {
      if (usage_vector[i] != USE_EMPTY && !ALLOCATED (i))
        {
          start = i; start_what = usage_vector[i];
          start_path = path_vector != NULL ? path_vector[i] : NULL;
          do
            {
              ++i;
            } while (i < total_sectors
                     && usage_vector[i] != USE_EMPTY && !ALLOCATED (i)
                     && usage_vector[i] == start_what
                     && ((path_vector != NULL ? path_vector[i] : NULL)
                         == start_path));
          if (first)
            {
              warning (1, "There are used sectors which are not marked "
                       "as allocated:");
              first = FALSE;
            }
          warning (1, "Used (%s) but not marked as allocated: %s",
                   sec_usage (start_what),
                   format_sector_range (start, i - start));
          if (start_path != NULL)
            warning_cont ("File: \"%s\"",
                          format_path_chain (start_path, NULL));
        }
      else
        ++i;
    }

  /* List unused sectors marked as allocated. */

  i = 0; count = 0;
  while (i < total_sectors)
    {
      if (usage_vector[i] == USE_EMPTY && ALLOCATED (i))
        {
          start = i;
          do
            {
              ++i;
            } while (i < total_sectors
                     && usage_vector[i] == USE_EMPTY && ALLOCATED (i));
          if (check_unused)
            warning (0, "Unused but marked as allocated: %s",
                     format_sector_range (start, i - start));
          count += i - start;
          if (IN_RANGE (18, start, i - start))
            count -= 1;
          if (IN_RANGE (19, start, i - start))
            count -= 1;
        }
      else
        ++i;
    }
  if (count == 1)
    warning (0, "The file system has 1 lost sector");
  else if (count > 1)
    warning (0, "The file system has %lu lost sectors", count);
}


/* Process the DIRBLK band bitmap starting in sector BSECNO.  The
   bitmap describes COUNT DIRBLKs starting at sector number START. */

static void do_dirblk_bitmap (DISKIO *d, ULONG bsecno, ULONG start,
                              ULONG count)
{
  ULONG sectors, i, dsecno;
  BYTE bitmap[2048];

  /* Compute the number of sectors used for the DIRBLK bitmap. */

  sectors = DIVIDE_UP (count, 512 * 8);

  /* The DIRBLK band bitmap always occupies 4 sectors. */

  if (sectors > 4)
    {
      warning (1, "DIRBLK band too big\n");
      sectors = 4;
    }
  read_sec (d, bitmap, bsecno, sectors, TRUE);

  /* Compare the bitmap to our usage_vector[]. */

  dsecno = start;
  for (i = 0; i < count; ++i)
    {
      if (BITSETP (bitmap, i))
        {
          if (usage_vector[dsecno] != USE_BANDDIRBLK)
            warning (1, "Sector #%lu is marked available in the "
                     "DIRBLK bitmap, but is used as %s\n",
                     dsecno, sec_usage (usage_vector[dsecno]));
        }
      else
        {
          if (usage_vector[dsecno] != USE_DIRBLK)
            warning (1, "Sector #%lu is marked used in the DIRBLK bitmap, "
                     "but is used as %s\n",
                     dsecno, sec_usage (usage_vector[dsecno]));
        }
      dsecno += 4;
    }
}


/* Check the spare DIRBLKs.  LIST points to an array of COUNT sector
   numbers (in HPFS representation). */

static void check_sparedirblk (const ULONG *list, ULONG total, ULONG free)
{
  ULONG i, secno;

  for (i = free; i < total; ++i)
    {
      secno = ULONG_FROM_FS (list[i]);
      if (secno < total_sectors && usage_vector[secno] != USE_DIRBLK)
        warning (1, "Spare DIRBLK #%lu is not used for a DIRBLK", secno);
    }
}


/* Build a time stamp for year D, month M, day D. */

static ULONG make_time (int y, int m, int d)
{
  struct tm tm;

  tm.tm_sec = 0; tm.tm_min = 0; tm.tm_hour = 0;
  tm.tm_mday = d; tm.tm_mon = m - 1; tm.tm_year = y - 1900;
  tm.tm_isdst = 0;
  return (ULONG)mktime (&tm);
}


/* Process an HPFS volume. */

void do_hpfs (DISKIO *d)
{
  HPFS_SECTOR superb;
  HPFS_SECTOR spareb, spareb_tmp;
  ULONG i, n, superb_chksum, spareb_chksum;
  ULONG dirband_sectors;

  if (a_what && what_cluster_flag)
    error ("Cluster numbers not supported on HPFS");

  min_time = make_time (1980, 1, 1);
  alloc_ready = FALSE;

  /* All structures of HPFS are anchored in the Superblock (LSN 16)
     and Spare block (LSN 17) sectors. */

  /* Superblock. */

  read_sec (d, &superb, 16, 1, TRUE);
  if (ULONG_FROM_FS (superb.superb.sig1) != SUPER_SIG1
      || ULONG_FROM_FS (superb.superb.sig2) != SUPER_SIG2)
    error ("Invalid signature of superblock -- this is not an HPFS partition");

  /* Spare block. */

  read_sec (d, &spareb, 17, 1, TRUE);
  if (ULONG_FROM_FS (spareb.spareb.sig1) != SPARE_SIG1
      || ULONG_FROM_FS (spareb.spareb.sig2) != SPARE_SIG2)
    error ("Invalid signature of spare block");

  /* Compute and check the total number of sectors. */

  total_sectors = ULONG_FROM_FS (superb.superb.culSectsOnVol);
  if (a_what && what_sector >= total_sectors)
    warning (0, "Sector number #%lu is too big", what_sector);
  if (diskio_type (d) == DIO_DISK && total_sectors > diskio_total_sectors (d))
    warning (1, "HPFS extends beyond end of partition indicated by BPB");

  /* Set the block size.  The meaning of bAlign[1] is guessed. */

  sectors_per_block = 1;
  if (superb.superb.bFuncVersion == 4)
    sectors_per_block = 1 << spareb.spareb.bAlign[1];

  /* Allocate usage vector.  Initially, all sectors are unused. */

  usage_vector = (BYTE *)xmalloc (total_sectors);
  memset (usage_vector, USE_EMPTY, total_sectors);

  /* Allocate `have seen' vector which is used for avoiding loops.
     Initially, no sector has been seen. */

  seen_vector = (BYTE *)xmalloc (total_sectors);
  memset (seen_vector, 0, total_sectors);

  /* Allocate vector of path chains for improving error messages.
     Note that the following condition must match the condition in the
     PATH_CHAIN_NEW macro! */

  if (a_check && plenty_memory)
    {
      path_vector = xmalloc (total_sectors * sizeof (*path_vector));
      for (i = 0; i < total_sectors; ++i)
        path_vector[i] = NULL;
    }
  else
    path_vector = NULL;

  code_page_count = ULONG_FROM_FS (spareb.spareb.culCP);

  /* Compute the checksums. */

  superb_chksum = chksum (superb.raw, 512);
  spareb_tmp = spareb;
  spareb_tmp.spareb.bFlag &= (SPF_VER | SPF_FASTFMT);
  spareb_tmp.spareb.aulExtra[1] = 0; /* TODO: ULONG_TO_HPFS */
  spareb_chksum = chksum (spareb_tmp.raw, 512);

  /* Boot sector. */

  if (a_what && what_sector == 0)
    info ("Sector #%lu: Boot sector\n", what_sector);
  use_sectors (0, 1, USE_BOOT, NULL);

  /* Boot loader. */

  if (a_what && IN_RANGE (what_sector, 1, 15))
    info ("Sector #%lu: Boot loader\n", what_sector);
  use_sectors (1, 15, USE_LOADER, NULL);

  /* Superblock. */

  use_sectors (16, 1, USE_SUPER, NULL);
  if (a_info || (a_what && what_sector == 16))
    {
      info ("Sector #%lu: Super block\n", (ULONG)16);
      info ("  HPFS Version:                       %d\n",
            superb.superb.bVersion);
      info ("  Functional version:                 %d",
            superb.superb.bFuncVersion);
      if (superb.superb.bFuncVersion == 2)
        info (" (<=4GB)\n");
      else if (superb.superb.bFuncVersion == 3)
        info (" (>4GB)\n");
      else if (superb.superb.bFuncVersion == 4)
        info (" (multimedia)\n");
      else
        info ("\n");
      info ("  Root directory FNODE at:            #%lu\n",
            ULONG_FROM_FS (superb.superb.lsnRootFNode));
      info ("  Total number of sectors:            %lu\n",
            ULONG_FROM_FS (superb.superb.culSectsOnVol));
      if (sector_number_format != 0
          && ULONG_FROM_FS (superb.superb.culSectsOnVol) != 0)
        info (  "Last sector:                        #%lu\n",
              ULONG_FROM_FS (superb.superb.culSectsOnVol) - 1);
      info ("  Number of bad sectors:              %lu\n",
            ULONG_FROM_FS (superb.superb.culNumBadSects));
      info ("  Bitmap indirect block at:           #%lu\n",
            ULONG_FROM_FS (superb.superb.rspBitMapIndBlk.lsnMain));
      info ("  Bad block list starts at:           #%lu\n",
            ULONG_FROM_FS (superb.superb.rspBadBlkList.lsnMain));
      info ("  Time of last chkdsk:                %s\n",
            format_time (ULONG_FROM_FS (superb.superb.datLastChkdsk)));
      info ("  Time of last optimization:          %s\n",
            format_time (ULONG_FROM_FS (superb.superb.datLastOptimize)));
      info ("  Number of sectors in DIRBLK band:   %lu\n",
            ULONG_FROM_FS (superb.superb.clsnDirBlkBand));
      info ("  First sector in DIRBLK band:        #%lu\n",
            ULONG_FROM_FS (superb.superb.lsnFirstDirBlk));
      info ("  Last sector in DIRBLK band:         #%lu\n",
            ULONG_FROM_FS (superb.superb.lsnLastDirBlk));
      info ("  First sector of DIRBLK band bitmap: #%lu\n",
            ULONG_FROM_FS (superb.superb.lsnDirBlkMap));
      info ("  Sector number of user ID table:     #%lu\n",
            ULONG_FROM_FS (superb.superb.lsnSidTab));
      info ("  Check sum (computed):               0x%.8lx\n", superb_chksum);
    }

  /* The Superblock points to the DIRBLK band. */

  dirband_sectors = ULONG_FROM_FS (superb.superb.clsnDirBlkBand);
  dirband_start = ULONG_FROM_FS (superb.superb.lsnFirstDirBlk);
  dirband_end = ULONG_FROM_FS (superb.superb.lsnLastDirBlk);
  dirblk_total = 0; dirblk_outside = 0;

  /* Spare block. */

  use_sectors (17, 1, USE_SPARE, NULL);
  if (a_info || (a_what && what_sector == 17))
    {
      info ("Sector #%lu: Spare block\n", (ULONG)17);
      info ("  Spare block flags:                  0x%.2x (",
            spareb.spareb.bFlag);
      if (spareb.spareb.bFlag & SPF_DIRT)
        info ("dirty");
      else
        info ("clean");
      if (spareb.spareb.bFlag & SPF_SPARE) info (" spare");
      if (spareb.spareb.bFlag & SPF_HFUSED) info (" hotfix");
      if (spareb.spareb.bFlag & SPF_BADSEC) info (" badsec");
      if (spareb.spareb.bFlag & SPF_BADBM) info (" badbmp");
      if (spareb.spareb.bFlag & SPF_FASTFMT) info (" fastfmt");
      if (spareb.spareb.bFlag & SPF_VER) info (" version");
      info (")\n");
      info ("  Block size:                         %lu\n",
            sectors_per_block * 512);
      info ("  Hotfix sector mapping table at:     #%lu\n",
            ULONG_FROM_FS (spareb.spareb.lsnHotFix));
      info ("  Number of hotfixes used:            %lu\n",
            ULONG_FROM_FS (spareb.spareb.culHotFixes));
      info ("  Maximum number of hotfixes:         %lu\n",
            ULONG_FROM_FS (spareb.spareb.culMaxHotFixes));
      info ("  Number of free spare DIRBLKs:       %lu\n",
            ULONG_FROM_FS (spareb.spareb.cdbSpares));
      info ("  Total number of spare DIRBLKs:      %lu\n",
            ULONG_FROM_FS (spareb.spareb.cdbMaxSpare));
      info ("  Code page information sector at:    #%lu\n",
            ULONG_FROM_FS (spareb.spareb.lsnCPInfo));
      info ("  Number of code pages:               %lu\n",
            ULONG_FROM_FS (spareb.spareb.culCP));
      info ("  Checksum of Super block:            0x%.8lx\n",
            ULONG_FROM_FS (spareb.spareb.aulExtra[0]));
      info ("  Checksum of Spare block:            0x%.8lx\n",
            ULONG_FROM_FS (spareb.spareb.aulExtra[1]));
      info ("  Check sum (computed):               0x%.8lx\n", spareb_chksum);
      n = ULONG_FROM_FS (spareb.spareb.cdbMaxSpare);
      for (i = 0; i < n; ++i)
        info ("  Spare DIRBLK at #%lu\n",
              ULONG_FROM_FS (spareb.spareb.alsnSpareDirBlks[i]));
    }

  /* DIRBLK band.  This is where DIRBLK preferably live.  If there is
     not enough space in the DIRBLK band, HPFS puts DIRBLKs in other
     available sectors (starting at a 4-sector boundary). */

  i = dirband_end - dirband_start + 1;
  if (a_what && IN_RANGE (what_sector, dirband_start, i))
    info ("Sector #%lu is in the DIRBLK band\n", what_sector);
  use_sectors (dirband_start, i, USE_BANDDIRBLK, NULL);

  /* The DIRBLK band has its own allocation bitmap.  Each bit maps one
     DIRBLK, that is, 4 sectors. */

  i = ULONG_FROM_FS (superb.superb.lsnDirBlkMap);
  if (a_info)
    info ("Sectors #%lu-#%lu: DIRBLK band bitmap\n", i, i + 3);
  if (a_what && IN_RANGE (what_sector, i, 4))
    info ("Sector #%lu is in the DIRBLK band bitmap (+%lu)\n",
          what_sector, what_sector - i);
  use_sectors (i, 4, USE_DIRBLKBITMAP, NULL);

  /* 8 sectors are reserved for storing user IDs.  Currently
     unused. */

  i = ULONG_FROM_FS (superb.superb.lsnSidTab);
  if (a_what && IN_RANGE (what_sector, i, 8))
    info ("Sector #%lu: User ID\n", what_sector);
  use_sectors (i, 8, USE_SID, NULL);

  /* Spare DIRBLKs.  If HPFS runs out of disk space for DIRBLKs in a
     split operation, it will use preallocated Spare DIRBLKs. */

  n = ULONG_FROM_FS (spareb.spareb.cdbMaxSpare);
  for (i = 0; i < n; ++i)
    {
      if (a_what
          && IN_RANGE (what_sector,
                       ULONG_FROM_FS (spareb.spareb.alsnSpareDirBlks[i]), 4))
        info ("Sector #%lu: Spare DIRBLK (+%lu)\n",
              what_sector,
              what_sector
              - ULONG_FROM_FS (spareb.spareb.alsnSpareDirBlks[i]));
      use_sectors (ULONG_FROM_FS (spareb.spareb.alsnSpareDirBlks[i]),
                   4, USE_SPAREDIRBLK, NULL);
    }

  /* Allocate the allocation vector which contains one bit per sector,
     indicating whether the sector is in use (0) or available (1).
     The vector will be filled in by do_bitmap(), called by
     do_bitmap_indirect().  */

  if (a_check || a_info || a_what)
    {
      total_alloc = DIVIDE_UP (total_sectors, 8);
      alloc_vector = (BYTE *)xmalloc (total_alloc);
      memset (alloc_vector, 0, total_alloc);
    }

  /* Check the Superblock and the Spare block. */

  if (a_check)
    {
      if (dirband_start > dirband_end)
        warning (1, "SUPERBLK #%lu: DIRBLK band start greater than "
                 "DIRBLK band end", (ULONG)16);
      if (dirband_sectors & 3)
        warning (1, "SUPERBLK #%lu: Number of DIRBLK band sectors is "
                 "not a multiple of 4", (ULONG)16);
      if (dirband_start + dirband_sectors - 1 != dirband_end)
        warning (1, "SUPERBLK #%lu: Wrong DIRBLK band size", (ULONG)16);
      if (ULONG_FROM_FS (superb.superb.lsnDirBlkMap) & 3)
        warning (1, "SUPERBLK #%lu: DIRBLK band bitmap not on a 2K boundary",
                 (ULONG)16);

      if (!(spareb.spareb.bFlag & SPF_HFUSED)
          != (ULONG_FROM_FS (spareb.spareb.culHotFixes) == 0))
        warning (1, "SPAREBLK #%lu: Hotfix bit is wrong", (ULONG)17);
      if (!(spareb.spareb.bFlag & SPF_BADSEC)
          != (ULONG_FROM_FS (superb.superb.culNumBadSects) == 0))
        warning (1, "SPAREBLK #%lu: Bad sector bit is wrong", (ULONG)17);
      if (!(spareb.spareb.bFlag & SPF_SPARE)
          != (ULONG_FROM_FS (spareb.spareb.cdbSpares)
              == ULONG_FROM_FS (spareb.spareb.cdbMaxSpare)))
        warning (1, "SPAREBLK #%lu: Spare DIRBLK bit is wrong", (ULONG)17);
      if (ULONG_FROM_FS (spareb.spareb.cdbSpares)
          > ULONG_FROM_FS (spareb.spareb.cdbMaxSpare))
        warning (1, "SPAREBLK #%lu: Number of free spare DIRBLKs exceeds "
                 "maximum number", (ULONG)17);
      if (ULONG_FROM_FS (spareb.spareb.aulExtra[0]) != superb_chksum)
        warning (1, "SPAREBLK #%lu: Incorrect checksum for Super block",
                 (ULONG)17);
      if (ULONG_FROM_FS (spareb.spareb.aulExtra[1]) != spareb_chksum)
        warning (1, "SPAREBLK #%lu: Incorrect checksum for Spare block",
                 (ULONG)17);

      if (superb.superb.bFuncVersion == 4)
        {
          /* `Multimedia format'.  To collect more samples for
             .bAlign, we display warnings if the values differ from
             those on my test partition.  I guess that .bAlign[0] is a
             set of flag bits and .bAlign[1] indicates the block size
             (1 << .bAlign[1] sectors per block).  .bAlign[2] is still
             unused. */

          if (spareb.spareb.bAlign[0] != 8)
            warning (0, "SPAREBLK #%lu: .bAlign[0] is %u", (ULONG)17,
                     spareb.spareb.bAlign[0]);
          if (spareb.spareb.bAlign[1] != 9)
            warning (0, "SPAREBLK #%lu: .bAlign[1] is %u", (ULONG)17,
                     spareb.spareb.bAlign[1]);
        }
      if (check_pedantic)
        {
          if (spareb.spareb.bAlign[2] != 0)
            warning (0, "SPAREBLK #%lu: .bAlign[2] is %u", (ULONG)17,
                     spareb.spareb.bAlign[2]);
        }
    }

  if (a_check || a_info || a_save || a_what)
    {
      /* Process the bad block list. */

      do_bad (d, ULONG_FROM_FS (superb.superb.rspBadBlkList.lsnMain),
              ULONG_FROM_FS (superb.superb.culNumBadSects));

      /* Process the hotfix list. */

      do_hotfix_list (d, ULONG_FROM_FS (spareb.spareb.lsnHotFix),
                      ULONG_FROM_FS (spareb.spareb.culMaxHotFixes));
    }

  /* Process the allocation bitmaps.  This fills in alloc_vector. */

  if (a_check || a_info || a_save || a_what)
    do_bitmap_indirect (d, ULONG_FROM_FS (superb.superb.rspBitMapIndBlk.lsnMain));

  /* Process the list of code page sectors. */

  if (a_check || a_info || a_save || a_what || a_find)
    do_cpinfosec (d, ULONG_FROM_FS (spareb.spareb.lsnCPInfo));

  /* Now comes the most interesting part: Walk through all directories
     and files, starting in the root directory. */

  file_count = 0; dir_count = 0;
  extents_init (&file_extents);
  extents_init (&ea_extents);
  if (a_check || a_save || a_what || a_find)
    {
      path_chain link, *plink;

      plink = PATH_CHAIN_NEW (&link, NULL, "");
      do_fnode (d, ULONG_FROM_FS (superb.superb.lsnRootFNode), plink,
                TRUE, ULONG_FROM_FS (superb.superb.lsnRootFNode),
                0, 0, FALSE, FALSE, (a_dir && *find_path == 0));
    }

  /* Process the DIRBLK bitmap. */

  if (a_check || a_save)
    do_dirblk_bitmap (d, ULONG_FROM_FS (superb.superb.lsnDirBlkMap),
                      dirband_start, dirband_sectors / 4);

  if (a_check)
    {
      /* Check the Spare DIRBLKs. */

      check_sparedirblk (spareb.spareb.alsnSpareDirBlks,
                         ULONG_FROM_FS (spareb.spareb.cdbMaxSpare),
                         ULONG_FROM_FS (spareb.spareb.cdbSpares));

      /* Check allocation bits. */

      check_alloc ();

      /* Show a summary. */

      if (show_summary)
        {
          info ("Number of directories: %lu\n", dir_count);
          info ("Number of files:       %lu\n", file_count);
          info ("Number of DIRBLKs:     %lu (%lu outside DIRBLK band)\n",
                dirblk_total, dirblk_outside);
          info ("Number of ALSECs:      %lu\n", alsec_count);
        }
    }

  /* Show fragmentation of free space. */

  if (a_info && show_free_frag)
    do_free_frag ();

  /* Show fragmentation of files and extended attributes. */

  if (show_frag)
    {
      extents_show (&file_extents, "file data");
      extents_show (&ea_extents, "extended attributes");
    }

  /* Clean up. */

  extents_exit (&file_extents);
  extents_exit (&ea_extents);
}
