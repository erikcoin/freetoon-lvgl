#include "backlight.h"
#include <stdio.h>
#include <stdlib.h>

#define BL_PATH "/sys/class/backlight/mp3309-bl/brightness"

void backlight_set(int level) {
    if (level < 0)    level = 0;
    if (level > 1000) level = 1000;
    FILE * f = fopen(BL_PATH, "w");
    if (!f) return;
    fprintf(f, "%d\n", level);
    fclose(f);
}

int backlight_get(void) {
    FILE * f = fopen(BL_PATH, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}
