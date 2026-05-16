/*
 * Home screen — tile grid matching qt-gui's layout.
 * Big thermostat tile top-left (240*2+gap = 500 wide, 220*2+gap = 460 tall).
 * Smaller tiles laid out 4 per row to its right and below.
 *
 * Tap a tile to navigate to its detail screen.
 */
#include "screens.h"
#include "boxtalk.h"
#include "icons.h"
#include "homewizard.h"
#include "settings.h"
#include "weather.h"
#include "wastecollection.h"
#include "ventilation.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define COL_BG          0x0f1a2a
#define COL_TILE_BG     0x1a2a44
#define COL_TILE_ACCENT 0x335577
#define COL_TEXT_HI     0xffffff
#define COL_TEXT_DIM    0x88aabb
#define COL_TEMP_YELLOW 0xffcc44
#define COL_BURNER_RED  0xff6644

static lv_obj_t * scr_root = NULL;

/* Big thermostat tile widgets — updated by refresh timer */
static lv_obj_t * lbl_t_clock;
static lv_obj_t * lbl_t_temp;
static lv_obj_t * lbl_t_setpoint;
static lv_obj_t * lbl_t_burner;
static lv_obj_t * lbl_t_pressure;
static lv_obj_t * lbl_t_humidity;
static lv_obj_t * lbl_t_ppm;
static lv_obj_t * lbl_t_tvoc;
static lv_obj_t * lbl_t_date;
static lv_obj_t * lbl_t_aq;
static lv_obj_t * lbl_waste_date;
static lv_obj_t * lbl_waste_type;
static lv_obj_t * lbl_waste_date_2;
static lv_obj_t * lbl_waste_type_2;
static lv_obj_t * waste_icon_1;
static lv_obj_t * waste_icon_2;
/* Pressure-warning banner — covers the top of the Heater tile when the
 * boiler's CH water pressure falls into the "low" / "very low" zones.
 * Hidden when pressure is OK or unknown. */
static lv_obj_t * pressure_banner = NULL;
static lv_obj_t * pressure_banner_lbl = NULL;
static lv_obj_t * lbl_t_program;
static lv_obj_t * tile_img_flame;
static lv_obj_t * tile_img_faucet;
static lv_obj_t * tile_img_drop;
/* Bottom-row labels (Energy/Weather/Waste) — updated by refresh_cb */
static lv_obj_t * lbl_bot_energy;
static lv_obj_t * lbl_bot_weather;
static lv_obj_t * lbl_bot_waste;
static lv_obj_t * lbl_inbox_main;
static lv_obj_t * lbl_inbox_sub;
static lv_obj_t * water_spinner;
static lv_obj_t * forecast_box;
static lv_obj_t * lbl_outside_main;
static lv_obj_t * lbl_outside_sub;
static lv_obj_t * fc_day_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_temp_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_wind_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_icon[WEATHER_FORECAST_DAYS];
static lv_obj_t * envelope_btn;
static lv_obj_t * envelope_badge;
static lv_obj_t * envelope_badge_lbl;
static lv_obj_t * water_spinner;
static lv_obj_t * forecast_box;
static lv_obj_t * lbl_outside_main;
static lv_obj_t * lbl_outside_sub;
static lv_obj_t * fc_day_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_temp_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_wind_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_icon[WEATHER_FORECAST_DAYS];
static lv_obj_t * water_spinner;
static lv_obj_t * forecast_box;
static lv_obj_t * lbl_outside_main;
static lv_obj_t * lbl_outside_sub;
static lv_obj_t * fc_day_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_temp_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_desc_lbl[WEATHER_FORECAST_DAYS];
static lv_obj_t * fc_wind_arrow[WEATHER_FORECAST_DAYS];
static lv_obj_t * lbl_inbox_main;
static lv_obj_t * lbl_inbox_sub;
/* Bottom-row labels (Energy/Weather/Waste) — updated by refresh_cb */
static lv_obj_t * lbl_bot_energy;
static lv_obj_t * lbl_bot_weather;
static lv_obj_t * lbl_bot_waste;
static lv_obj_t * tile_img_flame;
static lv_obj_t * tile_img_faucet;
static lv_obj_t * tile_img_drop;

/* Smaller tile widgets */
static lv_obj_t * lbl_air_eco2;
static lv_obj_t * lbl_air_tvoc;
static lv_obj_t * lbl_humid_val;       /* removed widget — kept as NULL for old refs */
static lv_obj_t * lbl_energy_w;
static lv_obj_t * lbl_energy_gas;
static lv_obj_t * lbl_energy_today;
static lv_obj_t * lbl_boiler_state;
static lv_obj_t * lbl_boiler_pressure;
static lv_obj_t * vent_fan_img = NULL;
static lv_obj_t * vent_fan_wrap = NULL;
static int        vent_anim_period_ms = -1;

static lv_timer_t * refresh_timer = NULL;

/* ---------- tile builder helpers ---------- */
typedef struct {
    lv_obj_t * tile;
    lv_obj_t * title;
    lv_obj_t * value;  /* main big-ish value */
    lv_obj_t * sub;    /* sub-line under value */
} tile_t;

/* Generic tile: w x h pixels, given title, optional click handler */
static void make_tile(lv_obj_t * parent, int x, int y, int w, int h,
                      const char * title, uint32_t accent_color,
                      lv_event_cb_t click_cb, tile_t * out) {
    lv_obj_t * t = lv_obj_create(parent);
    lv_obj_set_size(t, w, h);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_style_bg_color(t, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_radius(t, 14, 0);
    lv_obj_set_style_pad_all(t, 10, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    if (click_cb) {
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, click_cb, LV_EVENT_CLICKED, NULL);
    }

    /* Thin accent bar on top */
    lv_obj_t * bar = lv_obj_create(t);
    lv_obj_set_size(bar, w - 20, 4);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(accent_color), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * lbl_title = lv_label_create(t);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_title, title);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 0, 14);

    out->tile = t;
    out->title = lbl_title;
    out->value = NULL;
    out->sub = NULL;
}

/* ---------- navigation handlers ---------- */
static void open_thermostat(lv_event_t * e) {
    (void)e;
    ui_push(screen_thermostat_create());
}
/* +/- on the tile: child clicks don't bubble to parent, so these
   don't navigate. They just adjust setpoint in place. */
static void on_tile_sp_up(lv_event_t * e)   { (void)e; boxtalk_setpoint_increase(); }
static void on_tile_sp_down(lv_event_t * e) { (void)e; boxtalk_setpoint_decrease(); }

/* ---------- program-picker modal ---------- */
typedef struct {
    int        state_value;   /* 0..3 = named scheme, -1 = Manual */
    lv_obj_t * modal;
} picker_entry_t;
static picker_entry_t picker_entries[5];

static void picker_close_cb(lv_event_t * e) {
    lv_obj_t * modal = lv_event_get_user_data(e);
    if (modal) lv_obj_del(modal);
}

static void picker_apply_cb(lv_event_t * e) {
    picker_entry_t * pe = lv_event_get_user_data(e);
    if (!pe) return;
    if (pe->state_value < 0) boxtalk_set_manual();
    else                     boxtalk_set_program(pe->state_value);
    if (pe->modal) lv_obj_del(pe->modal);
}

static void on_program_tap(lv_event_t * e) {
    (void)e;
    lv_obj_t * modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(modal, 1024, 600);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, 200, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(modal);
    lv_label_set_text(title, "Select program");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);

    const char* names[]  = {"Comfort", "Home", "Sleep", "Away", "Manual"};
    int         values[] = {0, 1, 2, 3, -1};
    uint32_t    colors[] = {0xff8866, 0x66cc88, 0x4466cc, 0xaa66ff, 0xffaa44};
    for (int i = 0; i < 5; i++) {
        picker_entries[i].state_value = values[i];
        picker_entries[i].modal       = modal;
        lv_obj_t * btn = lv_btn_create(modal);
        lv_obj_set_size(btn, 170, 220);
        lv_obj_set_pos(btn, 30 + i * 195, 160);
        lv_obj_set_style_bg_color(btn, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_radius(btn, 16, 0);
        lv_obj_add_event_cb(btn, picker_apply_cb, LV_EVENT_CLICKED, &picker_entries[i]);
        lv_obj_t * lbl = lv_label_create(btn);
        lv_label_set_text(lbl, names[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
        lv_obj_center(lbl);
    }

    lv_obj_t * cancel = lv_btn_create(modal);
    lv_obj_set_size(cancel, 220, 70);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(cancel, 14, 0);
    lv_obj_add_event_cb(cancel, picker_close_cb, LV_EVENT_CLICKED, modal);
    lv_obj_t * cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_22, 0);
    lv_obj_center(cancel_lbl);
}
static void open_placeholder(lv_event_t * e) {
    (void)e;  /* TODO: per-tile detail screens */
}
static void open_vent(lv_event_t * e) {
    (void)e;
    ui_push(screen_vent_remote_create());
}

/* +/- buttons send a direct PWM bump (~15/255 ≈ 6%-step). e->user_data is
 * the int delta cast to a pointer. */
static void on_vent_bump(lv_event_t * e) {
    intptr_t d = (intptr_t)lv_event_get_user_data(e);
    vent_bump_speed((int)d);
}

/* The remaining corner buttons send virtual-remote commands directly.
 * user_data is a literal C string (low/high/auto). The async send keeps
 * the LVGL event loop snappy — the HTTP roundtrip used to block ~1 s. */
static void on_vent_mode(lv_event_t * e) {
    const char * cmd = (const char *)lv_event_get_user_data(e);
    if (cmd) vent_send_vremote_async(cmd);
}

/* The Low/High pair collapsed into a single switch:
 *   off (left)  → vremotecmd=low
 *   on  (right) → vremotecmd=high
 * A small "Low/High" label sits above so the meaning is obvious. */
static void on_vent_switch(lv_event_t * e) {
    lv_obj_t * sw = lv_event_get_target(e);
    int checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    vent_send_vremote_async(checked ? "high" : "low");
}

/* Timer button cycles 10 → 20 → 30 → off and updates its own label.
 * "off" maps to vremotecmd=auto (the most natural resting state). */
static lv_obj_t * vent_timer_lbl = NULL;
static int        vent_timer_step = 0;
static void on_vent_timer(lv_event_t * e) {
    (void)e;
    vent_timer_step = (vent_timer_step + 1) & 3;
    const char * cmd; const char * label;
    switch (vent_timer_step) {
        case 1:  cmd = "timer1"; label = "10m"; break;
        case 2:  cmd = "timer2"; label = "20m"; break;
        case 3:  cmd = "timer3"; label = "30m"; break;
        default: cmd = "auto";   label = "Timer"; vent_timer_step = 0; break;
    }
    if (vent_timer_lbl) lv_label_set_text(vent_timer_lbl, label);
    vent_send_vremote_async(cmd);
}

/* Apply rotation animation directly to the fan image. TRUE_COLOR_ALPHA
 * format rotates fine via lv_img_set_angle. */
static void vent_fan_anim_cb(void * obj, int32_t v) {
    lv_img_set_angle((lv_obj_t *)obj, v);
}
static void vent_apply_fan_anim(int pct) {
    if (!vent_fan_img) return;
    /* Fan effectively off below ~3%: stop the animation entirely so the
       icon parks at its current angle instead of spinning misleadingly. */
    if (pct < 3) {
        if (vent_anim_period_ms == 0) return;     /* already stopped */
        vent_anim_period_ms = 0;
        lv_anim_del(vent_fan_img, NULL);
        return;
    }
    /* Linear: 600ms/turn at 100% → 3000ms/turn at ~5%. The on-screen
       sweep can't match real fan rpm (a 3000-rpm fan is 50 turns/sec —
       a flicker) but the *relative* speed should track ExhFanSpeed so
       100% feels meaningfully faster than 25%. */
    int period = 600 + (100 - pct) * 24;
    if (period < 400)  period = 400;
    if (period > 3000) period = 3000;
    if (period == vent_anim_period_ms) return;
    vent_anim_period_ms = period;
    lv_anim_del(vent_fan_img, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, vent_fan_img);
    lv_anim_set_exec_cb(&a, vent_fan_anim_cb);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_time(&a, period);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}
static void open_settings(lv_event_t * e) {
    (void)e;
    ui_push(screen_settings_create());
}
static void open_stats(lv_event_t * e) {
    (void)e;
    ui_push(screen_stats_create());
}
static void open_forecast(lv_event_t * e) {
    (void)e;
    fprintf(stderr, "[ui] forecast tap\n");
    ui_push(screen_forecast_create());
}
static void open_inbox(lv_event_t * e) {
    (void)e;
    screen_inbox_show();
}

/* ---------- refresh timer ---------- */
static void refresh_cb(lv_timer_t * t) {
    (void)t;

    /* Clock on thermostat tile */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk[16];
    strftime(clk, sizeof(clk), "%H:%M", &tm);
    lv_label_set_text(lbl_t_clock, clk);
    if (lbl_t_date) {
        char dt[48];
        strftime(dt, sizeof(dt), "%a %d %b", &tm);
        lv_label_set_text(lbl_t_date, dt);
    }

    if (toon_state.indoor_temp > 0)
        lv_label_set_text_fmt(lbl_t_temp, "%.1f C", display_indoor_temp(toon_state.indoor_temp));
    if (toon_state.setpoint > 0)
        lv_label_set_text_fmt(lbl_t_setpoint, "to %.1f C", toon_state.setpoint);

    /* Active scheme (Comfort/Home/Sleep/Away) or "Manual" if overridden. */
    lv_label_set_text(lbl_t_program, program_label());
    if (toon_state.active_state < 0)
        lv_obj_set_style_text_color(lbl_t_program, lv_color_hex(0xffaa44), 0); /* manual = amber */
    else
        lv_obj_set_style_text_color(lbl_t_program, lv_color_hex(COL_TEXT_DIM), 0);

    /* Active scheme (Comfort/Home/Sleep/Away) or "Manual" if overridden. */
    lv_label_set_text(lbl_t_program, program_label());
    if (toon_state.active_state < 0)
        lv_obj_set_style_text_color(lbl_t_program, lv_color_hex(0xffaa44), 0); /* manual = amber */
    else
        lv_obj_set_style_text_color(lbl_t_program, lv_color_hex(COL_TEXT_DIM), 0);

    /* Drop the "heating" / "hot water" / "idle" word — the flame / faucet
     * icons say it more clearly. The label is reused to print just the CH
     * target temp when the boiler is firing ("-> 90 C"); it stays empty
     * for DHW (the faucet icon is sufficient) and for idle. */
    if (toon_state.burner_on) {
        if (toon_state.ch_setpoint > 0)
            lv_label_set_text_fmt(lbl_t_burner, "-> %.0f C",
                                  toon_state.ch_setpoint);
        else
            lv_label_set_text(lbl_t_burner, "");
        lv_obj_set_style_text_color(lbl_t_burner, lv_color_hex(COL_BURNER_RED), 0);
        lv_obj_clear_flag(tile_img_flame,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tile_img_faucet, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tile_img_drop,   LV_OBJ_FLAG_HIDDEN);
    } else if (toon_state.dhw_on) {
        lv_label_set_text(lbl_t_burner, "");
        lv_obj_add_flag(tile_img_flame, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(tile_img_faucet, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(tile_img_drop,   LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(lbl_t_burner, "");
        lv_obj_add_flag(tile_img_flame,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tile_img_faucet, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tile_img_drop,   LV_OBJ_FLAG_HIDDEN);
    }

    if (lbl_t_pressure) {
        if (toon_state.water_pressure > 0.1f)
            lv_label_set_text_fmt(lbl_t_pressure, "%.1f bar",
                                  toon_state.water_pressure);
        else
            lv_label_set_text(lbl_t_pressure, "-- bar");
    }
    /* Low-pressure banner. Thresholds: <0.6 = critical (red), <0.8 =
       warning (amber). >=0.8 or unknown (0) → hidden. */
    if (pressure_banner && pressure_banner_lbl) {
        float p = toon_state.water_pressure;
        if (p > 0.1f && p < 0.6f) {
            lv_obj_set_style_bg_color(pressure_banner,
                                      lv_color_hex(0xff3344), 0);
            lv_obj_set_style_bg_opa(pressure_banner, LV_OPA_COVER, 0);
            lv_label_set_text_fmt(pressure_banner_lbl,
                                  "CH water pressure CRITICAL: %.1f bar", p);
            lv_obj_clear_flag(pressure_banner, LV_OBJ_FLAG_HIDDEN);
        } else if (p > 0.1f && p < 0.8f) {
            lv_obj_set_style_bg_color(pressure_banner,
                                      lv_color_hex(0xffcc44), 0);
            lv_obj_set_style_bg_opa(pressure_banner, LV_OPA_COVER, 0);
            lv_label_set_text_fmt(pressure_banner_lbl,
                                  "CH water pressure low: %.1f bar", p);
            lv_obj_clear_flag(pressure_banner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(pressure_banner, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (lbl_t_humidity && toon_state.humidity > 0)
        lv_label_set_text_fmt(lbl_t_humidity, "RH %.0f%%",
                              toon_state.humidity);
    if (lbl_t_ppm && toon_state.eco2)
        lv_label_set_text_fmt(lbl_t_ppm, "%d ppm", toon_state.eco2);
    if (lbl_t_tvoc && toon_state.tvoc)
        lv_label_set_text_fmt(lbl_t_tvoc, "TVOC %d", toon_state.tvoc);
    if (lbl_t_aq) {
        const char * aql = air_quality_label(toon_state.eco2, toon_state.tvoc);
        if (*aql) {
            lv_label_set_text_fmt(lbl_t_aq, "Air: %s", aql);
            lv_obj_set_style_text_color(lbl_t_aq,
                lv_color_hex(air_quality_color(toon_state.eco2, toon_state.tvoc)),
                0);
        } else {
            lv_label_set_text(lbl_t_aq, "");
        }
    }

    /* (Air quality tile removed — those readings now live on the
       Heater bottom strip.) */

    /* Waste tile (top + next-up rows). Each row gets the icon and accent
       colour for its type label. */
    if (lbl_waste_date && lbl_waste_type) {
        waste_pickup_t p1 = {{0}}, p2 = {{0}};
        int n = waste_state.connected ? waste_next_2_pickups(&p1, &p2) : 0;
        if (n >= 1) {
            int mo = atoi(p1.date + 5), d = atoi(p1.date + 8);
            lv_label_set_text_fmt(lbl_waste_date, "%d-%d", d, mo);
            lv_label_set_text(lbl_waste_type, p1.labels);
            if (waste_icon_1) {
                lv_img_set_src(waste_icon_1, waste_icon_for_label(p1.labels));
                lv_obj_set_style_img_recolor(waste_icon_1,
                    lv_color_hex(waste_accent_for_label(p1.labels)), 0);
            }
        } else {
            lv_label_set_text(lbl_waste_date,
                              waste_state.connected ? "--" : "...");
            lv_label_set_text(lbl_waste_type,
                              waste_state.connected ? "geen" : "");
        }
        if (n >= 2 && lbl_waste_date_2 && lbl_waste_type_2 && waste_icon_2) {
            int mo = atoi(p2.date + 5), d = atoi(p2.date + 8);
            lv_label_set_text_fmt(lbl_waste_date_2, "%d-%d", d, mo);
            lv_label_set_text(lbl_waste_type_2, p2.labels);
            lv_img_set_src(waste_icon_2, waste_icon_for_label(p2.labels));
            lv_obj_set_style_img_recolor(waste_icon_2,
                lv_color_hex(waste_accent_for_label(p2.labels)), 0);
            lv_obj_clear_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
        } else if (waste_icon_2) {
            lv_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);
            if (lbl_waste_date_2) lv_label_set_text(lbl_waste_date_2, "");
            if (lbl_waste_type_2) lv_label_set_text(lbl_waste_type_2, "");
        }
    }

    /* Humidity tile is gone — humidity now lives on the Heater bottom strip.
       Keep the guarded write for any legacy reference. */
    if (toon_state.humidity > 0 && lbl_humid_val)
        lv_label_set_text_fmt(lbl_humid_val, "%.0f %%", toon_state.humidity);

    /* Energy tile — live power + cumulative gas. "Today" stays blank until
       we track a midnight-baseline; intent is to show kWh-since-midnight
       once that book-keeping lands. */
    if (lbl_energy_w) {
        if (hw_state.connected_p1)
            lv_label_set_text_fmt(lbl_energy_w, "%.0f W", hw_state.power_w);
        else
            lv_label_set_text(lbl_energy_w, "P1 offline");
    }
    if (lbl_energy_gas && hw_state.connected_p1)
        lv_label_set_text_fmt(lbl_energy_gas, "%.0f m3 gas", hw_state.gas_m3);

    /* Vent tile: setpoint % above, fan rpm below, spinning fan icon centre. */
    if (lbl_boiler_state) {
        if (vent_state.connected)
            lv_label_set_text_fmt(lbl_boiler_state, "%d %%", vent_state.speed_pct);
        else
            lv_label_set_text(lbl_boiler_state, "-- %");
    }
    if (lbl_boiler_pressure) {
        if (vent_state.connected)
            lv_label_set_text_fmt(lbl_boiler_pressure, "%d rpm", vent_state.fan_rpm);
        else
            lv_label_set_text(lbl_boiler_pressure, "-- rpm");
    }
    /* spin speed tracks the actual exhaust-fan %, not just the setpoint */
    if (vent_state.connected)
        vent_apply_fan_anim(vent_state.speed_pct);

    /* Energy bottom tile: live power + cumulative gas. */
    if (lbl_bot_energy) {
        if (hw_state.connected_p1) {
            lv_label_set_text_fmt(lbl_bot_energy, "%.0f W\n%.0f m3 gas",
                                  hw_state.power_w, hw_state.gas_m3);
        } else {
            lv_label_set_text(lbl_bot_energy, "P1 offline");
        }
    }
    if (lbl_bot_weather && lv_label_get_text(lbl_bot_weather)
        && strcmp(lv_label_get_text(lbl_bot_weather), "...") == 0)
        lv_label_set_text(lbl_bot_weather, "(soon)");
    if (lbl_bot_waste) {
        if (waste_state.connected) {
            char date[16], labels[40];
            waste_next_pickup(date, sizeof(date), labels, sizeof(labels));
            if (date[0]) {
                /* "YYYY-MM-DD" → "DD-M" short. Keep label tight. */
                int y = atoi(date), mo = atoi(date+5), d = atoi(date+8);
                (void)y;
                lv_label_set_text_fmt(lbl_bot_waste, "%d-%d  %s", d, mo, labels);
            } else {
                lv_label_set_text(lbl_bot_waste, "geen");
            }
        } else {
            lv_label_set_text(lbl_bot_waste, "...");
        }
    }

    /* Water tile (replaces Inbox placeholder) — total + live l/min.
       Spinner is shown only when water is actively flowing. */
    if (lbl_inbox_main && hw_state.connected_water) {
        lv_label_set_text_fmt(lbl_inbox_main, "%.2f m3", hw_state.water_total_m3);
    }
    if (lbl_inbox_sub && hw_state.connected_water) {
        lv_label_set_text_fmt(lbl_inbox_sub, "%.1f L/min", hw_state.water_lpm);
    }
    if (water_spinner) {
        if (hw_state.water_lpm > 0.01f)
            lv_obj_clear_flag(water_spinner, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(water_spinner, LV_OBJ_FLAG_HIDDEN);
    }

    /* Outside tile — current temp + short description. */
    if (lbl_outside_main) {
        if (weather_state.connected)
            lv_label_set_text_fmt(lbl_outside_main, "%.1f C", weather_state.current_temp);
        else
            lv_label_set_text(lbl_outside_main, "-- C");
    }

    /* Forecast band — prefer the 3-hourly forecast when the location id
       resolves, fall back to the national 5-day feed otherwise. */
    if (weather_state.hour_count > 0) {
        for (int i = 0; i < weather_state.hour_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_hour_t * h = &weather_state.hours[i];
            lv_label_set_text(fc_day_lbl[i], h->label);
            lv_label_set_text_fmt(fc_temp_lbl[i], "%.0f\xc2\xb0",
                                  h->temperature);
            lv_img_set_src(fc_icon[i], weather_icon_for(h->icon));
            if (h->wind_dir[0]) {
                lv_label_set_text_fmt(fc_wind_lbl[i], "%s %d Bft",
                                      h->wind_dir, h->wind_bft);
                int ang = wind_dir_angle(h->wind_dir);
                if (fc_wind_arrow[i] && ang >= 0) {
                    lv_img_set_angle(fc_wind_arrow[i], ang);
                    lv_obj_clear_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                } else if (fc_wind_arrow[i]) {
                    lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                lv_label_set_text(fc_wind_lbl[i], "");
                if (fc_wind_arrow[i])
                    lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        for (int i = 0; i < weather_state.day_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_day_t * d = &weather_state.days[i];
            lv_label_set_text(fc_day_lbl[i], d->day);
            lv_label_set_text_fmt(fc_temp_lbl[i],
                                  "%.0f\xc2\xb0 (%.0f\xc2\xb0)",
                                  d->max_temp, d->min_temp);
            lv_img_set_src(fc_icon[i], weather_icon_for(d->icon));
            if (d->wind_dir[0]) {
                lv_label_set_text_fmt(fc_wind_lbl[i], "%s %d Bft",
                                      d->wind_dir, d->wind_bft);
                int ang = wind_dir_angle(d->wind_dir);
                if (fc_wind_arrow[i] && ang >= 0) {
                    lv_img_set_angle(fc_wind_arrow[i], ang);
                    lv_obj_clear_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                lv_label_set_text(fc_wind_lbl[i], "");
                if (fc_wind_arrow[i])
                    lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    /* Show the spinner only while water is actually flowing. */
    if (water_spinner) {
        if (hw_state.connected_water && hw_state.water_lpm > 0.05f)
            lv_obj_clear_flag(water_spinner, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(water_spinner, LV_OBJ_FLAG_HIDDEN);
    }

    /* Outside tile — current temp + short description on second line. */
    if (lbl_outside_main && weather_state.connected) {
        lv_label_set_text_fmt(lbl_outside_main, "%.1f C",
                              weather_state.current_temp);
    }

    /* Forecast band — splat-recovery left two more copies of this
       writer further down; gate them on hour_count too so the hourly
       data the first writer paints isn't immediately overwritten with
       the daily values. */
    if (weather_state.hour_count == 0) {
        for (int i = 0; i < weather_state.day_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_day_t * d = &weather_state.days[i];
            if (fc_day_lbl[i])
                lv_label_set_text(fc_day_lbl[i], d->day);
            if (fc_temp_lbl[i])
                lv_label_set_text_fmt(fc_temp_lbl[i],
                                      "%.0f\xc2\xb0 (%.0f\xc2\xb0)",
                                      d->max_temp, d->min_temp);
            if (fc_desc_lbl[i])
                lv_label_set_text(fc_desc_lbl[i], d->desc);
        }
    }

    /* Energy bottom tile: live power + cumulative gas. */
    if (lbl_bot_energy) {
        if (hw_state.connected_p1) {
            lv_label_set_text_fmt(lbl_bot_energy, "%.0f W\n%.0f m3 gas",
                                  hw_state.power_w, hw_state.gas_m3);
        } else {
            lv_label_set_text(lbl_bot_energy, "P1 offline");
        }
    }
    if (lbl_bot_weather && lv_label_get_text(lbl_bot_weather)
        && strcmp(lv_label_get_text(lbl_bot_weather), "...") == 0)
        lv_label_set_text(lbl_bot_weather, "(soon)");
    if (lbl_bot_waste) {
        if (waste_state.connected) {
            char date[16], labels[40];
            waste_next_pickup(date, sizeof(date), labels, sizeof(labels));
            if (date[0]) {
                /* "YYYY-MM-DD" → "DD-M" short. Keep label tight. */
                int y = atoi(date), mo = atoi(date+5), d = atoi(date+8);
                (void)y;
                lv_label_set_text_fmt(lbl_bot_waste, "%d-%d  %s", d, mo, labels);
            } else {
                lv_label_set_text(lbl_bot_waste, "geen");
            }
        } else {
            lv_label_set_text(lbl_bot_waste, "...");
        }
    }

    /* Water tile (replaces Inbox placeholder) — total + live l/min.
       Spinner is shown only when water is actively flowing. */
    if (lbl_inbox_main && hw_state.connected_water) {
        lv_label_set_text_fmt(lbl_inbox_main, "%.2f m3", hw_state.water_total_m3);
    }
    if (lbl_inbox_sub && hw_state.connected_water) {
        lv_label_set_text_fmt(lbl_inbox_sub, "%.1f L/min", hw_state.water_lpm);
    }
    if (water_spinner) {
        if (hw_state.water_lpm > 0.01f)
            lv_obj_clear_flag(water_spinner, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(water_spinner, LV_OBJ_FLAG_HIDDEN);
    }

    /* Outside tile — current temp + short description. */
    if (lbl_outside_main) {
        if (weather_state.connected)
            lv_label_set_text_fmt(lbl_outside_main, "%.1f C", weather_state.current_temp);
        else
            lv_label_set_text(lbl_outside_main, "-- C");
    }

    /* Forecast band — third splat-recovery copy of the daily writer;
       gate on hour_count so we don't clobber the hourly slots. */
    if (weather_state.hour_count == 0) {
        for (int i = 0; i < weather_state.day_count
                     && i < WEATHER_FORECAST_DAYS; i++) {
            const weather_day_t * d = &weather_state.days[i];
            lv_label_set_text(fc_day_lbl[i], d->day);
            lv_label_set_text_fmt(fc_temp_lbl[i],
                                  "%.0f\xc2\xb0 (%.0f\xc2\xb0)",
                                  d->max_temp, d->min_temp);
            lv_img_set_src(fc_icon[i], weather_icon_for(d->icon));
            if (d->wind_dir[0])
                lv_label_set_text_fmt(fc_wind_lbl[i], "%s %d Bft",
                                      d->wind_dir, d->wind_bft);
            else
                lv_label_set_text(fc_wind_lbl[i], "");
        }
    }

    /* Energy bottom tile: live power + cumulative gas. */
    if (lbl_bot_energy) {
        if (hw_state.connected_p1) {
            lv_label_set_text_fmt(lbl_bot_energy, "%.0f W\n%.0f m3 gas",
                                  hw_state.power_w, hw_state.gas_m3);
        } else {
            lv_label_set_text(lbl_bot_energy, "P1 offline");
        }
    }
    /* (Old splat-recovered "(soon)" override block removed — it was
       clobbering the real waste-pickup text written earlier in this
       refresh callback.) */

    /* Water tile (replaces Inbox placeholder) — total + live l/min. */
    if (lbl_inbox_main && hw_state.connected_water) {
        lv_label_set_text_fmt(lbl_inbox_main, "%.2f m3", hw_state.water_total_m3);
    }
    if (lbl_inbox_sub && hw_state.connected_water) {
        lv_label_set_text_fmt(lbl_inbox_sub, "%.1f L/min", hw_state.water_lpm);
    }

    lv_obj_invalidate(scr_root);
}

/* ---------- screen builder ---------- */
lv_obj_t * screen_home_create(void) {
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Layout (1024x600):
         Big thermostat tile:  20,20  520x420   (col 0-1 spanned, row 0-1 spanned)
         Tile col 2 row 0:    560,20  220x200   Air quality
         Tile col 3 row 0:    790,20  220x200   Humidity
         Tile col 2 row 1:    560,230 220x200   Boiler / heating
         Tile col 3 row 1:    790,230 220x200   Inbox (placeholder)
         Bottom row (4 small tiles): 20,450 -> 1004,580
           Energy (P1), Buienradar, Waste, Settings
    */

    /* --- Big thermostat tile (height 410 — leaves room for forecast band) --- */
    lv_obj_t * th = lv_obj_create(scr_root);
    lv_obj_set_size(th, 520, 410);
    lv_obj_set_pos(th, 20, 20);
    lv_obj_set_style_bg_color(th, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_border_width(th, 0, 0);
    lv_obj_set_style_radius(th, 18, 0);
    lv_obj_set_style_pad_all(th, 20, 0);
    lv_obj_clear_flag(th, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(th, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(th, open_thermostat, LV_EVENT_CLICKED, NULL);

    lbl_t_clock = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_clock, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_t_clock, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_t_clock, "--:--");
    lv_obj_align(lbl_t_clock, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Date directly under the clock so the top-left corner is a full
       date+time block. */
    lbl_t_date = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_date, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_t_date, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_t_date, "");
    lv_obj_align(lbl_t_date, LV_ALIGN_TOP_LEFT, 0, 36);

    /* Air-quality badge — top-right of the Heater tile, coloured per
       eCO2/TVOC bucket (green/lime/yellow/orange/red). */
    lbl_t_aq = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_aq, lv_color_hex(0x66cc88), 0);
    lv_obj_set_style_text_font(lbl_t_aq, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_t_aq, "");
    lv_obj_align(lbl_t_aq, LV_ALIGN_TOP_RIGHT, 0, 6);

    /* "Thermostat" title removed per request — the gear icon and tile
       content already make this tile's role obvious. */

    lbl_t_temp = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_temp, lv_color_hex(COL_TEMP_YELLOW), 0);
    lv_obj_set_style_text_font(lbl_t_temp, &lv_font_montserrat_48, 0);
    lv_label_set_text(lbl_t_temp, "-- C");
    lv_obj_align(lbl_t_temp, LV_ALIGN_CENTER, 0, -90);

    /* Setpoint row: [-] setpoint [+]. Children, so their clicks don't
       bubble into the tile's open-detail handler. */
    lv_obj_t * sp_row = lv_obj_create(th);
    lv_obj_set_size(sp_row, 460, 84);
    lv_obj_align(sp_row, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_opa(sp_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp_row, 0, 0);
    lv_obj_set_style_pad_all(sp_row, 0, 0);
    lv_obj_clear_flag(sp_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * btn_dn = lv_btn_create(sp_row);
    lv_obj_set_size(btn_dn, 84, 76);
    lv_obj_align(btn_dn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_dn, lv_color_hex(COL_TILE_ACCENT), 0);
    lv_obj_set_style_radius(btn_dn, 12, 0);
    lv_obj_set_ext_click_area(btn_dn, 12);
    lv_obj_add_event_cb(btn_dn, on_tile_sp_down, LV_EVENT_CLICKED, NULL);
    lv_obj_t * btn_dn_lbl = lv_label_create(btn_dn);
    lv_label_set_text(btn_dn_lbl, "-");
    lv_obj_set_style_text_font(btn_dn_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(btn_dn_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_center(btn_dn_lbl);

    lbl_t_setpoint = lv_label_create(sp_row);
    lv_obj_set_style_text_color(lbl_t_setpoint, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_t_setpoint, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_t_setpoint, "to -- C");
    lv_obj_align(lbl_t_setpoint, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t * btn_up = lv_btn_create(sp_row);
    lv_obj_set_size(btn_up, 84, 76);
    lv_obj_align(btn_up, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_up, lv_color_hex(COL_TILE_ACCENT), 0);
    lv_obj_set_style_radius(btn_up, 12, 0);
    lv_obj_set_ext_click_area(btn_up, 12);
    lv_obj_add_event_cb(btn_up, on_tile_sp_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t * btn_up_lbl = lv_label_create(btn_up);
    lv_label_set_text(btn_up_lbl, "+");
    lv_obj_set_style_text_font(btn_up_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(btn_up_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_center(btn_up_lbl);

    /* Active program (Comfort/Home/Sleep/Away/Manual). Wrapped in a clickable
       pill so taps land on a generous hit area and open the picker. */
    lv_obj_t * prog_pill = lv_obj_create(th);
    lv_obj_set_size(prog_pill, 240, 52);
    lv_obj_align(prog_pill, LV_ALIGN_CENTER, 0, 70);
    lv_obj_set_style_bg_color(prog_pill, lv_color_hex(0x1a3a5a), 0);
    lv_obj_set_style_radius(prog_pill, 22, 0);
    lv_obj_set_style_border_width(prog_pill, 1, 0);
    lv_obj_set_style_border_color(prog_pill, lv_color_hex(COL_TILE_ACCENT), 0);
    lv_obj_set_style_pad_all(prog_pill, 0, 0);
    lv_obj_clear_flag(prog_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(prog_pill, 14);
    lv_obj_add_event_cb(prog_pill, on_program_tap, LV_EVENT_CLICKED, NULL);

    lbl_t_program = lv_label_create(prog_pill);
    lv_obj_set_style_text_color(lbl_t_program, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_t_program, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_t_program, "--");
    lv_obj_center(lbl_t_program);

    /* Burner status sits above the metrics strip. The icons follow it so
       the flame/faucet stays paired with the text. */
    lbl_t_burner = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_burner, lv_color_hex(COL_BURNER_RED), 0);
    lv_obj_set_style_text_font(lbl_t_burner, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_t_burner, "idle");
    lv_obj_align(lbl_t_burner, LV_ALIGN_BOTTOM_MID, 30, -40);

    /* Bottom strip: humidity | eCO2 | TVOC | water-pressure on one row.
       Font 18 keeps the 4 values from running into each other on a
       520-wide tile. */
    lbl_t_humidity = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_humidity, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_t_humidity, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_t_humidity, "RH --%");
    lv_obj_align(lbl_t_humidity, LV_ALIGN_BOTTOM_LEFT, 12, -4);

    lbl_t_ppm = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_ppm, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_t_ppm, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_t_ppm, "-- ppm");
    lv_obj_align(lbl_t_ppm, LV_ALIGN_BOTTOM_LEFT, 140, -4);

    lbl_t_tvoc = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_tvoc, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_t_tvoc, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_t_tvoc, "TVOC --");
    lv_obj_align(lbl_t_tvoc, LV_ALIGN_BOTTOM_LEFT, 280, -4);

    lbl_t_pressure = lv_label_create(th);
    lv_obj_set_style_text_color(lbl_t_pressure, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_t_pressure, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_t_pressure, "-- bar");
    lv_obj_align(lbl_t_pressure, LV_ALIGN_BOTTOM_RIGHT, -12, -4);

    /* CH-pressure warning banner. Sits on top of the Heater tile so the
       user can't miss it. EVENT_BUBBLE keeps the tile clickable through
       the banner. Hidden by default; shown only when pressure is low. */
    pressure_banner = lv_obj_create(th);
    lv_obj_set_size(pressure_banner, 520, 42);
    lv_obj_align(pressure_banner, LV_ALIGN_TOP_LEFT, -10, -10);
    lv_obj_set_style_radius(pressure_banner, 0, 0);
    lv_obj_set_style_border_width(pressure_banner, 0, 0);
    lv_obj_clear_flag(pressure_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pressure_banner, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(pressure_banner, LV_OBJ_FLAG_HIDDEN);

    pressure_banner_lbl = lv_label_create(pressure_banner);
    lv_obj_set_style_text_color(pressure_banner_lbl,
                                lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(pressure_banner_lbl,
                               &lv_font_montserrat_22, 0);
    lv_label_set_text(pressure_banner_lbl, "");
    lv_obj_center(pressure_banner_lbl);
    lv_obj_add_flag(pressure_banner_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Burner indicator icons — replace the old "heating"/"hot water" word.
     * Native 32×40 source; render at full size (zoom 256) so the symbol
     * reads at a glance. Flame sits left of the target-temp label, faucet
     * & drop pair on the left for DHW. */
    tile_img_flame = lv_img_create(th);
    lv_img_set_src(tile_img_flame, &icon_flame);
    lv_img_set_zoom(tile_img_flame, 256);
    lv_obj_set_style_img_recolor(tile_img_flame, lv_color_hex(COL_BURNER_RED), 0);
    lv_obj_set_style_img_recolor_opa(tile_img_flame, 255, 0);
    lv_obj_align(tile_img_flame, LV_ALIGN_BOTTOM_MID, -20, -32);
    lv_obj_add_flag(tile_img_flame, LV_OBJ_FLAG_HIDDEN);

    tile_img_faucet = lv_img_create(th);
    lv_img_set_src(tile_img_faucet, &icon_faucet);
    lv_img_set_zoom(tile_img_faucet, 256);
    lv_obj_set_style_img_recolor(tile_img_faucet, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_img_recolor_opa(tile_img_faucet, 255, 0);
    lv_obj_align(tile_img_faucet, LV_ALIGN_BOTTOM_MID, -30, -32);
    lv_obj_add_flag(tile_img_faucet, LV_OBJ_FLAG_HIDDEN);

    tile_img_drop = lv_img_create(th);
    lv_img_set_src(tile_img_drop, &icon_drop);
    lv_img_set_zoom(tile_img_drop, 256);
    lv_obj_set_style_img_recolor(tile_img_drop, lv_color_hex(0xff3333), 0);
    lv_obj_set_style_img_recolor_opa(tile_img_drop, 255, 0);
    lv_obj_align(tile_img_drop, LV_ALIGN_BOTTOM_MID, -50, -22);
    lv_obj_add_flag(tile_img_drop, LV_OBJ_FLAG_HIDDEN);

    /* --- Waste tile: two stacked pickup rows ---
       Each row gets its own type-specific icon (newspaper for Papier,
       milk carton for Plastic/PMD, leaf for GFT, trashcan fallback) so
       the user can read at a glance what's coming when. */
    tile_t waste_big;
    make_tile(scr_root, 560, 20, 220, 200, "Waste", 0x88dd66,
              open_placeholder, &waste_big);

    waste_icon_1 = lv_img_create(waste_big.tile);
    lv_img_set_src(waste_icon_1, &icon_trash);
    lv_obj_set_style_img_recolor_opa(waste_icon_1, 255, 0);
    lv_obj_align(waste_icon_1, LV_ALIGN_TOP_LEFT, 4, 26);
    lv_obj_add_flag(waste_icon_1, LV_OBJ_FLAG_EVENT_BUBBLE);

    lbl_waste_date = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_date, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_waste_date, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_waste_date, "--");
    lv_obj_align(lbl_waste_date, LV_ALIGN_TOP_LEFT, 72, 28);

    lbl_waste_type = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_type, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_waste_type, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_waste_type, "");
    lv_obj_align(lbl_waste_type, LV_ALIGN_TOP_LEFT, 72, 62);

    /* Second-pickup icon renders at native size — lv_img_set_zoom on an
       alpha-8 source skips the recolor pass in LVGL 8.3 so the icon
       comes out invisible. Better to live with one consistent size. */
    waste_icon_2 = lv_img_create(waste_big.tile);
    lv_img_set_src(waste_icon_2, &icon_trash);
    lv_obj_set_style_img_recolor_opa(waste_icon_2, 255, 0);
    lv_obj_align(waste_icon_2, LV_ALIGN_TOP_LEFT, 4, 116);
    lv_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(waste_icon_2, LV_OBJ_FLAG_HIDDEN);

    lbl_waste_date_2 = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_date_2, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_waste_date_2, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_waste_date_2, "");
    lv_obj_align(lbl_waste_date_2, LV_ALIGN_TOP_LEFT, 72, 118);

    lbl_waste_type_2 = lv_label_create(waste_big.tile);
    lv_obj_set_style_text_color(lbl_waste_type_2, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_waste_type_2, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_waste_type_2, "");
    lv_obj_align(lbl_waste_type_2, LV_ALIGN_TOP_LEFT, 72, 152);

    /* Live Energy tile (replaces the old Humidity tile — humidity is now
       on the Heater bottom strip). Big live power on top, gas total below,
       today's kWh in the corner. */
    tile_t energy_t;
    make_tile(scr_root, 790, 20, 214, 200, "Energy", 0xaa77ff,
              open_stats, &energy_t);
    lbl_energy_w = lv_label_create(energy_t.tile);
    lv_obj_set_style_text_color(lbl_energy_w, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_energy_w, &lv_font_montserrat_48, 0);
    lv_label_set_text(lbl_energy_w, "-- W");
    lv_obj_align(lbl_energy_w, LV_ALIGN_CENTER, 0, -14);

    lbl_energy_gas = lv_label_create(energy_t.tile);
    lv_obj_set_style_text_color(lbl_energy_gas, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_energy_gas, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_energy_gas, "-- m3 gas");
    lv_obj_align(lbl_energy_gas, LV_ALIGN_BOTTOM_LEFT, 0, -4);

    lbl_energy_today = lv_label_create(energy_t.tile);
    lv_obj_set_style_text_color(lbl_energy_today, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_energy_today, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_energy_today, "");
    lv_obj_align(lbl_energy_today, LV_ALIGN_TOP_RIGHT, -4, 4);

    /* Vent tile — spinning fan in centre, manual +/- on the sides, live
       % above and rpm below the fan. Tap on the fan itself opens the remote
       (the buttons get their own click handlers so they don't bubble). */
    tile_t vent;
    make_tile(scr_root, 560, 230, 220, 200, "Vent", 0x66bbdd,
              (lv_event_cb_t)open_vent, &vent);

    /* Speed % shares the title row with the "Vent" label so the
       top-row buttons can sit cleanly below without overlap. */
    lbl_boiler_state = lv_label_create(vent.tile);
    lv_obj_set_style_text_color(lbl_boiler_state, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_boiler_state, &lv_font_montserrat_18, 0);
    lv_label_set_text(lbl_boiler_state, "-- %");
    lv_obj_align(lbl_boiler_state, LV_ALIGN_TOP_RIGHT, -4, 14);

    /* Spinning fan: 80x80 TRUE_COLOR_ALPHA icon (color baked in). Rotated
       directly with lv_img_set_angle — works reliably on this format. */
    vent_fan_img = lv_img_create(vent.tile);
    lv_img_set_src(vent_fan_img, &icon_fan);
    lv_img_set_pivot(vent_fan_img, 40, 40);
    lv_obj_align(vent_fan_img, LV_ALIGN_CENTER, 0, 14);
    lv_obj_add_flag(vent_fan_img, LV_OBJ_FLAG_EVENT_BUBBLE);  /* tap → tile */

    /* Four corner buttons — Low / High at top, Auto / Timer at bottom.
       Each issues an Itho-Wifi virtual-remote command (vremotecmd=…).
       Timer cycles 10/20/30 min and resets to "auto". */
    /* Top half: Low|High switch in place of two buttons. Bottom corners
       still hold Auto + Timer as before. */
    {
        lv_obj_t * lbl_lo = lv_label_create(vent.tile);
        lv_obj_set_style_text_color(lbl_lo, lv_color_hex(0x88aabb), 0);
        lv_obj_set_style_text_font(lbl_lo, &lv_font_montserrat_14, 0);
        lv_label_set_text(lbl_lo, "Low");
        lv_obj_align(lbl_lo, LV_ALIGN_TOP_LEFT, 16, 48);

        lv_obj_t * lbl_hi = lv_label_create(vent.tile);
        lv_obj_set_style_text_color(lbl_hi, lv_color_hex(0x88aabb), 0);
        lv_obj_set_style_text_font(lbl_hi, &lv_font_montserrat_14, 0);
        lv_label_set_text(lbl_hi, "High");
        lv_obj_align(lbl_hi, LV_ALIGN_TOP_RIGHT, -16, 48);

        lv_obj_t * sw = lv_switch_create(vent.tile);
        lv_obj_set_size(sw, 64, 26);
        lv_obj_align(sw, LV_ALIGN_TOP_MID, 0, 46);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0x224d70), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0x804030),
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, on_vent_switch, LV_EVENT_VALUE_CHANGED, NULL);
    }

    struct { lv_align_t a; int x, y; uint32_t col; const char * cmd;
             const char * txt; lv_event_cb_t cb; } btn[] = {
        { LV_ALIGN_BOTTOM_LEFT,  4,  -4, 0x2e6e3a, "auto", "Auto",  on_vent_mode  },
        { LV_ALIGN_BOTTOM_RIGHT,-4,  -4, 0x6a5424, NULL,   "Timer", on_vent_timer },
    };
    for (size_t i = 0; i < sizeof(btn)/sizeof(btn[0]); i++) {
        lv_obj_t * b = lv_btn_create(vent.tile);
        lv_obj_set_size(b, 60, 28);
        lv_obj_align(b, btn[i].a, btn[i].x, btn[i].y);
        lv_obj_set_style_bg_color(b, lv_color_hex(btn[i].col), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_add_event_cb(b, btn[i].cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(uintptr_t)btn[i].cmd);
        lv_obj_t * bl = lv_label_create(b);
        lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
        lv_label_set_text(bl, btn[i].txt);
        lv_obj_center(bl);
        if (btn[i].cb == on_vent_timer) vent_timer_lbl = bl;
    }

    /* rpm sits just above the bottom-row buttons. Small font so it
       doesn't crowd the fan. */
    lbl_boiler_pressure = lv_label_create(vent.tile);
    lv_obj_set_style_text_color(lbl_boiler_pressure, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_boiler_pressure, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_boiler_pressure, "-- rpm");
    lv_obj_align(lbl_boiler_pressure, LV_ALIGN_BOTTOM_MID, 0, -38);

    tile_t water_t;
    make_tile(scr_root, 790, 230, 214, 200, "Water", 0x44aaff, open_placeholder, &water_t);
    lbl_inbox_main = lv_label_create(water_t.tile);
    lv_obj_set_style_text_color(lbl_inbox_main, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(lbl_inbox_main, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_inbox_main, "-- m3");
    lv_obj_align(lbl_inbox_main, LV_ALIGN_CENTER, 0, -8);
    lbl_inbox_sub = lv_label_create(water_t.tile);
    lv_obj_set_style_text_color(lbl_inbox_sub, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_inbox_sub, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_inbox_sub, "-- L/min");
    lv_obj_align(lbl_inbox_sub, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* Spinner overlay: visible only while water is flowing. lv_spinner draws
       a continuously-rotating arc that reads as "something is moving". */
    water_spinner = lv_spinner_create(water_t.tile, 1200, 80);
    lv_obj_set_size(water_spinner, 48, 48);
    lv_obj_align(water_spinner, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_set_style_arc_color(water_spinner, lv_color_hex(0x223344), LV_PART_MAIN);
    lv_obj_set_style_arc_color(water_spinner, lv_color_hex(0x44aaff), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(water_spinner, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(water_spinner, 6, LV_PART_INDICATOR);
    lv_obj_add_flag(water_spinner, LV_OBJ_FLAG_HIDDEN);

    /* --- Forecast band — fills the area below the upper-row tiles.
           5 day columns; each shows day label + min/max temp + a big
           weather icon. The wordy description is gone (was rendering
           clipped/garbled on a narrow column) — full text still lives
           on the forecast-detail screen. --- */
    forecast_box = lv_obj_create(scr_root);
    lv_obj_set_size(forecast_box, 1004, 158);
    lv_obj_set_pos(forecast_box, 10, 434);
    lv_obj_set_style_bg_color(forecast_box, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_radius(forecast_box, 12, 0);
    lv_obj_set_style_border_width(forecast_box, 0, 0);
    lv_obj_set_style_pad_all(forecast_box, 6, 0);
    lv_obj_clear_flag(forecast_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(forecast_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(forecast_box, open_forecast, LV_EVENT_CLICKED, NULL);
    {
        int col_w = 1004 / WEATHER_FORECAST_DAYS;
        for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
            lv_obj_t * col = lv_obj_create(forecast_box);
            lv_obj_set_size(col, col_w - 4, 148);
            lv_obj_set_pos(col, i * col_w + 2, 0);
            lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(col, 0, 0);
            lv_obj_set_style_pad_all(col, 0, 0);
            lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);

            fc_day_lbl[i] = lv_label_create(col);
            lv_obj_set_style_text_color(fc_day_lbl[i], lv_color_hex(COL_TEXT_HI), 0);
            lv_obj_set_style_text_font(fc_day_lbl[i], &lv_font_montserrat_22, 0);
            lv_label_set_text(fc_day_lbl[i], "--");
            lv_obj_align(fc_day_lbl[i], LV_ALIGN_TOP_LEFT, 8, 4);

            fc_temp_lbl[i] = lv_label_create(col);
            lv_obj_set_style_text_color(fc_temp_lbl[i], lv_color_hex(COL_TEMP_YELLOW), 0);
            lv_obj_set_style_text_font(fc_temp_lbl[i], &lv_font_montserrat_22, 0);
            lv_label_set_text(fc_temp_lbl[i], "");
            lv_obj_align(fc_temp_lbl[i], LV_ALIGN_TOP_RIGHT, -8, 4);

            /* Big weather icon centered in the column, fills the room
               freed by the dropped description text. */
            fc_icon[i] = lv_img_create(col);
            lv_obj_set_style_img_recolor(fc_icon[i], lv_color_hex(0xc8d4e0), 0);
            lv_obj_set_style_img_recolor_opa(fc_icon[i], 255, 0);
            lv_img_set_src(fc_icon[i], weather_icon_for_lg("d"));   /* default cloud */
            lv_obj_align(fc_icon[i], LV_ALIGN_CENTER, 0, 8);
            lv_obj_add_flag(fc_icon[i], LV_OBJ_FLAG_EVENT_BUBBLE);

            fc_wind_lbl[i] = lv_label_create(col);
            lv_obj_set_style_text_color(fc_wind_lbl[i], lv_color_hex(COL_TEXT_DIM), 0);
            lv_obj_set_style_text_font(fc_wind_lbl[i], &lv_font_montserrat_18, 0);
            lv_label_set_text(fc_wind_lbl[i], "");
            lv_obj_align(fc_wind_lbl[i], LV_ALIGN_BOTTOM_MID, 12, -4);

            /* Wind-direction arrow next to the wind label. Rotated per
               forecast via lv_img_set_angle. Hidden until a valid
               direction code arrives. */
            fc_wind_arrow[i] = lv_img_create(col);
            lv_img_set_src(fc_wind_arrow[i], &icon_wind_arrow);
            lv_img_set_pivot(fc_wind_arrow[i], 16, 16);
            lv_obj_align(fc_wind_arrow[i], LV_ALIGN_BOTTOM_LEFT, 8, -4);
            lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_add_flag(fc_wind_arrow[i], LV_OBJ_FLAG_HIDDEN);

            /* fc_desc_lbl is gone but the refresh-cb still touches it via
               its array slot; NULL so the writer no-ops. */
            fc_desc_lbl[i] = NULL;
        }
    }

    /* Bottom row removed entirely — the live W + cumulative gas now live
       on the top-right Energy tile. Null the refs so legacy refresh
       writers no-op cleanly. */
    lbl_bot_energy   = NULL;
    lbl_outside_main = NULL;
    lbl_outside_sub  = NULL;
    lbl_bot_waste    = NULL;
    lbl_bot_weather  = NULL;

    /* (Old narrow bottom-row Waste tile + its trash icon removed — the
       full-size Waste tile now lives in the right column.) */

    /* Settings + Inbox icons are placed at the absolute screen top-right
       (not inside the big tile) per the user's spec. They float in the
       gap above the Humidity tile's accent bar — overlapping just the
       accent line, never the humidity value. */
    envelope_btn = lv_btn_create(scr_root);
    lv_obj_set_size(envelope_btn, 44, 44);
    lv_obj_align(envelope_btn, LV_ALIGN_TOP_RIGHT, -66, 4);
    lv_obj_set_style_bg_color(envelope_btn, lv_color_hex(0x335577), 0);
    lv_obj_set_style_radius(envelope_btn, 22, 0);
    lv_obj_set_ext_click_area(envelope_btn, 14);
    lv_obj_add_event_cb(envelope_btn, open_inbox, LV_EVENT_CLICKED, NULL);
    lv_obj_t * env_lbl = lv_label_create(envelope_btn);
    lv_label_set_text(env_lbl, LV_SYMBOL_ENVELOPE);
    lv_obj_set_style_text_color(env_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(env_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(env_lbl);

    envelope_badge = lv_obj_create(scr_root);
    lv_obj_set_size(envelope_badge, 20, 20);
    lv_obj_align(envelope_badge, LV_ALIGN_TOP_RIGHT, -60, 2);
    lv_obj_set_style_bg_color(envelope_badge, lv_color_hex(0xff3344), 0);
    lv_obj_set_style_radius(envelope_badge, 10, 0);
    lv_obj_set_style_border_width(envelope_badge, 2, 0);
    lv_obj_set_style_border_color(envelope_badge, lv_color_hex(0x0f1a2a), 0);
    lv_obj_set_style_pad_all(envelope_badge, 0, 0);
    lv_obj_clear_flag(envelope_badge, LV_OBJ_FLAG_SCROLLABLE);
    envelope_badge_lbl = lv_label_create(envelope_badge);
    lv_obj_set_style_text_color(envelope_badge_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(envelope_badge_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(envelope_badge_lbl, "0");
    lv_obj_center(envelope_badge_lbl);
    lv_obj_add_flag(envelope_btn,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envelope_badge, LV_OBJ_FLAG_HIDDEN);

    /* Gear in the very top-right corner of the screen. */
    lv_obj_t * gear = lv_btn_create(scr_root);
    lv_obj_set_size(gear, 44, 44);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_bg_color(gear, lv_color_hex(0x223344), 0);
    lv_obj_set_style_radius(gear, 22, 0);
    lv_obj_set_ext_click_area(gear, 14);
    lv_obj_add_event_cb(gear, open_settings, LV_EVENT_CLICKED, NULL);
    lv_obj_t * gear_lbl = lv_label_create(gear);
    lv_label_set_text(gear_lbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gear_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(gear_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(gear_lbl);

    if (!refresh_timer) refresh_timer = lv_timer_create(refresh_cb, 500, NULL);
    return scr_root;
}
