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

#define BLOCK_SUPER 1
#define BLOCK_INODE 2
#define BLOCK_EXTENT 3
#define BLOCK_FREE 4

#define DEFAULT_TABLE_SIZE 32
#define BLOCK_HEADER_SIZE 4
#define MAX_FILENAME_SIZE 8
#define INODE_HEADER_SIZE (BLOCK_HEADER_SIZE + MAX_FILENAME_SIZE + sizeof(int))

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
	Block inode;
	Block buf;
	int ptr, size;
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
	int err = closeDisk(mnt);
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
		if (fp->inode.bNum < 0) {
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
#ifdef DEBUG_FLAG
	printf("next free block: %d\n", next);
#endif
	if (next < superBlock.data[4]) {
		return next;
	}
	return -1;
}

int findFile(char* name, File* file) {
	int n = superBlock.data[4];
	for (int i = 1; i < n; i++) {
		if (bitset_is_set(superBlock.data+5, i)) {
			// Block is free
			continue;
		}
		int err = readBlock(mnt, i, file->inode.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		if (file->inode.data[0] == BLOCK_INODE && memcmp(file->inode.data+4, name, 8) == 0) {
			return i;
		}
	}
	return -1;
}

fileDescriptor tfs_openFile(char* name) {
	int nameSize = strlen(name);
	if (nameSize > MAX_FILENAME_SIZE) {
		return ERR_NAMETOOLONG;
	}
	int err, idx;
	File file = {
		{0, {BLOCK_INODE, 0x44}},
		{0, {0}},
		0,
		0,
	};
	char nameBuf[MAX_FILENAME_SIZE] = {0};
	memcpy(nameBuf, name, nameSize);
	int bNum = findFile(nameBuf, &file);
	if (bNum < 0) {
		idx = BLOCK_HEADER_SIZE;
		memcpy(file.inode.data+idx, nameBuf, MAX_FILENAME_SIZE);
		idx += MAX_FILENAME_SIZE;
		memset(file.inode.data+idx, 0, BLOCKSIZE-idx);
		if ((bNum = nextFreeBlock()) <= 0) {
			return ERR_NOMEMORY;
		}
		file.inode.bNum = bNum;
		err = writeBlock(mnt, bNum, file.inode.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		bitset_clear(superBlock.data+5, bNum);
	} else {
		idx = BLOCK_HEADER_SIZE + MAX_FILENAME_SIZE;
		file.size = ((uint32_t) file.inode.data[idx])       |
				    ((uint32_t) file.inode.data[idx+1])<<8  |
				    ((uint32_t) file.inode.data[idx+2])<<16 |
				    ((uint32_t) file.inode.data[idx+3])<<24;
	}
	file.inode.bNum = bNum;
	memcpy(&file.buf, &file.inode, sizeof(Block));
	fileDescriptor fd = nextFreeFD();
	if (fd < 0) {
#ifdef DEBUG_FLAG
		printf("appending\n");
#endif
		fd = fileTable.len;
		fileTable = slice_append(fileTable, &file);
	} else {
		memcpy(((File*) fileTable.ptr) + fd, &file, sizeof(File));
	}
#ifdef DEBUG_FLAG
	printf("%s opened with fd %d\n", name, fd);
#endif
	return fd;
}

int tfs_closeFile(fileDescriptor fd) {
	if (fd >= fileTable.len || ((File*) fileTable.ptr)[fd].inode.bNum < 0) {
		return ERR_BADF;
	}
	((File*) fileTable.ptr)[fd].inode.bNum = -1;
	if (nextFD < 0) {
		nextFD = fd;
	}
	return 0;
}

int tfs_writeFile(fileDescriptor fd, char* buffer, int size) {
	if (fd >= fileTable.len || ((File*) fileTable.ptr)[fd].inode.bNum < 0) {
		return ERR_BADF;
	}
	File* fp = ((File*) fileTable.ptr) + fd;
	int bNum = fp->inode.bNum;
	if (bNum <= 0) {
		return ERR_BADF;
	}
	int off = BLOCK_HEADER_SIZE + MAX_FILENAME_SIZE;
	fp->inode.data[off] = size;
	fp->inode.data[off+1] = size >> 8;
	fp->inode.data[off+2] = size >> 16;
	fp->inode.data[off+3] = size >> 24;
	off = INODE_HEADER_SIZE;
	int nBytes = BLOCKSIZE - off;
	int n = size < nBytes ? size : nBytes;
	memcpy(fp->inode.data+off, buffer, n * sizeof(char));
	int err = writeBlock(mnt, bNum, fp->inode.data);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	buffer += n;
	off = BLOCK_HEADER_SIZE;
	nBytes = BLOCKSIZE - off;
	bNum = fp->inode.data[2];
	size -= nBytes;
	while (size > 0) {
		if (bNum > 0) {
			err = readBlock(mnt, bNum, fp->buf.data);
			if (IS_TFS_ERROR(err)) {
				return err;
			}
		} else {
			bNum = nextFreeBlock();
		}
		n = size < nBytes ? size : nBytes;
		memcpy(fp->buf.data+off, buffer, n * sizeof(char));
		err = writeBlock(mnt, bNum, fp->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		bitset_clear(superBlock.data+5, bNum);
		bNum = fp->buf.data[2];
		size -= nBytes;
		buffer += n;
	}
	fp->size = size;
	fp->ptr = 0;
	memcpy(&fp->buf, &fp->inode, sizeof(Block));
	return 0;
}

int tfs_deleteFile(fileDescriptor fd) {
	if (fd >= fileTable.len) {
		return ERR_BADF;
	}
	File* fp = ((File*) fileTable.ptr) + fd;
	if (fp->inode.bNum <= 0) {
		return ERR_BADF;
	}
	int err, bNum = fp->inode.bNum;
	while (bNum > 0) {
		err = readBlock(mnt, bNum, fp->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		fp->buf.data[0] = BLOCK_FREE;
		err = writeBlock(mnt, bNum, fp->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		if (nextBlock <= 0) {
			nextBlock = bNum;
		}
		bitset_set(superBlock.data+5, bNum);
		bNum = fp->buf.data[2];
	}

	return 0;
}

int tfs_readByte(fileDescriptor fd, char* buffer) {
	if (fd >= fileTable.len) {
		return ERR_BADF;
	}
	File* fp = ((File*) fileTable.ptr) + fd;
	if (fp->inode.bNum <= 0) {
		return ERR_BADF;
	}
	if (fp->ptr >= fp->size) {
		return ERR_FAULT;
	}
	int off = fp->buf.bNum == fp->inode.bNum ? INODE_HEADER_SIZE : BLOCK_HEADER_SIZE;
	int err, idx = (fp->ptr + off) % BLOCKSIZE;
	if (idx < off) {
		// Read next block of file
		int next = fp->buf.data[2];
		if (next <= 0) {
			return ERR_FAULT;
		}
		err = readBlock(mnt, next, fp->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		fp->buf.bNum = next;
	}
	*buffer = fp->buf.data[idx];
	++fp->ptr;
	return 0;
}

int tfs_seek(fileDescriptor fd, int offset) {
	if (fd >= fileTable.len) {
		return ERR_BADF;
	}
	File* fp = ((File*) fileTable.ptr) + fd;
	if (fp->inode.bNum <= 0) {
		return ERR_BADF;
	}
	if (offset == fp->ptr) {
		return 0;
	}
	return 0;
}
