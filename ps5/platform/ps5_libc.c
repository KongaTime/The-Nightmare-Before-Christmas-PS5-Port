/* Two libc calls the runtime uses that the PS5 SDK's libc only half provides.
 *
 * Both are declared and called by win32_compat.c (password scrubbing and the
 * anonymous-mapping path), so they have to exist for the link; each is a few
 * lines with the same contract as the glibc original.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* glibc: erase n bytes and do not let the compiler elide the store. */
static void *(*const volatile oogie_memset)(void *, int, size_t) = memset;

void explicit_bzero(void *s, size_t n)
{
    if (s && n) oogie_memset(s, 0, n);
}

/* glibc: a file descriptor on an unnamed, ftruncate-able, mmap-able object --
 * the guest's memory mapping is built on it (CreateFileMappingA). The original
 * glibc implementation is Linux's memfd_create, which the PS5 libc does not
 * provide, and the console's sandbox has no writable /tmp to fall back on, so
 * the first choice is FreeBSD's shm_open: a kernel object that needs no
 * directory at all, which is exactly the memfd contract this caller needs.
 *
 * Name collisions are handled by unlinking the shm name the moment the fd
 * exists, and retrying with a fresh suffix if the name was taken. */
int memfd_create(const char *name, unsigned int flags)
{
    (void)flags;
    static int seq;

    char base[40];
    size_t n = 0;
    for (const char *p = name; p && *p && n < 24; p++)
        base[n++] = (*p == '/' || *p == '.') ? '_' : *p;
    base[n] = '\0';

    for (int i = 0; i < 16; i++) {
        char shm_name[64];
        snprintf(shm_name, sizeof shm_name, "/oogie_%s_%d_%d",
                 base[0] ? base : "map", (int)getpid(), seq++);
        int fd = shm_open(shm_name, O_CREAT | O_RDWR | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(shm_name);   /* anonymous once unlinked */
            return fd;
        }
        if (errno != EEXIST) break;
    }

    /* No shm object (or none allowed here): a temp file that is unlinked the
     * moment it exists answers the same need, whichever directory takes it. */
    static const char *const dirs[] = { "/tmp", "/dev/shm", "/data/oogie-ps5" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        char full[192];
        snprintf(full, sizeof full, "%s/oogie-%s-XXXXXX", dirs[d], base[0] ? base : "map");
        int fd = mkstemp(full);
        if (fd >= 0) {
            unlink(full);         /* vanishes with its last reference */
            return fd;
        }
    }
    return -1;
}
