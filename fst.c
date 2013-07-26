/* fst.c -- Main module of fst
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
#include <os2.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "fst.h"
#include "crc.h"
#include "diskio.h"
#include "fat.h"
#include "do_hpfs.h"
#include "do_fat.h"

static char banner[] =
"fst 0.3f -- Copyright (c) 1995-1996 by Eberhard Mattes\n";

/* Increase the heap for -Zsys to 64MB. */
unsigned _sys_heap_size = 0x4000000;

char verbose;                   /* Non-zero for `check -v' and `save -v' */
char sector_number_format;      /* 'x' for `-x', 0 otherwise */
char a_info;                    /* Non-zero for `info' action */
char a_save;                    /* Non-zero for `save' action */
char a_check;                   /* Non-zero for `check' action */
char a_what;                    /* Non-zero for `info <number>' action */
char a_where;                   /* Non-zero for `info <path>' action */
char a_copy;                    /* Non-zero for `copy' action */
char a_dir;                     /* Non-zero for `dir' action */
char a_find;                    /* Non-zero for finding a file */
char plenty_memory;             /* Non-zero for `check -m' */
char check_unused;              /* Non-zero for `check -u' */
char check_pedantic;            /* Non-zero for `check -p' */
char show_unused;               /* Non-zero for `info -u' */
char show_free_frag;            /* Non-zero for `info -f' */
char show_frag;                 /* Non-zero for `check -f' */
char show_eas;                  /* Non-zero for `info -e <path>' */
char show_summary;              /* Non-zero for `check -s' */
char force_fs;                  /* Non-zero to force to a specific fs */
ULONG what_sector;              /* Sector number for `info <number>' action */
char what_cluster_flag;         /* `what_sector' is a cluster number */
const char *find_path;          /* (Remainder of) pathname for a_find */

/* This table maps lower-case letters to upper case, using the current
   code page. */
BYTE cur_case_map[256];

FILE *diag_file;                /* Stream for diagnostics */
FILE *prog_file;                /* Stream for progress report */
static FILE *info_file;         /* Stream for information */

static int warning_count[2];    /* Index 0: warnings, index 1: errors */

static char list_going;         /* Non-zero if list started */
static int list_x;              /* Column */
static char list_msg[80];       /* Message */

/* path_chain_new() tries to avoid malloc()'s memory overhead by
   allocating reasonably big buffers out of which the strings and
   path_chains will be allotted.  Fortunately, we never free those
   structures. */

struct str_buf
{
  char *buf;
  size_t size;
  size_t used;
  struct str_buf *next;
};

struct str_buf *str_buf_head;

static path_chain *pc_buf;
static size_t pc_count;
static size_t pc_used;

/* Clean up and terminate the process.  RC is the return code passed
   to the parent process.  If SHOW is non-zero, show the number of
   warnings and errors even if both numbers are zero. */

void quit (int rc, int show)
{
  if (save_file != NULL)
    {
      fclose (save_file);
      save_file = NULL;
      remove (save_fname);
    }
  if (warning_count[0] != 0 || warning_count[1] != 0 || show)
    fprintf (stdout, "Total warnings: %d, total errors: %d\n",
             warning_count[0], warning_count[1]);
  if (rc == 0 && warning_count[1] != 0)
    rc = 1;
  exit (rc);
}


/* Treat `#%lu' in FMT as format specifiers for sector numbers.  We
   cannot introduce our own format specifier (such as `%N') because
   that would cause loads of GCC warnings or we would loose checking
   of format strings. */

static void adjust_format_string (char *dst, const char *src)
{
  while (*src != 0)
    if (src[0] != '#' || src[1] != '%' || src[2] != 'l' || src[3] != 'u')
      *dst++ = *src++;
    else
      {
        src += 4;
        switch (sector_number_format)
          {
          case 'x':
            *dst++ = '0'; *dst++ = 'x'; *dst++ = '%';
            *dst++ = '.'; *dst++ = '8'; *dst++ = 'l'; *dst++ = 'x';
            break;
          default:
            *dst++ = '%'; *dst++ = 'l'; *dst++ = 'u';
            break;
          }
      }
  *dst = 0;
}


/* Like vfprintf(), but treat #%lu specially (sector number).  Return
   the number of characters printed. */

int my_vfprintf (FILE *f, const char *fmt, va_list arg_ptr)
{
  char new_fmt[512];

  adjust_format_string (new_fmt, fmt);
  return vfprintf (f, new_fmt, arg_ptr);
}


/* Like fprintf(), but treat #%lu specially (sector number).  Return
   the number of characters printed. */

int my_fprintf (FILE *f, const char *fmt, ...)
{
  va_list arg_ptr;
  char new_fmt[512];
  int r;

  adjust_format_string (new_fmt, fmt);
  va_start (arg_ptr, fmt);
  r = my_vfprintf (f, new_fmt, arg_ptr);
  va_end (arg_ptr);
  return r;
}


/* Like vsprintf(), but treat #%lu specially (sector number).  Return
   the number of characters printed. */

int my_vsprintf (char *buf, const char *fmt, va_list arg_ptr)
{
  char new_fmt[512];

  adjust_format_string (new_fmt, fmt);
  return vsprintf (buf, new_fmt, arg_ptr);
}


/* Like sprintf(), but treat #%lu specially (sector number).  Return
   the number of characters printed. */

int my_sprintf (char *buf, const char *fmt, ...)
{
  va_list arg_ptr;
  char new_fmt[512];
  int r;

  adjust_format_string (new_fmt, fmt);
  va_start (arg_ptr, fmt);
  r = my_vsprintf (buf, new_fmt, arg_ptr);
  va_end (arg_ptr);
  return r;
}


/* Display a fatal error message and terminate the process. */

void error (const char *fmt, ...)
{
  va_list arg_ptr;

  list_end ();
  fflush (info_file);
  fprintf (stderr, "ERROR: ");
  va_start (arg_ptr, fmt);
  my_vfprintf (stderr, fmt, arg_ptr);
  va_end (arg_ptr);
  fputc ('\n', stderr);
  warning_count[1] += 1;
  quit (2, TRUE);
}


/* Common code for all functions which display warnings.  Show the
   severity (LEVEL) of the message and increment the appropriate
   counter. */

void warning_prolog (int level)
{
  list_end ();
  fflush (info_file);
  switch (level)
    {
    case 0:
      fprintf (diag_file, "WARNING: ");
      break;
    case 1:
      fprintf (diag_file, "ERROR: ");
      break;
    default:
      abort ();
    }
  warning_count[level] += 1;
}


/* Common code for all functions which display warnings.  This is
   called after printing a message. */

void warning_epilog (void)
{
  fflush (diag_file);
}


/* Display a warning of severity LEVEL (0=warning, 1=error). */

void warning (int level, const char *fmt, ...)
{
  va_list arg_ptr;

  warning_prolog (level);
  va_start (arg_ptr, fmt);
  my_vfprintf (diag_file, fmt, arg_ptr);
  va_end (arg_ptr);
  fputc ('\n', diag_file);
  warning_epilog ();
}


/* Continue a warning. */

void warning_cont (const char *fmt, ...)
{
  va_list arg_ptr;

  va_start (arg_ptr, fmt);
  fputs ("  ", diag_file);
  my_vfprintf (diag_file, fmt, arg_ptr);
  va_end (arg_ptr);
  fputc ('\n', diag_file);
  warning_epilog ();
}


/* Display information.  No linefeed is appended!  Return the number
   of characters printed. */

int info (const char *fmt, ...)
{
  va_list arg_ptr;
  int r;

  va_start (arg_ptr, fmt);
  r = my_vfprintf (info_file, fmt, arg_ptr);
  va_end (arg_ptr);
  return r;
}


/* Display information, indented with INDENT spaces (up to 8).  No
   linefeed is appended! */

void infoi (const char *fmt, int indent, ...)
{
  va_list arg_ptr;

  if (indent > 0)
    fprintf (info_file, "%.*s", indent, "        ");
  va_start (arg_ptr, indent);
  my_vfprintf (info_file, fmt, arg_ptr);
  va_end (arg_ptr);
}


/* Allocate N bytes of memory.  Terminate the process on error. */

void *xmalloc (size_t n)
{
  void *p;

  p = malloc (n);
  if (p == NULL)
    error ("Out of memory");
  return p;
}


/* Return a pointer to a string containing a formatted range of sector
   numbers.  Note that the pointer points to static memory; do not use
   format_sector_range() more than once in one expression! */

const char *format_sector_range (ULONG start, ULONG count)
{
  static char buf[60];
  static char fmt[40];

  if (count == 1)
    {
      adjust_format_string (fmt, "sector #%lu");
      sprintf (buf, fmt, start);
    }
  else
    {
      adjust_format_string (fmt, "%lu sectors #%lu-#%lu");
      sprintf (buf, fmt, count, start, start + count - 1);
    }
  return buf;
}


/* Return a pointer to a string containing a printable representation
   of the string of N characters pointed to by S.  If ZERO_TERM is
   true, the string pointed to by S can be zero terminated. */

const char *format_string (const unsigned char *s, size_t n, int zero_term)
{
  size_t i;
  char *p;
  static char buf[800];
  static const char hex_digits[] = "0123456789abcdef";

  for (i = 0; i < n; ++i)
    if (s[i] < 0x20 || s[i] == 0xff)
      break;
  if (i >= n || (zero_term && s[i] == 0))
    {
      buf[0] = '"';
      memcpy (buf + 1, s, i);
      buf[i+1] = '"';
      buf[i+2] = 0;
    }
  else
    {
      memcpy (buf, "0x", 2);
      p = buf + 2;
      for (i = 0; i < n; ++i)
        {
          *p++ = hex_digits[(s[i] >> 4) & 0x0f];
          *p++ = hex_digits[s[i] & 0x0f];
        }
      *p = 0;
    }
  return buf;
}


/* Format the name of the extended attribute pointed to by PFEA. */

const char *format_ea_name (const FEA *pfea)
{
  return format_string ((const unsigned char *)pfea + sizeof (FEA),
                        pfea->cbName, FALSE);
}


/* Allocate a new path chain link.  PARENT points to the parent link,
   NAME is the name to add (at the bottom of the link). */

path_chain *path_chain_new (const path_chain *parent, const char *name)
{
  char *n;
  size_t size;
  struct str_buf *sb;

  size = strlen (name) + 1;
  for (sb = str_buf_head; sb != NULL; sb = sb->next)
    if (sb->used + size <= sb->size)
      break;
  if (sb == NULL)
    {
      /* Unlink string buffers which are too full to be useful.  This
         decreases search time in the loop above. */

      while (str_buf_head != NULL
             && str_buf_head->used + 1 >= str_buf_head->size)
        {
          sb = str_buf_head->next;
          free (str_buf_head);
          str_buf_head = sb;
        }
      sb = xmalloc (sizeof (struct str_buf));
      sb->used = 0;
      sb->size = 65520;
      sb->buf = xmalloc (sb->size);
      sb->next = str_buf_head;
      str_buf_head = sb;
    }
  n = sb->buf + sb->used;
  memcpy (n, name, size);
  sb->used += size;

  if (pc_used >= pc_count)
    {
      pc_used = 0;
      pc_count = 65520 / sizeof (*pc_buf);
      pc_buf = xmalloc (pc_count * sizeof (*pc_buf));
    }

  pc_buf[pc_used].name = n;
  pc_buf[pc_used].parent = parent;
  return &pc_buf[pc_used++];
}


/* Return the length of a path name chain, in characters. */

int path_chain_len (const path_chain *p)
{
  if (p == NULL)
    return -1;
  else
    return strlen (p->name) + 1 + path_chain_len (p->parent);
}


/* Recursive helper function for format_path_chain(). */

static int fpc_recurse (char *dst, size_t dst_size, const path_chain *p)
{
  int start;
  size_t len;

  if (p->parent == NULL)
    start = 0;
  else
    start = fpc_recurse (dst, dst_size, p->parent);
  if (start < 0)
    return start;

  len = strlen (p->name);
  if (start + len + 2 > dst_size)
    return -1;
  if (start == 0 || dst[start-1] != '\\')
    dst[start++] = '\\';
  memcpy (dst + start, p->name, len);
  return start + len;
}


/* Format the path name chain, optionally appending the file name
   LAST.  Return a pointer to a statically allocated string. */

const char *format_path_chain (const path_chain *bottom, const char *last)
{
  static char buf[260];
  int len;
  path_chain link;

  if (last == NULL)
    len = fpc_recurse (buf, sizeof (buf), bottom);
  else
    {
      link.parent = bottom;
      link.name = last;
      len = fpc_recurse (buf, sizeof (buf), &link);
    }

  if (len >= 0)
    buf[len] = 0;
  else
    {
      if (strlen (bottom->name) + 5 < sizeof (buf))
        sprintf (buf, "...\\%s", bottom->name);
      else if (strlen (bottom->name) < sizeof (buf))
        strcpy (buf, bottom->name);
      else
        strcpy (buf, "...");
    }
  return buf;
}


/* Initialize cur_case_map. */

static void init_cur_case_map (void)
{
  COUNTRYCODE cc;
  int i;

  for (i = 0; i < 256; ++i)
    cur_case_map[i] = (BYTE)i;
  for (i = 'a'; i <= 'z'; ++i)
    cur_case_map[i] = (BYTE)toupper (i);
  cc.country = 0;
  cc.codepage = 0;
  DosMapCase (128, &cc, (PCHAR)cur_case_map + 128);
}


/* Apply the selected action to an entire disk. */

static void do_disk (DISKIO *d)
{
  FAT_SECTOR boot;

  read_sec (d, &boot, 0, 1, TRUE);
  if (a_info)
    {
      info ("Boot sector:\n");
      info ("  OEM:                      %s\n",
            format_string (boot.boot.oem, 8, FALSE));
      info ("  Bytes per sector:         %u\n",
            USHORT_FROM_FS (boot.boot.bytes_per_sector));
      info ("  Sectors per cluster:      %u\n",
            boot.boot.sectors_per_cluster);
      info ("  Reserved sectors:         %u\n",
            USHORT_FROM_FS (boot.boot.reserved_sectors));
      info ("  FATs:                     %u\n",
            boot.boot.fats);
      info ("  Root directory entries:   %u\n",
            USHORT_FROM_FS (boot.boot.root_entries));
      if (USHORT_FROM_FS (boot.boot.sectors) != 0)
        info ("  Sectors:                  %u\n",
              USHORT_FROM_FS (boot.boot.sectors));
      else
        info ("  Sectors:                  %lu\n",
              ULONG_FROM_FS (boot.boot.large_sectors));
      info ("  Media descriptor:         0x%x\n",
            boot.boot.media);
      info ("  Sectors per FAT:          %u\n",
            USHORT_FROM_FS (boot.boot.sectors_per_fat));
      info ("  Sectors per track:        %u\n",
            USHORT_FROM_FS (boot.boot.sectors_per_track));
      info ("  Heads:                    %u\n",
            USHORT_FROM_FS (boot.boot.heads));
      info ("  Hidden sectors:           %u\n",
            USHORT_FROM_FS (boot.boot.hidden_sectors_lo));
      info ("  Drive number:             %u\n",
            boot.boot.drive_no);
      info ("  Extended signature:       0x%x\n",
            boot.boot.extended_sig);
      if (boot.boot.extended_sig == 40 || boot.boot.extended_sig == 41)
        {
          info ("  Volume ID:                0x%.8lx\n",
                ULONG_FROM_FS (boot.boot.vol_id));
          info ("  Volume label:             %s\n",
                format_string (boot.boot.vol_label, 11, TRUE));
          info ("  Volume type:              %s\n",
                format_string (boot.boot.vol_type, 8, FALSE));
        }
    }

  if (force_fs == 'h')
    do_hpfs (d);
  else if (force_fs == 'f')
    do_fat (d, &boot);
  else if (boot.boot.extended_sig == 40
           && memcmp (boot.boot.vol_type, "HPFS", 4) == 0)
    do_hpfs (d);
  else if (boot.boot.extended_sig == 41
           && memcmp (boot.boot.vol_type, "HPOFS", 5) == 0)
    error ("HPOFS not supported");
  else
    do_fat (d, &boot);
}


/* Start a list.  Print FMT before the first entry of the list. */

void list_start (const char *fmt, ...)
{
  va_list arg_ptr;

  list_going = FALSE;
  va_start (arg_ptr, fmt);
  my_vsprintf (list_msg, fmt, arg_ptr);
  va_end (arg_ptr);
}


/* List one number (NUMBER) using format FMT. */

void list (const char *fmt, ...)
{
  int len;
  va_list arg_ptr;
  char temp[256];

  if (!list_going)
    {
      list_going = TRUE;
      list_x = info ("%s", list_msg);
    }
  va_start (arg_ptr, fmt);
  len = my_vsprintf (temp, fmt, arg_ptr);
  va_end (arg_ptr);
  if (list_x + len + 1 >= 80 - 1)
    {
      info ("\n ");
      list_x = 1;
    }
  list_x += info (" %s", temp);
}


/* End of a list of sector numbers.  (The list can be continued after
   calling this function, see warning()!) */

void list_end (void)
{
  if (list_going)
    {
      info ("\n");
      list_going = FALSE;
    }
  list_x = 0;
}


/* Tell them how to call this program. */

static void usage (void)
{
  char buf[10];

  puts (banner);

  puts ("fst comes with ABSOLUTELY NO WARRANTY. For details see file\n"
        "`COPYING' that should have come with this program.\n"
        "fst is free software, and you are welcome to redistribute it\n"
        "under certain conditions. See the file `COPYING' for details.\n");
  fputs ("Type RETURN to contine: ", stdout); fflush (stdout);
  fgets (buf, sizeof (buf), stdin);

  puts ("\nUsage:\n"
        "  fst [<fst_options>] <action> [<action_options>] <arguments>\n"
        "\n<fst_options>:\n"
        "  -h        Show help about <action>\n"
        "  -d        Use DosRead/DosWrite (default: logical disk track I/O)\n"
        "  -n        Continue if disk cannot be locked\n"
        "  -w        Enable writing to disk\n"
        "  -x        Show sector numbers in hexadecimal\n"
        "\n<action>:\n"
        "  info      Show information about the file system, a sector, or a path name\n"
        "  check     Check the file system\n"
        "  save      Take a snapshot of the file system\n"
        "  diff      Compare snapshot files, CRC files, and disks\n"
        "  restore   Copy sectors from snapshot file to disk\n"
        "  dir       List a directory\n"
        "  copy      Copy a file from the disk\n"
        "  read      Copy a sector to a file\n"
        "  write     Write a sector from a file to disk\n"
        "  crc       Save CRCs for all sectors of a disk");
  quit (1, FALSE);
}


static void usage_info (void)
{
  puts (banner);
  puts ("Usage:\n"
        " fst [<fst_options>] info [-f] [-u] <source>\n"
        " fst [<fst_options>] info [-e]      <source> <path>\n"
        " fst [<fst_options>] info [-c]      <source> <number>\n"
        "Options:\n"
        "  -c        <number> is a cluster number instead of a sector number\n"
        "  -e        Show names of extended attributes\n"
        "  -f        Show fragmentation of free space\n"
        "  -u        Show unallocated sectors\n"
        "Arguments:\n"
        "  <source>  A drive name (eg, \"C:\") or snapshot file\n"
        "  <path>    Full path name of a file or directory (without drive name)\n"
        "  <number>  A sector number (without -c) or a cluster number (-c)");
  quit (1, FALSE);
}


static void usage_check (void)
{
  puts (banner);
  puts ("Usage:\n"
        "  fst [<fst_options>] check [-f] [-m] [-p] [-s] [-u] [-v] <source>\n"
        "Options:\n"
        "  -f        Show fragmentation\n"
        "  -m        Use more memory\n"
        "  -p        Pedantic checks\n"
        "  -s        Show summary\n"
        "  -u        List sectors which are allocated but not used\n"
        "  -v        Verbose -- show path names\n"
        "Arguments:\n"
        "  <source>  A drive name (eg, \"C:\") or a snapshot file");
  quit (1, FALSE);
}


static void usage_save (void)
{
  puts (banner);
  puts ("Usage:\n"
        "  fst [<fst_options>] save [-v] <source> <target>\n"
        "Options:\n"
        "  -v        Verbose -- show path names\n"
        "Arguments:\n"
        "  <source>  A drive name (eg, \"C:\") or a snapshot file\n"
        "  <target>  Name of target file");
  quit (1, FALSE);
}


static void usage_restore (void)
{
  puts (banner);
  puts ("Usage:\n"
        "  fst [<fst_options>] restore [-s=<backup>] <target> <source> [<sector>]\n"
        "Options:\n"
        "  -s        Save old sectors into snapshot file <backup>\n"
        "Arguments:\n"
        "  <target>  A drive name (eg, \"C:\") or a snapshot file\n"
        "  <source>  Name of the snapshot file to be copied to disk\n"
        "  <sector>  A sector number (optional)");
  quit (1, FALSE);
}


static void usage_copy (void)
{
  puts (banner);
  puts ("Usage:\n"
        "  fst [<fst_options>] copy <source> <path> <target>\n"
        "Arguments:\n"
        "  <source>  A drive name (eg, \"C:\")\n"
        "  <path>    Full path name of the source file (without drive name)\n"
        "  <target>  Name of target file");
  quit (1, FALSE);
}


static void usage_dir (void)
{
  puts (banner);
  puts ("Usage:\n"
        "  fst [<fst_options>] dir <source> <path>\n"
        "Arguments:\n"
        "  <source>  A drive name (eg, \"C:\") or a snapshot file\n"
        "  <path>    Full path name of directory or file (without drive name)");
  quit (1, FALSE);
}


static void usage_read (void)
{
  puts (banner);
  puts ("Usage:\n"
        "  fst [<fst_options>] read <source> <target> <sector>\n"
        "Arguments:\n"
        "  <source>  A drive name (eg, \"C:\") or a snapshot file\n"
        "  <target>  Name of target file\n"
        "  <sector>  A sector number");
  quit (1, FALSE);
}


static void usage_write (void)
{
  puts (banner);
  puts ("Usage:\n"
        "  fst [<fst_options>] write <target> <source> <sector>\n"
        "Arguments:\n"
        "  <target>  A drive name (eg, \"C:\") or a snapshot file\n"
        "  <source>  Name of source file\n"
        "  <sector>  A sector number");
  quit (1, FALSE);
}


static void usage_diff (void)
{
  puts (banner);
  puts ("Usage:\n"
        "  fst [<fst_options>] diff <file1> <file2>\n"
        "Arguments:\n"
        "  <file1>   Drive name, snapshot file, or CRC file (old)\n"
        "  <file2>   Drive name, snapshot file, or CRC file (new)");
  quit (1, FALSE);
}


static void usage_crc (void)
{
  puts (banner);
  puts ("Usage:\n"
        "  fst [<fst_options>] crc <source> <target>\n"
        "Arguments:\n"
        "  <source>  A drive name (eg, \"C:\")\n"
        "  <target>  Name of CRC file to be written");
  quit (1, FALSE);
}


/* `info' action. */

static void cmd_info (int argc, char *argv[])
{
  DISKIO *d;
  int i;
  char *e;
  cyl_head_sec chs;

  i = 1;
  while (i < argc)
    if (strcmp (argv[i], "-c") == 0)
      {
        what_cluster_flag = TRUE; ++i;
      }
    else if (strcmp (argv[i], "-e") == 0)
      {
        show_eas = TRUE; ++i;
      }
    else if (strcmp (argv[i], "-f") == 0)
      {
        show_free_frag = TRUE; ++i;
      }
    else if (strcmp (argv[i], "-u") == 0)
      {
        show_unused = TRUE; ++i;
      }
    else
      break;
  if (i >= argc || argv[i][0] == '-')
    usage_info ();
  if (argc - i == 1)
    {
      a_info = TRUE;
      if (what_cluster_flag || show_eas)
        usage_info ();
    }
  else if (argc - i == 2)
    {
      if (argv[i+1][0] == '\\')
        {
          if (show_free_frag || show_unused || what_cluster_flag)
            usage_info ();
          a_find = TRUE; a_where = TRUE;
          find_path = argv[i+1] + 1;
        }
      else
        {
          if (show_free_frag || show_unused || show_eas)
            usage_info ();
          errno = 0;
          what_sector = strtoul (argv[i+1], &e, 0);
          if (errno != 0 || e == argv[i+1] || *e != 0)
            usage_info ();
          a_what = TRUE;
        }
    }
  else
    usage_info ();
  info_file = stdout; diag_file = stderr; prog_file = stderr;
  d = diskio_open ((PCSZ)argv[i], DIO_DISK | DIO_SNAPSHOT, FALSE);

  if (a_what && !what_cluster_flag
      && diskio_cyl_head_sec (d, &chs, what_sector))
    info ("Sector #%lu: Cylinder %lu, head %lu, sector %lu\n",
          what_sector, chs.cyl, chs.head, chs.sec);

  do_disk (d);
  diskio_close (d);
}


/* `check' action. */

static void cmd_check (int argc, char *argv[])
{
  DISKIO *d;
  int i;

  i = 1;
  while (i < argc)
    if (strcmp (argv[i], "-f") == 0)
      {
        show_frag = TRUE; ++i;
      }
    else if (strcmp (argv[i], "-s") == 0)
      {
        show_summary = TRUE; ++i;
      }
    else if (strcmp (argv[i], "-m") == 0)
      {
        plenty_memory = TRUE; ++i;
      }
    else if (strcmp (argv[i], "-p") == 0)
      {
        check_pedantic = TRUE; ++i;
      }
    else if (strcmp (argv[i], "-u") == 0)
      {
        check_unused = TRUE; ++i;
      }
    else if (strcmp (argv[i], "-v") == 0)
      {
        verbose = TRUE; ++i;
      }
    else
      break;
  if (argc - i != 1)
    usage_check ();
  if (argv[i][0] == '-')
    usage_check ();
  a_check = TRUE;
  info_file = stderr; diag_file = stdout; prog_file = stderr;
  d = diskio_open ((PCSZ)argv[i], DIO_DISK | DIO_SNAPSHOT, FALSE);
  do_disk (d);
  diskio_close (d);
  quit (0, TRUE);
}


/* Compare sectors of two snapshot files.  The operation is controlled
   by WHICH:
        0    compare sectors which are in both files
        1    list sectors which are in the first file only
        2    list sectors which are in the second file only */

static void diff_sectors (DISKIO *d1, DISKIO *d2,
                          const ULONG *p1, const ULONG *p2,
                          ULONG n1, ULONG n2, int which)
{
  int cmp;
  BYTE raw1[512], raw2[512];

  if (which == 0)
    list_start ("Differing sectors:");
  else
    list_start ("Sectors only in file %d:", which);
  while (n1 != 0 || n2 != 0)
    {
      if (n1 == 0)
        cmp = 1;
      else if (n2 == 0)
        cmp = -1;
      else if (*p1 > *p2)
        cmp = 1;
      else if (*p1 < *p2)
        cmp = -1;
      else
        cmp = 0;
      switch (which)
        {
        case 0:                 /* Compare sectors */
          if (cmp == 0)
            {
              read_sec (d1, raw1, *p1, 1, FALSE);
              read_sec (d2, raw2, *p1, 1, FALSE);
              if (memcmp (raw1, raw2, 512) != 0)
                list ("#%lu", *p1);
            }
          break;
        case 1:
          if (cmp < 0)
            list ("#%lu", *p1);
          break;
        case 2:
          if (cmp > 0)
            list ("#%lu", *p2);
          break;
        }
      if (cmp <= 0)
        ++p1, --n1;
      if (cmp >= 0)
        ++p2, --n2;
    }
  list_end ();
}


/* Compare sectors of a snapshot file to sectors of a disk or to a CRC
   file.  N sector numbers are passed in the array pointed to by
   ARRAY. */

static void compare_sectors_array (DISKIO *d1, DISKIO *d2, ULONG *array,
                                   ULONG n)
{
  BYTE raw1[512], raw2[512];
  int ok1, ok2;
  ULONG idx, secno, n1, n2;
  crc_t crc1, crc2;

  list_start ("Differing sectors:");
  n1 = diskio_total_sectors (d1); n2 = diskio_total_sectors (d2);
  if (diskio_type (d1) == DIO_CRC || diskio_type (d2) == DIO_CRC)
    {
      for (idx = 0; idx < n; ++idx)
        {
          secno = array[idx];
          if ((n1 != 0 && secno >= n1) || (n2 != 0 && secno >= n2))
            break;
          ok1 = crc_sec (d1, &crc1, secno);
          ok2 = crc_sec (d2, &crc2, secno);
          if (ok1 && ok2 && crc1 != crc2)
            list ("#%lu", secno);
        }
    }
  else
    {
      for (idx = 0; idx < n; ++idx)
        {
          secno = array[idx];
          if ((n1 != 0 && secno >= n1) || (n2 != 0 && secno >= n2))
            break;
          read_sec (d1, raw1, secno, 1, FALSE);
          read_sec (d2, raw2, secno, 1, FALSE);
          if (memcmp (raw1, raw2, 512) != 0)
            list ("#%lu", secno);
        }
    }
  list_end ();
  if (idx < n)
    {
      list_start ("Missing sectors in source %d:", n1 == 0 ? 2 : 1);
      for (; idx < n; ++idx)
        list ("#%lu", array[idx]);
      list_end ();
    }
}


/* Compare all sectors of two disks, two CRC files, or a disk and a
   CRC file. */

static void compare_sectors_all (DISKIO *d1, DISKIO *d2)
{
  BYTE raw1[512], raw2[512];
  crc_t crc1, crc2;
  int ok1, ok2;
  ULONG secno, n, n1, n2;

  list_start ("Differing sectors:");
  n1 = diskio_total_sectors (d1); n2 = diskio_total_sectors (d2);
  n = MIN (n1, n2);
  if (diskio_type (d1) == DIO_CRC || diskio_type (d2) == DIO_CRC)
    {
      if (diskio_type (d1) == DIO_CRC && diskio_type (d2) == DIO_CRC)
        diskio_crc_load (d1);
      for (secno = 0; secno < n; ++secno)
        {
          ok1 = crc_sec (d1, &crc1, secno);
          ok2 = crc_sec (d2, &crc2, secno);
          if (ok1 && ok2 && crc1 != crc2)
            list ("#%lu", secno);
        }
    }
  else
    {
      for (secno = 0; secno < n; ++secno)
        {
          read_sec (d1, raw1, secno, 1, FALSE);
          read_sec (d2, raw2, secno, 1, FALSE);
          if (memcmp (raw1, raw2, 512) != 0)
            list ("#%lu", secno);
        }
    }
  list_end ();
  if (n1 > n2)
    info ("First disk has more sectors than second disk\n");
  else if (n1 < n2)
    info ("Second disk has more sectors than first disk\n");
}


/* `diff' action. */

static void cmd_diff (int argc, char *argv[])
{
  int i;
  const char *fname1;
  const char *fname2;
  DISKIO *d1, *d2;
  ULONG *sort1, *sort2;
  ULONG n1, n2;

  i = 1;
  if (argc - i != 2)
    usage_diff ();
  if (argv[i][0] == '-')
    usage_diff ();
  info_file = stdout; diag_file = stderr; prog_file = stderr;
  fname1 = argv[i+0];
  fname2 = argv[i+1];
  d1 = diskio_open ((PCSZ)fname1, DIO_DISK | DIO_SNAPSHOT | DIO_CRC, FALSE);
  d2 = diskio_open ((PCSZ)fname2, DIO_DISK | DIO_SNAPSHOT | DIO_CRC, FALSE);
  crc_build_table ();
  if (diskio_access == ACCESS_DASD
      && (diskio_type (d1) == DIO_CRC || diskio_type (d2) == DIO_CRC))
    error ("Cannot use the -d option for the `diff' action with CRC files");
  sort1 = diskio_snapshot_sort (d1);
  sort2 = diskio_snapshot_sort (d2);
  if (sort1 != NULL && sort2 != NULL)
    {
      n1 = diskio_snapshot_sectors (d1); n2 = diskio_snapshot_sectors (d2);
      for (i = 0; i <= 2; ++i)
        diff_sectors (d1, d2, sort1, sort2, n1, n2, i);
    }
  else if (sort1 != NULL)
    compare_sectors_array (d1, d2, sort1, diskio_snapshot_sectors (d1));
  else if (sort2 != NULL)
    compare_sectors_array (d1, d2, sort2, diskio_snapshot_sectors (d2));
  else
    compare_sectors_all (d1, d2);
  free (sort1); free (sort2);
  diskio_close (d1);
  diskio_close (d2);
}


/* `save' action. */

static void cmd_save (int argc, char *argv[])
{
  DISKIO *d;
  int i;
  const char *src_fname;

  i = 1;
  while (i < argc)
    if (strcmp (argv[i], "-v") == 0)
      {
        verbose = TRUE; ++i;
      }
    else
      break;
  if (argc - i != 2)
    usage_save ();
  if (argv[i][0] == '-')
    usage_save ();
  src_fname = argv[i+0];
  save_fname = argv[i+1];
  a_save = TRUE;
  info_file = stdout; diag_file = stderr; prog_file = stderr;
  d = diskio_open ((PCSZ)src_fname, DIO_DISK | DIO_SNAPSHOT, FALSE);
  save_create (src_fname, SAVE_SNAPSHOT);
  do_disk (d);
  diskio_close (d);
  save_close ();
}


/* `restore' action. */

static void cmd_restore (int argc, char *argv[])
{
  DISKIO *d1, *d2;
  int i;
  const char *dst_fname;
  const char *src_fname;
  char temp, all = TRUE, *e;
  ULONG *sort, idx, sec, bad, n, secno = 0;
  char buf[10];
  BYTE data[512];

  i = 1; save_fname = NULL;
  while (i < argc)
    if (strncmp (argv[i], "-s=", 3) == 0)
      {
        if (argv[i][3] == 0)
          usage_restore ();
        save_fname = argv[i] + 3;
        ++i;
      }
    else
      break;

  if (argc - i == 2)
    {
      secno = 0; all = TRUE;
    }
  else if (argc - i == 3)
    {
      errno = 0;
      secno = strtoul (argv[i+2], &e, 0);
      if (errno != 0 || e == argv[i+2] || *e != 0)
        usage_write ();
      all = FALSE;
    }
  else
    usage_restore ();

  if (argv[i][0] == '-')
    usage_restore ();
  dst_fname = argv[i+0];
  src_fname = argv[i+1];

  printf ("Do you really want to overwrite the file system data "
          "structures\nof \"%s\" (type \"YES!\" to confirm)? ", dst_fname);
  fflush (stdout);
  if (fgets (buf, sizeof (buf), stdin) == NULL)
    quit (2, FALSE);
  if (strcmp (buf, "YES!\n") != 0)
    quit (0, FALSE);

  info_file = stdout; diag_file = stderr; prog_file = stderr;
  fprintf (prog_file, "Preliminary actions...\n"); fflush (prog_file);
  temp = write_enable; write_enable = FALSE;
  ignore_lock_error = FALSE; dont_lock = FALSE;
  d2 = diskio_open ((PCSZ)src_fname, DIO_SNAPSHOT, FALSE);
  write_enable = temp;
  d1 = diskio_open ((PCSZ)dst_fname, DIO_DISK | DIO_SNAPSHOT, TRUE);
  if (save_fname != NULL)
    save_create (dst_fname, SAVE_SNAPSHOT);

  /* Set up a sorted list of sectors. */

  if (all)
    {
      sort = diskio_snapshot_sort (d2);
      n = diskio_snapshot_sectors (d2);
    }
  else
    {
      sort = &secno; n = 1;
    }

  /* Check the snapshot file. */

  for (idx = 0; idx < n; ++idx)
    read_sec (d2, data, sort[idx], 1, FALSE);

  /* Make a backup if requested. */

  if (save_fname != NULL)
    {
      a_save = TRUE;
      for (idx = 0; idx < n; ++idx)
        read_sec (d1, data, sort[idx], 1, TRUE);
      a_save = FALSE;
      save_close ();
    }
  fprintf (prog_file, "Writing...DO NOT INTERRUPT!...\n"); fflush (prog_file);
  bad = 0;
  for (idx = 0; idx < n; ++idx)
    {
      sec = sort[idx];
      read_sec (d2, data, sec, 1, FALSE);
      if (!write_sec (d1, data, sec))
        ++bad;
    }
  diskio_close (d2);
  diskio_close (d1);
  if (sort != &secno)
    free (sort);
  if (bad == 0)
    {
      fprintf (prog_file, "Done\n");
      quit (0, FALSE);
    }
  else if (bad == 1)
    {
      fprintf (prog_file, "Done, 1 sector not written\n");
      quit (2, FALSE);
    }
  else
    {
      fprintf (prog_file, "Done, %lu sectors not written\n", bad);
      quit (2, FALSE);
    }
}


/* `copy' action. */

static void cmd_copy (int argc, char *argv[])
{
  DISKIO *d;
  const char *src_fname;
  int i;

  i = 1;
  if (argc - i != 3)
    usage_copy ();
  if (argv[i][0] == '-')
    usage_copy ();
  a_find = TRUE; a_copy = TRUE;
  info_file = stdout; diag_file = stderr; prog_file = stderr;
  src_fname = argv[i+0];
  save_fname = argv[i+2];
  d = diskio_open ((PCSZ)argv[i+0], DIO_DISK, FALSE);
  find_path = argv[i+1];
  if (*find_path == '\\')
    ++find_path;
  save_create (src_fname, SAVE_RAW);
  do_disk (d);
  save_close ();
  diskio_close (d);
}


/* `dir' action. */

static void cmd_dir (int argc, char *argv[])
{
  DISKIO *d;
  int i;

  i = 1;
  if (argc - i != 2)
    usage_dir ();
  if (argv[i][0] == '-')
    usage_dir ();
  a_find = TRUE; a_dir = TRUE;
  info_file = stdout; diag_file = stderr; prog_file = stderr;
  d = diskio_open ((PCSZ)argv[i+0], DIO_DISK | DIO_SNAPSHOT, FALSE);
  find_path = argv[i+1];
  if (*find_path == '\\')
    ++find_path;
  do_disk (d);
  diskio_close (d);
}


/* `read' action. */

static void cmd_read (int argc, char *argv[])
{
  DISKIO *d;
  int i;
  ULONG n;
  const char *src_fname;
  char *e;
  char data[512];

  i = 1;
  if (argc - i != 3)
    usage_read ();
  if (argv[i][0] == '-')
    usage_read ();
  info_file = stdout; diag_file = stderr; prog_file = stderr;
  src_fname = argv[i+0];
  save_fname = argv[i+1];
  d = diskio_open ((PCSZ)src_fname, DIO_DISK | DIO_SNAPSHOT, FALSE);
  errno = 0;
  n = strtoul (argv[i+2], &e, 0);
  if (errno != 0 || e == argv[i+2] || *e != 0)
    usage_read ();
  save_create (src_fname, SAVE_RAW);
  read_sec (d, data, n, 1, FALSE);
  fwrite (data, 512, 1, save_file);
  save_close ();
  diskio_close (d);
}


/* `write' action. */

static void cmd_write (int argc, char *argv[])
{
  DISKIO *d;
  int i, ok;
  size_t nread;
  ULONG n;
  const char *dst_fname;
  const char *src_fname;
  char *e;
  char data[512+1];             /* Extra byte for checking the file length */
  FILE *f;

  i = 1;
  if (argc - i != 3)
    usage_write ();
  if (argv[i][0] == '-')
    usage_write ();
  info_file = stdout; diag_file = stderr; prog_file = stderr;
  dst_fname = argv[i+0];
  src_fname = argv[i+1];

  errno = 0;
  n = strtoul (argv[i+2], &e, 0);
  if (errno != 0 || e == argv[i+2] || *e != 0)
    usage_write ();

  f = fopen (src_fname, "rb");
  if (f == NULL)
    error ("%s: %s", src_fname, strerror (errno));
  nread = fread (data, 1, sizeof (data), f);
  if (nread == 0)
    error ("%s: %s", src_fname, strerror (errno));
  fclose (f);
  if (nread != 512)
    error ("The source file must contain exactly 512 bytes");

  d = diskio_open ((PCSZ)dst_fname, DIO_DISK | DIO_SNAPSHOT, TRUE);
  ok = write_sec (d, data, n);
  diskio_close (d);
  quit (ok ? 0 : 2, FALSE);
}


/* `crc' action. */

static void cmd_crc (int argc, char *argv[])
{
  DISKIO *d;
  int i;
  const char *src_fname;
  ULONG secno, n;
  crc_t *acrc;

  i = 1;
  if (argc - i != 2)
    usage_crc ();
  if (argv[i][0] == '-')
    usage_crc ();
  if (diskio_access == ACCESS_DASD)
    error ("Cannot use the -d option with the `crc' action");
  src_fname = argv[i+0];
  save_fname = argv[i+1];
  info_file = stdout; diag_file = stderr; prog_file = stderr;
  d = diskio_open ((PCSZ)src_fname, DIO_DISK, FALSE);
  save_create (src_fname, SAVE_CRC);
  crc_build_table ();
  n = diskio_total_sectors (d);
  acrc = xmalloc (n * sizeof (*acrc));
  for (secno = 0; secno < n; ++secno)
    {
      if (!crc_sec (d, acrc + secno, secno))
        warning (1, "Sector #%lu not readable", secno);
      acrc[secno] = ULONG_TO_FS (acrc[secno]);
    }
  if (fwrite (acrc, sizeof (*acrc), n, save_file) != n)
    save_error ();
  free (acrc);
  diskio_close (d);
  save_sector_count = n;
  save_close ();
}


/* Here's where the program starts. */

int main (int argc, char *argv[])
{
  int i;

  /* Initialize variables which are not initialized to zero. */

  info_file = stdout; diag_file = stderr; prog_file = stderr;
  diskio_access = ACCESS_LOG_TRACK;
  removable_allowed = TRUE;

  /* Initialize cur_case_map. */

  init_cur_case_map ();

  /* Parse initial command line options. */

  i = 1;
  if (argc - i == 2 && strcmp (argv[i], "-h") == 0)
    {
      ++i; --argc;
    }
  else
    {
      while (i < argc && argv[i][0] == '-')
        if (strcmp (argv[i], "-d") == 0)
          {
            diskio_access = ACCESS_DASD; ++i;
          }
        else if (strcmp (argv[i], "-n") == 0)
          {
            ignore_lock_error = TRUE; ++i;
          }
        else if (strcmp (argv[i], "-w") == 0)
          {
            write_enable = TRUE; ++i;
          }
        else if (strcmp (argv[i], "-x") == 0)
          {
            sector_number_format = 'x'; ++i;
          }
        else if (strcmp (argv[i], "-Don't lock") == 0)
          {
            dont_lock = TRUE; ++i;
          }
        else if (strcmp (argv[i], "-FAT") == 0)
          {
            force_fs = 'f'; ++i;
          }
        else if (strcmp (argv[i], "-HPFS") == 0)
          {
            force_fs = 'h'; ++i;
          }
        else
          usage ();
      if (argc - i < 1)
        usage ();
    }

  /* Parse the action. */

  if (strcmp (argv[i], "info") == 0)
    cmd_info (argc - i, argv + i);
  else if (strcmp (argv[i], "check") == 0)
    cmd_check (argc - i, argv + i);
  else if (strcmp (argv[i], "save") == 0)
    cmd_save (argc - i, argv + i);
  else if (strcmp (argv[i], "restore") == 0)
    cmd_restore (argc - i, argv + i);
  else if (strcmp (argv[i], "diff") == 0)
    cmd_diff (argc - i, argv + i);
  else if (strcmp (argv[i], "copy") == 0)
    cmd_copy (argc - i, argv + i);
  else if (strcmp (argv[i], "dir") == 0)
    cmd_dir (argc - i, argv + i);
  else if (strcmp (argv[i], "read") == 0)
    cmd_read (argc - i, argv + i);
  else if (strcmp (argv[i], "write") == 0)
    cmd_write (argc - i, argv + i);
  else if (strcmp (argv[i], "crc") == 0)
    cmd_crc (argc - i, argv + i);
  else
    usage ();

  /* Done. */

  quit (0, FALSE);
  return 0;                     /* Keep the compiler happy */
}
