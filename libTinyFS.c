#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef DEBUG_FLAG
	#include <stdio.h>
#endif

#include "tinyFS.h"
#include "libDisk.h"
#include "slice.h"
#include "bitset.h"

#ifdef DEBUG_FLAG
	#define dbg(...) fprintf(stderr, __VA_ARGS__)
#else
	#define dbg(...)
#endif

#define BLOCK_SUPER 1
#define BLOCK_INODE 2
#define BLOCK_EXTENT 3
#define BLOCK_FREE 4

#define DEFAULT_TABLE_SIZE 32
#define BLOCK_HEADER_SIZE 4
#define MAX_FILENAME_SIZE 8
#define INODE_HEADER_SIZE (BLOCK_HEADER_SIZE + MAX_FILENAME_SIZE + sizeof(int))
#define BLOCK_DATA_SIZE (BLOCKSIZE - BLOCK_HEADER_SIZE)
#define INODE_DATA_SIZE (BLOCKSIZE - INODE_HEADER_SIZE)

#define IS_BAD_BLOCK(blk) ((blk)[0] > BLOCK_FREE && (blk)[1] != 0x44 && (blk)[2] != 0)

/* Mounted disk number */
int mnt = -1;

/* Open file table */
slice_t fileTable;
fileDescriptor nextFD = -1;

typedef struct {
	int bNum;
	uint8_t data[BLOCKSIZE];
} Block;

typedef struct {
	int inode;
	char name[MAX_FILENAME_SIZE];
	int ptr, size;
	Block buf;
} File;

Block superBlock = {0};
int nextBlock = -1;

int tfs_mkfs(char* filename, int nBytes) {
	int nBlocks = nBytes / BLOCKSIZE;
	int disk = openDisk(filename, nBytes);
	if (IS_TFS_ERROR(disk)) {
		return disk;
	}
	/* Initialize free blocks */
	uint8_t block[BLOCKSIZE] = {BLOCK_FREE, 0x44};
	int i, err;
	for (i = 1; i < nBlocks; i++) {
		err = writeBlock(disk, i, block);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
	}
	/* Initialize superblock */
	block[0] = BLOCK_SUPER;
	int n = (nBlocks + 7) >> 3; // num. bytes needed to represent each block as a bit
	block[4] = (uint8_t) n;
	n += 5;
	for (i = 5; i < n; i++) {
		block[i] = 0xff;
	}
	block[n-1] >>= (8 - (nBlocks & 7)) & 7;
	block[5] &= ~1;
	err = writeBlock(disk, 0, block);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	dbg("made fs of %d blocks\n", nBlocks);
	return 0;
}

int tfs_verify(void) {
	superBlock.bNum = 0;
	int err = readBlock(mnt, 0, superBlock.data);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	if (IS_BAD_BLOCK(superBlock.data) || superBlock.data[0] != BLOCK_SUPER) {
		return ERR_INVALID;
	}
	uint8_t block[BLOCKSIZE];
	int n = superBlock.data[4];
	for (int i = 1; i < n; i++) {
		err = readBlock(mnt, i, block);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		if (IS_BAD_BLOCK(block)) {
			return ERR_INVALID;
		}
	}
	return err == ERR_INVALID ? 0 : err;
}

int tfs_mount(char* diskname) {
	if (mnt >= 0) {
		// Another disk is already mounted
		return ERR_TXTBUSY;
	}
	int retValue;
	if ((retValue = openDisk(diskname, 0)) < 0) {
		return retValue;
	}
	mnt = retValue;
	if ((retValue = tfs_verify()) < 0) {
		return retValue;
	}
	fileTable = slice_new(DEFAULT_TABLE_SIZE, sizeof(File));

	return 0;
}

int tfs_unmount(void) {
	if (mnt < 0) {
		return ERR_BADF;
	}
	int err = writeBlock(mnt, 0, superBlock.data);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	err = closeDisk(mnt);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	mnt = -1;
	nextFD = -1;
	nextBlock = -1;
	slice_free(fileTable);
	return 0;
}

fileDescriptor nextFreeFD() {
	int next = nextFD;
	if (next >= 0) {
		nextFD = -1;
		return next;
	}
	File* fp;
	for (int i = 0; i < fileTable.len; i++) {
		fp = ((File*) fileTable.ptr) + i;
		if (fp->inode < 0) {
			return i;
		}
	}
	return -1;
}

int nextFreeBlock() {
	int next = nextBlock;
	if (next > 0) {
		nextBlock = -1;
		return next;
	}
	next = bitset_ctz(superBlock.data+5, superBlock.data[4]);
	dbg("next free block: %d\n", next);
	if (next < superBlock.data[4]<<3) {
		return next;
	}
	return -1;
}

int findFile(File* file) {
	int n = superBlock.data[4];
	for (int i = 1; i < n; i++) {
		if (bitset_is_set(superBlock.data+5, i)) {
			// Block is free
			dbg("block %d is free, skipping\n", i);
			continue;
		}
		int err = readBlock(mnt, i, file->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		int cmp = memcmp(file->buf.data+BLOCK_HEADER_SIZE, file->name, MAX_FILENAME_SIZE);
		dbg("'%s' vs. '%s': %d\n", file->buf.data+BLOCK_HEADER_SIZE, file->name, cmp);
		if (file->buf.data[0] == BLOCK_INODE && cmp == 0) {
			return i;
		}
	}
	dbg("file %s not found\n", file->name);
	return -1;
}

fileDescriptor tfs_openFile(char* name) {
	int nameSize = strlen(name);
	if (nameSize > MAX_FILENAME_SIZE) {
		return ERR_NAMETOOLONG;
	}
	int err, idx;
	File file = {0};
	memcpy(file.name, name, nameSize);
	int bNum = findFile(&file);
	if (bNum < 0) {
		idx = BLOCK_HEADER_SIZE;
		memcpy(file.buf.data+idx, file.name, MAX_FILENAME_SIZE);
		idx += MAX_FILENAME_SIZE;
		memset(file.buf.data+idx, 0, BLOCKSIZE-idx);
		if ((bNum = nextFreeBlock()) <= 0) {
			return ERR_NOMEMORY;
		}
		err = writeBlock(mnt, bNum, file.buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		bitset_clear(superBlock.data+5, bNum);
	} else {
		idx = BLOCK_HEADER_SIZE + MAX_FILENAME_SIZE;
		dbg("size offset: %d\n", idx);
		file.size = ((uint32_t) file.buf.data[idx])       |
				    ((uint32_t) file.buf.data[idx+1])<<8  |
				    ((uint32_t) file.buf.data[idx+2])<<16 |
				    ((uint32_t) file.buf.data[idx+3])<<24;
	}
	file.inode = bNum;
	file.buf.bNum = bNum;
	fileDescriptor fd = nextFreeFD();
	if (fd < 0) {
		dbg("appending\n");
		fd = fileTable.len;
		fileTable = slice_append(fileTable, &file);
	} else {
		memcpy(((File*) fileTable.ptr) + fd, &file, sizeof(File));
	}
	dbg("%s opened with fd %d (size %d)\n", name, fd, file.size);
	return fd;
}

int tfs_closeFile(fileDescriptor fd) {
	if (fd >= fileTable.len) {
		return ERR_BADF;
	}
	File* fp = ((File*) fileTable.ptr) + fd;
	if (fp->inode <= 0) {
		return ERR_BADF;
	}
	fp->inode = -1;
	fp->buf.bNum = -1;
	memset(fp->buf.data, 0, BLOCKSIZE);
	if (nextFD < 0) {
		nextFD = fd;
	}
	return 0;
}

/* Free all blocks of fp from bNum to the EOF */
int freeBlocks(File* fp, int bNum) {
	int err, next;
	while (bNum > 0) {
		err = readBlock(mnt, bNum, fp->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		next = fp->buf.data[2];
		fp->buf.data[0] = BLOCK_FREE;
		fp->buf.data[2] = 0;
		err = writeBlock(mnt, bNum, fp->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		if (nextBlock <= 0) {
			nextBlock = bNum;
		}
		bitset_set(superBlock.data+5, bNum);
		bNum = next;
	}
	return 0;
}

int tfs_writeFile(fileDescriptor fd, char* buffer, int size) {
	if (fd >= fileTable.len) {
		return ERR_BADF;
	}
	File* fp = ((File*) fileTable.ptr) + fd;
	int bNum = fp->inode;
	if (bNum <= 0) {
		return ERR_BADF;
	}
	int next;
	fp->size = size;
	int err, off = BLOCK_HEADER_SIZE;
	int n, nBytes = BLOCK_DATA_SIZE;
	while (size > 0 && bNum > 0) {
		err = readBlock(mnt, bNum, fp->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		dbg("read block %d\n", bNum);
		if (bNum == fp->inode) {
			dbg("writing inode (size %d)\n", size);
			off = BLOCK_HEADER_SIZE + MAX_FILENAME_SIZE;
			fp->buf.data[0] = BLOCK_INODE;
			dbg("size offset: %d\n", off);
			fp->buf.data[off++] = size;
			fp->buf.data[off++] = size>>8;
			fp->buf.data[off++] = size>>16;
			fp->buf.data[off++] = size>>24;
			n = (size < (BLOCKSIZE-INODE_HEADER_SIZE)) ? size : (BLOCKSIZE-INODE_HEADER_SIZE);
			memcpy(fp->buf.data+off, buffer, n * sizeof(char));
			off = BLOCK_HEADER_SIZE;
		} else {
			fp->buf.data[0] = BLOCK_EXTENT;
			n = size < nBytes ? size : nBytes;
			memcpy(fp->buf.data+off, buffer, n * sizeof(char));
		}
		next = fp->buf.data[2];
		if (size <= n) {
			dbg("final block of file\n");
			fp->buf.data[2] = 0;
		} else if (next <= 0) {
			dbg("need free block\n");
			if ((next = nextFreeBlock()) <= 0) {
				dbg("error getting next free block\n");
				return ERR_NOMEMORY;
			}
			fp->buf.data[2] = next;
		}
		dbg("next block: %d\n", next);
		err = writeBlock(mnt, bNum, fp->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		dbg("wrote block %d\n", bNum);
		bitset_clear(superBlock.data+5, bNum);
		if (next > 0) {
			bitset_clear(superBlock.data+5, next);
		}
		bNum = next;
		size -= n;
		buffer += n;
	}
	if (size > 0) {
		return ERR_IO;
	}
	err = freeBlocks(fp, bNum);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	fp->ptr = 0;
	return 0;
}

int tfs_deleteFile(fileDescriptor fd) {
	if (fd >= fileTable.len) {
		return ERR_BADF;
	}
	File* fp = ((File*) fileTable.ptr) + fd;
	if (fp->inode <= 0) {
		return ERR_BADF;
	}
	int err = freeBlocks(fp, fp->inode);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	return tfs_closeFile(fd);
}

int tfs_readByte(fileDescriptor fd, char* buffer) {
	dbg("reading byte\n");
	if (fd >= fileTable.len) {
		dbg("bad fd %d\n", fd);
		return ERR_BADF;
	}
	File* fp = ((File*) fileTable.ptr) + fd;
	if (fp->inode <= 0) {
		dbg("bad inode %d\n", fp->inode);
		return ERR_BADF;
	}
	dbg("reading byte %d\n", fp->ptr);
	if (fp->ptr >= fp->size) {
		dbg("bad ptr %d (size %d)\n", fp->ptr, fp->size);
		return ERR_FAULT;
	}
	int err;
	if (fp->ptr == 0 && fp->buf.bNum != fp->inode) {
		err = readBlock(mnt, fp->inode, fp->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		fp->buf.bNum = fp->inode;
	}
	int off, idx = fp->ptr;
	if (idx < INODE_DATA_SIZE) {
		off = INODE_HEADER_SIZE;
	} else {
		idx = (fp->ptr - INODE_DATA_SIZE) % BLOCK_DATA_SIZE;
		off = BLOCK_HEADER_SIZE;
	}
	idx += off;
	dbg("off: %d, idx: %d\n", off, idx);
	*buffer = fp->buf.data[idx];
	if (idx >= BLOCKSIZE - 1) {
		// Read next block of file
		int next = fp->buf.data[2];
		dbg("Read next block %d\n", next);
		if (next <= 0) {
			return ERR_FAULT;
		}
		err = readBlock(mnt, next, fp->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		fp->buf.bNum = next;
	}
	++fp->ptr;
	return 0;
}

int tfs_seek(fileDescriptor fd, int offset) {
	if (fd >= fileTable.len) {
		return ERR_BADF;
	}
	File* fp = ((File*) fileTable.ptr) + fd;
	if (fp->inode <= 0) {
		return ERR_BADF;
	}
	if (offset == fp->ptr) {
		return 0;
	} else if (offset >= fp->size) {
		return ERR_OVERFLOW;
	}
	return 0;
}
