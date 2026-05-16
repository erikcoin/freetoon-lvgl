/*
 * connsniff.c — LD_PRELOAD that logs every connect() to /tmp/conn_log.txt.
 * Used to confirm qt-gui hits localhost:10080 (HTTP) when adjusting setpoint.
 *
 * Build:
 *   /tmp/qt_rebuild/linaro/bin/arm-linux-gnueabihf-gcc \
 *     --sysroot=/tmp/qt_rebuild/linaro/arm-linux-gnueabihf/libc \
 *     -shared -fPIC -O2 -o libconnsniff.so connsniff.c -ldl
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

typedef int (*connect_fn)(int, const struct sockaddr*, socklen_t);
static connect_fn real_connect = NULL;
static int logfd = -1;

__attribute__((constructor))
static void init(void) {
    real_connect = (connect_fn)dlsym(RTLD_NEXT, "connect");
    logfd = open("/tmp/conn_log.txt", O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (logfd >= 0) {
        char hdr[80];
        int n = snprintf(hdr, sizeof(hdr), "\n=== connsniff pid=%d t=%ld ===\n",
                         (int)getpid(), (long)time(NULL));
        write(logfd, hdr, n);
    }
}

int connect(int fd, const struct sockaddr* addr, socklen_t len) {
    if (!real_connect) real_connect = (connect_fn)dlsym(RTLD_NEXT, "connect");
    if (addr && addr->sa_family == AF_INET && logfd >= 0) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)addr;
        char ip[16];
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
        struct timeval tv;
        gettimeofday(&tv, NULL);
        char line[160];
        int n = snprintf(line, sizeof(line),
                         "%ld.%06ld fd=%d → %s:%d\n",
                         (long)tv.tv_sec, (long)tv.tv_usec, fd,
                         ip, ntohs(a->sin_port));
        write(logfd, line, n);
    }
    return real_connect(fd, addr, len);
}
