/*
 * Dim/ambient screen — pure black background, large white clock plus
 * indoor temp and setpoint. Tap anywhere to wake.
 * No colour, no icons; this is the screen we want visible while idle.
 */
#include "screens.h"
#include "boxtalk.h"
#include "settings.h"
#include "weather.h"
#include "wastecollection.h"
#include "icons.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(lv_font_montserrat_96_custom);

LV_FONT_DECLARE(lv_font_montserrat_96_custom);

static lv_obj_t * scr_root = NULL;
static lv_obj_t * lbl_clock;
static lv_obj_t * lbl_date;
static lv_obj_t * lbl_temp;
static lv_obj_t * lbl_setpoint;
static lv_obj_t * lbl_program;
static lv_obj_t * lbl_metrics;     /* TVOC / eCO2 / CH-water-pressure row */
static lv_obj_t * lbl_burner;      /* "-> 90 C" when CH, hidden otherwise */
static lv_obj_t * dim_img_faucet;  /* DHW faucet — visible only on dhw_on */
static lv_obj_t * dim_img_drop;    /* paired water-drop next to the faucet */
static lv_obj_t * wx_icon = NULL;
static lv_obj_t * lbl_outside = NULL;
static lv_obj_t * waste_icon = NULL;
static lv_obj_t * lbl_waste = NULL;
static lv_obj_t * dim_fc_icon[WEATHER_FORECAST_DAYS];
static lv_obj_t * dim_fc_day[WEATHER_FORECAST_DAYS];
static lv_obj_t * dim_fc_temp[WEATHER_FORECAST_DAYS];
static lv_timer_t * refresh_timer = NULL;

static void on_wake_tap(lv_event_t * e) {
    (void)e;
    ui_wake_now();
}

static void refresh_cb(lv_timer_t * t) {
    (void)t;
    static int n = 0;
    if (++n % 5 == 0) fprintf(stderr, "[dim] tick t=%.2f sp=%.2f prog=%s\n",
                              toon_state.indoor_temp, toon_state.setpoint,
                              program_label());

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk[16];
    strftime(clk, sizeof(clk), "%H:%M", &tm);
    lv_label_set_text(lbl_clock, clk);
    char dt[64];
    strftime(dt, sizeof(dt), "%A %d %B", &tm);
    lv_label_set_text(lbl_date, dt);

    /* Always paint values: if data not yet present, fall back to a
       "wait..." marker instead of leaving the stale "-- C" default. */
    if (toon_state.indoor_temp > 0)
        lv_label_set_text_fmt(lbl_temp, "%.1f C", display_indoor_temp(toon_state.indoor_temp));
    else
        lv_label_set_text(lbl_temp, "...");
    if (toon_state.setpoint > 0)
        lv_label_set_text_fmt(lbl_setpoint, "to %.1f C", toon_state.setpoint);
    else
        lv_label_set_text(lbl_setpoint, "");

    lv_label_set_text(lbl_program, program_label());

    if (lbl_metrics) {
        /* TVOC / eCO2 ppm / CH water pressure / air-quality badge on one
           greyed row. Missing inputs collapse to "--" so the strip layout
           stays stable. AQ label is appended only when we actually have
           air-quality data to classify. */
        char buf[160];
        char tvoc[16] = "TVOC --";
        char ppm[20]  = "-- ppm";
        char bar[16]  = "-- bar";
        if (toon_state.tvoc)
            snprintf(tvoc, sizeof tvoc, "TVOC %d", toon_state.tvoc);
        if (toon_state.eco2)
            snprintf(ppm, sizeof ppm, "%d ppm", toon_state.eco2);
        if (toon_state.water_pressure > 0.1f)
            snprintf(bar, sizeof bar, "%.1f bar", toon_state.water_pressure);
        const char * aql = air_quality_label(toon_state.eco2, toon_state.tvoc);
        if (*aql)
            snprintf(buf, sizeof buf, "%s    %s    %s    %s",
                     tvoc, ppm, bar, aql);
        else
            snprintf(buf, sizeof buf, "%s    %s    %s", tvoc, ppm, bar);
        lv_label_set_text(lbl_metrics, buf);
    }

    /* Burner state — symbol-first now. CH-heating shows just the target
       degrees ("-> 90 C", red); DHW shows a faucet + water-drop icon pair
       (no text — the icons say it). Idle hides everything so the dim
       screen stays clean. */
    if (lbl_burner) {
        if (toon_state.burner_on) {
            if (toon_state.ch_setpoint > 0)
                lv_label_set_text_fmt(lbl_burner, "-> %.0f C",
                                      toon_state.ch_setpoint);
            else
                lv_label_set_text(lbl_burner, "");
            lv_obj_set_style_text_color(lbl_burner, lv_color_hex(0xff8866), 0);
            lv_obj_clear_flag(lbl_burner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_burner, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (dim_img_faucet && dim_img_drop) {
        if (toon_state.dhw_on) {
            lv_obj_clear_flag(dim_img_faucet, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(dim_img_drop,   LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(dim_img_faucet, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(dim_img_drop,   LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (wx_icon) {
        if (settings.show_dim_weather && weather_state.day_count > 0) {
            lv_img_set_src(wx_icon, weather_icon_for_lg(weather_state.days[0].icon));
            lv_obj_clear_flag(wx_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(wx_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* Forecast strip — same gating as the weather icon. */
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        if (!dim_fc_icon[i]) continue;
        if (settings.show_dim_weather && i < weather_state.day_count) {
            const weather_day_t * d = &weather_state.days[i];
            lv_img_set_src(dim_fc_icon[i], weather_icon_for(d->icon));
            lv_label_set_text(dim_fc_day[i], d->day);
            lv_label_set_text_fmt(dim_fc_temp[i], "%.0f/%.0f",
                                  d->min_temp, d->max_temp);
            lv_obj_clear_flag(dim_fc_icon[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(dim_fc_day[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(dim_fc_temp[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(dim_fc_icon[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(dim_fc_day[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(dim_fc_temp[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (lbl_outside) {
        if (settings.show_dim_weather && weather_state.connected) {
            lv_label_set_text_fmt(lbl_outside, "%.1f C", weather_state.current_temp);
            lv_obj_clear_flag(lbl_outside, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_outside, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Waste: show if user enabled AND next pickup is within the lead window. */
    int show_waste = 0;
    char wtype[40] = "", wdate_iso[16] = "", waste_text[60] = "";
    if (settings.show_dim_waste && settings.dim_waste_lead_days > 0 &&
        waste_state.connected) {
        waste_next_pickup(wdate_iso, sizeof(wdate_iso), wtype, sizeof(wtype));
        if (wdate_iso[0]) {
            /* days_until = pickup_date − today */
            struct tm pt = {0};
            pt.tm_year = atoi(wdate_iso)     - 1900;
            pt.tm_mon  = atoi(wdate_iso + 5) - 1;
            pt.tm_mday = atoi(wdate_iso + 8);
            time_t pickup = mktime(&pt);
            time_t now_t = time(NULL);
            struct tm nt; localtime_r(&now_t, &nt);
            nt.tm_hour = 0; nt.tm_min = 0; nt.tm_sec = 0;
            time_t midnight = mktime(&nt);
            long days_until = (long)((pickup - midnight) / 86400);
            /* lead_days N => show if pickup is within N days (inclusive),
               0 => off entirely. */
            if (days_until >= 0 && days_until <= settings.dim_waste_lead_days) {
                show_waste = 1;
                const char * when =
                      (days_until == 0) ? "Vandaag"
                    : (days_until == 1) ? "Morgen"
                    : NULL;
                if (when) snprintf(waste_text, sizeof(waste_text), "%s: %s", when, wtype);
                else      snprintf(waste_text, sizeof(waste_text), "Over %ld d: %s",
                                   days_until, wtype);
            }
        }
    }
    if (waste_icon) {
        if (show_waste) lv_obj_clear_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);
        else            lv_obj_add_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (lbl_waste) {
        if (show_waste) {
            lv_label_set_text(lbl_waste, waste_text);
            lv_obj_clear_flag(lbl_waste, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_waste, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_invalidate(scr_root);
}

lv_obj_t * screen_dim_create(void) {
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Whole screen is a wake target. */
    lv_obj_add_flag(scr_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_root, on_wake_tap, LV_EVENT_CLICKED, NULL);

    /* Clock — custom 96pt Montserrat (digits + ':' + space only,
       generated via lv_font_conv into lv_font_montserrat_96_custom.c). */
    lbl_clock = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_96_custom, 0);
    lv_label_set_text(lbl_clock, "--:--");
    lv_obj_align(lbl_clock, LV_ALIGN_CENTER, 0, -130);

    /* All labels positioned against screen center with explicit Y offsets so
       different content widths can't drift them out of alignment. */
    lbl_date = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_date, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_date, "");
    lv_obj_align(lbl_date, LV_ALIGN_CENTER, 0, -50);

    lbl_temp = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_temp, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_temp, &lv_font_montserrat_48, 0);
    lv_label_set_text(lbl_temp, "-- C");
    lv_obj_align(lbl_temp, LV_ALIGN_CENTER, 0, 45);

    lbl_setpoint = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_setpoint, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_setpoint, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_setpoint, "to -- C");
    lv_obj_align(lbl_setpoint, LV_ALIGN_CENTER, 0, 115);

    /* Active program — sits directly under the setpoint and above the
       air-quality / pressure metrics strip. Same vertical-ordering as the
       home thermostat tile so the eye-tracking matches. */
    lbl_program = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_program, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_program, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_program, "--");
    lv_obj_align(lbl_program, LV_ALIGN_CENTER, 0, 150);

    /* Air-quality + CH-pressure strip — moved below program so the TVOC /
       ppm / bar / AQ block doesn't shove the manual label off the layout. */
    lbl_metrics = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_metrics, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_metrics, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_metrics, "");
    lv_obj_align(lbl_metrics, LV_ALIGN_CENTER, 0, 190);

    /* Burner state — between metrics and the forecast strip.
       CH-heating shows "-> 90 C" (red). DHW shows the faucet+drop pair
       slightly to the left of where the text would be. Idle hides both. */
    lbl_burner = lv_label_create(scr_root);
    lv_obj_set_style_text_font(lbl_burner, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_burner, "");
    lv_obj_align(lbl_burner, LV_ALIGN_CENTER, 0, 225);
    lv_obj_add_flag(lbl_burner, LV_OBJ_FLAG_HIDDEN);

    dim_img_faucet = lv_img_create(scr_root);
    lv_img_set_src(dim_img_faucet, &icon_faucet);
    lv_img_set_zoom(dim_img_faucet, 256);
    lv_obj_set_style_img_recolor(dim_img_faucet, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_faucet, 255, 0);
    lv_obj_align(dim_img_faucet, LV_ALIGN_CENTER, -20, 225);
    lv_obj_add_flag(dim_img_faucet, LV_OBJ_FLAG_HIDDEN);

    dim_img_drop = lv_img_create(scr_root);
    lv_img_set_src(dim_img_drop, &icon_drop);
    lv_img_set_zoom(dim_img_drop, 256);
    lv_obj_set_style_img_recolor(dim_img_drop, lv_color_hex(0x66bbff), 0);
    lv_obj_set_style_img_recolor_opa(dim_img_drop, 255, 0);
    lv_obj_align(dim_img_drop, LV_ALIGN_CENTER, 18, 235);
    lv_obj_add_flag(dim_img_drop, LV_OBJ_FLAG_HIDDEN);

    /* Weather icon (large, top-right) + outside temp underneath.
       Visibility is gated by settings.show_dim_weather in refresh_cb. */
    wx_icon = lv_img_create(scr_root);
    lv_img_set_src(wx_icon, &icon_wx_cloud_lg);
    lv_obj_set_style_img_recolor(wx_icon, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_img_recolor_opa(wx_icon, 255, 0);
    /* Native 80×80 — bigger source bitmap, no transform. */
    lv_obj_align(wx_icon, LV_ALIGN_TOP_RIGHT, -60, 50);

    lbl_outside = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_outside, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_outside, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_outside, "-- C");
    lv_obj_align(lbl_outside, LV_ALIGN_TOP_RIGHT, -30, 140);

    /* Waste — 80×80 trash icon top-LEFT (mirroring the weather block).
       Visibility + label gated by settings + lead-days window. */
    waste_icon = lv_img_create(scr_root);
    lv_img_set_src(waste_icon, &icon_trash_lg);
    lv_obj_set_style_img_recolor(waste_icon, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_img_recolor_opa(waste_icon, 255, 0);
    lv_obj_align(waste_icon, LV_ALIGN_TOP_LEFT, 60, 50);
    lv_obj_add_flag(waste_icon, LV_OBJ_FLAG_HIDDEN);

    lbl_waste = lv_label_create(scr_root);
    lv_obj_set_style_text_color(lbl_waste, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(lbl_waste, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_waste, "");
    lv_obj_align(lbl_waste, LV_ALIGN_TOP_LEFT, 30, 140);
    lv_obj_add_flag(lbl_waste, LV_OBJ_FLAG_HIDDEN);

    /* 5-day forecast strip across the bottom of dim. Black/white style:
       40×40 icon at top, day label below, temp range under that. */
    int strip_y = 514;       /* sits below the program line (CENTER + 190
                                → bottom edge ~512); was 470 which overlapped
                                "manual" once the metrics row was added. */
    int col_w   = 1024 / WEATHER_FORECAST_DAYS;
    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        int cx = i * col_w + col_w / 2;

        dim_fc_icon[i] = lv_img_create(scr_root);
        lv_img_set_src(dim_fc_icon[i], &icon_wx_cloud);
        lv_obj_set_style_img_recolor(dim_fc_icon[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_img_recolor_opa(dim_fc_icon[i], 255, 0);
        lv_obj_set_pos(dim_fc_icon[i], cx - 20, strip_y);
        lv_obj_add_flag(dim_fc_icon[i], LV_OBJ_FLAG_HIDDEN);

        dim_fc_day[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_color(dim_fc_day[i], lv_color_hex(0xbbbbbb), 0);
        lv_obj_set_style_text_font(dim_fc_day[i], &lv_font_montserrat_18, 0);
        lv_label_set_text(dim_fc_day[i], "");
        lv_obj_set_style_text_align(dim_fc_day[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(dim_fc_day[i], col_w);
        lv_obj_set_pos(dim_fc_day[i], i * col_w, strip_y + 44);
        lv_obj_add_flag(dim_fc_day[i], LV_OBJ_FLAG_HIDDEN);

        dim_fc_temp[i] = lv_label_create(scr_root);
        lv_obj_set_style_text_color(dim_fc_temp[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(dim_fc_temp[i], &lv_font_montserrat_18, 0);
        lv_label_set_text(dim_fc_temp[i], "");
        lv_obj_set_style_text_align(dim_fc_temp[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(dim_fc_temp[i], col_w);
        lv_obj_set_pos(dim_fc_temp[i], i * col_w, strip_y + 64);
        lv_obj_add_flag(dim_fc_temp[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* "tap to wake" hint moved to the top-right corner so it doesn't fight
       with the lowered forecast strip for the last 80 px of the screen. */
    lv_obj_t * hint = lv_label_create(scr_root);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_18, 0);
    lv_label_set_text(hint, "tap to wake");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 6);

    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    return scr_root;
}
