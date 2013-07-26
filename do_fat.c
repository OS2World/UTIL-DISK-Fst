/* do_fat.c -- FAT-specific code for fst
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


/* TODO: Don't assume a sector size of 512 bytes. */

#include <os2.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "fst.h"
#include "crc.h"
#include "diskio.h"
#include "fat.h"


struct vfat
{
  char flag;
  char unprintable;
  BYTE total;
  BYTE index;
  BYTE checksum;
  int start;
  BYTE name[256+1];
};

static ULONG first_sector;
static ULONG total_sectors;
static ULONG total_clusters;
static ULONG sectors_per_cluster;
static ULONG bytes_per_cluster;
static ULONG sectors_per_fat;
static ULONG number_of_fats;
static ULONG root_entries;
static ULONG root_sectors;
static ULONG data_sector;
static ULONG what_cluster;
static USHORT **fats;
static USHORT *fat;

static BYTE *usage_vector;      /* One byte per cluster, indicating usage */
static const path_chain **path_vector; /* One path name chain per sector */

static BYTE find_comp[256];     /* Current component of `find_path' */

static char ea_ok;
static ULONG ea_data_start;
static ULONG ea_data_size;
static ULONG ea_data_clusters;
static USHORT ea_table1[240];   /* First table from `EA DATA. SF' */
static USHORT *ea_table2;       /* Second table from `EA DATA. SF' */
static ULONG ea_table2_entries;
static BYTE *ea_usage;          /* One byte per cluster of `EA DATA. SF' */

#define CLUSTER_TO_SECTOR(c) (((c) - 2) * sectors_per_cluster + data_sector)
#define SECTOR_TO_CLUSTER(s) (((s) - data_sector) / sectors_per_cluster + 2)


/* Rotate right a byte by one bit. */

static INLINE BYTE rorb1 (BYTE b)
{
  return (b & 1) ? (b >> 1) | 0x80 : b >> 1;
}


/* Compare two file names pointed to by P1 and P2. */

static int compare_fname (const BYTE *p1, const BYTE *p2)
{
  for (;;)
    {
      if (*p1 == 0 && *p2 == 0)
        return 0;
      if (*p2 == 0)
        return 1;
      if (*p1 == 0)
        return -1;
      if (cur_case_map[*p1] > cur_case_map[*p2])
        return 1;
      if (cur_case_map[*p1] < cur_case_map[*p2])
        return -1;
      ++p1; ++p2;
    }
}


/* Return a pointer to a string containing a formatted range of
   cluster numbers.  Note that the pointer points to static memory; do
   not use format_cluster_range() more than once in one expression! */

static const char *format_cluster_range (ULONG start, ULONG count)
{
  static char buf[60];

  if (count == 1)
    sprintf (buf, "cluster %lu", start);
  else
    sprintf (buf, "%lu clusters %lu-%lu", count, start, start + count - 1);
  return buf;
}


/* Return a pointer to a string containing a file time as string.
   Note that the pointer points to static memory; do not use
   format_time() more than once in one expression! */

static const char *format_time (unsigned t)
{
  static char buf[20];

  sprintf (buf, "%.2d:%.2d:%.2d",
           (t >> 11) & 31, (t >> 5) & 63, (t & 31) << 1);
  return buf;
}


/* Return a pointer to a string containing a file date as string.
   Note that the pointer points to static memory; do not use
   format_date() more than once in one expression! */

static const char *format_date (unsigned d)
{
  static char buf[20];

  sprintf (buf, "%d-%.2d-%.2d",
           ((d >> 9) & 127) + 1980, (d >> 5) & 15, d & 31);
  return buf;
}


/* Return the number of days in month M of year Y. */

static int days (int y, int m)
{
  static int month_len[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  if (m < 1 || m > 12)
    return 0;
  else if (m != 2)
    return month_len[m-1];
  else
    return y % 4 != 0 ? 28 : y % 100 != 0 ? 29 : y % 400 != 0 ? 28 : 29;
}


/* Types of clusters. */

#define USE_EMPTY       0
#define USE_FILE        1
#define USE_DIR         2


/* Return a pointer to a string containing a description of a cluster
   type. */

static const char *cluster_usage (BYTE what)
{
  switch (what)
    {
    case USE_EMPTY:
      return "empty";
    case USE_DIR:
      return "directory";
    case USE_FILE:
      return "file";
    default:
      return "INTERNAL_ERROR";
    }
}


/* Use cluster CLUSTER for WHAT.  PATH points to a string naming the
   object for which the cluster is used.  PATH points to the path name
   chain for the file or directory.  Return FALSE if a cycle has been
   detected. */

static int use_cluster (ULONG cluster, BYTE what, const path_chain *path)
{
  BYTE old;

  if (cluster >= total_clusters)
    abort ();
  old = usage_vector[cluster];
  if (old != USE_EMPTY)
    {
      warning (1, "Cluster %lu usage conflict: %s vs. %s",
               cluster, cluster_usage (usage_vector[cluster]),
               cluster_usage (what));
      if (path_vector != NULL && path_vector[cluster] != NULL)
        warning_cont ("File 1: \"%s\"",
                      format_path_chain (path_vector[cluster], NULL));
      if (path != NULL)
        warning_cont ("File 2: \"%s\"",
                      format_path_chain (path, NULL));
      return !(path != NULL && path == path_vector[cluster]);
    }
  else
    {
      usage_vector[cluster] = what;
      if (path_vector != NULL)
        path_vector[cluster] = path;
      return TRUE;
    }
}


static void dirent_warning (int level, const char *fmt, ULONG secno,
                            const path_chain *path,
                            const BYTE *name, ...) ATTR_PRINTF (2, 6);


static USHORT *read_fat16 (DISKIO *d, ULONG secno)
{
  USHORT *fat;
  ULONG sectors, clusters, i;

  clusters = total_clusters;
  sectors = DIVIDE_UP (clusters * 2, 512);
  if (sectors != sectors_per_fat)
    warning (1, "Incorrect FAT size: %lu vs. %lu", sectors, sectors_per_fat);
  fat = xmalloc (sectors * 512);
  read_sec (d, fat, secno, sectors, TRUE);
  for (i = 0; i < clusters; ++i)
    fat[i] = USHORT_FROM_FS (fat[i]);
  return fat;
}


static USHORT *read_fat12 (DISKIO *d, ULONG secno)
{
  USHORT *fat;
  ULONG clusters, sectors, i, s, t;
  USHORT c1, c2;
  BYTE *raw;

  clusters = total_clusters;
  sectors = DIVIDE_UP (clusters * 3, 512*2);
  if (sectors != sectors_per_fat)
    warning (1, "Incorrect FAT size: %lu vs. %lu", sectors, sectors_per_fat);
  raw = xmalloc (sectors * 512 + 2);
  read_sec (d, raw, secno, sectors, TRUE);
  fat = xmalloc (clusters * 2 + 1);
  s = 0;
  for (i = 0; i < clusters; i += 2)
    {
      t = raw[s+0] | (raw[s+1] << 8) | (raw[s+2] << 16);
      c1 = t & 0xfff;
      if (c1 >= 0xff7)
        c1 |= 0xf000;
      c2 = (t >> 12) & 0xfff;
      if (c2 >= 0xff7)
        c2 |= 0xf000;
      fat[i+0] = c1;
      fat[i+1] = c2;
      s += 3;
    }
  free (raw);
  return fat;
}


static USHORT *read_fat (DISKIO *d, ULONG secno, ULONG fatno)
{
  if (a_what)
    {
      if (!what_cluster_flag && IN_RANGE (what_sector, secno, sectors_per_fat))
        info ("Sector #%lu: Fat %lu (+%lu)\n", what_sector, fatno + 1,
              what_sector - secno);
    }
  if (total_clusters - 2 > 4085)
    return read_fat16 (d, secno);
  else
    return read_fat12 (d, secno);
}


static void do_fats (DISKIO *d)
{
  ULONG secno, i, j, k, free, bad;

  fats = xmalloc (number_of_fats * sizeof (*fats));
  secno = first_sector;
  for (i = 0; i < number_of_fats; ++i)
    {
      if (a_info)
        info ("FAT %lu:                      %s\n",
              i + 1, format_sector_range (secno, sectors_per_fat));
      fats[i] = read_fat (d, secno, i);
      secno += sectors_per_fat;
    }
  for (i = 0; i < number_of_fats; ++i)
    for (j = i + 1; j < number_of_fats; ++j)
      if (memcmp (fats[i], fats[j], total_clusters * sizeof (USHORT)) != 0)
        {
          warning (1, "FATs %lu and %lu differ", i, j);
          list_start ("Differing clusters:");
          for (k = 0; k < total_clusters; ++k)
            if (fats[i][k] != fats[j][k])
              list ("%lu", k);
          list_end ();
        }
  fat = fats[0];
  free = bad = 0;
  for (i = 2; i < total_clusters; ++i)
    if (fat[i] == 0)
      ++free;
    else if (fat[i] == 0xfff7)
      ++bad;
  if (a_info)
    {
      info ("Number of free clusters:    %lu\n", free);
      info ("Number of bad clusters:     %lu\n", bad);
    }
}


static void read_ea_data (DISKIO *d)
{
  FAT_SECTOR ea1;
  ULONG i, min_cluster, cluster, pos, size2;
  char *tab2, *cluster_buffer;

  if (ea_data_start == 0xffff)
    return;
  if (ea_data_start < 2 || ea_data_start >= total_clusters)
    {
      warning (1, "\"EA DATA. SF\": Start cluster (%lu) is invalid",
               ea_data_start);
      return;
    }

  /* Read the first table and copy it to `ea_table1'. */

  read_sec (d, &ea1, CLUSTER_TO_SECTOR (ea_data_start), 1, FALSE);
  if (memcmp (ea1.ea1.magic, "ED", 2) != 0)
    {
      warning (1, "\"EA DATA. SF\": Incorrect signature");
      return;
    }
  memcpy (ea_table1, ea1.ea1.table, sizeof (ea_table1));

  /* Find the smallest cluster number referenced by the first table.
     This gives us the maximum size of the second table. */

  min_cluster = ea_table1[0];
  for (i = 1; i < 240; ++i)
    if (ea_table1[i] < min_cluster)
      min_cluster = ea_table1[i];

  if (min_cluster < 1)
    {
      warning (1, "\"EA DATA. SF\": First table contains a zero entry");
      return;
    }
  if (min_cluster >= total_clusters)
    {
      warning (1, "\"EA DATA. SF\": Second table is too big (%lu clusters)",
               min_cluster);
      return;
    }

  /* We make the table too big by one sector to simplify reading the
     table. */

  size2 = min_cluster * bytes_per_cluster;
  if (size2 > ea_data_size)
    {
      warning (1, "\"EA DATA. SF\": Beyond end of file");
      return;
    }
  tab2 = xmalloc (size2);
  cluster_buffer = xmalloc (bytes_per_cluster);

  cluster = ea_data_start;
  for (pos = 0; pos < ea_data_size; pos += bytes_per_cluster)
    {
      if (cluster < 2 || cluster >= total_clusters)
        {
          warning (1, "\"EA DATA. SF\": Invalid FAT chain");
          free (cluster_buffer);
          return;
        }
      read_sec (d, cluster_buffer, CLUSTER_TO_SECTOR (cluster),
                sectors_per_cluster, TRUE);
      if (pos < size2)
        memcpy (tab2 + pos, cluster_buffer, 
                MIN (bytes_per_cluster, size2 - pos));
      cluster = fat[cluster];
    }
  free (cluster_buffer);

  /* Skip the first 512 bytes (table 1). */

  ea_table2 = (USHORT *)(tab2 + 512);
  ea_table2_entries = (size2 - 512) / 2;

  ea_usage = xmalloc (ea_data_clusters);
  memset (ea_usage, FALSE, ea_data_clusters);
  ea_ok = TRUE;
}


static void do_ea (DISKIO *d, const path_chain *path, ULONG ea_index,
                   int show)
{
  ULONG rel_cluster, cluster, size, size2, i, n;
  ULONG pos, need_eas, value_size, secno;
  FAT_SECTOR ea3;
  char *buf;
  const char *name, *value;
  const FEA *pfea;

  if ((ea_index >> 7) >= 240 || ea_index >= ea_table2_entries)
    {
      warning (1, "\"%s\": Invalid EA index (%lu)",
               format_path_chain (path, NULL), ea_index);
      return;
    }
  if (ea_table2[ea_index] == 0xffff)
    {
      warning (1, "\"%s\": EA index (%lu) points to unused slot",
               format_path_chain (path, NULL), ea_index);
      return;
    }
  rel_cluster = ea_table1[ea_index >> 7] + ea_table2[ea_index];
  if (show)
    info ("Rel. EA cluster:    %lu\n", rel_cluster);
  if ((rel_cluster + 1) * bytes_per_cluster > ea_data_size)
    {
      warning (1, "\"%s\": Invalid relative EA cluster (%lu)",
               format_path_chain (path, NULL), rel_cluster);
      return;
    }

  cluster = ea_data_start;
  for (i = 0; i < rel_cluster; ++i)
    {
      if (cluster < 2 || cluster >= total_clusters)
        abort ();               /* Already checked */
      cluster = fat[cluster];
    }

  secno = CLUSTER_TO_SECTOR (cluster);
  read_sec (d, &ea3, secno, 1, FALSE);
  if (memcmp (ea3.ea3.magic, "EA", 2) != 0)
    {
      warning (1, "\"%s\": Incorrect signature for EA (sector #%lu)",
               format_path_chain (path, NULL), secno);
      return;
    }

  if (USHORT_FROM_FS (ea3.ea3.rel_cluster) != ea_index)
    warning (1, "\"%s\": Incorrect EA index in \"EA DATA. SF\" "
             "(sector #%lu)", format_path_chain (path, NULL), secno);

  if (memchr (ea3.ea3.name, 0, 13) == NULL)
    warning (1, "\"%s\": Name in \"EA DATA. SF\" not null-terminated "
             "(sector #%lu)", format_path_chain (path, NULL), secno);
  /* OS/2 does not update "EA DATA. SF" when renaming a file!
     Therefore we check this only if -p is given. */
  else if (check_pedantic
           && strcmp ((const char *)ea3.ea3.name, path->name) != 0)
    warning (0, "\"%s\": Name in \"EA DATA. SF\" does not match "
             "(sector #%lu)", format_path_chain (path, NULL), secno);

  size = ULONG_FROM_FS (ea3.ea3.fealist.cbList);
  if (size >= 0x40000000
      || (rel_cluster * bytes_per_cluster
          + offsetof (FAT_SECTOR, ea3.fealist)) > ea_data_size)
    {
      warning (1, "\"%s\": EAs too big (sector #%lu, %lu bytes)",
               format_path_chain (path, NULL), secno, size);
      return;
    }

  if (show)
    info ("Size of EAs:        %lu\n", size);

  size2 = size + offsetof (FAT_SECTOR, ea3.fealist);

  if (a_check)
    {
      n = DIVIDE_UP (size2, bytes_per_cluster);
      for (i = 0; i < n; ++i)
        {
          if (ea_usage[rel_cluster+i] != FALSE)
            {
              warning (1, "Relative cluster %lu of \"EA DATA. SF\" "
                       "multiply used", rel_cluster + i);
              warning_cont ("File 2: \"%s\"", format_path_chain (path, NULL));
            }
          else
            ea_usage[rel_cluster+i] = TRUE;
        }
    }

  if (a_where)
    {
      ULONG c;

      n = DIVIDE_UP (size2, bytes_per_cluster);
      c = cluster;
      for (i = 0; i < n; ++i)
        {
          if (c < 2 || c >= total_clusters)
            abort ();           /* Already checked */
          info ("Extended attributes in cluster %lu\n", c);
          c = fat[c];
        }
    }

  if (a_what)
    {
      for (pos = 0; pos < size2; pos += bytes_per_cluster)
        {
          if (cluster < 2 || cluster >= total_clusters)
            abort ();           /* Already checked */
          if (what_cluster_flag && cluster == what_cluster)
            info ("Cluster %lu: Extended attributes for \"%s\"\n",
                  cluster, format_path_chain (path, NULL));
          else if (!what_cluster_flag)
            {
              secno = CLUSTER_TO_SECTOR (cluster);
              if (IN_RANGE (what_sector, secno,
                            MIN (sectors_per_cluster,
                                 DIVIDE_UP (size2 - pos, 512))))
                info ("Sector #%lu: Extended attributes for \"%s\"\n",
                      what_sector, format_path_chain (path, NULL));
            }
          cluster = fat[cluster];
        }
    }
  else if (size2 <= 0x100000 && (a_check || a_where))
    {
      buf = xmalloc (ROUND_UP (size2, 512));
      /* TODO: This reads the first sector twice. */
      for (pos = 0; pos < size2; pos += bytes_per_cluster)
        {
          if (cluster < 2 || cluster >= total_clusters)
            abort ();           /* Already checked */
          read_sec (d, buf + pos, CLUSTER_TO_SECTOR (cluster),
                    MIN (sectors_per_cluster, DIVIDE_UP (size2 - pos, 512)),
                    FALSE);
          cluster = fat[cluster];
        }

      pos = offsetof (FEALIST, list); need_eas = 0;
      while (pos < size)
        {
          if (pos + sizeof (FEA) > size)
            {
              warning (1, "\"%s\": Truncated FEA structure",
                       format_path_chain (path, NULL));
              break;
            }
          pfea = (const FEA *)(buf + pos + offsetof (FAT_SECTOR, ea3.fealist));
          if (pfea->fEA & FEA_NEEDEA)
            ++need_eas;
          value_size = USHORT_FROM_FS (pfea->cbValue);
          if (pos + sizeof (FEA) + pfea->cbName + 1 + value_size > size)
            {
              warning (1, "\"%s\": Incorrect EA size",
                       format_path_chain (path, NULL));
              break;
            }
          name = (const char *)pfea + sizeof (FEA);
          value = name + 1 + pfea->cbName;
          if (name[pfea->cbName] != 0)
            warning (1, "\"%s\": EA name not null-terminated",
                     format_path_chain (path, NULL));
          if (show_eas >= 1)
            info ("Extended attribute %s (%lu bytes)\n",
                  format_ea_name (pfea), value_size);
          pos += sizeof (FEA) + pfea->cbName + 1 + value_size;
        }
      if (need_eas != ULONG_FROM_FS (ea3.ea3.need_eas))
        warning (1, "\"%s\": Incorrect number of `need' EAs",
                 format_path_chain (path, NULL));
      free (buf);
    }
}


static void do_dir (DISKIO *d, ULONG secno, ULONG entries,
                    const path_chain *path, struct vfat *pv,
                    ULONG parent_cluster, ULONG start_cluster,
                    ULONG this_cluster, ULONG dirent_index, int list);

static void dirent_warning (int level, const char *fmt, ULONG secno,
                            const path_chain *path, const BYTE *name, ...)
{
  va_list arg_ptr;

  warning_prolog (level);
  my_fprintf (diag_file, "Directory sector #%lu (\"%s\"): \"%s\": ",
              secno, format_path_chain (path, NULL), name);
  va_start (arg_ptr, name);
  my_vfprintf (diag_file, fmt, arg_ptr);
  va_end (arg_ptr);
  fputc ('\n', diag_file);
  warning_epilog ();
}


static void do_enddir (DISKIO *d, const path_chain *path, struct vfat *pv,
                       int found)
{
  if (pv->flag)
    warning (1, "\"%s\": No real directory entry after VFAT name",
             format_path_chain (path, NULL));
  if (a_find)
    {
      if (found)
        quit (0, FALSE);
      else
        error ("\"%s\" not found in \"%s\"",
               find_comp, format_path_chain (path, NULL));
    }
}


static void do_file (DISKIO *d, ULONG start_cluster, int dir_flag,
                     const path_chain *path, ULONG parent_cluster,
                     ULONG file_size, ULONG ea_index, int list)
{
  ULONG count, cluster, dirent_index, extents, ext_start, ext_length;
  int show, found;
  char *copy_buf = NULL;
  struct vfat v;

  found = (a_find && *find_path == 0);
  show = (a_where && found);
  if (found && a_copy)
    {
      if (dir_flag)
        error ("Directories cannot be copied");
      copy_buf = xmalloc (bytes_per_cluster);
    }
  count = 0; cluster = start_cluster; dirent_index = 0; v.flag = FALSE;
  extents = 0; ext_start = 0; ext_length = 0;
  if (cluster != 0)
    while (cluster < 0xfff8)
      {
        if (ext_length == 0)
          {
            ++extents; ext_start = cluster; ext_length = 1;
          }
        else if (cluster == ext_start + ext_length)
          ++ext_length;
        else
          {
            if (show)
              info ("File data in %s\n",
                    format_cluster_range (ext_start, ext_length));
            ++extents; ext_start = cluster; ext_length = 1;
          }
        if (cluster == 0)
          {
            warning (1, "\"%s\": References unused cluster",
                     format_path_chain (path, NULL));
            break;
          }
        else if (cluster == 0xfff7)
          {
            warning (1, "\"%s\": References bad cluster",
                     format_path_chain (path, NULL));
            break;
          }
        else if (cluster < 0xfff8
                 && (cluster < 2 || cluster >= total_clusters))
          {
            warning (1, "\"%s\": %lu: Invalid cluster number",
                     format_path_chain (path, NULL), cluster);
            break;
          }
        else
          {
            if (!use_cluster (cluster, dir_flag ? USE_DIR : USE_FILE, path))
              {
                warning (1, "\"%s\": Cycle after %lu clusters",
                         format_path_chain (path, NULL), count);
                break;
              }
            if (a_what)
              {
                if (what_cluster_flag && what_cluster == cluster)
                  info ("Cluster %lu: Relative cluster %lu of \"%s\"\n",
                        what_cluster, count, format_path_chain (path, NULL));
                else if (!what_cluster_flag
                         && IN_RANGE (what_sector, CLUSTER_TO_SECTOR (cluster),
                                      sectors_per_cluster))
                  info ("Sector #%lu: Relative sector %lu of \"%s\"\n",
                        what_sector,
                        (count * sectors_per_cluster
                         + what_sector - CLUSTER_TO_SECTOR (cluster)),
                        format_path_chain (path, NULL));
              }
            if (dir_flag && (!found || !list))
              {
                do_dir (d, CLUSTER_TO_SECTOR (cluster),
                        bytes_per_cluster/32, path, &v,
                        parent_cluster, start_cluster, cluster, dirent_index,
                        found && a_dir);
                dirent_index += bytes_per_cluster/32;
              }
            if (a_copy && found && count * bytes_per_cluster < file_size)
              {
                read_sec (d, copy_buf, CLUSTER_TO_SECTOR (cluster),
                          sectors_per_cluster, FALSE);
                fwrite (copy_buf,
                        MIN (file_size - count * bytes_per_cluster,
                             bytes_per_cluster), 1, save_file);
                if (ferror (save_file))
                  save_error ();
              }
            cluster = fat[cluster];
            ++count;
          }
      }

  if (dir_flag && !found)
    do_enddir (d, path, &v, FALSE);

  if (show)
    {
      if (ext_length != 0)
        info ("File data in %s\n",
              format_cluster_range (ext_start, ext_length));
      info ("Number of clusters: %lu\n", count);
      info ("Number of extents:  %lu\n", extents);
    }

  if (ea_index != 0)
    do_ea (d, path, ea_index, show);

  if (a_check)
    {
      if (!dir_flag)
        {
          if (count * bytes_per_cluster < file_size)
            warning (1, "\"%s\": Not enough clusters allocated",
                     format_path_chain (path, NULL));
          if (count > DIVIDE_UP (file_size, bytes_per_cluster))
            warning (1, "\"%s\": Too many clusters allocated",
                     format_path_chain (path, NULL));
        }
    }
  if (found)
    {
      if (a_copy)
        save_close ();
      if (!a_dir)
        quit (0, FALSE);
    }
}


static void do_dirent (DISKIO *d, ULONG secno, const FAT_DIRENT *p,
                       const path_chain *path, struct vfat *pv,
                       ULONG parent_cluster, ULONG start_cluster,
                       ULONG dirent_index, int *label_flag, int show, int list)
{
  BYTE name[13], cs;
  int i, n;
  char found;
  unsigned date, time;
  ULONG cluster;
  const VFAT_DIRENT *v;
  USHORT vname[13];

  if (p->name[0] == 0xe5)
    {
      if (pv->flag)
        {
          warning (1, "\"%s\": Unused directory entry after VFAT name "
                   "(sector #%lu)", format_path_chain (path, NULL), secno);
          pv->flag = FALSE;
        }
      return;
    }

  if (p->attr == 0x0f)
    {
      v = (const VFAT_DIRENT *)p;
      for (i = 0; i < 5; ++i)
        vname[i+0] = USHORT_FROM_FS (v->name1[i]);
      for (i = 0; i < 6; ++i)
        vname[i+5] = USHORT_FROM_FS (v->name2[i]);
      for (i = 0; i < 2; ++i)
        vname[i+11] = USHORT_FROM_FS (v->name3[i]);
      n = 13;
      while (n > 0 && vname[n-1] == 0xffff)
        --n;

      if (show)
        {
          info ("Directory entry %lu of \"%s\":\n", dirent_index,
                format_path_chain (path, NULL));
          info ("  VFAT name frag:   \"");
          for (i = 0; i < n; ++i)
            if (vname[i] >= 0x20 && vname[i] <= 0xff)
              info ("%c", vname[i]);
            else
              info ("<0x%x>", vname[i]);
          info ("\"\n");
        }

      if (v->flag > 0x7f)
        {
          warning (1, "\"%s\": Invalid VFAT name (sector #%lu)",
                   format_path_chain (path, NULL), secno);
          pv->flag = FALSE;
          return;
        }
      else if (v->flag & 0x40)
        {
          if (pv->flag)
            warning (1, "\"%s\": No real directory entry after VFAT name "
                     "(sector #%lu)", format_path_chain (path, NULL), secno);
          else if (n == 0 || vname[n-1] != 0)
            {
              warning (1, "\"%s\": VFAT name not null-terminated "
                       "(sector #%lu)", format_path_chain (path, NULL), secno);
              return;
            }
          else
            {
              --n;
              pv->flag = TRUE;
              pv->unprintable = FALSE;
              pv->name[256] = 0;
              pv->start = 256;
              pv->total = v->flag & 0x3f;
              pv->index = v->flag & 0x3f;
              pv->checksum = v->checksum;
            }
        }
      if ((v->flag & 0x3f) != pv->index || pv->index == 0)
        {
          warning (1, "\"%s\": Incorrect VFAT name index (sector #%lu)",
                   format_path_chain (path, NULL), secno);
          pv->flag = FALSE;
          return;
        }
      if (v->checksum != pv->checksum)
        warning (1, "\"%s\": Incorrect VFAT checksum (sector #%lu)",
                 format_path_chain (path, NULL), secno);
      pv->index -= 1;
      if (pv->start < n)
        {
          warning (1, "\"%s\": VFAT name too long (sector #%lu)",
                   format_path_chain (path, NULL), secno);
          pv->flag = FALSE;
          return;
        }
      for (i = n-1; i >= 0; --i)
        {
          if (vname[i] < 0x20 || vname[i] > 0xff)
            pv->unprintable = TRUE;
          pv->name[--pv->start] = (BYTE)vname[i];
        }
      return;
    }

  cluster = USHORT_FROM_FS (p->cluster);
  found = FALSE;

  if (p->name[0] == '.')
    {
      i = 1;
      if (p->name[1] == '.')
        ++i;
      memcpy (name, p->name, i);
      name[i] = 0;
    }
  else if (p->attr & ATTR_LABEL)
    {
      memcpy (name, p->name, 11);
      i = 11;
      while (i > 0 && name[i-1] == ' ')
        --i;
      name[i] = 0;
    }
  else
    {
      memcpy (name, p->name + 0, 8);
      i = 8;
      while (i > 0 && name[i-1] == ' ')
        --i;
      if (memcmp (p->name + 8, "   ", 3) != 0)
        {
          name[i++] = '.';
          memcpy (name + i, p->name + 8, 3);
          i += 3;
          while (name[i-1] == ' ')
            --i;
        }
      name[i] = 0;
    }
  if (name[0] == 0x05)
    name[0] = 0xe5;

  if (pv->flag)
    {
      if (pv->index != 0)
        {
          warning (1, "\"%s\": Incomplete VFAT name for \"%s\" (sector #%lu)",
                   format_path_chain (path, NULL), name, secno);
          pv->flag = FALSE;
        }
      /* TODO: Compare VFAT name to regular name */

      /* Compute and check the checksum. */

      cs = 0;
      for (i = 0; i < 8+3; ++i)
        cs = rorb1 (cs) + p->name[i];
      if (cs != pv->checksum)
        warning (1, "\"%s\": Checksum mismatch for \"%s\" (sector #%lu): "
                 "0x%.2x vs. 0x%.2x", format_path_chain (path, NULL), name,
                 secno, pv->checksum, cs);
    }

  if (a_find && !show && !list)
    {
      if (compare_fname (name, find_comp) != 0)
        goto done;
      if (*find_path == 0)
        {
          found = TRUE;
          if (a_where)
            {
              info ("Directory entry in sector #%lu\n", secno);
              show = TRUE;
            }
          if (a_dir)
            show = TRUE;
        }
    }

  date = USHORT_FROM_FS (p->date);
  time = USHORT_FROM_FS (p->time);

  if (list || (a_dir && show && !(p->attr & ATTR_DIR)))
    {
      info ("%s %s ", format_date (date), format_time (time));
      if (p->attr & ATTR_DIR)
        info ("     <DIR>      ");
      else
        info ("%10lu %c%c%c%c%c",
              ULONG_FROM_FS (p->size),
              (p->attr & ATTR_READONLY) ? 'R' : '-',
              (p->attr & ATTR_HIDDEN)   ? 'H' : '-',
              (p->attr & ATTR_SYSTEM)   ? 'S' : '-',
              (p->attr & ATTR_LABEL)    ? 'V' : '-',
              (p->attr & ATTR_ARCHIVED) ? 'A' : '-');
      info (" \"%s\"\n", name);
    }

  if (show && !a_dir)
    {
      info ("Directory entry %lu of \"%s\":\n", dirent_index,
            format_path_chain (path, NULL));
      info ("  Name:             \"%s\"\n", name);
      info ("  Attributes:       0x%.2x", p->attr);
      if (p->attr & ATTR_DIR)
        info (" dir");
      if (p->attr & ATTR_READONLY)
        info (" r/o");
      if (p->attr & ATTR_HIDDEN)
        info (" hidden");
      if (p->attr & ATTR_SYSTEM)
        info (" system");
      if (p->attr & ATTR_LABEL)
        info (" label");
      if (p->attr & ATTR_ARCHIVED)
        info (" arch");
      info ("\n");
      info ("  Cluster:          %lu\n", cluster);
      info ("  Time:             0x%.4x (%s)\n", time, format_time (time));
      info ("  Date:             0x%.4x (%s)\n", date, format_date (date));
      info ("  Size:             %lu\n", ULONG_FROM_FS (p->size));
      info ("  EA pointer:       %u\n", USHORT_FROM_FS (p->ea));
      if (pv->flag)
        if (pv->unprintable)
          info ("  VFAT name:        (not printable)\n");
        else
          info ("  VFAT name:        \"%s\"\n", pv->name + pv->start);
    }

  if (a_check)
    {
      unsigned y, m, d, h, s;
      int i;

      y = ((date >> 9) & 127) + 1980;
      m = (date >> 5) & 15;
      d = date & 31;
      if (m < 1 || m > 12 || d < 1 || d > days (y, m))
        dirent_warning (0, "Invalid date (0x%.4x)", secno, path, name, date);

      h = (time >> 11) & 31;
      m = (time >> 5) & 63;
      s = (time & 31) << 1;
      if (h > 23 || m > 59 || s > 59)
        dirent_warning (0, "Invalid time (0x%.4x)", secno, path, name, time);
      if (p->attr & ~0x3f)
        dirent_warning (0, "Undefined attribute bit is set",
                        secno, path, name);

      /* Check the file name.  Note that file names starting with `.'
         are checked further down. */

      if (p->name[0] != '.')
        {
          for (i = 0; i < 11; ++i)
            if (p->name[i] != 0x05
                && (p->name[i] < 0x20
                    || strchr ("\"*+,./;:<=>?[\\]|", p->name[i]) != NULL))
              break;
          if (i < 11)
            dirent_warning (1, "Invalid character in file name",
                            secno, path, name);
        }
    }

  if (p->name[0] == '.')
    {
      if (pv->flag)
        {
          dirent_warning (1, "Must not have a VFAT name", secno, path, name);
          pv->flag = FALSE;
        }
      if (!a_check)
        return;
      if (memcmp (p->name + i, "          ", 11 - i) != 0)
        dirent_warning (1, "File name starting with \".\"",
                        secno, path, name);
      else if (!(p->attr & ATTR_DIR))
        dirent_warning (1, "Not a directory", secno, path, name);
      else if (cluster != (i == 1 ? start_cluster : parent_cluster))
        dirent_warning (1, "Incorrect cluster (%lu vs. %lu)",
                        secno, path, name,
                        cluster, (i == 1 ? start_cluster : parent_cluster));
      return;
    }


  if (verbose)
    my_fprintf (prog_file, "%s\n", format_path_chain (path, (char *)name));

  if (a_check)
    {
      if (p->attr & ATTR_LABEL)
        {
          if (path->parent != NULL)
            dirent_warning (1, "Unexpected volume label",
                            secno, path, name);
          else if (*label_flag)
            dirent_warning (1, "More than one volume label",
                            secno, path, name);
          else
            *label_flag = TRUE;
        }
    }

  if (!(p->attr & ATTR_LABEL)
      && !list
      && !(a_what && !what_cluster_flag && what_sector < data_sector))
    {
      path_chain link, *plink;

      plink = PATH_CHAIN_NEW (&link, path, (char *)name);
      do_file (d, cluster, p->attr & ATTR_DIR, plink, start_cluster,
               ULONG_FROM_FS (p->size), USHORT_FROM_FS (p->ea), list);
    }
  if (found && !list)
    quit (0, FALSE);
done:
  pv->flag = FALSE;
}


static void do_dir (DISKIO *d, ULONG secno, ULONG entries,
                    const path_chain *path, struct vfat *pv,
                    ULONG parent_cluster, ULONG start_cluster,
                    ULONG this_cluster, ULONG dirent_index, int list)
{
  FAT_DIRENT dir[512/32];
  ULONG i, n, last_secno;
  int show, label_flag;

  if (a_find && dirent_index == 0)
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

  label_flag = FALSE; last_secno = 0;
  while (entries != 0)
    {
      show = FALSE;
      if (a_what)
        {
          if (what_cluster_flag && what_cluster == this_cluster)
            {
              info ("Cluster %lu: Directory \"%s\"\n", what_cluster,
                    format_path_chain (path, NULL));
              show = TRUE;
            }
          else if (!what_cluster_flag && what_sector == secno)
            {
              info ("Sector #%lu: Directory \"%s\"\n", what_sector,
                    format_path_chain (path, NULL));
              show = TRUE;
            }
        }
      read_sec (d, dir, secno, 1, TRUE);
      last_secno = secno;
      n = MIN (512/32, entries);
      for (i = 0; i < n; ++i)
        {
          if (dir[i].name[0] == 0)
            goto done;
          do_dirent (d, secno, &dir[i], path, pv, parent_cluster,
                     start_cluster, dirent_index, &label_flag,
                     show, list);
          ++dirent_index;
        }
      ++secno; entries -= 512/32;
    }
done:;
}


static void find_ea_data (DISKIO *d, ULONG secno, ULONG entries)
{
  ULONG i, n;
  FAT_DIRENT dir[512/32];

  ea_data_start = 0xffff; ea_data_size = 0;
  while (entries != 0)
    {
      read_sec (d, dir, secno, 1, FALSE);
      n = MIN (512/32, entries);
      for (i = 0; i < n; ++i)
        {
          if (dir[i].name[0] == 0)
            return;
          if (memcmp (dir[i].name, "EA DATA  SF", 8+3) == 0
              && !(dir[i].attr & (ATTR_LABEL|ATTR_DIR)))
            {
              ea_data_start = USHORT_FROM_FS (dir[i].cluster);
              ea_data_size = ULONG_FROM_FS (dir[i].size);
              ea_data_clusters = DIVIDE_UP (ea_data_size, bytes_per_cluster);
              if (a_info)
                {
                  info ("\"EA DATA. SF\" 1st cluster:  %lu\n", ea_data_start);
                  info ("\"EA DATA. SF\" size:         %lu\n", ea_data_size);
                }
              return;
            }
        }
      ++secno; entries -= 512/32;
    }
}


static void do_root_dir (DISKIO *d)
{
  ULONG secno;
  int list;

  secno = first_sector + number_of_fats * sectors_per_fat;
  list = FALSE;

  if (a_find && *find_path == 0)
    {
      if (a_where)
        info ("Root directory in %s\n",
              format_sector_range (secno, root_sectors));
      if (a_dir)
        list = TRUE;
      else
        quit (0, FALSE);
    }
  if (a_info)
    info ("Root directory:             %s\n",
          format_sector_range (secno, root_sectors));
  if (a_what)
    {
      if (!what_cluster_flag && IN_RANGE (what_sector, secno, root_sectors))
        info ("Sector #%lu: Root directory (+%lu)\n", what_sector,
              what_sector - secno);
    }

  find_ea_data (d, secno, root_entries);
  read_ea_data (d);

  if (a_save || a_check || a_what || a_find)
    {
      path_chain link, *plink;
      struct vfat v;

      v.flag = FALSE;
      plink = PATH_CHAIN_NEW (&link, NULL, "");
      do_dir (d, secno, root_entries, plink, &v, 0, 0, 0, 0, list);
      do_enddir (d, plink, &v, list);
    }
}


#define ALLOCATED(x)  (fat[x] != 0 && fat[x] != 0xfff7)

static void check_alloc (void)
{
  ULONG i, start, count;

  i = 2; count = 0;
  while (i < total_clusters)
    {
      if (usage_vector[i] == USE_EMPTY && ALLOCATED (i))
        {
          start = i;
          do
            {
              ++i;
            } while (i < total_clusters
                     && usage_vector[i] == USE_EMPTY && ALLOCATED (i));
          if (check_unused)
            warning (0, "Unused but marked as allocated: %s",
                     format_cluster_range (start, i - start));
          count += i - start;
        }
      else
        ++i;
    }
  if (count == 1)
    warning (0, "The file system has 1 lost cluster");
  else if (count > 1)
    warning (0, "The file system has %lu lost clusters", count);
}


/* Process a FAT volume. */

void do_fat (DISKIO *d, const FAT_SECTOR *pboot)
{
  ULONG i;

  plenty_memory = TRUE;
  if (USHORT_FROM_FS (pboot->boot.bytes_per_sector) != 512)
    error ("Sector size %u is not supported",
           USHORT_FROM_FS (pboot->boot.bytes_per_sector));
  if (pboot->boot.sectors_per_cluster == 0)
    error ("Cluster size is zero");
  if (pboot->boot.fats == 0)
    error ("Number of FATs is zero");
  first_sector = USHORT_FROM_FS (pboot->boot.reserved_sectors);
  sectors_per_cluster = pboot->boot.sectors_per_cluster;
  bytes_per_cluster = sectors_per_cluster * 512;
  sectors_per_fat = USHORT_FROM_FS (pboot->boot.sectors_per_fat);
  number_of_fats = pboot->boot.fats;

  if (USHORT_FROM_FS (pboot->boot.sectors) != 0)
    total_sectors = USHORT_FROM_FS (pboot->boot.sectors);
  else
    total_sectors = ULONG_FROM_FS (pboot->boot.large_sectors);
  if (total_sectors < USHORT_FROM_FS (pboot->boot.reserved_sectors))
    error ("Number of reserved sectors exceeds total number of sectors");
  total_sectors -= USHORT_FROM_FS (pboot->boot.reserved_sectors);

  root_entries = USHORT_FROM_FS (pboot->boot.root_entries);
  root_sectors = DIVIDE_UP (root_entries, 512/32);

  if (total_sectors < number_of_fats * sectors_per_fat + root_sectors)
    error ("Disk too small for FATs and root directory");
  total_clusters = ((total_sectors - number_of_fats * sectors_per_fat
                     - root_sectors)
                    / sectors_per_cluster);
  total_clusters += 2;
  if (total_clusters < 2)
    error ("Disk too small, no data clusters");
  if (total_clusters > 0xffff)
    warning (0, "Too many clusters");

  data_sector = (first_sector + number_of_fats * sectors_per_fat
                 + root_sectors);

  if (a_info)
    {
      info ("Number of clusters:         %lu\n", total_clusters - 2);
      info ("First data sector:          #%lu\n", data_sector);
    }

  if (a_what && what_cluster_flag)
    {
      if (what_sector < 2 || what_sector >= total_clusters)
        error ("Invalid cluster number");
      what_cluster = what_sector;
      what_sector = CLUSTER_TO_SECTOR (what_sector);
    }

  if (a_what)
    {
      if (!what_cluster_flag && what_sector == 0)
        info ("Sector #%lu: Boot sector\n", what_sector);
    }

  /* Allocate usage vector.  Initially, all clusters are unused. */

  usage_vector = (BYTE *)xmalloc (total_clusters);
  memset (usage_vector, USE_EMPTY, total_clusters);

  path_vector = xmalloc (total_clusters * sizeof (*path_vector));
  for (i = 0; i < total_clusters; ++i)
    path_vector[i] = NULL;

  do_fats (d);

  if (a_what)
    {
      if (!what_cluster_flag
          && what_sector >= data_sector && what_sector < total_sectors)
        {
          i = SECTOR_TO_CLUSTER (what_sector);
          if (i >= 2 && i < total_clusters)
            {
              info ("Sector #%lu: Cluster %lu\n", what_sector, i);
              if (fat[i] == 0xfff7)
                info ("Sector #%lu: Cluster contains bad sector\n",
                      what_sector);
              else if (fat[i] >= 0xfff8)
                info ("Sector #%lu: In last cluster of a file or directory\n",
                      what_sector);
              else if (fat[i] == 0)
                info ("Sector #%lu: In an unused cluster\n", what_sector);
              else
                info ("Sector #%lu: In a used cluster\n", what_sector);
            }
        }
      else if (what_cluster_flag)
        {
          info ("Cluster %lu: %s\n", what_cluster,
                format_sector_range (CLUSTER_TO_SECTOR (what_cluster),
                                     sectors_per_cluster));
          if (fat[what_cluster] == 0xfff7)
            info ("Cluster %lu: Cluster contains bad sector\n", what_cluster);
          else if (fat[what_cluster] >= 0xfff8)
            info ("Cluster %lu: Last cluster of a file or directory\n",
                  what_cluster);
          else if (fat[what_cluster] == 0)
            info ("Cluster %lu: Unused\n", what_cluster);
          else
            info ("Cluster %lu: Used\n", what_cluster);
        }
    }

  do_root_dir (d);

  if (a_check)
    {
      check_alloc ();
    }
}
