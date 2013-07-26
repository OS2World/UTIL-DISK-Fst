/* fst.h -- Global header file for fst
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


/* Possible byte orders. */

#define LITTLE_ENDIAN   1234
#define BIG_ENDIAN      4321

/* Target byte order. */

#define BYTE_ORDER      LITTLE_ENDIAN

/* Convert numbers from filesystem format to host format and vice
   versa.  These are a no-ops on little-endian machines (Intel
   80386). */

#if BYTE_ORDER == LITTLE_ENDIAN
#define ULONG_FROM_FS(x)        (ULONG)(x)
#define USHORT_FROM_FS(x)       (USHORT)(x)
#define ULONG_TO_FS(x)          (ULONG)(x)
#define USHORT_TO_FS(x)         (USHORT)(x)
#else
/* TODO */
#endif

/* Return the smaller one of the two arguments. */
#define MIN(x,y)         ((x) < (y) ? (x) : (y))

/* Round up X to the smallest multiple of Y which is >= X.  Y must be
   a power of two. */
#define ROUND_UP(x,y)    (((x)+(y)-1) & ~((y)-1))

/* Divide X by Y, rounding up. */
#define DIVIDE_UP(x,y)   (((x)+(y)-1) / (y))

/* Return true iff X is in the range given by the starting number S
   and the count C (inclusive). */
#define IN_RANGE(x,s,c)  ((s) <= (x) && (x) < (s) + (c))

/* Return a non-zero value if bit X is set in bit vector BV.  BV must
   be an array of BYTEs. */
#define BITSETP(bv,x)   ((bv)[(x)>>3] & (1 << ((x) & 7)))

/* Let GCC check the format string and all the arguments of
   printf()-like functions.  S is the argument# of the format string,
   F is the argument# of the first argument checked against the format
   string. */
#ifdef __GNUC__
#define ATTR_PRINTF(s,f) __attribute__ ((__format__ (__printf__, s, f)))
#else
#define ATTR_PRINTF(s,f)
#endif

/* Tell GCC that a function never returns. */
#ifdef __GNUC__
#define ATTR_NORETURN __attribute__ ((__noreturn__))
#else
#define ATTR_NORETURN
#endif

/* Tell GCC to inline a function. */
#ifdef __GNUC__
#define INLINE __inline__
#else
#define INLINE
#endif

/* File attributes. */

#define ATTR_READONLY   0x01    /* Read only */
#define ATTR_HIDDEN     0x02    /* Hidden */
#define ATTR_SYSTEM     0x04    /* System */
#define ATTR_LABEL      0x08    /* Volume label */
#define ATTR_DIR        0x10    /* Directory */
#define ATTR_ARCHIVED   0x20    /* Archived */
#define ATTR_NONFAT     0x40    /* Name not FAT-compatible (HPFS only) */


typedef struct path_chain
{
  const struct path_chain *parent;
  const char *name;
} path_chain;

#define PATH_CHAIN_NEW(L, P, N) \
  (a_check && plenty_memory \
   ? path_chain_new ((P), (N)) \
   : ((L)->parent = (P), (L)->name = (N), (L)))

/* See fst.c */
extern char verbose;
extern char sector_number_format;
extern char a_info;
extern char a_save;
extern char a_check;
extern char a_what;
extern char a_where;
extern char a_copy;
extern char a_dir;
extern char a_find;
extern char plenty_memory;
extern char check_unused;
extern char check_pedantic;
extern char show_unused;
extern char show_free_frag;
extern char show_frag;
extern char show_eas;
extern char show_summary;
extern ULONG what_sector;
extern char what_cluster_flag;
extern const char *find_path;
extern BYTE cur_case_map[256];
extern FILE *diag_file;
extern FILE *prog_file;

/* See fst.c */
void quit (int rc, int show) ATTR_NORETURN;
int my_vfprintf (FILE *f, const char *fmt, va_list arg_ptr);
int my_fprintf (FILE *f, const char *fmt, ...) ATTR_PRINTF (2, 3);
int my_sprintf (char *buf, const char *fmt, ...) ATTR_PRINTF (2, 3);
void error (const char *fmt, ...) ATTR_NORETURN ATTR_PRINTF (1, 2);
void warning_prolog (int level);
void warning_epilog (void);
void warning (int level, const char *fmt, ...) ATTR_PRINTF (2, 3);
void warning_cont (const char *fmt, ...) ATTR_PRINTF (1, 2);
int info (const char *fmt, ...) ATTR_PRINTF (1, 2);
void infoi (const char *fmt, int indent, ...) ATTR_PRINTF (1, 3);
const char *format_sector_range (ULONG start, ULONG count);
const char *format_string (const unsigned char *s, size_t n, int zero_term);
const char *format_ea_name (const FEA *pfea);
void *xmalloc (size_t n);
path_chain *path_chain_new (const path_chain *parent, const char *name);
int path_chain_len (const path_chain *p);
const char *format_path_chain (const path_chain *bottom, const char *last);
void list_start (const char *fmt, ...) ATTR_PRINTF (1, 2);
void list (const char *fmt, ...) ATTR_PRINTF (1, 2);
void list_end (void);
