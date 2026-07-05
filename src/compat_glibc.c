#include <stdarg.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* On Buster sysroot, both fcntl and fcntl64 are versioned to GLIBC_2.28.
 * Kindle glibc (~2.17) has only fcntl@GLIBC_2.4.
 * Pin our reference to the old version, then provide fcntl64 ourselves. */
__asm__(".symver __fcntl_compat, fcntl@GLIBC_2.4");
extern int __fcntl_compat(int fd, int cmd, ...);

int fcntl64(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return __fcntl_compat(fd, cmd, arg);
}

/* getentropy requires GLIBC_2.25 which Kindle lacks.
 * OpenSSL uses it for entropy; provide our own via /dev/urandom. */
int getentropy(void *buf, size_t buflen)
{
    if (buflen > 256) { errno = EIO; return -1; }
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < buflen) {
        ssize_t n = read(fd, (char *)buf + done, buflen - done);
        if (n <= 0) { close(fd); errno = EIO; return -1; }
        done += (size_t)n;
    }
    close(fd);
    return 0;
}
