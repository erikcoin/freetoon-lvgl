/*
 * calendar.c — upcoming-events agenda from Home Assistant (REST) and/or an
 * iCal (.ics) URL. Both sources are parsed into a common event list, merged,
 * de-duplicated, sorted by date+time, and trimmed to the next CAL_MAX events.
 *
 * Parsing is deliberately brittle/jq-free (string scanning), matching the rest
 * of the codebase — calendar payloads are regular enough. Times are taken as
 * the wall-clock value in the feed (no timezone conversion) which is fine for
 * an at-a-glance agenda.
 */
#define _GNU_SOURCE
#include "calendar.h"
#include "settings.h"
#include "http.h"
#include "homeassistant.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

calendar_state_t calendar_state = {0};

#define CAL_SCRATCH (96 * 1024)
#define CAL_TMP_MAX 64                  /* parse capacity before trim to CAL_MAX */

static void today_str(char * out, size_t n)   { time_t t=time(NULL); struct tm tm; localtime_r(&t,&tm);
    strftime(out, n, "%Y-%m-%d", &tm); }
static void iso_at(char * out, size_t n, int days_from_now) {
    time_t t = time(NULL) + (time_t)days_from_now * 86400;
    struct tm tm; localtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%dT00:00:00Z", &tm);
}

/* time field for compare: empty (all-day) sorts to start of day. */
static const char * cmp_time(const char * t) { return t[0] ? t : "00:00"; }

static int ev_cmp(const void * a, const void * b) {
    const calendar_event_t * x = a, * y = b;
    int d = strcmp(x->date, y->date);
    if (d) return d;
    return strcmp(cmp_time(x->time), cmp_time(y->time));
}

/* Append one event to tmp[] if it's today-or-future and not a duplicate. */
static void add_event(calendar_event_t * tmp, int * n, const char * today,
                      const char * date, const char * time, const char * summary) {
    if (!date[0] || !summary[0]) return;
    if (strcmp(date, today) < 0) return;            /* past */
    for (int i = 0; i < *n; i++)                     /* dedup (HA + iCal overlap) */
        if (!strcmp(tmp[i].date, date) && !strcmp(tmp[i].time, time) &&
            !strcmp(tmp[i].summary, summary)) return;
    if (*n >= CAL_TMP_MAX) return;
    calendar_event_t * e = &tmp[*n];
    snprintf(e->date, sizeof e->date, "%s", date);
    snprintf(e->time, sizeof e->time, "%s", time);
    snprintf(e->summary, sizeof e->summary, "%s", summary);
    (*n)++;
}

/* Copy a JSON/ICS string value, unescaping the common ICS/JSON escapes. */
static void copy_text(const char * src, char * out, size_t osz) {
    size_t n = 0;
    while (*src && *src != '"' && *src != '\r' && *src != '\n' && n + 1 < osz) {
        if (*src == '\\' && src[1]) {
            src++;
            out[n++] = (*src == 'n' || *src == 'N') ? ' ' : *src;
            src++;
        } else out[n++] = *src++;
    }
    out[n] = 0;
}

/* ---- Home Assistant: array of {"start":{"dateTime"|"date":..},"summary":..} */
static void parse_ha(const char * body, calendar_event_t * tmp, int * n, const char * today) {
    const char * p = body;
    const char * s;
    while ((s = strstr(p, "\"start\"")) != NULL) {
        const char * next = strstr(s + 7, "\"start\"");
        const char * wend = next ? next : s + strlen(s);

        char date[12] = "", tm[8] = "";
        const char * dt = strstr(s, "\"dateTime\"");
        if (dt && dt < wend) {
            const char * c = strchr(dt, ':');
            if (c) { c++; while (*c==' '||*c=='"') c++;
                if (strlen(c) >= 16 && c[4]=='-' && c[10]=='T') {
                    snprintf(date, sizeof date, "%.10s", c);
                    snprintf(tm, sizeof tm, "%.5s", c + 11);   /* HH:MM */
                } }
        } else {
            const char * d = strstr(s, "\"date\"");
            if (d && d < wend) {
                const char * c = strchr(d, ':');
                if (c) { c++; while (*c==' '||*c=='"') c++;
                    if (strlen(c) >= 10 && c[4]=='-') snprintf(date, sizeof date, "%.10s", c); }
            }
        }
        char summary[80] = "";
        const char * sm = strstr(s, "\"summary\"");
        if (sm && sm < wend) {
            const char * c = strchr(sm, ':');
            if (c) { c++; while (*c==' ') c++; if (*c=='"') c++; copy_text(c, summary, sizeof summary); }
        }
        add_event(tmp, n, today, date, tm, summary);
        p = wend;
    }
}

/* ---- iCal: walk VEVENTs, DTSTART (date [+ Thhmm]) + SUMMARY. */
static void parse_ics_cal(const char * body, calendar_event_t * tmp, int * n, const char * today) {
    const char * p = body;
    while ((p = strstr(p, "BEGIN:VEVENT")) != NULL) {
        const char * evend = strstr(p, "END:VEVENT");
        if (!evend) evend = p + strlen(p);
        char date[12] = "", tm[8] = "";
        const char * d = strstr(p, "DTSTART");
        if (d && d < evend) {
            for (const char * q = d; *q && q < evend; q++) {
                if (isdigit((unsigned char)*q)) {
                    char buf[16]; int k = 0;
                    for (const char * r = q; k < 15 && (isdigit((unsigned char)*r) || *r=='T'); r++) buf[k++] = *r;
                    buf[k] = 0;
                    if (k >= 8) snprintf(date, sizeof date, "%.4s-%.2s-%.2s", buf, buf+4, buf+6);
                    if (k >= 13 && buf[8] == 'T')                       /* YYYYMMDDThhmm.. */
                        snprintf(tm, sizeof tm, "%.2s:%.2s", buf+9, buf+11);
                    break;
                }
            }
        }
        char summary[80] = "";
        const char * s = strstr(p, "SUMMARY");
        if (s && s < evend) {
            const char * c = strchr(s, ':');
            if (c) { c++; copy_text(c, summary, sizeof summary); }
        }
        add_event(tmp, n, today, date, tm, summary);
        p = evend;
    }
}

void calendar_refresh_now(void) {
    if (!settings.calendar_enabled) { calendar_state.count = 0; calendar_state.connected = 0; return; }
    static char buf[CAL_SCRATCH];
    static calendar_event_t tmp[CAL_TMP_MAX];
    int n = 0, ok = 0;
    char today[12]; today_str(today, sizeof today);

    if (settings.calendar_ha_entity[0]) {
        char start[24], end[24];
        iso_at(start, sizeof start, 0);
        iso_at(end, sizeof end, 31);
        if (ha_fetch_calendar(settings.calendar_ha_entity, start, end, buf, sizeof buf) == 0) {
            parse_ha(buf, tmp, &n, today); ok = 1;
        }
    }
    if (settings.calendar_ics_url[0]) {
        if (http_fetch(settings.calendar_ics_url, buf, sizeof buf) == 0 && strstr(buf, "VEVENT")) {
            parse_ics_cal(buf, tmp, &n, today); ok = 1;
        }
    }

    qsort(tmp, n, sizeof tmp[0], ev_cmp);
    if (n > CAL_MAX) n = CAL_MAX;
    for (int i = 0; i < n; i++) calendar_state.ev[i] = tmp[i];
    calendar_state.count = n;
    if (ok) calendar_state.connected = 1;
}

/* One-shot refresh on a detached thread — safe to call from the LVGL/UI thread
 * (the fetch does blocking curl I/O and must never run on the UI thread). */
static void * kick_thread(void * arg) { (void)arg; calendar_refresh_now(); return NULL; }
void calendar_refresh_async(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, kick_thread, NULL) == 0) pthread_detach(t);
}

static void * cal_thread(void * arg) {
    (void)arg;
    for (;;) {
        calendar_refresh_now();
        sleep(1800);                    /* 30 min — calendars change slowly */
    }
    return NULL;
}

int calendar_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, cal_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
