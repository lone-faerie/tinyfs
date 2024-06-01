#ifndef TINYFS_ERRNO_H
#define TINYFS_ERRNO_H

/* End of file */
#define ERR_EOF -1
/* Permission denied */
#define ERR_ACCESS -2
/* Resource temporarily unavailable */
#define ERR_AGAIN -3
/* Bad file descriptor */
#define ERR_BADF -4
/* Disk quota exceeded */
#define ERR_DQUOTA -5
/* Bad address */
#define ERR_FAULT -6
/* Interrupted by a signal handler */
#define ERR_INTERRUPT -7
/* Invalid argument */
#define ERR_INVALID -8
/* Input/output error */
#define ERR_IO -9
/* Is a directory */
#define ERR_ISDIR -10
/* Too many levels of symbolic links */
#define ERR_LOOP -11
/* Too many open files */
#define ERR_MFILES -12
/* Filename too long */
#define ERR_NAMETOOLONG -13
/* Not enough space/cannot allocate memory */
#define ERR_NOMEMORY -14
/* Value too large to be stored in datatype */
#define ERR_OVERFLOW -15
/* Operation not permitted */
#define ERR_PERMIT -16
/* Read-only filesystem */
#define ERR_RDONLYFS -17
/* Invalid seek */
#define ERR_SEEKPIPE -18
/* Text file busy */
#define ERR_TXTBUSY -19
/* Unknown error */
#define ERR_UNKNOWN -128

#define IS_TFS_ERROR(err) ((err) < 0 && (err) >= ERR_TXTBUSY)

// TINYFS_ERRNO_H
#endif
