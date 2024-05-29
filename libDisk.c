#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef DEBUG_FLAG
	#include <stdio.h>
	#include <string.h>
#endif

#include "libDisk.h"
#include "tinyFS.h"

int tfs_error(int errnum);

int openDisk(char* filename, int nBytes) {
	int fd, flags = O_RDWR;
	if (nBytes != 0) {
		if (nBytes < BLOCKSIZE) {
			return ERR_INVALID;
		}
		flags |= O_CREAT;
	}
	if ((fd = open(filename, flags, 0666)) == -1) {
		return tfs_error(errno);
	}
	if (nBytes == 0) {
		return fd;
	}
	nBytes = (nBytes / BLOCKSIZE) * BLOCKSIZE;
	if (ftruncate(fd, nBytes) == -1) {
		int err = tfs_error(errno);
		close(fd);
		return err;
	}
#ifdef DEBUG_FLAG
	printf("Opened Disk #%d\n\t%d bytes\n", fd, nBytes);
#endif
	return fd;
}

int closeDisk(int disk) {
	if (close(disk) == -1) {
		return tfs_error(errno);
	}
#ifdef DEBUG_FLAG
	printf("Closed Disk #%d\n", disk);
#endif
	return 0;
}

int log2phys(int disk, int bNum) {
	int off = bNum * BLOCKSIZE;
	int size = lseek(disk, 0, SEEK_END);
	if (size == (off_t) -1) {
		return tfs_error(errno);
	} else if (size < off+BLOCKSIZE) {
		return ERR_INVALID;
	}
	return off;
}

int readBlock(int disk, int bNum, void* block) {
	int off = log2phys(disk, bNum);
	if (off < 0) {
		return off;
	}
	if (pread(disk, block, BLOCKSIZE, off) == -1) {
		return tfs_error(errno);
	}
#ifdef DEBUG_FLAG
	printf("Read Block #%d\n\tType: %d\n", bNum, *(char*)block);
#endif
	return 0;
}

int writeBlock(int disk, int bNum, void* block) {
	int off = log2phys(disk, bNum);
	if (off < 0) {
		return off;
	}
	if (pwrite(disk, block, BLOCKSIZE, off) == -1) {
		return tfs_error(errno);
	}
	return 0;
}

int tfs_error(int errnum) {
#ifdef DEBUG_FLAG
	printf("%s\n", strerror(errnum));
#endif
	switch (errnum) {
		case EACCES:
			return ERR_ACCESS;
		case EAGAIN:
#if EWOULDBLOCK != EAGAIN
		case EWOULDBLOCK:
#endif
			return ERR_AGAIN;
		case EBADF:
			return ERR_BADF;
		case EDQUOT:
			return ERR_DQUOTA;
		case EFAULT:
			return ERR_FAULT;
		case EINTR:
			return ERR_INTERRUPT;
		case EINVAL:
			return ERR_INVALID;
		case EIO:
			return ERR_IO;
		case EISDIR:
			return ERR_ISDIR;
		case ELOOP:
			return ERR_LOOP;
		case EMFILE:
			return ERR_MFILES;
		case ENAMETOOLONG:
			return ERR_NAMETOOLONG;
		case ENOMEM:
		case ENOSPC:
			return ERR_NOMEMORY;
		case EOVERFLOW:
			return ERR_OVERFLOW;
		case EPERM:
			return ERR_PERMIT;
		case EROFS:
			return ERR_RDONLYFS;
		case ESPIPE:
			return ERR_SEEKPIPE;
		case ETXTBSY:
			return ERR_TXTBUSY;
		default:
			return ERR_UNKNOWN;
	}
}
