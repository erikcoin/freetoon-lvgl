/*
 * Tiny key=value config file at /mnt/data/toonui.cfg.
 * No JSON parser dependency. Loads defaults if file missing.
 */
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CFG_PATH "/mnt/data/toonui.cfg"

settings_t settings = {
    .auto_dim_enabled  = 1,
    .auto_dim_seconds  = 10,
    .active_brightness = 800,
    .dim_brightness    = 80,
    .temp_offset_centi = 0,
    .show_dim_weather  = 1,
    .show_dim_waste    = 1,
    .dim_waste_lead_days = 2,
    .vnc_enabled       = 0,
    .vnc_pass          = "",
    .weather_location    = "Medemblik",
    /* GeoNames id understood by forecast.buienradar.nl/2.0/forecast/<id>.
     * 2757783 = De Bilt — central NL national fallback. The KNMI station
     * code 6249 happens to collide with an unrelated mid-east location
     * in buienradar's GeoNames mapping, so we default to De Bilt and let
     * the user paste their own id into /mnt/data/toonui.cfg if they want
     * Medemblik-specific hourly data (the URL bar on
     * https://www.buienradar.nl/weer/medemblik/nl/<ID> reveals it). */
    .weather_location_id = 2757783,
};

float display_indoor_temp(float raw) {
    if (raw <= 0) return raw;
    return raw + (float)settings.temp_offset_centi / 100.0f;
}

void settings_load(void) {
    FILE * f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char k[64], v[64];
        if (sscanf(line, "%63[^=]=%63s", k, v) != 2) continue;
        int iv = atoi(v);
        if      (strcmp(k, "auto_dim_enabled")  == 0) settings.auto_dim_enabled  = iv;
        else if (strcmp(k, "auto_dim_seconds")  == 0) settings.auto_dim_seconds  = iv;
        else if (strcmp(k, "active_brightness") == 0) settings.active_brightness = iv;
        else if (strcmp(k, "dim_brightness")    == 0) settings.dim_brightness    = iv;
        else if (strcmp(k, "temp_offset_centi") == 0) settings.temp_offset_centi = iv;
        else if (strcmp(k, "show_dim_weather")  == 0) settings.show_dim_weather  = iv;
        else if (strcmp(k, "show_dim_waste")    == 0) settings.show_dim_waste    = iv;
        else if (strcmp(k, "dim_waste_lead_days") == 0) settings.dim_waste_lead_days = iv;
        else if (strcmp(k, "show_dim_weather")  == 0) settings.show_dim_weather  = iv;
        else if (strcmp(k, "temp_offset_centi") == 0) settings.temp_offset_centi = iv;
        else if (strcmp(k, "weather_location_id") == 0) settings.weather_location_id = iv;
        else if (strcmp(k, "weather_location")  == 0)
            snprintf(settings.weather_location,
                     sizeof settings.weather_location, "%s", v);
    }
    fclose(f);
}

void settings_save(void) {
    FILE * f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "auto_dim_enabled=%d\n",  settings.auto_dim_enabled);
    fprintf(f, "auto_dim_seconds=%d\n",  settings.auto_dim_seconds);
    fprintf(f, "active_brightness=%d\n", settings.active_brightness);
    fprintf(f, "dim_brightness=%d\n",    settings.dim_brightness);
    fprintf(f, "temp_offset_centi=%d\n", settings.temp_offset_centi);
    fprintf(f, "show_dim_weather=%d\n",  settings.show_dim_weather);
    fprintf(f, "show_dim_waste=%d\n",    settings.show_dim_waste);
    fprintf(f, "dim_waste_lead_days=%d\n", settings.dim_waste_lead_days);
    fprintf(f, "vnc_enabled=%d\n",       settings.vnc_enabled);
    fprintf(f, "vnc_pass=%s\n",          settings.vnc_pass);
    fprintf(f, "weather_location=%s\n",    settings.weather_location);
    fprintf(f, "weather_location_id=%d\n", settings.weather_location_id);
    fclose(f);
}
