#
# Copyright (c) 1995-1996 by Eberhard Mattes
#
# This file is part of fst.
#
# fst is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# fst is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with fst; see the file COPYING.  If not, write to the
# the Free Software Foundation, 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.
#

#CC=gcc -g -Wall -pedantic
CC=gcc -Zomf -Zsys -O2 -s -Wall -pedantic
#CC=icc -O

default: fst.exe

fst.exe: fst.obj do_hpfs.obj do_fat.obj diskio.obj crc.obj fst.def
	$(CC) fst.obj do_hpfs.obj do_fat.obj diskio.obj crc.obj fst.def

fst.obj: fst.c fst.h do_hpfs.h do_fat.h crc.h diskio.h fat.h
	$(CC) -c fst.c

do_hpfs.obj: do_hpfs.c fst.h do_hpfs.h crc.h diskio.h hpfs.h
	$(CC) -c do_hpfs.c

do_fat.obj: do_fat.c fst.h do_fat.h crc.h diskio.h
	$(CC) -c do_fat.c

diskio.obj: diskio.c fst.h crc.h diskio.h
	$(CC) -c diskio.c

crc.obj: crc.c crc.h
	$(CC) -c crc.c
