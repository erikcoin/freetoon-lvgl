/*
 * Settings screen — a category landing page. Five tiles (Display, Weather,
 * Waste, Heating, About); tapping a tile opens a distinct modal with just
 * that category's controls.
 *
 * Modal infrastructure:
 *   modal_open(title, h)  builds a dimmed backdrop + centred panel and
 *                         returns the panel for the caller to fill.
 *   modal_close()         tears it down (async-deleted) and persists.
 * Only one category modal is open at a time, so the per-control widget
 * pointers below are simply reassigned each time a modal is built.
 *
 * The Heating modal's OpenTherm/On-Off switch does NOT write immediately —
 * it raises a confirm dialog first, because SetBoilerType reconfigures the
 * live boiler (see boxtalk.c).
 */
#include "screens.h"
#include "settings.h"
#include "backlight.h"
#include "boxtalk.h"
#include "icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdlib.h>

static lv_obj_t * scr_root = NULL;

/* ---- modal state ---- */
static lv_obj_t *   cur_modal     = NULL;   /* backdrop of the open category modal */
static lv_obj_t *   confirm_box   = NULL;   /* boiler-type confirm dialog (child of cur_modal) */
static lv_timer_t * modal_timer   = NULL;   /* live refresh for Heating/About modals */
static void       (*modal_tick_fn)(void) = NULL;

/* ---- per-control widget pointers (valid only while a modal is open) ---- */
static lv_obj_t * sw_enable;
static lv_obj_t * sl_timeout,  * lbl_timeout_val;
static lv_obj_t * sl_act,      * lbl_act_val;
static lv_obj_t * sl_dim,      * lbl_dim_val;
static lv_obj_t * sw_dim_wx;
static lv_obj_t * sw_dim_waste;
static lv_obj_t * sl_waste_lead, * lbl_waste_lead;
static lv_obj_t * sl_offset,   * lbl_offset_val;
static lv_obj_t * sw_boiler;
static lv_obj_t * lbl_boiler_mode;
static lv_obj_t * lbl_boiler_ot;
static lv_obj_t * lbl_about_status;
static int        pending_boiler_type = -1;

/* ============================ value callbacks ============================ */

static void on_enable_change(lv_event_t * e) {
    settings.auto_dim_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static void on_dim_wx_change(lv_event_t * e) {
    settings.show_dim_weather = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static void on_dim_waste_change(lv_event_t * e) {
    settings.show_dim_waste = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
}
static void on_waste_lead_change(lv_event_t * e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    settings.dim_waste_lead_days = v;
    if (v == 0)      lv_label_set_text(lbl_waste_lead, "uit");
    else if (v == 1) lv_label_set_text(lbl_waste_lead, "vanaf 1 dag vooraf");
    else             lv_label_set_text_fmt(lbl_waste_lead, "vanaf %d dagen vooraf", v);
}
static void on_timeout_change(lv_event_t * e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    settings.auto_dim_seconds = v;
    lv_label_set_text_fmt(lbl_timeout_val, "%d s", v);
}
static void on_act_change(lv_event_t * e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    settings.active_brightness = v;
    lv_label_set_text_fmt(lbl_act_val, "%d", v);
    backlight_set(v);                       /* live preview */
}
static void on_dim_change(lv_event_t * e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    settings.dim_brightness = v;
    lv_label_set_text_fmt(lbl_dim_val, "%d", v);
}
static void on_offset_change(lv_event_t * e) {
    int v = lv_slider_get_value(lv_event_get_target(e));
    settings.temp_offset_centi = v;
    lv_label_set_text_fmt(lbl_offset_val, "%+.1f C", v / 100.0f);
}

/* ============================ small builders ============================ */

/* One settings row inside a modal panel: title at left, optional value
   label at right, control area below. Returns the row container. */
static lv_obj_t * panel_row(lv_obj_t * parent, int y, const char * title,
                            lv_obj_t ** out_val_lbl) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, 800, 74);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 4, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1f3050), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_all(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);   /* swallow taps, don't close modal */

    lv_obj_t * lbl = lv_label_create(row);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl, title);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    if (out_val_lbl) {
        *out_val_lbl = lv_label_create(row);
        lv_obj_set_style_text_color(*out_val_lbl, lv_color_hex(0x88aabb), 0);
        lv_obj_set_style_text_font(*out_val_lbl, &lv_font_montserrat_22, 0);
        lv_obj_align(*out_val_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    return row;
}

/* A slider that fills the bottom of a row built by panel_row(). */
static lv_obj_t * row_slider(lv_obj_t * row, int lo, int hi, int val,
                             lv_event_cb_t cb) {
    lv_obj_t * s = lv_slider_create(row);
    lv_obj_set_size(s, 760, 14);
    lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_slider_set_range(s, lo, hi);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return s;
}

/* A switch pinned to the right of a panel_row(). */
static lv_obj_t * row_switch(lv_obj_t * row, int checked, lv_event_cb_t cb) {
    lv_obj_t * sw = lv_switch_create(row);
    lv_obj_set_size(sw, 64, 30);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -4, 0);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

/* ============================ modal infra ============================ */

static void modal_timer_cb(lv_timer_t * t) { (void)t; if (modal_tick_fn) modal_tick_fn(); }

static void modal_close(lv_event_t * e) {
    (void)e;
    if (confirm_box) { lv_obj_del_async(confirm_box); confirm_box = NULL; }
    if (modal_timer) { lv_timer_del(modal_timer); modal_timer = NULL; }
    modal_tick_fn = NULL;
    if (cur_modal) {
        lv_obj_t * m = cur_modal;
        cur_modal = NULL;
        lv_obj_del_async(m);          /* async: we're inside a descendant's event */
    }
    settings_save();                  /* persist whatever the modal changed */
}

/* Build a dimmed full-screen backdrop + centred panel. Returns the panel;
   caller positions its content below y≈64 (title + close button live there). */
static lv_obj_t * modal_open(const char * title, int panel_h) {
    cur_modal = lv_obj_create(scr_root);
    lv_obj_remove_style_all(cur_modal);
    lv_obj_set_size(cur_modal, 1024, 600);
    lv_obj_set_pos(cur_modal, 0, 0);
    lv_obj_set_style_bg_color(cur_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cur_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(cur_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cur_modal, LV_OBJ_FLAG_CLICKABLE);          /* tap-outside target */
    lv_obj_add_event_cb(cur_modal, modal_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t * panel = lv_obj_create(cur_modal);
    lv_obj_set_size(panel, 860, panel_h);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x16243a), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_pad_all(panel, 20, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);              /* stop taps reaching backdrop */

    lv_obj_t * t = lv_label_create(panel);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 4, 4);

    lv_obj_t * x = lv_btn_create(panel);
    lv_obj_set_size(x, 56, 56);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 4, -4);
    lv_obj_set_style_bg_color(x, lv_color_hex(0x33445a), 0);
    lv_obj_set_style_radius(x, 12, 0);
    lv_obj_add_event_cb(x, modal_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t * xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xl, lv_color_hex(0xffffff), 0);
    lv_obj_center(xl);

    return panel;
}

/* ====================== boiler-type confirm dialog ====================== */

static void confirm_dismiss(void) {
    if (confirm_box) { lv_obj_del_async(confirm_box); confirm_box = NULL; }
}

static void on_boiler_confirm_yes(lv_event_t * e) {
    (void)e;
    if (pending_boiler_type == 0 || pending_boiler_type == 1)
        boxtalk_set_boiler_type(pending_boiler_type);
    confirm_dismiss();
}

static void on_boiler_confirm_no(lv_event_t * e) {
    (void)e;
    /* revert the switch to whatever the boiler currently reports */
    if (sw_boiler) {
        if (toon_state.boiler_type == 1) lv_obj_add_state(sw_boiler, LV_STATE_CHECKED);
        else                             lv_obj_clear_state(sw_boiler, LV_STATE_CHECKED);
    }
    confirm_dismiss();
}

static void on_boiler_switch(lv_event_t * e) {
    int want_onoff = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    pending_boiler_type = want_onoff;     /* 1 = On/Off, 0 = OpenTherm */

    /* Confirm dialog, layered on top of the Heating modal. */
    confirm_box = lv_obj_create(cur_modal);
    lv_obj_remove_style_all(confirm_box);
    lv_obj_set_size(confirm_box, 1024, 600);
    lv_obj_set_pos(confirm_box, 0, 0);
    lv_obj_set_style_bg_color(confirm_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(confirm_box, LV_OPA_60, 0);
    lv_obj_clear_flag(confirm_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(confirm_box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * dlg = lv_obj_create(confirm_box);
    lv_obj_set_size(dlg, 720, 360);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x2a1c1c), 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0xcc5544), 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 16, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_clear_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dlg, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * dt = lv_label_create(dlg);
    lv_obj_set_style_text_font(dt, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(dt, lv_color_hex(0xffcc66), 0);
    lv_label_set_text(dt, "Change boiler control?");
    lv_obj_align(dt, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t * body = lv_label_create(dlg);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(0xddddee), 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, 672);
    lv_label_set_text_fmt(body,
        "Switch boiler control to %s?\n\n"
        "This reconfigures how Toon drives your boiler and is written to "
        "the device immediately. Only change it if you know your boiler "
        "wiring — the wrong setting can stop your heating from working.",
        want_onoff ? "On/Off" : "OpenTherm");
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 48);

    lv_obj_t * b_no = lv_btn_create(dlg);
    lv_obj_set_size(b_no, 200, 64);
    lv_obj_align(b_no, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(b_no, lv_color_hex(0x44556a), 0);
    lv_obj_set_style_radius(b_no, 12, 0);
    lv_obj_add_event_cb(b_no, on_boiler_confirm_no, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_no_l = lv_label_create(b_no);
    lv_label_set_text(b_no_l, "Cancel");
    lv_obj_set_style_text_font(b_no_l, &lv_font_montserrat_22, 0);
    lv_obj_center(b_no_l);

    lv_obj_t * b_yes = lv_btn_create(dlg);
    lv_obj_set_size(b_yes, 260, 64);
    lv_obj_align(b_yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(b_yes, lv_color_hex(0xcc5544), 0);
    lv_obj_set_style_radius(b_yes, 12, 0);
    lv_obj_add_event_cb(b_yes, on_boiler_confirm_yes, LV_EVENT_CLICKED, NULL);
    lv_obj_t * b_yes_l = lv_label_create(b_yes);
    lv_label_set_text_fmt(b_yes_l, "Set %s", want_onoff ? "On/Off" : "OpenTherm");
    lv_obj_set_style_text_font(b_yes_l, &lv_font_montserrat_22, 0);
    lv_obj_center(b_yes_l);
}

/* ============================ live ticks ============================ */

static void heating_tick(void) {
    if (lbl_boiler_mode) {
        const char * m = (toon_state.boiler_type == 1) ? "On/Off"
                       : (toon_state.boiler_type == 0) ? "OpenTherm"
                       : "detecting...";
        lv_label_set_text_fmt(lbl_boiler_mode, "Current mode: %s", m);
    }
    if (lbl_boiler_ot) {
        if (toon_state.boiler_type == 0) {
            lv_label_set_text_fmt(lbl_boiler_ot, "Modulation %d%%   -   OT link %s",
                                  toon_state.modulation_level,
                                  toon_state.ot_comm_error ? "ERROR" : "OK");
        } else {
            lv_label_set_text(lbl_boiler_ot, "");
        }
    }
}

static void about_tick(void) {
    if (!lbl_about_status) return;
    double up = 0;
    FILE * f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
    int uh = (int)(up / 3600), um = (int)((up - uh * 3600) / 60);
    lv_label_set_text_fmt(lbl_about_status,
        "BoxTalk: %s   (msg %d)\n"
        "Boiler: %s\n"
        "Indoor %.1f C   -   Setpoint %.1f C\n"
        "System uptime: %dh %dm",
        toon_state.connected ? "connected" : "connecting...",
        toon_state.msg_count,
        toon_state.boiler_type == 1 ? "On/Off" :
        toon_state.boiler_type == 0 ? "OpenTherm" : "unknown",
        toon_state.indoor_temp, toon_state.setpoint, uh, um);
}

/* ============================ category modals ============================ */

static void open_display_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open("Display", 460);
    int y = 70;
    lv_obj_t * r;

    r = panel_row(p, y, "Auto-dim when idle", NULL);
    sw_enable = row_switch(r, settings.auto_dim_enabled, on_enable_change);
    y += 82;

    r = panel_row(p, y, "Idle timeout", &lbl_timeout_val);
    lv_label_set_text_fmt(lbl_timeout_val, "%d s", settings.auto_dim_seconds);
    sl_timeout = row_slider(r, 5, 300, settings.auto_dim_seconds, on_timeout_change);
    y += 82;

    r = panel_row(p, y, "Active brightness", &lbl_act_val);
    lv_label_set_text_fmt(lbl_act_val, "%d", settings.active_brightness);
    sl_act = row_slider(r, 100, 1000, settings.active_brightness, on_act_change);
    y += 82;

    r = panel_row(p, y, "Dim brightness", &lbl_dim_val);
    lv_label_set_text_fmt(lbl_dim_val, "%d", settings.dim_brightness);
    sl_dim = row_slider(r, 0, 400, settings.dim_brightness, on_dim_change);
}

static void open_weather_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open("Weather", 220);
    lv_obj_t * r = panel_row(p, 70, "Show weather on dim screen", NULL);
    sw_dim_wx = row_switch(r, settings.show_dim_weather, on_dim_wx_change);
}

static void open_waste_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open("Waste", 300);
    int y = 70;
    lv_obj_t * r;

    r = panel_row(p, y, "Show waste on dim screen", NULL);
    sw_dim_waste = row_switch(r, settings.show_dim_waste, on_dim_waste_change);
    y += 82;

    r = panel_row(p, y, "Waste alert window", &lbl_waste_lead);
    if      (settings.dim_waste_lead_days == 0) lv_label_set_text(lbl_waste_lead, "uit");
    else if (settings.dim_waste_lead_days == 1) lv_label_set_text(lbl_waste_lead, "vanaf 1 dag vooraf");
    else lv_label_set_text_fmt(lbl_waste_lead, "vanaf %d dagen vooraf",
                               settings.dim_waste_lead_days);
    sl_waste_lead = row_slider(r, 0, 7, settings.dim_waste_lead_days, on_waste_lead_change);
}

static void open_heating_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open("Heating", 470);
    int y = 70;

    /* indoor temp calibration */
    lv_obj_t * r = panel_row(p, y, "Indoor temp offset", &lbl_offset_val);
    lv_label_set_text_fmt(lbl_offset_val, "%+.1f C", settings.temp_offset_centi / 100.0f);
    sl_offset = row_slider(r, -500, 500, settings.temp_offset_centi, on_offset_change);
    y += 90;

    /* boiler control type */
    lv_obj_t * box = lv_obj_create(p);
    lv_obj_set_size(box, 800, 200);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 4, y);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1f3050), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * hdr = lv_label_create(box);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_22, 0);
    lv_label_set_text(hdr, "Boiler control");
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);

    sw_boiler = lv_switch_create(box);
    lv_obj_set_size(sw_boiler, 64, 30);
    lv_obj_align(sw_boiler, LV_ALIGN_TOP_RIGHT, -4, 2);
    if (toon_state.boiler_type == 1) lv_obj_add_state(sw_boiler, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_boiler, on_boiler_switch, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * swlbl = lv_label_create(box);
    lv_obj_set_style_text_color(swlbl, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(swlbl, &lv_font_montserrat_18, 0);
    lv_label_set_text(swlbl, "Off = OpenTherm    On = On/Off");
    lv_obj_align(swlbl, LV_ALIGN_TOP_RIGHT, -4, 38);

    lbl_boiler_mode = lv_label_create(box);
    lv_obj_set_style_text_color(lbl_boiler_mode, lv_color_hex(0xffcc44), 0);
    lv_obj_set_style_text_font(lbl_boiler_mode, &lv_font_montserrat_22, 0);
    lv_obj_align(lbl_boiler_mode, LV_ALIGN_TOP_LEFT, 0, 36);

    lbl_boiler_ot = lv_label_create(box);
    lv_obj_set_style_text_color(lbl_boiler_ot, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_boiler_ot, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl_boiler_ot, LV_ALIGN_TOP_LEFT, 0, 70);

    lv_obj_t * warn = lv_label_create(box);
    lv_obj_set_style_text_color(warn, lv_color_hex(0xcc7766), 0);
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_18, 0);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(warn, 772);
    lv_label_set_text(warn,
        "Changing this writes to the live boiler. You'll be asked to confirm.");
    lv_obj_align(warn, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* refresh the boiler labels live + re-query the type */
    heating_tick();
    boxtalk_get_boiler_type();
    modal_tick_fn = heating_tick;
    modal_timer = lv_timer_create(modal_timer_cb, 1000, NULL);
}

static void open_about_modal(lv_event_t * e) {
    (void)e;
    lv_obj_t * p = modal_open("About", 320);

    lv_obj_t * ver = lv_label_create(p);
    lv_obj_set_style_text_color(ver, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_22, 0);
    lv_label_set_text(ver, "toonui - LVGL rebuild   (build " __DATE__ ")");
    lv_obj_align(ver, LV_ALIGN_TOP_LEFT, 4, 70);

    lbl_about_status = lv_label_create(p);
    lv_obj_set_style_text_color(lbl_about_status, lv_color_hex(0x88aabb), 0);
    lv_obj_set_style_text_font(lbl_about_status, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl_about_status, LV_ALIGN_TOP_LEFT, 4, 112);

    about_tick();
    modal_tick_fn = about_tick;
    modal_timer = lv_timer_create(modal_timer_cb, 1000, NULL);
}

/* ============================ landing page ============================ */

static void on_back(lv_event_t * e) { (void)e; ui_pop(); }

/* One category tile: icon (optional), big title, caption. */
static void make_tile(int x, int y, const lv_img_dsc_t * icon, const char * sym,
                      const char * title, const char * caption, lv_event_cb_t cb) {
    lv_obj_t * tile = lv_btn_create(scr_root);
    lv_obj_set_size(tile, 308, 188);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x1a2a44), 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x24385c), LV_STATE_PRESSED);
    lv_obj_set_style_radius(tile, 16, 0);
    lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, NULL);

    if (icon) {
        lv_obj_t * im = lv_img_create(tile);
        lv_img_set_src(im, icon);
        lv_obj_set_style_img_recolor(im, lv_color_hex(0x9fc4e6), 0);
        lv_obj_set_style_img_recolor_opa(im, 255, 0);
        lv_obj_align(im, LV_ALIGN_TOP_MID, 0, 18);
    } else if (sym) {
        lv_obj_t * s = lv_label_create(tile);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(s, lv_color_hex(0x9fc4e6), 0);
        lv_label_set_text(s, sym);
        lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 26);
    }

    lv_obj_t * t = lv_label_create(tile);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 14);

    lv_obj_t * c = lv_label_create(tile);
    lv_obj_set_style_text_font(c, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(c, lv_color_hex(0x7d97b5), 0);
    lv_label_set_text(c, caption);
    lv_obj_align(c, LV_ALIGN_BOTTOM_MID, 0, -14);
}

lv_obj_t * screen_settings_create(void) {
    if (scr_root) return scr_root;

    scr_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_root, lv_color_hex(0x0f1a2a), 0);
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);

    /* header */
    lv_obj_t * btn_back = lv_btn_create(scr_root);
    lv_obj_set_size(btn_back, 140, 80);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x223344), 0);
    lv_obj_set_style_radius(btn_back, 14, 0);
    lv_obj_set_ext_click_area(btn_back, 20);
    lv_obj_add_event_cb(btn_back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_lbl = lv_label_create(btn_back);
    lv_label_set_text(back_lbl, "< Back");
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_22, 0);
    lv_obj_center(back_lbl);

    lv_obj_t * title = lv_label_create(scr_root);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_label_set_text(title, "Settings");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 180, 30);

    /* 5 category tiles — 3 on top, 2 below */
    int x0 = 25, gap = 22, row1 = 120, row2 = 326;
    make_tile(x0 + 0*(308+gap), row1, &icon_wx_cloud, NULL, "Display",
              "dim, timeout, brightness", open_display_modal);
    /* Display has no icon source — use a symbol instead (override above) */
    make_tile(x0 + 1*(308+gap), row1, &icon_wx_cloud, NULL, "Weather",
              "weather on dim screen", open_weather_modal);
    make_tile(x0 + 2*(308+gap), row1, &icon_trash, NULL, "Waste",
              "pickup alerts on dim", open_waste_modal);
    make_tile(x0 + 0*(308+gap), row2, &icon_flame, NULL, "Heating",
              "temp offset, boiler type", open_heating_modal);
    make_tile(x0 + 1*(308+gap), row2, NULL, LV_SYMBOL_LIST, "About",
              "status & diagnostics", open_about_modal);

    return scr_root;
}
