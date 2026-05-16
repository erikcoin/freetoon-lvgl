/*
 * ventilation.c — background poller for the NRG-Itho-Wifi bridge.
 * Pulls /api.html?get=ithostatus every VENT_POLL_S seconds, parses the few
 * fields we care about, and exposes them via vent_state. Commands are POSTed
 * (well, sent via GET — the API is all GET) by vent_send_vremote.
 *
 * Auth is by query string: ?username=…&password=… on every URL.
 *
 * Configurable via /mnt/data/vent.conf (one line):  user:pass
 * Falls back to compiled-in defaults if missing.
 */
#include "ventilation.h"
#include "http.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>          /* atoi */
#include <string.h>
#include <unistd.h>

#define VENT_HOST       "192.168.3.236"
#define VENT_POLL_S     8
#define CONF_PATH       "/mnt/data/vent.conf"

vent_state_t vent_state = {0};

static char g_user[32] = "brakero1";
static char g_pass[32] = "";

static char g_settings_json[8192];

static void load_conf(void) {
    FILE * f = fopen(CONF_PATH, "r");
    if (!f) return;
    char line[128];
    if (fgets(line, sizeof(line), f)) {
        char * nl = strchr(line, '\n'); if (nl) *nl = 0;
        char * c  = strchr(line, ':');
        if (c) {
            *c = 0;
            snprintf(g_user, sizeof(g_user), "%s", line);
            snprintf(g_pass, sizeof(g_pass), "%s", c + 1);
        }
    }
    fclose(f);
}

/* Tiny JSON helpers — the response is flat key/value so no recursion. */
static int extract_int(const char * json, const char * key, int * out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char * p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p == '"') return 0;  /* "not available" */
    *out = (int)strtol(p, NULL, 10);
    return 1;
}

static int extract_str(const char * json, const char * key, char * out, size_t outsz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char * p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    const char * e = strchr(p, '"');
    if (!e) return 0;
    size_t n = e - p; if (n > outsz - 1) n = outsz - 1;
    memcpy(out, p, n); out[n] = 0;
    return 1;
}

static int fetch_status(void) {
    char url[256], body[4096];
    snprintf(url, sizeof(url),
        "http://%s/api.html?get=ithostatus&username=%s&password=%s",
        VENT_HOST, g_user, g_pass);
    int rc = http_fetch(url, body, sizeof(body));
    if (rc != 0 || strstr(body, "AUTHENTICATION FAILED")) {
        vent_state.connected = 0;
        return -1;
    }
    vent_state.connected = 1;
    int v;
    /* speed_pct is the user-visible "running %" — Itho's "ExhFanSpeed (%)"
     * (actual exhaust fan output), NOT the "Ventilation setpoint (%)" we
     * used previously. The setpoint can read 100 while the fan is still
     * spinning down, so the old mapping made the home tile lie. */
    if (extract_int(body, "ExhFanSpeed (%)",            &v)) vent_state.speed_pct   = v;
    if (extract_int(body, "Ventilation setpoint (%)", &v))  vent_state.exh_fan_pct = v;
    if (extract_int(body, "Fan speed (rpm)",            &v)) vent_state.fan_rpm        = v;
    if (extract_int(body, "Filter dirty",               &v)) vent_state.filter_dirty   = v;
    if (extract_int(body, "Internal fault",             &v)) vent_state.internal_fault = v;
    if (extract_int(body, "Error",                      &v)) vent_state.error_code     = v;
    if (extract_int(body, "Total operation (hours)",    &v)) vent_state.total_hours    = v;
    if (extract_int(body, "RemainingTime (min)",        &v)) vent_state.remaining_min  = v;
    extract_str(body, "FanInfo", vent_state.fan_info, sizeof(vent_state.fan_info));
    return 0;
}

/* Map a vremote command to the speed % the Itho will park at. Used for
 * optimistic UI updates so the fan animation / "57 %" pill reflect the
 * new state without waiting for the next 8-second poll. */
static int vent_expected_pct(const char * cmd) {
    if (!cmd) return -1;
    if (!strcmp(cmd, "away"))    return  0;
    if (!strcmp(cmd, "low"))     return 20;
    if (!strcmp(cmd, "medium"))  return 50;
    if (!strcmp(cmd, "high"))    return 100;
    if (!strcmp(cmd, "auto"))    return 30;
    if (!strncmp(cmd, "timer", 5)) return 100;
    return -1;
}

/* Thread entry — strdup'd cmd, freed here. */
static void * vremote_thread(void * arg) {
    char * cmd = (char *)arg;
    vent_send_vremote(cmd);
    free(cmd);
    return NULL;
}

void vent_send_vremote_async(const char * cmd) {
    int p = vent_expected_pct(cmd);
    if (p >= 0) {
        vent_state.speed_pct   = p;
        vent_state.exh_fan_pct = p;     /* approximate — actual %≈setpoint */
    }
    pthread_t t;
    char * dup = strdup(cmd ? cmd : "");
    if (!dup) return;
    if (pthread_create(&t, NULL, vremote_thread, dup) != 0) {
        free(dup);
        return;
    }
    pthread_detach(t);
}

int vent_send_vremote(const char * cmd) {
    char url[256], body[256];
    /* vremoteindex defaults to 0 → first configured virtual remote. */
    snprintf(url, sizeof(url),
        "http://%s/api.html?vremotecmd=%s&vremoteindex=0&username=%s&password=%s",
        VENT_HOST, cmd, g_user, g_pass);
    int rc = http_fetch(url, body, sizeof(body));
    fprintf(stderr, "[vent] vremotecmd=%s rc=%d body=%.40s\n", cmd, rc, body);
    return (rc == 0 && !strstr(body, "AUTHENTICATION FAILED")) ? 0 : -1;
}

int vent_set_speed(int pwm) {
    if (pwm < 0)   pwm = 0;
    if (pwm > 255) pwm = 255;
    char url[256], body[256];
    snprintf(url, sizeof(url),
        "http://%s/api.html?speed=%d&timer=0&username=%s&password=%s",
        VENT_HOST, pwm, g_user, g_pass);
    int rc = http_fetch(url, body, sizeof(body));
    fprintf(stderr, "[vent] speed=%d rc=%d body=%.40s\n", pwm, rc, body);
    return (rc == 0 && !strstr(body, "AUTHENTICATION FAILED")) ? 0 : -1;
}

int vent_bump_speed(int delta) {
    char url[256], body[64];
    snprintf(url, sizeof(url),
        "http://%s/api.html?get=currentspeed&username=%s&password=%s",
        VENT_HOST, g_user, g_pass);
    if (http_fetch(url, body, sizeof(body)) != 0
        || strstr(body, "AUTHENTICATION FAILED"))
        return -1;
    int cur = atoi(body);
    return vent_set_speed(cur + delta);
}

int vent_refresh_settings(void) {
    char url[256];
    snprintf(url, sizeof(url),
        "http://%s/api.html?get=ithosettings&username=%s&password=%s",
        VENT_HOST, g_user, g_pass);
    int rc = http_fetch(url, g_settings_json, sizeof(g_settings_json));
    if (rc != 0 || strstr(g_settings_json, "AUTHENTICATION FAILED")) {
        snprintf(g_settings_json, sizeof(g_settings_json),
                 "(fetch failed: rc=%d)", rc);
        return -1;
    }
    return 0;
}

const char * vent_settings_json(void) {
    return g_settings_json;
}

/* --- Per-index settings (getsetting=N / setsetting=N&value=V) --- */
vent_setting_t vent_settings[VENT_SETTING_COUNT] = {0};

int vent_fetch_one(int idx) {
    if (idx < 0 || idx >= VENT_SETTING_COUNT) return -1;
    char url[256], body[1024];
    snprintf(url, sizeof(url),
        "http://%s/api.html?getsetting=%d&username=%s&password=%s",
        VENT_HOST, idx, g_user, g_pass);
    int rc = http_fetch(url, body, sizeof(body));
    if (rc != 0 || strstr(body, "AUTHENTICATION FAILED") ||
        strstr(body, "\"status\":\"fail\"")) {
        vent_settings[idx].idx    = idx;
        vent_settings[idx].loaded = -1;
        return -1;
    }
    vent_settings[idx].idx = idx;
    extract_str(body, "label",
                vent_settings[idx].label, sizeof(vent_settings[idx].label));
    int v;
    if (extract_int(body, "current", &v)) vent_settings[idx].current = v;
    if (extract_int(body, "minimum", &v)) vent_settings[idx].minimum = v;
    if (extract_int(body, "maximum", &v)) vent_settings[idx].maximum = v;
    vent_settings[idx].loaded = 1;
    return 0;
}

int vent_fetch_all_settings(void) {
    int ok = 0;
    for (int i = 0; i < VENT_SETTING_COUNT; i++) {
        if (vent_fetch_one(i) == 0) ok++;
        usleep(20 * 1000);                 /* 20 ms pause between requests */
    }
    fprintf(stderr, "[vent] settings warm-load: %d/%d ok\n",
            ok, VENT_SETTING_COUNT);
    return ok;
}

int vent_save_setting(int idx, int value) {
    if (idx < 0 || idx >= VENT_SETTING_COUNT) return -1;
    char url[256], body[512];
    snprintf(url, sizeof(url),
        "http://%s/api.html?setsetting=%d&value=%d&username=%s&password=%s",
        VENT_HOST, idx, value, g_user, g_pass);
    int rc = http_fetch(url, body, sizeof(body));
    fprintf(stderr, "[vent] setsetting=%d value=%d rc=%d body=%.80s\n",
            idx, value, rc, body);
    if (rc != 0) return -1;
    if (strstr(body, "settings API is disabled")) return -2;
    if (strstr(body, "\"status\":\"success\"")) {
        vent_fetch_one(idx);               /* refresh cache after write */
        return 0;
    }
    return -1;
}

static void * vent_thread(void * arg) {
    (void)arg;
    fetch_status();
    /* Warm-load all settings once. Takes ~8 s but lets the advanced page
       render immediately when the user opens it. */
    vent_fetch_all_settings();
    while (1) {
        fetch_status();
        sleep(VENT_POLL_S);
    }
    return NULL;
}

int vent_start(void) {
    load_conf();
    if (!g_pass[0]) {
        fprintf(stderr, "[vent] no /mnt/data/vent.conf — vent will fail auth\n");
    }
    pthread_t t;
    pthread_create(&t, NULL, vent_thread, NULL);
    pthread_detach(t);
    fprintf(stderr, "[vent] poller started (host=%s every %ds)\n",
            VENT_HOST, VENT_POLL_S);
    return 0;
}
