/* hpfs.h -- HPFS definitions
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


#define SUPER_SIG1      0xf995e849
#define SPARE_SIG1      0xf9911849

#define ALSEC_SIG1      0x37e40aae
#define DIRBLK_SIG1     0x77e40aae
#define FNODE_SIG1      0xf7e40aae

#define CPINFO_SIG1     0x494521f7
#define CPDATA_SIG1     0x894521f7

#define SUPER_SIG2      0xfa53e9c5
#define SPARE_SIG2      0xfa5229c5

#define SPF_DIRT        0x01    /* File system is dirty */
#define SPF_SPARE       0x02    /* Spare DIRBLKs are used */
#define SPF_HFUSED      0x04    /* Hot fix sectors are used */
#define SPF_BADSEC      0x08    /* Bad sector */
#define SPF_BADBM       0x10    /* Bad bitmap */
#define SPF_FASTFMT     0x20    /* Fast format was used */
#define SPF_VER         0x80    /* Written by old IFS */

#define FNF_DIR         0x01    /* FNODE is for a directory */

#define ABF_NODE        0x80    /* ALNODEs are following (not ALLEAFs) */
#define ABF_FNP         0x20    /* ALBLK is in an FNODE */

#define DF_SPEC         0x01    /* Special ".." entry */
#define DF_ACL          0x02    /* ACL present*/
#define DF_BTP          0x04    /* B-tree down pointer present */
#define DF_END          0x08    /* Dummy end entry */
#define DF_ATTR         0x10    /* EA list present */
#define DF_PERM         0x20    /* Extended permissions list present */
#define DF_XACL         0x40    /* Explicit ACL present */
#define DF_NEEDEAS      0x80    /* "Need" EAs present */

typedef ULONG LSN;

#pragma pack(1)

typedef struct
{
  USHORT usCountryCode;
  USHORT usCodePageID;
  ULONG  cksCP;
  LSN    lsnCPData;
  USHORT iCPVol;
  USHORT cDBCSRange;
} CPINFOENTRY;

typedef struct
{
  UCHAR ucStart;
  UCHAR ucEnd;
} DBCSRG;

typedef struct
{
  USHORT usCountryCode;
  USHORT usCodePageID;
  USHORT cDBCSRange;
  BYTE   bCaseMapTable[128];
  DBCSRG DBCSRange[1];
} CPDATAENTRY;

typedef struct
{
  LSN lsnMain;
  LSN lsnSpare;
} RSP;

/* SPTR: A storage pointer specifying a run length (number of bytes)
   and either the starting sector of the run or the sector number of
   an ALSEC which maps the data. */

typedef struct
{
  ULONG cbRun;
  LSN   lsn;
} SPTR;

typedef struct
{
  SPTR   sp;
  USHORT usFNL;
  BYTE   bDat;
} AUXINFO;

typedef struct
{
  ULONG lsnLog;
  ULONG csecRun;
  ULONG lsnPhys;
} ALLEAF;

typedef struct
{
  ULONG lsnLog;
  ULONG lsnPhys;
} ALNODE;

typedef struct
{
  BYTE   bFlag;
  BYTE   bPad[3];
  BYTE   cFree;
  BYTE   cUsed;
  USHORT oFree;
} ALBLK;

typedef struct
{
  ALBLK alb;
  union
    {
      ALLEAF aall[8];
      ALNODE aaln[12];
    } a;
  ULONG ulVLen;
} FILESTORAGE;

typedef struct
{
  USHORT cchThisEntry;
  BYTE   bFlags;
  BYTE   bAttr;
  LSN    lsnFNode;
  ULONG  timLastMod;
  ULONG  cchFSize;
  ULONG  timLastAccess;
  ULONG  timCreate;
  ULONG  ulEALen;
  BYTE   bFlex;
  BYTE   bCodePage;
  BYTE   cchName;
  BYTE   bName[1];
} DIRENT;

typedef struct
{
  ULONG       sig;
  ULONG       ulSRHist;
  ULONG       ulFRHist;
  BYTE        achName[16];
  LSN         lsnContDir;
  AUXINFO     aiACL;
  BYTE        cHistBits;
  AUXINFO     aiEA;
  BYTE        bFlag;
  FILESTORAGE fst;
  ULONG       ulRefCount;
  BYTE        achUID[16];
  USHORT      usACLBase;
  BYTE        abSpare[10];
  BYTE        abFree[1];
} FNODE;

typedef struct
{
  ULONG  sig1;
  ULONG  sig2;
  BYTE   bVersion;
  BYTE   bFuncVersion;
  USHORT usDummy;
  LSN    lsnRootFNode;
  ULONG  culSectsOnVol;
  ULONG  culNumBadSects;
  RSP    rspBitMapIndBlk;
  RSP    rspBadBlkList;
  ULONG  datLastChkdsk;
  ULONG  datLastOptimize;
  ULONG  clsnDirBlkBand;
  LSN    lsnFirstDirBlk;
  LSN    lsnLastDirBlk;
  LSN    lsnDirBlkMap;
  BYTE   achVolumeName[32];
  LSN    lsnSidTab;
} SUPERB;

typedef struct
{
  ULONG  sig1;
  ULONG  sig2;
  BYTE   bFlag;
  BYTE   bAlign[3];
  LSN    lsnHotFix;
  ULONG  culHotFixes;
  ULONG  culMaxHotFixes;
  ULONG  cdbSpares;
  ULONG  cdbMaxSpare;
  LSN    lsnCPInfo;
  ULONG  culCP;
  ULONG  aulExtra[17];
  LSN    alsnSpareDirBlks[101];
} SPAREB;

typedef  struct
{
  ULONG       sig;
  ULONG       cCodePage;
  ULONG       iFirstCP;
  LSN         lsnNext;
  CPINFOENTRY CPInfoEnt[31];
} CPINFOSEC;

typedef struct
{
  ULONG  sig;
  USHORT cCodePage;
  USHORT iFirstCP;
  ULONG  cksCP[3];
  USHORT offCPData[3];
} CPDATASEC;

typedef struct
{
  ULONG sig;
  ULONG lsnSelf;
  ULONG lsnRent;
  ALBLK alb;
  union
  {
    ALLEAF aall[40];
    ALNODE aaln[60];
  } a;
} ALSEC;

typedef union
{
  BYTE      raw[512];
  SUPERB    superb;
  SPAREB    spareb;
  CPINFOSEC cpinfosec;
  CPDATASEC cpdatasec;
  FNODE     fnode;
  ALSEC     alsec;
} HPFS_SECTOR;

typedef union
{
  BYTE raw[4*512];
  struct
    {
      ULONG  sig;
      ULONG  offulFirstFree;
      ULONG  culChange;
      LSN    lsnParent;
      LSN    lsnThisDir;
      DIRENT dirent[1];
    } dirblk;
} DIRBLK;

#pragma pack()
