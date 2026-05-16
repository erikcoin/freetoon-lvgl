/*
 * qubytap — feed 7-byte Quby frames into a pty (or any chardev) and print
 * whatever bytes come back. Bench-test fixture for quby_bridge.
 *
 * Usage:
 *   qubytap /tmp/quby_pty <hex-bytes> [<hex-bytes> ...]
 *
 *   Each <hex-bytes> arg is 14 hex chars (one 7-byte frame), no separators.
 *   Example:
 *     qubytap /tmp/quby_pty cd000019000000   (OT-Read DID 25 -- bridge fills CRC if 0)
 *
 *   If the last byte is 0x00, qubytap recomputes the Quby CRC8 (poly 0x21
 *   init 0xff) so you don't have to. If a real CRC is given, it's used as-is.
 *
 *   After each frame is sent, qubytap waits up to 500 ms for a reply and
 *   prints the bytes it receives (typically the bridge's 7-byte response).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/select.h>

static uint8_t crc8_quby(const uint8_t *p, size_t n) {
    uint8_t c = 0xff;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x21) : (uint8_t)(c << 1);
    }
    return c;
}

static int hex_to_bytes(const char *hex, uint8_t *out, int outsz) {
    int n = 0;
    while (hex[0] && hex[1] && n < outsz) {
        int hi = hex[0] >= 'a' ? hex[0] - 'a' + 10 :
                 hex[0] >= 'A' ? hex[0] - 'A' + 10 : hex[0] - '0';
        int lo = hex[1] >= 'a' ? hex[1] - 'a' + 10 :
                 hex[1] >= 'A' ? hex[1] - 'A' + 10 : hex[1] - '0';
        if (hi < 0 || hi > 15 || lo < 0 || lo > 15) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <pty-path> <hex-bytes> [<hex-bytes> ...]\n",
                argv[0]);
        return 1;
    }
    int fd = open(argv[1], O_RDWR | O_NOCTTY);
    if (fd < 0) { perror("open"); return 2; }

    /* raw mode, 9600 8N1 */
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetspeed(&t, B9600);
        tcsetattr(fd, TCSANOW, &t);
    }

    for (int i = 2; i < argc; i++) {
        uint8_t buf[16];
        int n = hex_to_bytes(argv[i], buf, sizeof(buf));
        if (n != 7) {
            fprintf(stderr, "skip %s (need exactly 14 hex chars / 7 bytes, got %d)\n",
                    argv[i], n); continue;
        }
        if (buf[6] == 0x00) buf[6] = crc8_quby(buf, 6);

        printf("--- frame %d ---\n", i - 1);
        printf("TX:");
        for (int b = 0; b < 7; b++) printf(" %02x", buf[b]);
        putchar('\n');

        if (write(fd, buf, 7) != 7) { perror("write"); continue; }

        /* read for 800 ms, skip leading 0x6A sync bytes, then collect 7 */
        struct timeval tv = {0, 800 * 1000};
        fd_set rfds;
        uint8_t rbuf[2048]; int rlen = 0;
        for (;;) {
            FD_ZERO(&rfds); FD_SET(fd, &rfds);
            if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) break;
            ssize_t r = read(fd, rbuf + rlen, sizeof(rbuf) - rlen);
            if (r <= 0) break;
            rlen += r;
            /* trim leading 0x6A */
            int s = 0;
            while (s < rlen && rbuf[s] == 0x6A) s++;
            if (s) { memmove(rbuf, rbuf + s, rlen - s); rlen -= s; }
            if (rlen >= 7) break;
            tv.tv_sec = 0; tv.tv_usec = 200 * 1000;
        }
        if (rlen <= 0) { printf("RX: (no reply)\n"); continue; }
        int show = rlen < 14 ? rlen : 14;
        printf("RX:");
        for (int b = 0; b < show; b++) printf(" %02x", rbuf[b]);
        if (rlen > show) printf(" ... (%d more bytes)", rlen - show);
        putchar('\n');
        if (rlen >= 7) {
            uint8_t want = crc8_quby(rbuf, 6);
            printf("    CRC: got 0x%02x, want 0x%02x  %s\n",
                   rbuf[6], want, (rbuf[6] == want) ? "OK" : "FAIL");
        }
        usleep(50 * 1000);
    }
    close(fd);
    return 0;
}
