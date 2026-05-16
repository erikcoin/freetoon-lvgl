/*
 * bxtsniff.c — LD_PRELOAD that captures BoxTalk traffic on TCP 127.0.0.1:1337
 * Hooks read/write/send/recv and logs to /tmp/bxt_capture.log.
 * Filter: only fd's connected to 127.0.0.1:1337 (BoxTalk port).
 *
 * Build:
 *   arm-linux-gnueabihf-gcc -shared -fPIC -O2 -o libbxtsniff.so bxtsniff.c -ldl
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

typedef ssize_t (*read_fn)(int, void*, size_t);
typedef ssize_t (*write_fn)(int, const void*, size_t);
typedef ssize_t (*send_fn)(int, const void*, size_t, int);
typedef ssize_t (*recv_fn)(int, void*, size_t, int);

static read_fn  real_read  = NULL;
static write_fn real_write = NULL;
static send_fn  real_send  = NULL;
static recv_fn  real_recv  = NULL;

static int logfd = -1;
static volatile int init_done = 0;
static __thread int in_hook = 0;

__attribute__((constructor))
static void init(void) {
    real_read  = (read_fn)dlsym(RTLD_NEXT, "read");
    real_write = (write_fn)dlsym(RTLD_NEXT, "write");
    real_send  = (send_fn)dlsym(RTLD_NEXT, "send");
    real_recv  = (recv_fn)dlsym(RTLD_NEXT, "recv");
    logfd = open("/tmp/bxt_capture.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logfd >= 0) {
        char hdr[128];
        int n = snprintf(hdr, sizeof(hdr), "\n=== bxtsniff loaded pid=%d t=%ld ===\n",
                         (int)getpid(), (long)time(NULL));
        real_write(logfd, hdr, n);
    }
    init_done = 1;
}

static int is_bxt_fd(int fd) {
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    if (getpeername(fd, (struct sockaddr*)&peer, &len) != 0) return 0;
    if (peer.sin_family != AF_INET) return 0;
    if (peer.sin_addr.s_addr != htonl(0x7f000001)) return 0;
    if (peer.sin_port != htons(1337)) return 0;
    return 1;
}

static int is_bxt_local(int fd) {
    /* fd may be the server side — check if WE bound to :1337 */
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    if (getsockname(fd, (struct sockaddr*)&local, &len) != 0) return 0;
    if (local.sin_family != AF_INET) return 0;
    if (local.sin_port != htons(1337)) return 0;
    return 1;
}

static void dump(char dir, int fd, const void* buf, ssize_t n) {
    if (in_hook) return;
    if (n <= 0 || logfd < 0 || !init_done) return;
    if (fd == logfd) return;
    in_hook = 1;
    int matched = is_bxt_fd(fd) || is_bxt_local(fd);
    if (matched) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        char header[128];
        int hl = snprintf(header, sizeof(header),
                          "\n=%c fd=%d t=%ld.%06ld n=%zd\n",
                          dir, fd, (long)tv.tv_sec, (long)tv.tv_usec, n);
        real_write(logfd, header, hl);
        size_t cap = n > 16384 ? 16384 : (size_t)n;
        real_write(logfd, buf, cap);
        if ((size_t)n > cap) {
            char trunc[] = "\n[...truncated...]\n";
            real_write(logfd, trunc, sizeof(trunc) - 1);
        }
    }
    in_hook = 0;
}

ssize_t read(int fd, void* buf, size_t count) {
    if (!real_read) real_read = (read_fn)dlsym(RTLD_NEXT, "read");
    ssize_t n = real_read(fd, buf, count);
    if (n > 0) dump('R', fd, buf, n);
    return n;
}

ssize_t write(int fd, const void* buf, size_t count) {
    if (!real_write) real_write = (write_fn)dlsym(RTLD_NEXT, "write");
    dump('W', fd, buf, (ssize_t)count);
    return real_write(fd, buf, count);
}

ssize_t send(int fd, const void* buf, size_t count, int flags) {
    if (!real_send) real_send = (send_fn)dlsym(RTLD_NEXT, "send");
    dump('S', fd, buf, (ssize_t)count);
    return real_send(fd, buf, count, flags);
}

ssize_t recv(int fd, void* buf, size_t count, int flags) {
    if (!real_recv) real_recv = (recv_fn)dlsym(RTLD_NEXT, "recv");
    ssize_t n = real_recv(fd, buf, count, flags);
    if (n > 0) dump('Q', fd, buf, n);
    return n;
}
