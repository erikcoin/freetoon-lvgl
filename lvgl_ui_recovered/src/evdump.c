/* Tiny evdev event dumper — prints every event from /dev/input/event1 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);  /* disable buffering — needed when piped to a file */
    setvbuf(stderr, NULL, _IONBF, 0);
    const char* path = argc > 1 ? argv[1] : "/dev/input/event1";
    fprintf(stderr, "opening %s ...\n", path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    fprintf(stderr, "opened fd=%d\n", fd);

    /* Print device name */
    char name[256];
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0)
        printf("device: %s\n", name);

    /* Print ABS axis ranges */
    int axes[] = {ABS_X, ABS_Y, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TOUCH_MAJOR, ABS_PRESSURE};
    const char* axn[] = {"ABS_X","ABS_Y","ABS_MT_POSITION_X","ABS_MT_POSITION_Y","ABS_MT_TOUCH_MAJOR","ABS_PRESSURE"};
    for (size_t i = 0; i < sizeof(axes)/sizeof(axes[0]); i++) {
        struct input_absinfo a;
        if (ioctl(fd, EVIOCGABS(axes[i]), &a) >= 0) {
            printf("axis %-22s: min=%d max=%d flat=%d fuzz=%d res=%d val=%d\n",
                   axn[i], a.minimum, a.maximum, a.flat, a.fuzz, a.resolution, a.value);
        }
    }

    printf("\nWatching events (touch the screen)...\n");
    struct input_event ev;
    while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
        const char* tname = "?";
        switch (ev.type) {
            case EV_SYN: tname = "SYN"; break;
            case EV_KEY: tname = "KEY"; break;
            case EV_ABS: tname = "ABS"; break;
            case EV_REL: tname = "REL"; break;
        }
        const char* cname = "?";
        if (ev.type == EV_ABS) {
            switch (ev.code) {
                case ABS_X: cname = "ABS_X"; break;
                case ABS_Y: cname = "ABS_Y"; break;
                case ABS_MT_POSITION_X: cname = "ABS_MT_POSITION_X"; break;
                case ABS_MT_POSITION_Y: cname = "ABS_MT_POSITION_Y"; break;
                case ABS_MT_TRACKING_ID: cname = "ABS_MT_TRACKING_ID"; break;
                case ABS_MT_SLOT: cname = "ABS_MT_SLOT"; break;
                case ABS_MT_TOUCH_MAJOR: cname = "ABS_MT_TOUCH_MAJOR"; break;
                case ABS_PRESSURE: cname = "ABS_PRESSURE"; break;
            }
        } else if (ev.type == EV_KEY) {
            switch (ev.code) {
                case BTN_TOUCH: cname = "BTN_TOUCH"; break;
                case BTN_LEFT:  cname = "BTN_LEFT(=BTN_MOUSE)";  break;
            }
        }
        printf("%6ld.%06ld  %s(%d) %s(%d) = %d\n",
               (long)ev.time.tv_sec, (long)ev.time.tv_usec,
               tname, ev.type, cname, ev.code, ev.value);
    }
    return 0;
}
