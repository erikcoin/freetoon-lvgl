/*
 * Background poller for two HomeWizard P1 devices.
 *   192.168.99.69  = HWE-P1   (electricity + gas)
 *   192.168.99.115 = HWE-WTR  (water)
 *
 * Both expose GET /api/v1/data returning a flat JSON object.
 * We parse the handful of fields we need with strstr/strtod; no JSON
 * library dependency.
 */
#include "homewizard.h"
#include "rrd_push.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

hw_state_t hw_state = {0};

static int http_get(const char * ip, const char * path, char * out, size_t outsz) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(80);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { close(s); return -1; }
    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) { close(s); return -1; }
    char req[256];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, ip);
    if (send(s, req, n, 0) != n) { close(s); return -1; }
    size_t got = 0;
    while (got < outsz - 1) {
        ssize_t k = recv(s, out + got, outsz - 1 - got, 0);
        if (k <= 0) break;
        got += (size_t)k;
    }
    out[got] = 0;
    close(s);
    return 0;
}

/* Find "key": <number> and return parsed double. Returns dflt if missing. */
static double parse_num(const char * json, const char * key, double dflt) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char * p = strstr(json, needle);
    if (!p) return dflt;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 'n') return dflt;   /* null */
    return strtod(p, NULL);
}

static void poll_p1(void) {
    static char body[4096];
    if (http_get("192.168.99.69", "/api/v1/data", body, sizeof(body)) != 0) {
        hw_state.connected_p1 = 0;
        return;
    }
    const char * j = strstr(body, "\r\n\r\n");
    j = j ? j + 4 : body;
    if (!strstr(j, "active_power_w")) {
        hw_state.connected_p1 = 0;
        return;
    }
    hw_state.power_w          = (float)parse_num(j, "active_power_w",         0);
    hw_state.kwh_import_t1    = (float)parse_num(j, "total_power_import_t1_kwh", 0);
    hw_state.kwh_import_t2    = (float)parse_num(j, "total_power_import_t2_kwh", 0);
    hw_state.kwh_import_total = (float)parse_num(j, "total_power_import_kwh", 0);
    hw_state.kwh_export_total = (float)parse_num(j, "total_power_export_kwh", 0);
    hw_state.tariff           = (int)  parse_num(j, "active_tariff",          1);
    hw_state.gas_m3           = (float)parse_num(j, "total_gas_m3",           0);
    hw_state.voltage_l1_v     = (float)parse_num(j, "active_voltage_l1_v",    0);
    hw_state.current_l1_a     = (float)parse_num(j, "active_current_a",       0);
    hw_state.connected_p1     = 1;
}

static void poll_water(void) {
    static char body[2048];
    if (http_get("192.168.99.115", "/api/v1/data", body, sizeof(body)) != 0) {
        hw_state.connected_water = 0;
        return;
    }
    const char * j = strstr(body, "\r\n\r\n");
    j = j ? j + 4 : body;
    if (!strstr(j, "total_liter_m3")) {
        hw_state.connected_water = 0;
        return;
    }
    hw_state.water_total_m3 = (float)parse_num(j, "total_liter_m3",   0);
    hw_state.water_lpm      = (float)parse_num(j, "active_liter_lpm", 0);
    hw_state.connected_water = 1;
}

/* Bin start helpers — round a timestamp down to the start of the current
   5-min / hour / day window. hcb_rrd expects the sample timestamp to land
   on a bin boundary. */
static long bin_5min(long ts) { return ts - (ts % 300); }
static long bin_hour(long ts) { return ts - (ts % 3600); }
static long bin_day(long ts)  { return ts - (ts % 86400); }

static void push_to_rrd(void) {
    long now = (long)time(NULL);
    static long last_5min = 0, last_hour = 0, last_day = 0;

    long b5 = bin_5min(now);
    long bh = bin_hour(now);
    long bd = bin_day(now);

    if (b5 != last_5min) {
        last_5min = b5;
        /* Flow archives — 5-minute live values from HomeWizard. */
        if (hw_state.connected_p1)
            rrd_push("elec_flow",  "5min", b5, hw_state.power_w);
        if (hw_state.connected_water)
            rrd_push("water_flow", "5min", b5, hw_state.water_lpm);
    }
    if (bh != last_hour) {
        last_hour = bh;
        /* Cumulative meters in their hourly archive. Toon stores the
           cumulative reading as integer in milli-units (litres for water,
           Wh for electricity, m³x1000 for gas). */
        if (hw_state.connected_p1) {
            rrd_push("elec_quantity_nt", "5yrhours", bh,
                     hw_state.kwh_import_t1 * 1000.0);
            rrd_push("elec_quantity_lt", "5yrhours", bh,
                     hw_state.kwh_import_t2 * 1000.0);
            rrd_push("gas_quantity",     "5yrhours", bh,
                     hw_state.gas_m3 * 1000.0);
        }
        if (hw_state.connected_water)
            rrd_push("water_quantity", "10yrhours", bh,
                     hw_state.water_total_m3 * 1000.0);
    }
    if (bd != last_day) {
        last_day = bd;
        /* Daily cumulative — same value rolled into the daily archive
           for the long-term graphs. */
        if (hw_state.connected_p1) {
            rrd_push("elec_quantity_nt", "10yrdays", bd,
                     hw_state.kwh_import_t1 * 1000.0);
            rrd_push("elec_quantity_lt", "10yrdays", bd,
                     hw_state.kwh_import_t2 * 1000.0);
            rrd_push("gas_quantity",     "10yrdays", bd,
                     hw_state.gas_m3 * 1000.0);
        }
    }
}

static void * hw_thread(void * arg) {
    (void)arg;
    while (1) {
        poll_p1();
        poll_water();
        push_to_rrd();
        sleep(5);
    }
    return NULL;
}

int homewizard_start(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, hw_thread, NULL) != 0) return -1;
    pthread_detach(th);
    return 0;
}
