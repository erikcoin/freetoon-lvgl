#ifndef TOON_SETTINGS_H
#define TOON_SETTINGS_H

typedef struct {
    int auto_dim_enabled;     /* 0/1 — switch to ambient screen after idle */
    int auto_dim_seconds;     /* 5..300 — idle timeout in seconds */
    int active_brightness;    /* 0..1000 backlight when active */
    int dim_brightness;       /* 0..1000 backlight while dimmed */
    int temp_offset_centi;    /* -500..+500 — added to displayed indoor temp,
                                 in centi-degrees (e.g. -120 = subtract 1.2°C) */
    int show_dim_weather;     /* 0/1 — show today's weather icon on the dim screen */
    int show_dim_waste;       /* 0/1 — show next-pickup on the dim screen */
    int dim_waste_lead_days;  /* 0..7 — only show if pickup is within this many days
                                 (0 disables; 1 = only on pickup day; 2 = day before + day of) */
    int  vnc_enabled;         /* 0/1 — run the x11vnc remote-control server */
    char vnc_pass[16];        /* VNC password (plaintext, max 8 effective chars;
                                 empty = no password). No spaces. */
    char weather_location[32];   /* Free-text location name shown in the UI
                                    (default "Medemblik"). Cosmetic for now. */
    int  weather_location_id;    /* Buienradar wikiCode for hourly-forecast
                                    fetch — default 6249 ≈ Berkhout (KNMI
                                    station closest to Medemblik). 0 disables
                                    the hourly fetch. */
} settings_t;

/* Display-side adjusted indoor temperature: raw + settings.temp_offset_centi/100 */
float display_indoor_temp(float raw);

extern settings_t settings;

void settings_load(void);
void settings_save(void);

#endif
