#ifndef LL_WIN32_SYS_STATVFS_H
#define LL_WIN32_SYS_STATVFS_H

// <sys/statvfs.h> for MinGW: the free space of a game's drive, over
// GetDiskFreeSpaceEx.

#include <stdint.h>
#include <sys/types.h>

// The block counts, which glibc's sys/types.h has and this one does not.
typedef uint64_t fsblkcnt_t;
typedef uint64_t fsfilcnt_t;

#ifdef __cplusplus
extern "C"
{
#endif

    struct statvfs
    {
        unsigned long f_bsize;    // file system block size
        unsigned long f_frsize;   // fragment size
        fsblkcnt_t f_blocks;      // size of fs in f_frsize units
        fsblkcnt_t f_bfree;       // free blocks
        fsblkcnt_t f_bavail;      // free blocks for unprivileged users
        fsfilcnt_t f_files;       // inodes
        fsfilcnt_t f_ffree;       // free inodes
        fsfilcnt_t f_favail;      // free inodes for unprivileged users
        unsigned long f_fsid;     // file system id
        unsigned long f_flag;     // mount flags
        unsigned long f_namemax;  // maximum filename length
    };

#define ST_RDONLY 0x0001
#define ST_NOSUID 0x0002

    int statvfs(const char *path, struct statvfs *buf);
    int fstatvfs(int fd, struct statvfs *buf);

#ifdef __cplusplus
}
#endif

#endif // LL_WIN32_SYS_STATVFS_H
