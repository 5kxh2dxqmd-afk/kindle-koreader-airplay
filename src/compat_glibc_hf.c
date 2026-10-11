#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <unistd.h>

/*
 * HF deps (ffmpeg/openssl) are built with Debian's gcc-arm-linux-gnueabihf,
 * which emits references to fcntl64 / getentropy. Shared .so links allow
 * those to stay unresolved; executables (airplay_ill) do not.
 *
 * Unlike src/compat_glibc.c (SF), do NOT use .symver — kindlehf's sysroot
 * already resolves fcntl with the versions its headers expect.
 */
int fcntl64(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return fcntl(fd, cmd, arg);
}

int getentropy(void *buf, size_t buflen)
{
    if (buflen > 256) {
        errno = EIO;
        return -1;
    }
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    size_t done = 0;
    while (done < buflen) {
        ssize_t n = read(fd, (char *)buf + done, buflen - done);
        if (n <= 0) {
            close(fd);
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    close(fd);
    return 0;
}
