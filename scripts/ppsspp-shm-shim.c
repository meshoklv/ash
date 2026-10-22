/*
 * LD_PRELOAD shim for PPSSPPHeadless: put the emulator's RAM arena in a file under $TMPDIR (default /tmp)
 * instead of /dev/shm. The container's /dev/shm is 64 MB, one emulator takes ~61 MB of it, so a second
 * concurrent instance dies with SIGBUS. Build (host gcc from scripts/env.sh buildtools):
 *   x86_64-conda-linux-gnu-gcc -shared -fPIC -O2 -o build/ppsspp-shm-shim.so scripts/ppsspp-shm-shim.c
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int shm_open(const char *name, int oflag, mode_t mode)
{
    (void)name;
    const char *dir = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/ppsspp-arena-%d-XXXXXX", dir && *dir ? dir : "/tmp", (int)getpid());
    int fd = mkostemp(path, (oflag & O_CLOEXEC) ? O_CLOEXEC : 0);
    if (fd < 0) return -1;
    fchmod(fd, mode & 0600);
    if (getenv("ASH_SHIM_DEBUG")) fprintf(stderr, "shm shim: %s -> %s\n", name, path);
    unlink(path); /* anonymous: freed when the emulator exits */
    return fd;
}

int shm_unlink(const char *name)
{
    (void)name;
    return 0;
}
