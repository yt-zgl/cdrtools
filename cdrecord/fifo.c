/* @(#)fifo.c	1.10 98/02/22 Copyright 1989,1997 J. Schilling */
#ifndef lint
static	char sccsid[] =
	"@(#)fifo.c	1.10 98/02/22 Copyright 1989,1997 J. Schilling";
#endif
/*
 *	A "fifo" that uses shared memory between two processes
 *
 *	The actual code is a mixture of borrowed code from star's fifo.c
 *	and a proposal from Finn Arne Gangstad <finnag@guardian.no>
 *	who had the idea to use a ring buffer to handle average size chunks.
 *
 *	Copyright (c) 1989,1997 J. Schilling
 */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define	DEBUG
#include <mconfig.h>
#if	!defined(HAVE_SMMAP) && !defined(HAVE_USGSHM)
#undef	FIFO			/* We cannot have a FIFO on this platform */
#endif
#ifdef	FIFO
#if !defined(USE_MMAP) && !defined(USE_USGSHM)
#define	USE_MMAP
#endif
#ifndef	HAVE_SMMAP
#	undef	USE_MMAP
#	define	USE_USGSHM	/* SYSV shared memory is the default	*/
#endif
#ifdef	USE_MMAP		/* Only want to have one implementation */
#	undef	USE_USGSHM	/* mmap() is preferred			*/
#endif

#include <fctldefs.h>
#include <sys/types.h>
#if defined(HAVE_SMMAP) && defined(USE_MMAP)
#include <sys/mman.h>
#endif
#include <stdio.h>
#include <stdxlib.h>
#include <unixstd.h>
#include <standard.h>
#include <errno.h>
#include <signal.h>

#include "cdrecord.h"

#ifdef DEBUG
#define	EDEBUG(a)	if (debug) error a
#else
#define	EDEBUG(a)
#endif

#define palign(x, a)	(((char *)(x)) + ((a) - 1 - (((unsigned)((x)-1))%(a))))

typedef enum faio_owner {
	owner_none,
	owner_reader,
	owner_writer,
	owner_faio
} fowner_t;

char	*onames[] = {
	"none",
	"reader",
	"writer",
	"faio"
};

typedef struct faio {
	int	len;
	volatile fowner_t owner;
	short	fd;
	short	saved_errno;
	char	*bufp;
} faio_t;

struct faio_stats {
	long	puts;
	long	gets;
	long	empty;
	long	full;
	long	done;
	long	cont_low;
} *sp;

#define	MIN_BUFFERS	3

#define	MSECS	1000
#define	SECS	(1000*MSECS)

/* microsecond delay between each buffer-ready probe by writing process */
#define	WRITER_DELAY	(20*MSECS)
#define	WRITER_MAXWAIT	(120*SECS)	/* 120 seconds max wait for data */

/* microsecond delay between each buffer-ready probe by reading process */
#define	READER_DELAY	(80*MSECS)
#define	READER_MAXWAIT	(120*SECS)	/* 120 seconds max wait for reader */

LOCAL	char	*buf;
LOCAL	char	*bufbase;
LOCAL	char	*bufend;
LOCAL	long	buflen;

extern	int	debug;
extern	int	lverbose;

EXPORT	void	init_fifo	__PR((long));
#ifdef	USE_MMAP
LOCAL	char*	mkshare		__PR((int size));
#endif
#ifdef	USE_USGSHM
LOCAL	char*	mkshm		__PR((int size));
#endif
EXPORT	BOOL	init_faio	__PR((int tracks, track_t *track, int));
EXPORT	BOOL	await_faio	__PR((void));
EXPORT	void	kill_faio	__PR((void));
LOCAL	void	faio_reader	__PR((int tracks, track_t *track));
LOCAL	void	faio_read_track	__PR((track_t *trackp));
LOCAL	void	faio_wait_on_buffer __PR((faio_t *f, fowner_t s,
					  unsigned long delay,
					  unsigned long max_wait));
LOCAL	int	faio_read_segment __PR((int fd, faio_t *f, int len));
LOCAL	faio_t	*faio_ref	__PR((int n));
EXPORT	int	faio_read_buf	__PR((int f, char *bp, int size));
EXPORT	int	faio_get_buf	__PR((int f, char **bpp, int size));
EXPORT	void	fifo_stats	__PR((void));
EXPORT	int	fifo_percent	__PR((BOOL addone));


EXPORT void
init_fifo(fs)
	long	fs;
{
	int	pagesize;

	if (fs == 0L)
		return;

#ifdef	_SC_PAGESIZE
	pagesize = sysconf(_SC_PAGESIZE);
#else
	pagesize = getpagesize();
#endif
	buflen = roundup(fs, pagesize) + pagesize;
	EDEBUG(("fs: %ld buflen: %ld\n", fs, buflen));

#if	defined(USE_MMAP)
	buf = mkshare(buflen);
#endif
#if	defined(USE_USGSHM)
	buf = mkshm(buflen);
#endif
	bufbase = buf;
	bufend = buf + buflen;
	EDEBUG(("buf: %X bufend: %X, buflen: %ld\n", buf, bufend, buflen));
	buf = palign(buf, pagesize);
	buflen -= buf - bufbase;
	EDEBUG(("buf: %X bufend: %X, buflen: %ld (align %ld)\n", buf, bufend, buflen, buf - bufbase));

	/*
	 * Dirty the whole buffer. This can die with various signals if
	 * we're trying to lock too much memory
	 */
	fillbytes(buf, buflen, '\0');
}

#ifdef	USE_MMAP
LOCAL char *
mkshare(size)
	int	size;
{
	int	f;
	char	*addr;

#ifdef	MAP_ANONYMOUS	/* HP/UX */
	f = -1;
	addr = mmap(0, size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, f, 0);
#else
	if ((f = open("/dev/zero", O_RDWR)) < 0)
		comerr("Cannot open '/dev/zero'.\n");
	addr = mmap(0, size, PROT_READ|PROT_WRITE, MAP_SHARED, f, 0);
#endif
	if (addr == (char *)-1)
		comerr("Cannot get mmap for %d Bytes on /dev/zero.\n", size);
	close(f);

	if (debug) errmsgno(EX_BAD, "shared memory segment attached: %x\n", addr);

	return (addr);
}
#endif

#ifdef	USE_USGSHM
#include <sys/ipc.h>
#include <sys/shm.h>
LOCAL char *
mkshm(size)
	int	size;
{
	int	id;
	char	*addr;
	/*
	 * Unfortunately, a declaration of shmat() is missing in old
	 * implementations such as AT&T SVr0 and SunOS.
	 * We cannot add this definition here because the return-type
	 * changed on newer systems.
	 *
	 * We will get a warning like this:
	 *
	 * warning: assignment of pointer from integer lacks a cast
	 * or
	 * warning: illegal combination of pointer and integer, op =
	 */
/*	extern	char *shmat();*/

	if ((id = shmget(IPC_PRIVATE, size, IPC_CREAT|0600)) == -1)
		comerr("shmget failed\n");

	if (debug) errmsgno(EX_BAD, "shared memory segment allocated: %d\n", id);

	if ((addr = shmat(id, (char *)0, 0600)) == (char *)-1)
		comerr("shmat failed\n");

	if (debug) errmsgno(EX_BAD, "shared memory segment attached: %x\n", addr);

	if (shmctl(id, IPC_RMID, 0) < 0)
		comerr("shmctl failed to detach shared memory segment\n");

	if (shmctl(id, SHM_LOCK, 0) < 0)
		comerr("shmctl failed to lock shared memory segment\n");

	return (addr);
}
#endif

LOCAL	int	faio_buffers;
LOCAL	int	faio_buf_size;
LOCAL	int	buf_idx;
LOCAL	pid_t	faio_pid;

/*#define	faio_ref(n)	(&((faio_t *)buf)[n])*/


EXPORT BOOL
init_faio(tracks, track, bufsize)
	int	tracks;
	track_t	*track;
	int	bufsize;
{
	int	n;
	faio_t	*f;
	int	pagesize;
	char	*base;

	if (buflen == 0L)
		return (FALSE);

#ifdef	_SC_PAGESIZE
	pagesize = sysconf(_SC_PAGESIZE);
#else
	pagesize = getpagesize();
#endif

	faio_buf_size = bufsize;
	f = (faio_t *)buf;

	/*
	 * Compute space for buffer headers.
	 */
	faio_buffers = (buflen - sizeof(*sp)) / bufsize;
	EDEBUG(("bufsize: %d buffers: %d hdrsize %d\n", bufsize, faio_buffers, faio_buffers * sizeof(struct faio)));

	/*
	 * Reduce buffer space by header space.
	 */
	n = sizeof(*sp) + faio_buffers * sizeof(struct faio);
	n = roundup(n, pagesize);
	faio_buffers = (buflen-n) / bufsize;
	EDEBUG(("bufsize: %d buffers: %d hdrsize %d\n", bufsize, faio_buffers, faio_buffers * sizeof(struct faio)));

	if (faio_buffers < MIN_BUFFERS) {
		errmsgno(EX_BAD,
			"write-buffer too small, minimum is %dk. Disabling.\n",
						MIN_BUFFERS*bufsize/1024);
		return (FALSE);
	}
	
	if (debug)
		printf("Using %d buffers of %d bytes.\n", faio_buffers, faio_buf_size);

	f = (faio_t *)buf;
	base = buf + roundup(sizeof(*sp) + faio_buffers * sizeof(struct faio),
				pagesize);

	for (n = 0; n < faio_buffers; n++, f++, base += bufsize) {
		/* Give all the buffers to the reader process */
		f->owner = owner_writer;
		f->bufp = base;
		f->fd = -1;
	}
	sp = (struct faio_stats *)f;	/* point past headers */
	sp->gets = sp->puts = sp->done = 0L;

	faio_pid = fork();
	if (faio_pid < 0)
		comerr("fork(2) failed");

	if (faio_pid == 0) {
		/* child process */
		raisepri(1);		/* almost max priority */
		faio_reader(tracks, track);
		/* NOTREACHED */
	} else {
		/* close all file-descriptors that only the child will use */
		for (n = 1; n <= tracks; n++)
			close(track[n].f);
	}

	return (TRUE);
}

EXPORT BOOL
await_faio()
{
	int	n;
	int	lastfd = -1;
	faio_t	*f;

	/*
	 * Wait until the reader is active and has filled the buffer.
	 */
	if (lverbose || debug) {
		printf("Waiting for reader process to fill input-buffer ... ");
		flush();
	}

	faio_wait_on_buffer(faio_ref(faio_buffers - 1), owner_reader,
			    500*MSECS, 0);

	if (lverbose || debug)
		printf("input-buffer ready.\n");

	sp->empty = sp->full = 0L;	/* set correct stat state */
	sp->cont_low = faio_buffers;	/* set cont to max value  */

	f = faio_ref(0);
	for (n = 0; n < faio_buffers; n++, f++) {
		if (f->fd != lastfd &&
			f->fd == STDIN_FILENO && f->len == 0) {
			errmsgno(EX_BAD, "Premature EOF on stdin.\n");
			kill(faio_pid, SIGKILL);
			return (FALSE);
		}
		lastfd = f->fd;
	}
	return (TRUE);
}

EXPORT void
kill_faio()
{
	kill(faio_pid, SIGKILL);
}

LOCAL void
faio_reader(tracks, track)
	int	tracks;
	track_t	*track;
{
	/* This function should not return, but _exit. */
	int	trackno;

	if (debug)
		printf("\nfaio_reader starting\n");

	for (trackno = 1; trackno <= tracks; trackno++) {
		if (debug)
			printf("\nfaio_reader reading track %d\n", trackno);
		faio_read_track(&track[trackno]);
	}
	sp->done++;
	if (debug)
		printf("\nfaio_reader all tracks read, exiting\n");

	/* Prevent hang if buffer is larger than all the tracks combined */
	if (sp->gets == 0)
		faio_ref(faio_buffers - 1)->owner = owner_reader;

	_exit(0);
}

#ifndef	faio_ref
LOCAL faio_t *
faio_ref(n)
	int	n;
{
	return (&((faio_t *)buf)[n]);
}
#endif


LOCAL void
faio_read_track(trackp)
	track_t *trackp;
{
	int	fd = trackp->f;
	int	bytespt = trackp->secsize * trackp->secspt;
	int	l;
	long	tracksize = trackp->tracksize;
	long	bytes_read = 0L;
	long	bytes_to_read;

	if (bytespt > faio_buf_size) {
		comerrno(EX_BAD,
		"faio_read_track fatal: secsize %d secspt %d, bytespt(%d) > %d !!\n",
			 trackp->secsize, trackp->secspt, bytespt,
			 faio_buf_size);
	}

	do {
		bytes_to_read = bytespt;
		if (tracksize > 0) {
			bytes_to_read = tracksize - bytes_read;
			if (bytes_to_read > bytespt)
				bytes_to_read = bytespt;
		}
		l = faio_read_segment(fd, faio_ref(buf_idx), bytes_to_read);
		if (++buf_idx >= faio_buffers)
			buf_idx = 0;
		if (l <= 0)
			break;
		bytes_read += l;
	} while (tracksize < 0 || bytes_read < tracksize);

	close(fd);	/* Don't keep files open longer than neccesary */
}

LOCAL void
faio_wait_on_buffer(f, s, delay, max_wait)
	faio_t	*f;
	fowner_t s;
	unsigned long delay;
	unsigned long max_wait;
{
	unsigned long max_loops;

	if (f->owner == s)
		return;		/* return immediately if the buffer is ours */

	if (s == owner_reader)
		sp->empty++;
	else
		sp->full++;

	max_loops = max_wait / delay + 1;

	while (max_wait == 0 || max_loops--) {
		usleep(delay);
		if (f->owner == s)
			return;
	}
	if (debug) {
		errmsgno(EX_BAD,
		"%lu microseconds passed waiting for %d current: %d idx: %d\n",
		max_wait, s, f->owner, (f - faio_ref(0))/sizeof(*f));
	}
	comerrno(EX_BAD, "faio_wait_on_buffer for %s timed out.\n",
	(s > owner_faio || s < owner_none) ? "bad_owner" : onames[s]);
}

LOCAL int
faio_read_segment(fd, f, len)
	int	fd;
	faio_t	*f;
	int	len;
{
	int l;

	faio_wait_on_buffer(f, owner_writer, WRITER_DELAY, WRITER_MAXWAIT);

	f->fd = fd;
	l = read_buf(fd, f->bufp, len);
	f->len = l;
	f->saved_errno = errno;
	f->owner = owner_reader;

	sp->puts++;

	return l;
}

EXPORT int
faio_read_buf(fd, bp, size)
	int fd;
	char *bp;
	int size;
{
	char *bufp;

	int len = faio_get_buf(fd, &bufp, size);
	if (len > 0) {
		movebytes(bufp, bp, len);
	}
	return len;
}

EXPORT int
faio_get_buf(fd, bpp, size)
	int fd;
	char **bpp;
	int size;
{
	faio_t	*f;
	int	len;

again:
	f = faio_ref(buf_idx);
	if (f->owner == owner_faio) {
		f->owner = owner_writer;
		if (++buf_idx >= faio_buffers)
			buf_idx = 0;
		f = faio_ref(buf_idx);
	}

	if ((sp->puts - sp->gets) < sp->cont_low && sp->done == 0) {
		EDEBUG(("gets: %d puts: %d cont: %d low: %d\n", sp->gets, sp->puts, sp->puts - sp->gets, sp->cont_low));
		sp->cont_low = sp->puts - sp->gets;
	}
	faio_wait_on_buffer(f, owner_reader, READER_DELAY, READER_MAXWAIT);
	len = f->len;
	
	if (f->fd != fd) {
		if (f->len == 0) {
			/*
			 * If the tracksize for this track was known, and
			 * the tracksize is 0 mod bytespt, this happens.
			 */
			goto again;
		}
		comerrno(EX_BAD,
		"faio_get_buf fatal: fd=%d, f->fd=%d, f->len=%d f->errno=%d\n",
		fd, f->fd, f->len, f->saved_errno);
	}
	if (size < len) {
		comerrno(EX_BAD,
		"unexpected short read-attempt in faio_get_buf. size = %d, len = %d\n",
		size, len);
	}

	if (len < 0)
		errno = f->saved_errno;

	sp->gets++;

	*bpp = f->bufp;
	f->owner = owner_faio;
	return len;
}

EXPORT void
fifo_stats()
{
	errmsgno(EX_BAD, "fifo had %ld puts and %ld gets.\n",
		sp->puts, sp->gets);
	errmsgno(EX_BAD, "fifo was %ld times empty and %ld times full, min fill was %ld%%.\n",
		sp->empty, sp->full, (100L*sp->cont_low)/faio_buffers);
}

EXPORT int
fifo_percent(addone)
	BOOL	addone;
{
	int	percent;

	if (buflen == 0L)
		return (-1);
	if (sp->done)
		return (100);
	percent = (100*(sp->puts + 1 - sp->gets)/faio_buffers);
	if (percent > 100)
		return (100);
	return (percent);
}
#else	/* FIFO */

#include <standard.h>
#include <sys/types.h>

#include "cdrecord.h"

EXPORT	void	init_fifo	__PR((long));
EXPORT	BOOL	init_faio	__PR((int tracks, track_t *track, int));
EXPORT	BOOL	await_faio	__PR((void));
EXPORT	void	kill_faio	__PR((void));
EXPORT	int	faio_read_buf	__PR((int f, char *bp, int size));
EXPORT	int	faio_get_buf	__PR((int f, char **bpp, int size));
EXPORT	void	fifo_stats	__PR((void));
EXPORT	int	fifo_percent	__PR((BOOL addone));


EXPORT void
init_fifo(fs)
	long	fs;
{
	errmsgno(EX_BAD, "Fifo not supported.\n");
}

EXPORT BOOL
init_faio(tracks, track, bufsize)
	int	tracks;
	track_t	*track;
	int	bufsize;
{
	return (FALSE);
}

EXPORT BOOL
await_faio()
{
	return (TRUE);
}

EXPORT void
kill_faio()
{
}

EXPORT int
faio_read_buf(fd, bp, size)
	int fd;
	char *bp;
	int size;
{
	return (0);
}

EXPORT int
faio_get_buf(fd, bpp, size)
	int fd;
	char **bpp;
	int size;
{
	return (0);
}

EXPORT void
fifo_stats()
{
}

EXPORT int
fifo_percent(addone)
	BOOL	addone;
{
	return (-1);
}

#endif	/* FIFO */