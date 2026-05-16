#ifndef TOON_BACKLIGHT_H
#define TOON_BACKLIGHT_H

/* Write to /sys/class/backlight/mp3309-bl/brightness.
   Valid range 0..1000 on this hardware. */
void backlight_set(int level);
int  backlight_get(void);

#endif
