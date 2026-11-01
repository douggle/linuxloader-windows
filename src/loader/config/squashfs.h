#ifndef SQUASHFS_H
#define SQUASHFS_H

#ifdef __cplusplus
extern "C"
{
#endif

    // Raw Thrills and Namco ES1 dumps are often distributed as a .squashfs
    // image of the game directory instead of the directory itself. A path
    // argument that is a squashfs image is mounted read-only and replaced in
    // argv by its mountpoint (see squashfs.c). A no-op for every other
    // argument, and on platforms without squashfs support.
    int squashfsPrepareArgs(int argc, char **argv);

    // Unmount the images this process mounted.
    void squashfsReleaseAll(void);

#ifdef __cplusplus
}
#endif

#endif /* SQUASHFS_H */
