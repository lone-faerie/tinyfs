#include <errno.h>
#include <limits.h>
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

#define FLAG_ISDIR 1
#define FLAG_WRITE 2
#define FLAG_READ 4
#define FLAGS_RDWR (FLAG_READ | FLAG_WRITE)
#define FLAGS_DIR (FLAG_ISDIR | FLAGS_RDWR)

#define SUPER_ADDRESS 0
#define ROOT_ADDRESS 1
#define START_ADDRESS (ROOT_ADDRESS + 1)

#define DEFAULT_TABLE_SIZE 32
#define BLOCK_HEADER_SIZE 4
#define MAX_FILENAME_SIZE 8
#define INODE_HEADER_SIZE (BLOCK_HEADER_SIZE + 1 + MAX_FILENAME_SIZE + sizeof(int) + 1)
#define BLOCK_DATA_SIZE (BLOCKSIZE - BLOCK_HEADER_SIZE)
#define INODE_DATA_SIZE (BLOCKSIZE - INODE_HEADER_SIZE)
#define MAX_DISK_SIZE (BLOCKSIZE * (UCHAR_MAX+1))

#define IS_BAD_BLOCK(blk) ((blk)[0] > BLOCK_FREE || (blk)[1] != 0x44 || (blk)[3] != 0)

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
	int inode, dir;
	char name[MAX_FILENAME_SIZE];
	uint8_t flags;
	int ptr, size;
	Block buf;
} File;

Block superBlock = {0};
int nextBlock = -1;

File rootDir = {0};
int nextRoot = -1;

int _tfs_seek(File* fp, int offset);

int _readBlock(int bNum, Block* block) {
	if (mnt < 0) {
		return ERR_BADF;
	}
	int err = readBlock(mnt, bNum, block->data);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	block->bNum = bNum;
	return 0;
}

int _writeBlock(int bNum, Block* block) {
	if (mnt < 0) {
		return ERR_BADF;
	}
	int err = writeBlock(mnt, bNum, block->data);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	return 0;
}


int tfs_mkfs(char* filename, int nBytes) {
	int nBlocks = nBytes / BLOCKSIZE;
	int disk = openDisk(filename, nBytes);
	if (IS_TFS_ERROR(disk)) {
		return disk;
	}
	/* Initialize free blocks */
	uint8_t block[BLOCKSIZE] = {BLOCK_FREE, 0x44};
	int i, err;
	for (i = START_ADDRESS; i < nBlocks; i++) {
		err = writeBlock(disk, i, block);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
	}
	/* Initialize root directory */
	block[0] = BLOCK_INODE;
	block[INODE_HEADER_SIZE-1] = FLAGS_DIR;
	err = writeBlock(disk, ROOT_ADDRESS, block);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	dbg("wrote root [%d, %d, %d, %d]\n", block[0], block[1], block[2], block[3]);
	block[INODE_HEADER_SIZE-1] = 0;
	/* Initialize superblock */
	block[0] = BLOCK_SUPER;
	block[2] = ROOT_ADDRESS;
	int n = (nBlocks + 7) >> 3; // num. bytes needed to represent each block as a bit
	dbg("bitset of %d bytes\n", n);
	block[4] = (uint8_t) nBlocks;
	n += 5;
	for (i = 5; i < n; i++) {
		block[i] = 0xff;
	}
	block[n-1] >>= (8 - (nBlocks & 7)) & 7;
	block[5] &= 0xff << START_ADDRESS;
	err = writeBlock(disk, SUPER_ADDRESS, block);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	err = closeDisk(disk);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	dbg("made fs of %d blocks\n", nBlocks);
	return 0;
}

int tfs_verify(void) {
	uint8_t block[BLOCKSIZE];
	int err, n = superBlock.data[4];
	for (int i = START_ADDRESS; i < n; i++) {
		err = readBlock(mnt, i, block);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		if (IS_BAD_BLOCK(block)) {
			dbg("bad block %d [%d, %d, %d, %d]\n", i, block[0], block[1], block[2], block[3]);
			return ERR_INVALID;
		}
	}
	return err;
}

int tfs_mount(char* diskname) {
	if (mnt >= 0) {
		// Another disk is already mounted
		return ERR_TXTBUSY;
	}
	int retValue = openDisk(diskname, 0);
	if (IS_TFS_ERROR(retValue)) {
		dbg("could not open disk\n");
		return retValue;
	}
	mnt = retValue;
	retValue = readBlock(mnt, SUPER_ADDRESS, superBlock.data);
	if (IS_TFS_ERROR(retValue)) {
		dbg("error reading superblock\n");
		return retValue;
	}
	if (IS_BAD_BLOCK(superBlock.data) || superBlock.data[0] != BLOCK_SUPER || superBlock.data[2] != 1) {
		dbg("bad superblock\n");
		return ERR_INVALID;
	}
	superBlock.bNum = SUPER_ADDRESS;
	retValue = tfs_verify();
	if (IS_TFS_ERROR(retValue)) {
		dbg("invalid FS\n");
		return retValue;
	}
	retValue = readBlock(mnt, ROOT_ADDRESS, rootDir.buf.data);
	if (IS_TFS_ERROR(retValue)) {
		dbg("error reading root\n");
		return retValue;
	}
	rootDir.buf.bNum = ROOT_ADDRESS;
	rootDir.inode = ROOT_ADDRESS;
	int off = BLOCK_HEADER_SIZE;
	rootDir.dir = rootDir.buf.data[off];
	off += 1;
	memcpy(rootDir.name, rootDir.buf.data+off, MAX_FILENAME_SIZE);
	off += MAX_FILENAME_SIZE;
	rootDir.size = ((uint32_t) rootDir.buf.data[off])       |
			       ((uint32_t) rootDir.buf.data[off+1])<<8  |
			       ((uint32_t) rootDir.buf.data[off+2])<<16 |
			       ((uint32_t) rootDir.buf.data[off+3])<<24;
	rootDir.flags = rootDir.buf.data[off+4];
	rootDir.ptr = 0;
	fileTable = slice_new(DEFAULT_TABLE_SIZE, sizeof(File));
	dbg("%d free blocks\n", bitset_popcnt(superBlock.data+5, superBlock.data[4]));
	return 0;
}

int tfs_unmount(void) {
	if (mnt < 0) {
		return ERR_BADF;
	}
	int err = writeBlock(mnt, SUPER_ADDRESS, superBlock.data);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	err = writeBlock(mnt, ROOT_ADDRESS, rootDir.buf.data);
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

/* Number of blocks into a file where ptr resides. (i.e. ptr < 240 = 0, ptr < 492 = 1, etc.) */
static inline int blockNum(int ptr) {
	return ((ptr - INODE_DATA_SIZE) / BLOCK_DATA_SIZE) + (ptr >= INODE_DATA_SIZE);
}

int ptrIndex(int ptr, int* off) {
	if (ptr < INODE_DATA_SIZE) {
		if (off) *off = INODE_HEADER_SIZE;
		return ptr + INODE_HEADER_SIZE;
	}
	if (off) *off = BLOCK_HEADER_SIZE;
	ptr = (ptr - INODE_DATA_SIZE) % BLOCK_DATA_SIZE;
	return ptr + BLOCK_HEADER_SIZE;
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

int nextFile(File* dir, char** name) {
	dbg("next file in /\n");
	int off;
	int idx = ptrIndex(dir->ptr, &off);
	int nBytes = BLOCKSIZE - idx;
	if (nBytes < MAX_FILENAME_SIZE+1) {
		int bNum = dir->buf.data[2];
		if (bNum <= 0) {
			return ERR_EOF;
		}
		int err = readBlock(mnt, bNum, dir->buf.data);
		if (IS_TFS_ERROR(err)) {
			dbg("error reading block\n");
			return err;
		}
		dir->buf.bNum = bNum;
		dir->ptr += nBytes;
		idx = BLOCK_HEADER_SIZE;
	}
	int addr = dir->buf.data[idx + MAX_FILENAME_SIZE];
	*name = (char*) (dir->buf.data + idx);
	dir->ptr += MAX_FILENAME_SIZE + 1;
	return addr;
}

int getFile(fileDescriptor fd, File** fp) {
	if (mnt < 0) {
		return ERR_IO;
	} else if (fd >= fileTable.len) {
		return ERR_BADF;
	}
	*fp = ((File*) fileTable.ptr) + fd;
	if ((*fp)->inode <= 0) {
		return ERR_BADF;
	}
	return 0;
}

int findFile(File* file) {
	dbg("finding file\n");
	int err;
	if (rootDir.buf.bNum != rootDir.inode) {
		dbg("reading root dir\n");
		err = readBlock(mnt, rootDir.inode, rootDir.buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		rootDir.buf.bNum = rootDir.inode;
	}
	rootDir.ptr = 0;
	char* name;
	int bNum, firstFree = -1;
	while ((bNum = nextFile(&rootDir, &name)) >= 0) {
		if (bNum == 0 && firstFree != -1) {
			firstFree = rootDir.ptr - (MAX_FILENAME_SIZE + 1);
		} else if (strncmp(file->name, name, MAX_FILENAME_SIZE) == 0) {
			break;
		}
//		rootDir.ptr += MAX_FILENAME_SIZE + 1;
	}
	if (IS_TFS_ERROR(bNum)) {
		dbg("file not found!\n");
		return bNum;
	}
	dbg("file found!\n");
	err = readBlock(mnt, bNum, file->buf.data);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	file->buf.bNum = bNum;
	file->inode = bNum;
	file->ptr = 0;
	int idx = BLOCK_HEADER_SIZE + MAX_FILENAME_SIZE;
	file->dir = file->buf.data[idx];
	file->size = ((uint32_t) file->buf.data[idx+1])       |
				 ((uint32_t) file->buf.data[idx+2])<<8  |
				 ((uint32_t) file->buf.data[idx+3])<<16 |
				 ((uint32_t) file->buf.data[idx+4])<<24;
	file->flags = file->buf.data[idx+5];
	return bNum;
}

int findFileInDir(char* name, File* file, File* dir) {
	dbg("finding file\n");
	int err;
	if (dir->buf.bNum != dir->inode) {
		dbg("reading root dir\n");
		err = readBlock(mnt, dir->inode, dir->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		dir->buf.bNum = dir->inode;
	}
	dir->ptr = 0;
	char* namep;
	int bNum, firstFree = -1;
	while ((bNum = nextFile(dir, &namep)) >= 0) {
		if (bNum == 0 && firstFree != -1) {
			firstFree = dir->ptr;
		} else if (strncmp(name, namep, MAX_FILENAME_SIZE) == 0) {
			break;
		}
//		dir->ptr += MAX_FILENAME_SIZE + 1;
	}
	if (bNum == ERR_EOF && firstFree != -1) {
		dbg("file not found!\n");
		return _tfs_seek(dir, firstFree);
	} else if (IS_TFS_ERROR(bNum)) {
		return bNum;
	} else {
		dbg("file found!\n");
		err = readBlock(mnt, bNum, file->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
	}
	file->buf.bNum = bNum;
	file->inode = bNum;
	file->ptr = 0;
	int idx = BLOCK_HEADER_SIZE + MAX_FILENAME_SIZE;
	file->dir = file->buf.data[idx];
	file->size = ((uint32_t) file->buf.data[idx+1])       |
				 ((uint32_t) file->buf.data[idx+2])<<8  |
				 ((uint32_t) file->buf.data[idx+3])<<16 |
				 ((uint32_t) file->buf.data[idx+4])<<24;
	file->flags = file->buf.data[idx+5];
	return bNum;
}

int _findFile(File* file) {
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

int findOrMakeFile(char* name, File* dir) {
	int err;
	if (dir->buf.bNum != dir->inode) {
		err = readBlock(mnt, dir->inode, dir->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		dir->buf.bNum = dir->inode;
	}
	dir->ptr = 0;
	int bNum, firstFree = -1;
	char* entry;
	while ((bNum = nextFile(dir, &entry)) >= 0) {
		if (bNum == 0 && firstFree == -1) {
			firstFree = dir->ptr - (MAX_FILENAME_SIZE + 1);
		} else if (strncmp(name, entry, MAX_FILENAME_SIZE) == 0) {
			return bNum;
		}
	}
	if (IS_TFS_ERROR(bNum) && bNum != ERR_EOF) {
		return bNum;
	} else if (firstFree == -1) {
		return ERR_NOMEMORY;
	}
	err = _tfs_seek(dir, firstFree);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	bNum = nextFreeBlock();
	if (bNum <= 0) {
		return ERR_NOMEMORY;
	}
	int off, idx;
	idx = ptrIndex(firstFree, &off);
	int nameLen = strlen(name);
	memcpy(dir->buf.data+idx, name, nameLen);
	dir->buf.data[idx+MAX_FILENAME_SIZE] = bNum;
	err = writeBlock(mnt, dir->buf.bNum, dir->buf.data);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	return bNum;
}

int openDir(char* path, File* dir) {
	int nameSize;
	char* name = strtok(path, "/");
	while (name) {
		nameSize = strlen(name);
		if (nameSize == 0) {
			return ERR_INVALID;
		} else if (nameSize > MAX_FILENAME_SIZE) {
			return ERR_NAMETOOLONG;
		}

		// do something

		name = strtok(NULL, "/");
	}
	return 0;
}

fileDescriptor tfs_openFile(char* name) {
	dbg("opening %s\n", name);
	int pathLen = strlen(name);
	if (pathLen == 0) {
		return ERR_INVALID;
	}
	if (name[0] == '/') {
		if (pathLen == 1) {
			return ERR_INVALID;
		}
		++name;
		--pathLen;
	}
	if (name[pathLen-1] == '/') {
		if (pathLen == 1) {
			return ERR_INVALID;
		}
		name[pathLen-1] = '\0';
		--pathLen;
	}
	char* path = name;
	char* tmp = strrchr(name, '/');
	if (tmp) {
		name = tmp + 1;
		*tmp = '\0';
	}
	if (path != name) {
		openDir(path, &rootDir);
	}
	int nameSize = strlen(name);
	if (nameSize == 0) {
		return ERR_INVALID;
	} else if (nameSize > MAX_FILENAME_SIZE) {
		return ERR_NAMETOOLONG;
	}
	int err, idx;
	File file = {0};
	memcpy(file.name, name, nameSize);
	int bNum = findFile(&file);
	if (IS_TFS_ERROR(bNum)) {
		return bNum;
	} else if (bNum == 0) {
		dbg("file not found!\n");
		file.buf.data[1] = 0x44;
		idx = BLOCK_HEADER_SIZE;
		file.buf.data[idx] = rootDir.inode;
		idx += 1;
		memcpy(file.buf.data+idx, file.name, MAX_FILENAME_SIZE);
		idx += MAX_FILENAME_SIZE;
		file.buf.data[idx] = FLAGS_RDWR;
		idx += 1;
		memset(file.buf.data+idx, 0, BLOCKSIZE-idx);
		if ((bNum = nextFreeBlock()) <= 0) {
			return ERR_NOMEMORY;
		}
		err = writeBlock(mnt, bNum, file.buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		file.buf.bNum = bNum;
		file.inode = bNum;
		file.dir = rootDir.inode;
		file.flags = FLAGS_RDWR;
		bitset_clear(superBlock.data+5, bNum);
	}
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
	File* fp;
	int err = getFile(fd, &fp);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	fp->inode = -1;
	fp->flags = 0;
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
	File* fp;
	int err = getFile(fd, &fp);
	if (IS_TFS_ERROR(err)) {
		dbg("error getting file\n");
		return err;
	} else if ((fp->flags & FLAG_ISDIR)) {
		dbg("file is dir\n");
		return ERR_ISDIR;
	} else if ((fp->flags & FLAG_WRITE) == 0) {
		dbg("no write access\n");
		return ERR_ACCESS;
	} else if (size > fp->size) {
		int need = blockNum(size-1) - blockNum(fp->size-1);
		int have = bitset_popcnt(superBlock.data+5, superBlock.data[4]);
		if (need > have) {
			return ERR_NOMEMORY;
		}
	}
	fp->size = size;
	int off = BLOCK_HEADER_SIZE;
	int n, nBytes = BLOCK_DATA_SIZE;
	int next, bNum = fp->inode;
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
	File* fp;
	int err = getFile(fd, &fp);
	if (IS_TFS_ERROR(err)) {
		return err;
	} else if ((fp->flags & FLAG_ISDIR)) {
		return ERR_ISDIR;
	} else if ((fp->flags & FLAG_WRITE) == 0) {
		return ERR_ACCESS;
	}
	err = freeBlocks(fp, fp->inode);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	return tfs_closeFile(fd);
}

int tfs_readByte(fileDescriptor fd, char* buffer) {
	dbg("reading byte\n");
	File* fp;
	int err = getFile(fd, &fp);
	if (IS_TFS_ERROR(err)) {
		return err;
	} else if (fp->flags & FLAG_ISDIR) {
		return ERR_ISDIR;
	} else if (fp->ptr >= fp->size) {
		return ERR_FAULT;
	}
	int off;
	int idx = ptrIndex(fp->ptr, &off);
	dbg("off: %d, idx: %d\n", off, idx);
	if (idx == off) {
		// Read next block of file
		int bNum = (fp->ptr == 0) ? fp->inode : fp->buf.data[2];
		if (bNum <= 0) {
			return ERR_FAULT;
		}
		dbg("Reading next block %d\n", bNum);
		err = readBlock(mnt, bNum, fp->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		fp->buf.bNum = bNum;
	}

	dbg("block[%d] = '%c'\n", idx, fp->buf.data[idx]);
	*buffer = fp->buf.data[idx];
	++fp->ptr;
	return 0;
}

int _tfs_seek(File* fp, int offset) {
	if (offset >= fp->size) {
		fp->ptr = offset;
		return 0;
	}
	int ptrBlock = blockNum(fp->ptr);
	int offBlock = blockNum(offset);
	int bNum, nBlocks;
	if (offBlock < ptrBlock) {
		bNum = fp->inode;
		nBlocks = offBlock + 1;
	} else {
		bNum = fp->buf.data[2];
		nBlocks = offBlock - ptrBlock;
	}
	int err;
	while (bNum > 0 && nBlocks > 0) {
		err = readBlock(mnt, bNum, fp->buf.data);
		if (IS_TFS_ERROR(err)) {
			return err;
		}
		fp->buf.bNum = bNum;
		bNum = fp->buf.data[2];
		nBlocks -= 1;
	}
	fp->ptr = offset;
	return 0;
}

int tfs_seek(fileDescriptor fd, int offset) {
	File* fp;
	int err = getFile(fd, &fp);
	if (IS_TFS_ERROR(err)) {
		return err;
	}
	return _tfs_seek(fp, offset);
}

