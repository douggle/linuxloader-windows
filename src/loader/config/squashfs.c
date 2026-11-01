// Squashfs game dumps.
//
// The Raw Thrills and Namco ES1 dumps are usually distributed as a .squashfs
// image of the game directory ("Dead Heat.wsquashfs", "Nirin.pc.squashfs"),
// while the loader takes the game directory as its path. A squashfs image
// given in the game path is therefore mounted read-only here and takes the
// argument's place, so the rest of the loader only ever sees a directory.
//
// The mount is done in userspace with squashfuse, which needs no root and is
// packaged on Arch (and most distributions); the kernel's loop mount is the
// fallback when the loader is run as root without squashfuse. The mountpoint
// is derived from the image, in the user's runtime directory, so a mount left
// behind (the 64-bit games replace the loader's process, see below) is reused
// by the next run instead of piling up.

// The dumps run to several gigabytes: in this 32-bit loader, stat() fails
// with EOVERFLOW above 2 GiB unless the offsets are 64-bit.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "squashfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../log/log.h"

#define SQUASHFS_MAGIC 0x73717368u
#define SQUASHFS_MAGIC_SWAPPED 0x68737173u
// A kernel mount reports squashfs; squashfuse's, fuse.
#define FUSE_SUPER_MAGIC 0x65735546u

#define SQUASHFS_MAX_MOUNTS 8

static char *mounted[SQUASHFS_MAX_MOUNTS];
static int mountedCount;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int isSquashfsImage(const char *path)
{
    struct stat st;
    uint32_t magic;
    int fd;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    // 64-bit offsets (above) are what let open() reach these dumps at all:
    // with a 32-bit off_t, opening one fails with EOVERFLOW above 2 GiB.
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return 0;
    if (read(fd, &magic, sizeof(magic)) != (ssize_t)sizeof(magic))
    {
        close(fd);
        return 0;
    }
    close(fd);
    return magic == SQUASHFS_MAGIC || magic == SQUASHFS_MAGIC_SWAPPED;
}

static int isSquashfsMount(const char *path)
{
    struct statfs sfs;

    if (statfs(path, &sfs) != 0)
        return 0;
    return (uint32_t)sfs.f_type == SQUASHFS_MAGIC || (uint32_t)sfs.f_type == FUSE_SUPER_MAGIC;
}

static int mkdirs(const char *path)
{
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++)
    {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    return mkdir(buf, 0755) != 0 && errno != EEXIST ? -1 : 0;
}

// Run a helper to completion; its output is the loader's log's business, not
// the screen's.
static int run(const char *program, char *const argv[])
{
    pid_t pid = fork();
    int status;

    if (pid < 0)
        return -1;
    if (pid == 0)
    {
        execvp(program, argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

// Where the mounts live: the login's runtime directory, or a per-user
// directory in /tmp when there is none (a console session, a launcher).
static void runtimeDir(char *out, size_t outLen)
{
    const char *xdg = getenv("XDG_RUNTIME_DIR");

    if (xdg && xdg[0] == '/')
        snprintf(out, outLen, "%s/linuxloader", xdg);
    else
        snprintf(out, outLen, "/tmp/linuxloader-%lu", (unsigned long)geteuid());
}

// One stable mountpoint per image: the image's name, its device and inode.
static int mountpointFor(const char *image, char *out, size_t outLen)
{
    struct stat st;
    char base[PATH_MAX], name[64], runtime[PATH_MAX];
    size_t n = 0;

    if (stat(image, &st) != 0)
        return -1;
    base[0] = '\0';
    if (strrchr(image, '/'))
        snprintf(base, sizeof(base), "%s", strrchr(image, '/') + 1);
    else
        snprintf(base, sizeof(base), "%s", image);
    // Keep the readable part, drop the extension and anything a path cannot
    // hold; the device and inode make it unique.
    for (size_t i = 0; base[i] && n + 1 < sizeof(name) - 24; i++)
        if (base[i] == '.' && !strncasecmp(base + i, ".squashfs", 9))
            break;
        else if ((base[i] >= 'a' && base[i] <= 'z') || (base[i] >= 'A' && base[i] <= 'Z') || (base[i] >= '0' && base[i] <= '9') || base[i] == '-' || base[i] == '_')
            name[n++] = base[i];
    name[n] = '\0';
    if (!n)
        snprintf(name, sizeof(name), "image");

    runtimeDir(runtime, sizeof(runtime));
    snprintf(out, outLen, "%s/squashfs/%s-%lx%lx", runtime, name, (unsigned long)st.st_dev, (unsigned long)st.st_ino);
    return 0;
}

// A dump may hold the game in a single top-level directory rather than at the
// root of the image; take that directory when there is nothing at the root.
static void gameRoot(char *dir, size_t dirLen)
{
    DIR *d;
    struct dirent *e;
    char only[PATH_MAX] = "";
    int count = 0;

    if (!(d = opendir(dir)))
        return;
    while ((e = readdir(d)))
    {
        struct stat st;
        char path[PATH_MAX];

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (count++)
            break;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            snprintf(only, sizeof(only), "%s", path);
    }
    closedir(d);

    if (count == 1 && only[0])
        snprintf(dir, dirLen, "%s", only);
}

// ---------------------------------------------------------------------------
// Mounting
// ---------------------------------------------------------------------------

static int waitForMount(const char *dir)
{
    // squashfuse returns once the filesystem is mounted, but it is a separate
    // process: give the mount a moment to appear rather than racing it.
    for (int i = 0; i < 100; i++)
    {
        if (isSquashfsMount(dir))
            return 0;
        usleep(50 * 1000);
    }
    return -1;
}

static int mountImage(const char *image)
{
    char dir[PATH_MAX];
    char *squashfuse[] = {"squashfuse", "-o", "ro", "-o", "auto_unmount", (char *)image, dir, NULL};
    char *loop[] = {"mount", "-t", "squashfs", "-o", "loop,ro", (char *)image, dir, NULL};

    if (mountpointFor(image, dir, sizeof(dir)) != 0 || mkdirs(dir) != 0)
    {
        log_error("Could not make a mountpoint for %s", image);
        return -1;
    }

    // Already mounted by an earlier run (the 64-bit games keep it): reuse it.
    if (isSquashfsMount(dir))
    {
        log_info("Squashfs image %s is already mounted at %s", image, dir);
    }
    else
    {
        if (run("squashfuse", squashfuse) == 0 && waitForMount(dir) == 0)
            log_info("Mounted squashfs image %s at %s", image, dir);
        else if (geteuid() == 0 && run("mount", loop) == 0 && isSquashfsMount(dir))
            log_info("Mounted squashfs image %s at %s (loop)", image, dir);
        else
        {
            log_error("Could not mount the squashfs image %s.", image);
            log_error("Install squashfuse (pacman -S squashfuse) to run games from a .squashfs dump.");
            return -1;
        }
    }

    if (mountedCount < SQUASHFS_MAX_MOUNTS)
    {
        if (!(mounted[mountedCount] = strdup(dir)))
            return -1;
        mountedCount++;
    }
    return 0;
}

// Mount the image and return its game directory, or NULL.
static char *resolve(const char *image)
{
    char dir[PATH_MAX], *result;

    log_info("Squashfs game dump: %s", image);
    if (mountImage(image) != 0)
        return NULL;
    if (mountpointFor(image, dir, sizeof(dir)) != 0)
        return NULL;
    gameRoot(dir, sizeof(dir));
    if (!(result = strdup(dir)))
        return NULL;
    log_info("Game directory: %s", result);
    return result;
}

void squashfsReleaseAll(void)
{
    static int released;
    char cwd[PATH_MAX];

    if (released)
        return;
    released = 1;

    // A mount cannot be unmounted while a process stands in it: the game was
    // started from the mountpoint, so leave it first.
    if (getcwd(cwd, sizeof(cwd)))
        for (int i = 0; i < mountedCount; i++)
            if (!strncmp(cwd, mounted[i], strlen(mounted[i])))
            {
                chdir("/");
                break;
            }

    for (int i = mountedCount - 1; i >= 0; i--)
    {
        char *fusermount3[] = {"fusermount3", "-u", mounted[i], NULL};
        char *fusermount[] = {"fusermount", "-u", mounted[i], NULL};
        char *umount[] = {"umount", mounted[i], NULL};

        // A loop mount is only ours to unmount when we could make it.
        if (run("fusermount3", fusermount3) != 0 && run("fusermount", fusermount) != 0 &&
            (geteuid() != 0 || run("umount", umount) != 0))
        {
            log_warn("Could not unmount %s", mounted[i]);
            continue;
        }
        rmdir(mounted[i]);
        free(mounted[i]);
        mounted[i] = NULL;
    }
    mountedCount = 0;
}

// ---------------------------------------------------------------------------

int squashfsPrepareArgs(int argc, char **argv)
{
    int prepared = 0;

    for (int i = 1; i < argc; i++)
    {
        char *path = NULL;

        if (!strcmp(argv[i], "-g") || !strcmp(argv[i], "--gamepath"))
        {
            if (i + 1 < argc && isSquashfsImage(argv[i + 1]))
                path = resolve(argv[i + 1]);
            if (path)
            {
                argv[i + 1] = path;
                prepared++;
            }
            i++;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0')
            continue;  // an option; -g's value was taken above
        if (isSquashfsImage(argv[i]) && (path = resolve(argv[i])))
        {
            argv[i] = path;
            prepared++;
        }
    }

    // The caller releases the mounts when the game is over (atexit is not
    // available to the preload library, which links without the C runtime's
    // start files).
    return prepared;
}
