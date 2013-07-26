/* diskio.c -- Disk/sector I/O for fst
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
#define INCL_DOSDEVICES
#include <os2.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <io.h>
#include <fcntl.h>
#include <limits.h>
#include "fst.h"
#include "crc.h"
#include "diskio.h"

/* Size of the hash table used for speeding up reading snapshot files. */

#define HASH_SIZE       997

/* This value marks the end of a hash chain.  It should be an
   `impossible' sector number. */

#define HASH_END        0xffffffff

/* Method for reading and writing sectors. */

enum disk_io_type
{
  DIOT_DISK_DASD,               /* DosRead, DosWrite */
  DIOT_DISK_TRACK,              /* Logical: DSK_READTRACK, DSK_WRITETRACK */
  DIOT_SNAPSHOT,                /* Snapshot file */
  DIOT_CRC                      /* CRC file */
};

/* Data for DIOT_DISK_DASD. */

struct diskio_dasd
{
  HFILE hf;                     /* File handle */
  char sec_mode;                /* TRUE iff sector mode active */
};

/* Data for DIOT_DISK_TRACK. */

struct diskio_track
{
  HFILE hf;                     /* File handle */
  ULONG layout_size;            /* Size of structure pointed to by playout */
  ULONG hidden;                 /* Hidden sectors */
  ULONG spt;                    /* Sectors per track */
  ULONG heads;                  /* Number of heads */
  TRACKLAYOUT *playout;         /* Parameter for DSK_READTRACK */
  BYTE *track_buf;              /* Buffer for one track */
};

/* Data for DIOT_SNAPSHOT. */

struct diskio_snapshot
{
  HFILE hf;                     /* File handle */
  ULONG sector_count;           /* Total number of sectors */
  ULONG *sector_map;            /* Table containing relative sector numbers */
  ULONG *hash_next;             /* Hash chains */
  ULONG version;                /* Format version number */
  ULONG hash_start[HASH_SIZE];  /* Hash chain heads */
};

/* Data for DIOT_CRC. */

struct diskio_crc
{
  FILE *f;                      /* Stream */
  ULONG version;                /* Format version number */
  crc_t *vec;                   /* See diskio_crc_load() */
};

/* DISKIO structure. */

struct diskio
{
  enum disk_io_type type;       /* Method */
  ULONG spt;                    /* Sectors per track */
  ULONG total_sectors;          /* Total number of sectors */
  union
    {
      struct diskio_dasd dasd;
      struct diskio_track track;
      struct diskio_snapshot snapshot;
      struct diskio_crc crc;
    } x;                        /* Method-specific data */
};

/* This variable selects the method of direct disk I/O to use.
   ACCESS_DASD selects DosRead and DosWrite, ACCESS_LOG_TRACK selects
   DSK_READTRACK and DSK_WRITETRACK for logical disks. */

enum access_type diskio_access;

/* Non-zero if write access is required. */

char write_enable;

/* Non-zero if removable disks are allowed. */

char removable_allowed;

/* Non-zero to ignore failure to lock the disk. */

char ignore_lock_error;

/* Non-zero to disable locking (FOR TESTING ONLY!). */

char dont_lock;

/* Type of the save file. */

enum save_type save_type;

/* This is the save file. */

FILE *save_file;

/* Name of the save file.  There is no save file if this variable is
   NULL. */

const char *save_fname;

/* Number of sectors written to the save file. */

ULONG save_sector_count;

/* Number of elements allocated for save_sector_map. */

ULONG save_sector_alloc;

/* This table maps logical sector numbers to relative sector numbers
   in the snap shot file, under construction. */

ULONG *save_sector_map;


/* Return the drive letter of a file name, if any, as upper-case
   letter.  Return 0 if there is no drive letter. */

static char fname_drive (const char *s)
{
  if (s[0] >= 'A' && s[0] <= 'Z' && s[1] == ':')
    return s[0];
  else if (s[0] >= 'a' && s[0] <= 'z' && s[1] == ':')
    return (char)(s[0]-'a'+'A');
  else
    return 0;
}


/* Return the currently selected drive as upper-case letter. */

static char cur_drive (void)
{
  ULONG drive, map;

  if (DosQueryCurrentDisk (&drive, &map) != 0)
    return 0;
  return (char)(drive - 1 + 'A');
}


/* Obtain access to a disk, snapshot file, or CRC file.  FNAME is the
   name of the disk or file to open.  FLAGS defines what types of
   files are allowed; FLAGS is the inclusive OR of one or more of
   DIO_DISK, DIO_SNAPSHOT, and DIO_CRC.  Open for writing if FOR_WRITE
   is non-zero. */

DISKIO *diskio_open (PCSZ fname, unsigned flags, int for_write)
{
  ULONG rc, action, mode, parmlen, datalen, pos, nread, hash, i, ulParm, *map;
  HFILE hf;
  UCHAR data;
  BIOSPARAMETERBLOCK bpb;
  BYTE parm[2];
  int h;
  DISKIO *d;

  /* Writing required the -w option.  On the other hand, -w should not
     be used unless writing is requested. */

  if (!for_write && write_enable)
    error ("Do not use the -w option for actions that don't write sectors");
  if (for_write && !write_enable)
    error ("Use the -w option for actions that write sectors");

  /* Allocate a DISKIO structure. */

  d = xmalloc (sizeof (*d));

  /* Check for drive letter (direct disk access). */

  if (isalpha ((unsigned char)fname[0]) && fname[1] == ':' && fname[2] == 0)
    {
      /* Direct disk access requested.  Check if this is allowed. */

      if (!(flags & DIO_DISK))
        error ("A drive name cannot be used for this action");

      /* Open a file handle for the logical disk drive. */

      if (for_write)
        mode = (OPEN_FLAGS_DASD | OPEN_FLAGS_FAIL_ON_ERROR
                | OPEN_FLAGS_RANDOM | OPEN_FLAGS_NOINHERIT
                | OPEN_SHARE_DENYREADWRITE | OPEN_ACCESS_READWRITE);
      else
        mode = (OPEN_FLAGS_DASD | OPEN_FLAGS_FAIL_ON_ERROR
                | OPEN_FLAGS_RANDOM | OPEN_FLAGS_NOINHERIT
                | OPEN_SHARE_DENYWRITE | OPEN_ACCESS_READONLY);
      rc = DosOpen (fname, &hf, &action, 0, FILE_NORMAL,
                    OPEN_ACTION_FAIL_IF_NEW | OPEN_ACTION_OPEN_IF_EXISTS,
                    mode, 0);
      if (rc != 0)
        error ("Cannot open %s (rc=%lu)", fname, rc);

      if (!dont_lock)
        {
          /* Attempt to lock the drive. */

          parm[0] = 0; parmlen = 1;
          datalen = 1;
          rc = DosDevIOCtl (hf, IOCTL_DISK, DSK_LOCKDRIVE,
                            parm, parmlen, &parmlen,
                            &data, datalen, &datalen);
          if (rc != 0 && (for_write || !ignore_lock_error))
            error ("Cannot lock drive");
          if (rc != 0)
            {
              warning (0, "Cannot lock drive -- proceeding without locking");
              warning_cont (" NOTE: Results are not reliable without locking!");
            }
          else
            {
              /* Issue DSK_REDETERMINEMEDIA to flush the buffers of
                 the file system driver. */

              parm[0] = 0; parmlen = 1;
              datalen = 1;
              rc = DosDevIOCtl (hf, IOCTL_DISK, DSK_REDETERMINEMEDIA,
                                parm, parmlen, &parmlen,
                                &data, datalen, &datalen);
              if (rc != 0)
                warning (0, "Cannot flush the buffers of the file system "
                         "driver");
            }
        }

      /* Obtain the disk geometry. */

      parm[0] = 0x01;       /* Current BPB */
      parm[1] = 0;
      parmlen = sizeof (parm);
      datalen = sizeof (bpb);
      rc = DosDevIOCtl (hf, IOCTL_DISK, DSK_GETDEVICEPARAMS,
                        &parm, parmlen, &parmlen,
                        &bpb, datalen, &datalen);
      if (rc != 0)
        error ("Cannot get device parameters (rc=%lu)", rc);

      /* CDROMs have a strange (faked) geometry which we cannot
         handle.  Reject all removable disks. */

      if (!removable_allowed && !(bpb.fsDeviceAttr & 1))
        error ("Disk is removable (use the -r option to override)");

      /* Compute the total number of sectors (TODO: check for
         overflow). */

      d->total_sectors = bpb.usSectorsPerTrack * bpb.cHeads * bpb.cCylinders;

      /* Show the BIOS parameter block. */

      if (a_info)
        {
          info ("BIOS parameter block:\n");
          info ("  Sectors per track:        %lu\n",
                (ULONG)bpb.usSectorsPerTrack);
          info ("  Heads:                    %lu\n",
                (ULONG)bpb.cHeads);
          info ("  Cylinders:                %lu\n",
                (ULONG)bpb.cCylinders);
          info ("  Total number of sectors:  %lu\n", d->total_sectors);
          info ("  Hidden sectors:           %lu\n", bpb.cHiddenSectors);
        }

      /* Adjust the number of sectors and remember the number of
         sectors per track. */

      d->total_sectors -= bpb.cHiddenSectors;
      d->spt = bpb.usSectorsPerTrack;

      if (diskio_access == ACCESS_LOG_TRACK)
        {
          /* Set up data for accessing tracks of a logical drive. */

          d->x.track.hf = hf;
          d->x.track.hidden = bpb.cHiddenSectors;
          d->x.track.spt = bpb.usSectorsPerTrack;
          d->x.track.heads = bpb.cHeads;

          /* Build the TRACKLAYOUT structure. */

          d->x.track.layout_size =
            sizeof (TRACKLAYOUT)
              + ((bpb.usSectorsPerTrack - 1)
                 * sizeof (d->x.track.playout->TrackTable[0]));
          d->x.track.playout = xmalloc (d->x.track.layout_size);
          for (i = 0; i < bpb.usSectorsPerTrack; ++i)
            {
              d->x.track.playout->TrackTable[i].usSectorNumber = i + 1;
              d->x.track.playout->TrackTable[i].usSectorSize = 512;
            }

          /* Allocate the track buffer.  TODO: Avoid memory overflow
             for large values of bpb.usSectorsPerTrack by
             reading/writing tracks partially. */

          d->x.track.track_buf = xmalloc (bpb.usSectorsPerTrack * 512);
          d->type = DIOT_DISK_TRACK;
        }
      else
        {
          /* Set up data for accessing the disk with DosRead and
             DosWrite. */

          if (d->total_sectors + bpb.cHiddenSectors < LONG_MAX / 512)
            d->x.dasd.sec_mode = FALSE;
          else
            {
              datalen = 0;
              parmlen = sizeof (ulParm);
              ulParm = 0xdeadface;
              rc = DosFSCtl (NULL, datalen, &datalen,
                             (PVOID)&ulParm, parmlen, &parmlen,
                             0x9014, NULL, hf, FSCTL_HANDLE);
              if (rc != 0)
                error ("Cannot switch handle to sector I/O mode, rc=%lu\n",
                       rc);
              d->x.dasd.sec_mode = TRUE;
            }
          d->x.dasd.hf = hf;
          d->type = DIOT_DISK_DASD;
        }
    }
  else
    {
      header hdr;

      /* Reading a regular file requested.  Check if this is
         allowed. */

      if (!(flags & (DIO_SNAPSHOT|DIO_CRC)))
        error ("Drive name required");

      /* Open the file. */

      if (for_write)
        mode = (OPEN_FLAGS_RANDOM | OPEN_FLAGS_NOINHERIT
                | OPEN_SHARE_DENYWRITE | OPEN_ACCESS_READWRITE);
      else
        mode = (OPEN_FLAGS_RANDOM | OPEN_FLAGS_NOINHERIT
                | OPEN_SHARE_DENYWRITE | OPEN_ACCESS_READONLY);
      rc = DosOpen (fname, &hf, &action, 0, FILE_NORMAL,
                    OPEN_ACTION_FAIL_IF_NEW | OPEN_ACTION_OPEN_IF_EXISTS,
                    mode, 0);
      if (rc != 0)
        error ("Cannot open %s (rc=%lu)", fname, rc);

      /* Read the header and check the magic number. */

      rc = DosRead (hf, &hdr, sizeof (hdr), &nread);
      if (rc != 0)
        error ("Cannot read %s (rc=%lu)", fname, rc);
      if (nread != 512
          || !(((flags & DIO_SNAPSHOT)
                && ULONG_FROM_FS (hdr.magic) == SNAPSHOT_MAGIC)
               || ((flags & DIO_CRC)
                   && ULONG_FROM_FS (hdr.magic) == CRC_MAGIC)))
        {
          switch (flags & (DIO_SNAPSHOT | DIO_CRC))
            {
            case DIO_SNAPSHOT:
              error ("%s is not a snapshot file", fname);
            case DIO_CRC:
              error ("%s is not a CRC file", fname);
            case DIO_SNAPSHOT|DIO_CRC:
              error ("%s is neither a snapshot file nor a CRC file", fname);
            }
        }

      /* Further processing depends on the magic number. */

      switch (ULONG_FROM_FS (hdr.magic))
        {
        case SNAPSHOT_MAGIC:

          /* Check the header of a snapshot file and remember the
             values of the header. */

          if (ULONG_FROM_FS (hdr.s.version) > 1)
            error ("Format of %s too new -- please upgrade this program",
                   fname);
          d->x.snapshot.hf = hf;
          d->x.snapshot.sector_count = ULONG_FROM_FS (hdr.s.sector_count);
          d->x.snapshot.version = ULONG_FROM_FS (hdr.s.version);
          rc = DosSetFilePtr (hf, ULONG_FROM_FS (hdr.s.map_pos),
                              FILE_BEGIN, &pos);
          if (rc != 0)
            error ("Cannot read %s (rc=%lu)", fname, rc);

          /* Load the sector map. */

          map = xmalloc (d->x.snapshot.sector_count * sizeof (ULONG));
          d->x.snapshot.sector_map = map;
          d->x.snapshot.hash_next = xmalloc (d->x.snapshot.sector_count * sizeof (ULONG));
          rc = DosRead (hf, map, d->x.snapshot.sector_count * sizeof (ULONG),
                        &nread);
          if (rc != 0)
            error ("Cannot read %s (rc=%lu)", fname, rc);
          if (nread != d->x.snapshot.sector_count * sizeof (ULONG))
            error ("Cannot read %s", fname);

          for (i = 0; i < d->x.snapshot.sector_count; ++i)
            map[i] = ULONG_FROM_FS (map[i]);

          /* Initialize hashing. */

          for (i = 0; i < HASH_SIZE; ++i)
            d->x.snapshot.hash_start[i] = HASH_END;
          for (i = 0; i < d->x.snapshot.sector_count; ++i)
            d->x.snapshot.hash_next[i] = HASH_END;
          for (i = 0; i < d->x.snapshot.sector_count; ++i)
            {
              hash = map[i] % HASH_SIZE;
              d->x.snapshot.hash_next[i] = d->x.snapshot.hash_start[hash];
              d->x.snapshot.hash_start[hash] = i;
            }
          d->total_sectors = 0;
          d->type = DIOT_SNAPSHOT;
          break;

        case CRC_MAGIC:

          /* Check the header of a CRC file and remember the values of
             the header. */

          if (ULONG_FROM_FS (hdr.c.version) > 1)
            error ("Format of %s too new -- please upgrade this program",
                   fname);

          /* Build a C stream from the file handle. */

#ifdef __EMX__
          h = _imphandle ((int)hf);
          if (h == -1)
            error ("%s: %s", (const char *)fname, strerror (errno));
#else
          h = (int)hf;
          if (setmode (h, O_BINARY) == -1)
            error ("%s: %s", (const char *)fname, strerror (errno));
#endif
          d->x.crc.f = fdopen (h, (for_write ? "r+b" : "rb"));

          /* Remember values of the header. */

          d->total_sectors = ULONG_FROM_FS (hdr.c.sector_count);
          d->x.crc.version = ULONG_FROM_FS (hdr.c.version);
          d->x.crc.vec = NULL;  /* CRCs not read into memory */

          /* Seek to the first CRC. */

          fseek (d->x.crc.f, 512, SEEK_SET);
          d->type = DIOT_CRC;
          break;

        default:
          abort ();
        }
    }
  return d;
}


/* Close a DISKIO returned by diskio_open(). */

void diskio_close (DISKIO *d)
{
  ULONG rc, parmlen, datalen;
  UCHAR parm, data;

  switch (d->type)
    {
    case DIOT_DISK_DASD:
      parm = 0; parmlen = 1;
      datalen = 1;
      rc = DosDevIOCtl (d->x.dasd.hf, IOCTL_DISK, DSK_LOCKDRIVE,
                        &parm, sizeof (parm), &parmlen,
                        &data, sizeof (data), &datalen);
      rc = DosClose (d->x.dasd.hf);
      break;
    case DIOT_DISK_TRACK:
      parm = 0; parmlen = 1;
      datalen = 1;
      rc = DosDevIOCtl (d->x.track.hf, IOCTL_DISK, DSK_LOCKDRIVE,
                        &parm, sizeof (parm), &parmlen,
                        &data, sizeof (data), &datalen);
      rc = DosClose (d->x.track.hf);
      free (d->x.track.track_buf);
      free (d->x.track.playout);
      break;
    case DIOT_SNAPSHOT:
      rc = DosClose (d->x.snapshot.hf);
      free (d->x.snapshot.sector_map);
      free (d->x.snapshot.hash_next);
      break;
    case DIOT_CRC:
      if (fclose (d->x.crc.f) != 0)
        error ("fclose(): %s", strerror (errno));
      rc = 0;
      break;
    default:
      abort ();
    }
  if (rc != 0)
    error ("disk_close failed, rc=%lu", rc);
  free (d);
}


/* Return the type of a DISKIO. */

unsigned diskio_type (DISKIO *d)
{
  switch (d->type)
    {
    case DIOT_DISK_DASD:
    case DIOT_DISK_TRACK:
      return DIO_DISK;
    case DIOT_SNAPSHOT:
      return DIO_SNAPSHOT;
    case DIOT_CRC:
      return DIO_CRC;
    default:
      abort ();
    }
}


/* Return the number of sectors covered by a DISKIO. */

ULONG diskio_total_sectors (DISKIO *d)
{
  return d->total_sectors;
}


/* Return the number of snapshot sectors. */

ULONG diskio_snapshot_sectors (DISKIO *d)
{
  if (d->type != DIOT_SNAPSHOT)
    abort ();
  return d->x.snapshot.sector_count;
}


/* Compare two sector numbers, for qsort(). */

static int snapshot_sort_comp (const void *x1, const void *x2)
{
  const ULONG *p1 = (const ULONG *)x1;
  const ULONG *p2 = (const ULONG *)x2;
  if (*p1 < *p2)
    return -1;
  else if (*p1 > *p2)
    return 1;
  else
    return 0;
}


/* Return a sorted array of all sector numbers of a snapshot file.  If
   DISKIO is not associated with a snapshot file, return NULL. */

ULONG *diskio_snapshot_sort (DISKIO *d)
{
  ULONG *p;
  size_t n;

  if (diskio_type (d) != DIO_SNAPSHOT)
    return NULL;
  n = (size_t)d->x.snapshot.sector_count;
  p = xmalloc (n * sizeof (ULONG));
  memcpy (p, d->x.snapshot.sector_map, n * sizeof (ULONG));
  qsort (p, n, sizeof (ULONG), snapshot_sort_comp);
  return p;
}


/* Read all CRCs of the CRC file associated with D into memory, unless
   there are too many CRCs.  This is used for speeding up
   processing. */

void diskio_crc_load (DISKIO *d)
{
  ULONG i;

  if (d->type != DIOT_CRC || d->x.crc.vec != NULL)
    abort ();
  if (d->total_sectors * sizeof (crc_t) >= 8*1024*1024)
    return;
  d->x.crc.vec = xmalloc (d->total_sectors * sizeof (crc_t));
  fseek (d->x.crc.f, 512, SEEK_SET);
  if (fread (d->x.crc.vec, sizeof (crc_t), d->total_sectors,
             d->x.crc.f) != d->total_sectors)
    error ("Cannot read CRC file");
  for (i = 0; i < d->total_sectors; ++i)
    d->x.crc.vec[i] = ULONG_FROM_FS (d->x.crc.vec[i]);
}


/* Write the sector with number SEC and data SRC to the save file. */

static void save_one_sec (const void *src, ULONG sec)
{
  ULONG i;
  BYTE raw[512];

  for (i = 0; i < save_sector_count; ++i)
    if (save_sector_map[i] == sec)
      return;
  if (save_sector_count >= save_sector_alloc)
    {
      save_sector_alloc += 1024;
      save_sector_map = realloc (save_sector_map,
                                 save_sector_alloc * sizeof (ULONG));
      if (save_sector_map == NULL)
        error ("Out of memory");
    }
  save_sector_map[save_sector_count++] = sec;
  memcpy (raw, src, 512);

  /* Scramble the signature so that there are no sectors with the
     original HPFS sector signatures.  This simplifies recovering HPFS
     file systems and undeleting files. */

  *(ULONG *)raw ^= ULONG_TO_FS (SNAPSHOT_SCRAMBLE);
  if (fwrite (raw, 512, 1, save_file) != 1)
    save_error ();
}


/* Write COUNT sectors starting at number SEC to the save file. */

void save_sec (const void *src, ULONG sec, ULONG count)
{
  const char *p;

  p = (const char *)src;
  while (count != 0)
    {
      save_one_sec (p, sec);
      p += 512; ++sec; --count;
    }
}


/* Create a save file of type TYPE.  The file name is passed in the
   global variable `save_fname'.  Complain if the file would be on the
   drive AVOID_FNAME. */

void save_create (const char *avoid_fname, enum save_type type)
{
  char drive;
  header hdr;

  if (isalpha ((unsigned char)avoid_fname[0]) && avoid_fname[1] == ':'
      && avoid_fname[2] == 0)
    {
      drive = fname_drive (save_fname);
      if (drive == 0)
        drive = cur_drive ();
      if (toupper (drive) == toupper (avoid_fname[0]))
        error ("The target file must not be on the source or target drive");
    }
  save_file = fopen (save_fname, "wb");
  if (save_file == NULL)
    save_error ();
  save_type = type;
  switch (save_type)
    {
    case SAVE_SNAPSHOT:
      save_sector_count = 0;
      save_sector_alloc = 0;
      save_sector_map = NULL;
      memset (&hdr, 0, sizeof (hdr));
      fwrite (&hdr, sizeof (hdr), 1, save_file);
      break;

    case SAVE_CRC:
      save_sector_count = 0;
      memset (&hdr, 0, sizeof (hdr));
      fwrite (&hdr, sizeof (hdr), 1, save_file);
      break;

    default:
      break;
    }
}


/* Error while writing to the save file. */

void save_error (void)
{
  error ("%s: %s", save_fname, strerror (errno));
}


/* Close the save file and update the header. */

void save_close (void)
{
  header hdr;
  ULONG i;

  switch (save_type)
    {
    case SAVE_SNAPSHOT:
      memset (&hdr, 0, sizeof (hdr));
      hdr.s.magic = ULONG_TO_FS (SNAPSHOT_MAGIC);
      hdr.s.sector_count = ULONG_TO_FS (save_sector_count);
      hdr.s.map_pos = ULONG_TO_FS (ftell (save_file));
      hdr.s.version = ULONG_TO_FS (1); /* Scrambled */
      for (i = 0; i < save_sector_count; ++i)
        save_sector_map[i] = ULONG_TO_FS (save_sector_map[i]);
      if (fwrite (save_sector_map, sizeof (ULONG), save_sector_count,
                  save_file)
          != save_sector_count || fseek (save_file, 0L, SEEK_SET) != 0)
        save_error ();
      fwrite (&hdr, sizeof (hdr), 1, save_file);
      for (i = 0; i < save_sector_count; ++i)
        save_sector_map[i] = ULONG_FROM_FS (save_sector_map[i]);
      break;

    case SAVE_CRC:
      memset (&hdr, 0, sizeof (hdr));
      hdr.c.magic = ULONG_TO_FS (CRC_MAGIC);
      hdr.c.sector_count = ULONG_TO_FS (save_sector_count);
      hdr.c.version = ULONG_TO_FS (1);
      if (fseek (save_file, 0L, SEEK_SET) != 0)
        save_error ();
      fwrite (&hdr, sizeof (hdr), 1, save_file);
      break;

    default:
      break;
    }

  if (fclose (save_file) != 0)
    save_error ();
  save_file = NULL;
}


/* Convert the logical sector number SECNO to cylinder numbe
   (0-based), head number (0-based), and sector number (1-based).
   Return TRUE iff successful. */

int diskio_cyl_head_sec (DISKIO *d, cyl_head_sec *dst, ULONG secno)
{
  if (d->type != DIOT_DISK_TRACK)
    return FALSE;

  secno += d->x.track.hidden;
  dst->sec = secno % d->x.track.spt + 1; secno /= d->x.track.spt;
  dst->head = secno % d->x.track.heads; secno /= d->x.track.heads;
  dst->cyl = secno;
  return TRUE;
}


/* Return the relative sector number of sector N in the snapshot file
   associated with D.  Return 0 if there is no such sector (relative
   sector number 0 is the header of the snapshot file). */

ULONG find_sec_in_snapshot (DISKIO *d, ULONG n)
{
  ULONG j;

  for (j = d->x.snapshot.hash_start[n % HASH_SIZE]; j != HASH_END;
       j = d->x.snapshot.hash_next[j])
    if (d->x.snapshot.sector_map[j] == n)
      return j + 1;
  return 0;
}


/* Seek to sector SEC in file HF. */

static void seek_sec_hfile (HFILE hf, int sec_io, ULONG sec)
{
  ULONG rc, act_pos, n, rest;
  LONG new_pos;

  new_pos = (sec_io ? sec : sec * 512);
  if (new_pos >= 0)
    rc = DosSetFilePtr (hf, new_pos, FILE_BEGIN, &act_pos);
  else
    {
      rc = DosSetFilePtr (hf, 0, FILE_BEGIN, &act_pos);
      rest = sec;
      while (rc == 0 && rest != 0)
        {
          n = MIN (rest, LONG_MAX / 512);
          new_pos = n * 512;
          rc = DosSetFilePtr (hf, new_pos, FILE_CURRENT, &act_pos);
          rest -= n;
        }
    }
  if (rc != 0)
    error ("Cannot seek to sector #%lu (rc=%lu)", sec, rc);
}


/* Read COUNT sectors from HF. */

static void read_sec_hfile (HFILE hf, int sec_io, void *dst,
                            ULONG sec, ULONG count)
{
  ULONG rc, n, i;

  i = (sec_io ? count : 512 * count);
  seek_sec_hfile (hf, sec_io, sec);
  rc = DosRead (hf, dst, i, &n);
  if (rc != 0)
    error ("Cannot read sector #%lu (rc=%lu)", sec, rc);
  if (n != i)
    error ("EOF reached while reading sector #%lu", sec);
}


/* Read COUNT sectors using DSK_READTRACK. */

static void read_sec_track (HFILE hf, struct diskio_track *dt, void *dst,
                            ULONG sec, ULONG count)
{
  ULONG rc, parmlen, datalen, temp;
  TRACKLAYOUT *ptl;
  char *p;

  /* Note: Reading an entire track (dt->spt sectors) slows down
     things even when caching one track, probably due to mapping done
     by the hard disk controller. */

  p = dst;
  while (count != 0)
    {
      temp = sec + dt->hidden;
      ptl = dt->playout;
      ptl->bCommand = 0x01;     /* Consecutive sectors */
      ptl->usFirstSector = temp % dt->spt; temp /= dt->spt;
      ptl->usHead = temp % dt->heads; temp /= dt->heads;
      ptl->usCylinder = temp;
      temp = dt->spt - ptl->usFirstSector;
      if (temp > count)
        temp = count;
      ptl->cSectors = temp;
      parmlen = dt->layout_size;
      datalen = ptl->cSectors * 512;
      rc = DosDevIOCtl (hf, IOCTL_DISK, DSK_READTRACK,
                        ptl, parmlen, &parmlen,
                        dt->track_buf, datalen, &datalen);
      if (rc != 0)
        error ("Cannot read sector #%lu (rc=%lu)", sec, rc);
      memcpy (p, dt->track_buf, temp * 512);
      sec += temp; p += temp * 512; count -= temp;
    }
}


/* Read COUNT sectors from D to DST.  SEC is the starting sector
   number.  Copy the sector to the save file if SAVE is non-zero. */

void read_sec (DISKIO *d, void *dst, ULONG sec, ULONG count, int save)
{
  ULONG i, j, n;
  char *p;

  switch (d->type)
    {
    case DIOT_DISK_DASD:
      read_sec_hfile (d->x.dasd.hf, d->x.dasd.sec_mode, dst, sec, count);
      break;
    case DIOT_DISK_TRACK:
      read_sec_track (d->x.track.hf, &d->x.track, dst, sec, count);
      break;
    case DIOT_SNAPSHOT:
      p = (char *)dst; n = sec;
      for (i = 0; i < count; ++i)
        {
          j = find_sec_in_snapshot (d, n);
          if (j == 0)
            error ("Sector #%lu not found in snapshot file", n);
          read_sec_hfile (d->x.snapshot.hf, FALSE, p, j, 1);
          if (d->x.snapshot.version >= 1)
            *(ULONG *)p ^= ULONG_TO_FS (SNAPSHOT_SCRAMBLE);
          p += 512; ++n;
        }
      break;
    default:
      abort ();
    }
  if (a_save && save)
    save_sec (dst, sec, count);
}


/* Store the CRC of sector SECNO to the object pointed to by PCRC. */

int crc_sec (DISKIO *d, crc_t *pcrc, ULONG secno)
{
  if (d->type == DIOT_CRC)
    {
      if (secno >= d->total_sectors)
        return FALSE;
      if (d->x.crc.vec != NULL)
        {
          *pcrc = d->x.crc.vec[secno];
          return TRUE;
        }
      fseek (d->x.crc.f, 512 + secno * sizeof (crc_t), SEEK_SET);
      if (fread (pcrc, sizeof (crc_t), 1, d->x.crc.f) != 1)
        error ("CRC file: %s", strerror (errno));
      *pcrc = ULONG_FROM_FS (*pcrc);
      return TRUE;
    }
  else
    {
      BYTE data[512];

      read_sec (d, data, secno, 1, FALSE);
      *pcrc = crc_compute (data, 512);
      return TRUE;
    }
}


/* Write sector SEC to HF. */

static int write_sec_hfile (HFILE hf, int sec_io, const void *src, ULONG sec)
{
  ULONG rc, n, i;

  i = (sec_io ? 1 : 512);
  seek_sec_hfile (hf, sec_io, sec);
  rc = DosWrite (hf, src, i, &n);
  if (rc != 0)
    {
      warning (1, "Cannot write sector #%lu (rc=%lu)", sec, rc);
      return FALSE;
    }
  if (n != i)
    {
      warning (1, "Incomplete write for sector #%lu", sec);
      return FALSE;
    }
  return TRUE;
}


/* Write COUNT sectors to HF, using DSK_WRITETRACK.  DT points to a
   structure containing the disk geometry. */

static int write_sec_track (HFILE hf, struct diskio_track *dt,
                            const void *src, ULONG sec, ULONG count)
{
  ULONG rc, parmlen, datalen, temp;
  TRACKLAYOUT *ptl;
  const char *p;

  p = src;
  while (count != 0)
    {
      temp = sec + dt->hidden;
      ptl = dt->playout;
      ptl->bCommand = 0x01;     /* Consecutive sectors */
      ptl->usFirstSector = temp % dt->spt; temp /= dt->spt;
      ptl->usHead = temp % dt->heads; temp /= dt->heads;
      ptl->usCylinder = temp;
      temp = dt->spt - ptl->usFirstSector;
      if (temp > count)
        temp = count;
      ptl->cSectors = temp;
      parmlen = dt->layout_size;
      datalen = ptl->cSectors * 512;
      memcpy (dt->track_buf, p, temp * 512);
      rc = DosDevIOCtl (hf, IOCTL_DISK, DSK_WRITETRACK,
                        ptl, parmlen, &parmlen,
                        dt->track_buf, datalen, &datalen);
      if (rc != 0)
        {
          warning (1, "Cannot write sector #%lu (rc=%lu)", sec, rc);
          return FALSE;
        }
      sec += temp; p += temp * 512; count -= temp;
    }
  return TRUE;
}


/* Replace the sector SEC in the snapshot file associated with D.
   Return FALSE on failure. */

static int write_sec_snapshot (DISKIO *d, const void *src, ULONG sec)
{
  BYTE raw[512];
  ULONG j;

  j = find_sec_in_snapshot (d, sec);
  if (j == 0)
    {
      warning (1, "Sector #%lu not found in snapshot file", sec);
      return FALSE;
    }

  memcpy (raw, src, 512);
  /* Scramble the signature so that there are no sectors with the
     original HPFS sector signatures.  This simplifies recovering HPFS
     file systems and undeleting files. */
  if (d->x.snapshot.version >= 1)
    *(ULONG *)raw ^= ULONG_TO_FS (SNAPSHOT_SCRAMBLE);

  return write_sec_hfile (d->x.snapshot.hf, FALSE, raw, j);
}


/* Write sector SRC to D. */

int write_sec (DISKIO *d, const void *src, ULONG sec)
{
  switch (d->type)
    {
    case DIOT_DISK_DASD:
      return write_sec_hfile (d->x.dasd.hf, d->x.dasd.sec_mode, src, sec);
    case DIOT_DISK_TRACK:
      return write_sec_track (d->x.track.hf, &d->x.track, src, sec, 1);
    case DIOT_SNAPSHOT:
      return write_sec_snapshot (d, src, sec);
    default:
      abort ();
    }
  return FALSE;
}
