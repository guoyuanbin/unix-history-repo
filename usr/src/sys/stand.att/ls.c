/*
 * Copyright (c) 1982, 1986, 1988 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that this notice is preserved and that due credit is given
 * to the University of California at Berkeley. The name of the University
 * may not be used to endorse or promote products derived from this
 * software without specific prior written permission. This software
 * is provided ``as is'' without express or implied warranty.
 *
 *	@(#)ls.c	7.3 (Berkeley) %G%
 */

#include "param.h"
#include "inode.h"
#include "dir.h"
#include "fs.h"
#include "saio.h"

main()
{
	struct inode *ip;
	int fd;

	fd = getfile("ls", 0);
	ip = &iob[fd - 3].i_ino;
	if ((ip->i_mode & IFMT) != IFDIR)
		_stop("ls: not a directory");
	if (ip->i_size == 0)
		_stop("ls: zero length directory");
	ls(fd);
}

typedef struct direct	DP;
static
ls(fd)
	register int fd;
{
	register int size;
	register char *dp;
	char dirbuf[DIRBLKSIZ];

	printf("\nname->inode\n");
	while ((size = read(fd, dirbuf, DIRBLKSIZ)) == DIRBLKSIZ)
		for(dp = dirbuf; (dp < (dirbuf + size)) &&
		    (dp + ((DP *)dp)->d_reclen) < (dirbuf + size);
		    dp += ((DP *)dp)->d_reclen) {
			if (((DP *)dp)->d_ino == 0)
				continue;
			if (((DP *)dp)->d_reclen > DIRSIZ(((DP *)dp)))
				continue;
			if (((DP *)dp)->d_namlen > MAXNAMLEN+1)
				_stop("Corrupt file name length!  Run fsck soon!\n");
			printf("%s->%d\n", ((DP *)dp)->d_name,
			    ((DP *)dp)->d_ino);
		}
}
