#include <stdio.h>
#include <unistd.h>

#include "tinyFS.h"
#include "tinyFS_errno.h"
#include "libDisk.h"

int clz(char x) {
	return __builtin_clz(x) - ((sizeof(int)-sizeof(char))*8);
}

int main() {
	int fd = openDisk(DEFAULT_DISK_NAME, 500);
	printf("%d\n", fd);
	printf("%ld\n", lseek(fd, 0, SEEK_END));
	closeDisk(fd);
	int x = 2;
	printf("clz: %d\n", clz(x));
	return 0;
}
