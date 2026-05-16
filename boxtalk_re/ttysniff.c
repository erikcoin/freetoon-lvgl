/*
 * ttysniff.c — LD_PRELOAD that captures bytes on /dev/ttyS0 (the Toon
 * keteladapter ↔ happ_thermstat link). Logs hex+timestamp per read/write.
 *
 * Tracks the fd via open()/open64() hooks; filters read/write/close by it.
 *
 * Build:
 *   arm-linux-gnueabihf-gcc -shared -fPIC -O2 -o libttysniff.so ttysniff.c -ldl
 * Load (test, one-shot):
 *   LD_PRELOAD=/mnt/data/libttysniff.so /qmf/sbin/happ_thermstat
 * Capture file:
 *   /var/volatile/tmp/ttyS0_capture.log   (hex frames, timestamped, R/W)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>

typedef int     (*open_fn)(const char *, int, ...);
typedef int     (*open64_fn)(const char *, int, ...);
typedef int     (*close_fn)(int);
typedef ssize_t (*read_fn)(int, void *, size_t);
typedef ssize_t (*write_fn)(int, const void *, size_t);
typedef int     (*dup_fn)(int);
typedef int     (*dup2_fn)(int, int);

static open_fn   real_open   = NULL;
static open64_fn real_open64 = NULL;
static close_fn  real_close  = NULL;
static read_fn   real_read   = NULL;
static write_fn  real_write  = NULL;
static dup_fn    real_dup    = NULL;
static dup2_fn   real_dup2   = NULL;

static int  log_fd   = -1;
static int  tty_fd   = -1;          /* fd happ_thermstat has on the keteladapter UART */
static __thread int in_hook = 0;
/* Toon i.MX6 keteladapter UART is /dev/ttymxc0; /dev/ttyS0 is the historical
   alias the binary still mentions. Match either. */
static const char *TTY  = "/dev/ttymxc0";
static const char *TTY2 = "/dev/ttyS0";
static const char *LOG  = "/var/volatile/tmp/ttymxc0_capture.log";

__attribute__((constructor))
static void init(void) {
    real_open   = (open_fn)   dlsym(RTLD_NEXT, "open");
    real_open64 = (open64_fn) dlsym(RTLD_NEXT, "open64");
    real_close  = (close_fn)  dlsym(RTLD_NEXT, "close");
    real_read   = (read_fn)   dlsym(RTLD_NEXT, "read");
    real_write  = (write_fn)  dlsym(RTLD_NEXT, "write");
    real_dup    = (dup_fn)    dlsym(RTLD_NEXT, "dup");
    real_dup2   = (dup2_fn)   dlsym(RTLD_NEXT, "dup2");

    log_fd = real_open ? real_open(LOG, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
    if (log_fd >= 0) {
        char hdr[160];
        int n = snprintf(hdr, sizeof(hdr),
            "\n=== ttysniff loaded pid=%d t=%ld ===\n",
            (int)getpid(), (long)time(NULL));
        real_write(log_fd, hdr, n);
    }
}

/* Hex-dump bytes to the log with a header line for the frame. */
static void dump(char dir, const void *buf, ssize_t n) {
    if (in_hook || log_fd < 0 || n <= 0) return;
    in_hook = 1;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    char head[96];
    int hl = snprintf(head, sizeof(head), "%c %ld.%06ld n=%zd ",
                      dir, (long)tv.tv_sec, (long)tv.tv_usec, n);
    real_write(log_fd, head, hl);
    /* hex bytes */
    static const char hx[] = "0123456789abcdef";
    char buf2[3 * 256 + 2];
    const unsigned char *p = buf;
    size_t cap = n > 256 ? 256 : (size_t)n;
    size_t o = 0;
    for (size_t i = 0; i < cap; i++) {
        buf2[o++] = hx[p[i] >> 4];
        buf2[o++] = hx[p[i] & 0xf];
        buf2[o++] = ' ';
    }
    buf2[o++] = '\n';
    real_write(log_fd, buf2, o);
    if ((size_t)n > cap) {
        const char trunc[] = "  [...truncated...]\n";
        real_write(log_fd, trunc, sizeof(trunc) - 1);
    }
    in_hook = 0;
}

/* Watch for opens of /dev/ttyS0 and remember the returned fd. */
static void note_open(const char *path, int fd) {
    if (fd < 0 || !path) return;
    if (strcmp(path, TTY) != 0 && strcmp(path, TTY2) != 0) return;
    tty_fd = fd;
    if (log_fd >= 0 && !in_hook) {
        in_hook = 1;
        char m[80];
        int n = snprintf(m, sizeof(m), "* open(%s)=%d t=%ld\n",
                         path, fd, (long)time(NULL));
        real_write(log_fd, m, n);
        in_hook = 0;
    }
}

int open(const char *path, int flags, ...) {
    if (!real_open) real_open = (open_fn)dlsym(RTLD_NEXT, "open");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    int fd = real_open(path, flags, mode);
    note_open(path, fd);
    return fd;
}
int open64(const char *path, int flags, ...) {
    if (!real_open64) real_open64 = (open64_fn)dlsym(RTLD_NEXT, "open64");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    int fd = real_open64(path, flags, mode);
    note_open(path, fd);
    return fd;
}

int close(int fd) {
    if (!real_close) real_close = (close_fn)dlsym(RTLD_NEXT, "close");
    if (fd == tty_fd) {
        if (log_fd >= 0 && !in_hook) {
            in_hook = 1;
            char m[64];
            int n = snprintf(m, sizeof(m), "* close(%d) t=%ld\n",
                             fd, (long)time(NULL));
            real_write(log_fd, m, n);
            in_hook = 0;
        }
        tty_fd = -1;
    }
    return real_close(fd);
}

/* dup/dup2 — track if the tty fd gets aliased. */
int dup(int oldfd) {
    if (!real_dup) real_dup = (dup_fn)dlsym(RTLD_NEXT, "dup");
    int nfd = real_dup(oldfd);
    if (oldfd == tty_fd && nfd >= 0) tty_fd = nfd;
    return nfd;
}
int dup2(int oldfd, int newfd) {
    if (!real_dup2) real_dup2 = (dup2_fn)dlsym(RTLD_NEXT, "dup2");
    int nfd = real_dup2(oldfd, newfd);
    if (oldfd == tty_fd && nfd >= 0) tty_fd = nfd;
    return nfd;
}

ssize_t read(int fd, void *buf, size_t count) {
    if (!real_read) real_read = (read_fn)dlsym(RTLD_NEXT, "read");
    ssize_t n = real_read(fd, buf, count);
    if (fd == tty_fd && n > 0) dump('R', buf, n);  /* BA -> happ_thermstat */
    return n;
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (!real_write) real_write = (write_fn)dlsym(RTLD_NEXT, "write");
    if (fd == tty_fd) dump('W', buf, (ssize_t)count);  /* happ_thermstat -> BA */
    return real_write(fd, buf, count);
}
