/*
 * quby_bridge — Quby (Toon BA serial) ↔ OTGW HTTP bridge.
 *
 * Pretends to be the keteladapter (BA AVR board) on /dev/ttymxc0 so
 * happ_thermstat sees a working OpenTherm master.  Real boiler I/O happens
 * via OTGW (192.168.99.21, Robert van den Breemen firmware).
 *
 * Architecture
 * ------------
 *   happ_thermstat  ──(7-byte Quby frames)──>  pty slave  ──>  this daemon
 *                   <──(7-byte Quby resp)──    pty slave  <──
 *
 *   this daemon ──HTTP GET /api/v1/otgw/otmonitor──> OTGW   (poll, fill cache)
 *               ──HTTP POST /api/v0/otgw/command──> OTGW   (forward OT writes)
 *
 * Modes (-m):
 *   passive   — open the pty pair, print all parsed frames, never bind-mount,
 *               never reply. Safe to run alongside the real keteladapter.
 *   bench     — same as passive + reply to ttymxc0 reads by writing back into
 *               the pty master end. happ_thermstat must be pointed at our pty
 *               manually (test fixture / unit test).
 *   active    — bind-mount the pty slave over /dev/ttymxc0 and serve
 *               happ_thermstat for real. INVASIVE.
 *
 * Quby protocol spec: /tmp/qt_rebuild/boxtalk_re/quby_protocol_re.md
 *
 * Build:  /tmp/qt_rebuild/linaro/bin/arm-linux-gnueabihf-gcc -O2 -Wall \
 *           -o quby_bridge quby_bridge.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <pty.h>
#include <utmp.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/mount.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ===================================================================== */
/* configuration                                                         */
/* ===================================================================== */
#define OTGW_HOST       "192.168.99.21"
#define OTGW_PORT       80
#define OTGW_SERIAL_PORT 25238    /* serial-over-IP — accepts CS=/CH=/TR=/TT=
                                     and actually injects them on the OT wire.
                                     /api/v0/otgw/command on port 80 returns
                                     "Status:0" but never reaches the PIC. */
#define OTMON_PATH      "/api/v1/otgw/otmonitor"
#define OTGW_CMD_PATH   "/api/v0/otgw/command"
#define POLL_INTERVAL_S 2
#define LOGFILE         "/var/volatile/tmp/quby_bridge.log"
#define PTY_LINK        "/tmp/quby_pty"          /* symlink target in passive/bench */
#define TTY_PATH        "/dev/ttymxc0"           /* bind-mount target in active */

/* BoxTalk publish — emit ThermostatInfo / BoilerInfo notifies sourced from
 * OTGW so toonui / hcb_rrd see real boiler state even though happ_thermstat's
 * own ThermostatInfo path zeros out the pressure (it does receive DID 18 over
 * the Quby serial but then publishes 0.00 — root cause not yet RE'd). The
 * notifies are tagged with a fresh UUID; toonui dispatches on serviceid tail,
 * so it picks them up unconditionally. */
#define BXT_HOST        "127.0.0.1"
#define BXT_PORT        1337
#define BXT_PUB_UUID    "qb-659918000101-2011A0LOHI:quby-otgw-bridge"
/* Fresh UUIDs for the two virtual devices the bridge announces. Not tied to
 * any existing hardware — purely a publish target. */
#define UUID_THERMSTAT  "e7b8a710-0042-4f6f-9fd2-5cac18fc0a01"
#define UUID_BOILER     "e7b8a710-0042-4f6f-9fd2-5cac18fc0a02"

/* ===================================================================== */
/* logging                                                               */
/* ===================================================================== */
static FILE *g_log = NULL;
static void logmsg(const char *fmt, ...) {
    char ts[24]; time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    fprintf(stderr, "[bridge %s] ", ts);
    if (g_log) fprintf(g_log, "[bridge %s] ", ts);
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a);
    if (g_log) { va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a); }
    va_end(a);
    fputc('\n', stderr); if (g_log) { fputc('\n', g_log); fflush(g_log); }
}

/* ===================================================================== */
/* CRC-8 Quby — poly=0x21, init=0xff (solved 2026-05-15 from 27 samples) */
/* ===================================================================== */
static uint8_t crc8_quby(const uint8_t *p, size_t n) {
    uint8_t c = 0xff;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x21) : (uint8_t)(c << 1);
    }
    return c;
}

/* ===================================================================== */
/* OT cache — indexed by DID 0..127, value is 16-bit data-value           */
/* ===================================================================== */
static struct {
    uint16_t value;
    time_t   updated;        /* 0 = never */
    int      master_owned;   /* 1 = last-written by happ_thermstat (priority
                                over poll_otgw refresh); 0 = OTGW-sourced. */
} g_cache[128];

/* Update the cache. `from_master` should be 1 for happ_thermstat writes,
 * 0 for OTGW poll updates. Once a DID is master-owned it stays master-owned
 * until another master write changes it — poll_otgw refreshes do not
 * overwrite. This protects the read-after-write verification path: when
 * happ_thermstat writes DID 0 = 0x0300 (CH+DHW enable) and immediately
 * reads it back, the bridge must serve 0x0300 even though OTGW is
 * reporting chenable=Off. */
static void cache_set(uint8_t did, uint16_t value) {
    if (did >= 128) return;
    /* OTGW refresh path — don't clobber values the master owns. */
    if (g_cache[did].master_owned) return;
    g_cache[did].value   = value;
    g_cache[did].updated = time(NULL);
}
static void cache_set_master(uint8_t did, uint16_t value) {
    if (did >= 128) return;
    g_cache[did].value        = value;
    g_cache[did].updated      = time(NULL);
    g_cache[did].master_owned = 1;
}
static int cache_get(uint8_t did, uint16_t *out) {
    if (did >= 128 || g_cache[did].updated == 0) return 0;
    *out = g_cache[did].value;
    return 1;
}

/* ===================================================================== */
/* OTGW HTTP client                                                      */
/* ===================================================================== */
static int tcp_connect(const char *host, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port);
    if (inet_aton(host, &a.sin_addr) == 0) {
        struct hostent *h = gethostbyname(host);
        if (!h) { close(s); return -1; }
        memcpy(&a.sin_addr, h->h_addr_list[0], h->h_length);
    }
    /* Do the connect *before* setting send/recv timeouts. On this Toon
     * kernel a non-zero SO_SNDTIMEO at connect time causes the call to
     * return EINPROGRESS for hosts on the VLAN99 leg, which then made
     * the OTGW serial path (port 25238) impossible to open. The timeouts
     * are restored once the link is established. */
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) != 0) { close(s); return -1; }
    struct timeval tv = {5, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return s;
}

static int http_drain(int fd, char *buf, size_t cap) {
    size_t got = 0;
    while (got < cap - 1) {
        ssize_t r = recv(fd, buf + got, cap - 1 - got, 0);
        if (r <= 0) break;
        got += r;
    }
    buf[got] = 0;
    return (int)got;
}

/* GET <path>, returns body in caller's buffer; -1 on error. */
static int http_get(const char *host, int port, const char *path,
                    char *body, size_t bodysz) {
    int s = tcp_connect(host, port);
    if (s < 0) return -1;
    char req[512];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
        path, host);
    if (send(s, req, n, 0) != n) { close(s); return -1; }
    static char raw[32768];
    int total = http_drain(s, raw, sizeof(raw));
    close(s);
    if (total <= 0) return -1;
    char *bp = strstr(raw, "\r\n\r\n");
    if (!bp) return -1;
    bp += 4;
    snprintf(body, bodysz, "%s", bp);
    return (int)strlen(body);
}

/* POST <path> with JSON body. Fire-and-forget. */
static int http_post_json(const char *host, int port, const char *path,
                          const char *json_body) {
    int s = tcp_connect(host, port);
    if (s < 0) return -1;
    char req[1024];
    int n = snprintf(req, sizeof(req),
        "POST %s HTTP/1.0\r\nHost: %s\r\n"
        "Content-Type: application/json; charset=UTF-8\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
        path, host, strlen(json_body), json_body);
    if (send(s, req, n, 0) != n) { close(s); return -1; }
    char drain[1024];
    http_drain(s, drain, sizeof(drain));
    close(s);
    return 0;
}

static void send_otgw_command(const char *cmd) {
    char body[160];
    snprintf(body, sizeof(body), "{\"command\":\"%s\"}", cmd);
    if (http_post_json(OTGW_HOST, OTGW_PORT, OTGW_CMD_PATH, body) != 0)
        logmsg("otgw command send failed: %s", cmd);
}

/* Send a one-shot command on OTGW's serial-over-IP port (25238). This is
 * the path that actually injects commands onto the OT wire — verified
 * 2026-05-16 with CS=55 producing R90013700/B50013700 and the boiler
 * temperature jumping from 22.4 → 25.7 °C with modulation 29%. The HTTP
 * /api/v0/otgw/command path accepts the same strings but silently drops
 * everything on the floor in this firmware build (PIC 6.5 / Robert van
 * den Breemen 0.10.3+e334c42). */
static void send_otgw_serial(const char *cmd) {
    int s = tcp_connect(OTGW_HOST, OTGW_SERIAL_PORT);
    if (s < 0) {
        logmsg("otgw serial connect failed: %s", strerror(errno));
        return;
    }
    char line[64];
    int n = snprintf(line, sizeof line, "%s\r\n", cmd);
    if (send(s, line, n, MSG_NOSIGNAL) != n)
        logmsg("otgw serial send failed: %s", strerror(errno));
    /* Drain a bit of response so OTGW doesn't see us as a stalled peer. */
    char drain[256];
    struct timeval tv = { 0, 200 * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    recv(s, drain, sizeof drain, 0);
    close(s);
    logmsg("otgw serial: %s", cmd);
}

/* Map a Quby OT-Write (DID + value) to OTGW's higher-level mnemonic
 * command on port 25238 and send it. This is what physically drives the
 * boiler — happ_thermstat → quby_bridge → OTGW → OT-bus → boiler. */
static void forward_write_to_otgw(uint8_t did, uint8_t hi, uint8_t lo) {
    char cmd[32];
    uint16_t raw = ((uint16_t)hi << 8) | lo;
    /* f8.8 fixed-point: signed 8.8 → divide by 256 for °C. */
    double f88 = (int16_t)raw / 256.0;
    switch (did) {
    case 0:      /* Master Status — high byte bit 0 = CH enable. */
        snprintf(cmd, sizeof cmd, "CH=%d", (hi & 0x01) ? 1 : 0);
        send_otgw_serial(cmd);
        return;
    case 1:      /* Control setpoint (CH water target, °C). */
        snprintf(cmd, sizeof cmd, "CS=%.0f", f88);
        send_otgw_serial(cmd);
        return;
    case 16:     /* Room setpoint (the user-facing target temp). */
        snprintf(cmd, sizeof cmd, "TT=%.1f", f88);
        send_otgw_serial(cmd);
        return;
    case 24:     /* Current room temperature (master reports to boiler). */
        snprintf(cmd, sizeof cmd, "TR=%.1f", f88);
        send_otgw_serial(cmd);
        return;
    case 56:     /* DHW setpoint. */
        snprintf(cmd, sizeof cmd, "SW=%.0f", f88);
        send_otgw_serial(cmd);
        return;
    default:
        /* Less-critical writes — log only, no OTGW translation today. */
        return;
    }
}

/* ===================================================================== */
/* otmonitor JSON parsing — find {"name":"X", "value":...}                */
/* ===================================================================== */
/* Extract the value (as string) for a given field name from otmonitor    */
/* response. Returns 0 on success, -1 if not found.                       */
static int otm_find(const char *json, const char *name, char *out, size_t outsz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"name\": \"%s\"", name);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p = strstr(p, "\"value\":");
    if (!p) return -1;
    p += 8;
    while (*p == ' ') p++;
    if (*p == '"') {           /* string value */
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < outsz) out[i++] = *p++;
        out[i] = 0;
    } else {                   /* numeric */
        size_t i = 0;
        while (*p && (*p == '.' || *p == '-' || (*p >= '0' && *p <= '9')) && i + 1 < outsz)
            out[i++] = *p++;
        out[i] = 0;
    }
    return 0;
}

static int otm_bool(const char *json, const char *name) {
    char v[32];
    if (otm_find(json, name, v, sizeof(v)) != 0) return -1;
    return (strcmp(v, "On") == 0 || strcmp(v, "true") == 0
            || strcmp(v, "1") == 0) ? 1 : 0;
}

/* OpenTherm f8.8 encoder — fixed-point with 8 fractional bits */
static uint16_t ot_f88(double v) {
    int s = (int)(v * 256.0 + (v >= 0 ? 0.5 : -0.5));
    return (uint16_t)(int16_t)s;
}

/* Compose a DID value from the otmonitor cache. Returns 1 on success.
 *
 * IMPORTANT: do NOT add household gas-meter or electricity-meter DIDs here.
 * Those are served by /mnt/data/p1bridge from the HomeWizard P1 (HWE-P1),
 * publishing on hcb_rrd's gas_flow/elec_flow series under UUIDs 2177068a
 * (gas) and bd1dfd97 (elec). Adding gas/elec DIDs to this bridge would
 * create duplicate counters in happ_pwrusage's aggregation.
 *
 * SCOPE OF THIS BRIDGE: boiler-side OT data only (CH temps, DHW, pressure,
 * modulation, status flags). Standard OT DataIds 116 (burner starts) and
 * 121 (burner hours) ARE boiler-internal and would be safe to add, but the
 * household gas meter (m³ usage) lives strictly on the P1 path.            */
static int compose_did(uint8_t did, const char *json, uint16_t *out) {
    char v[32];
    switch (did) {
    case 0: {                                  /* Master/Slave Status */
        uint8_t hi = 0, lo = 0;
        /* HB (master): CHenable b0, DHWenable b1, cool b2, OTC b3, CH2 b4 */
        hi |= otm_bool(json, "chenable")     == 1 ? 0x01 : 0;
        hi |= otm_bool(json, "dhwenable")    == 1 ? 0x02 : 0;
        hi |= otm_bool(json, "coolingmodus") == 1 ? 0x04 : 0;
        hi |= otm_bool(json, "otcactive")    == 1 ? 0x08 : 0;
        hi |= otm_bool(json, "ch2enable")    == 1 ? 0x10 : 0;
        /* LB (slave): fault b0, CHmode b1, DHWmode b2, flame b3, cool b4, CH2 b5, diag b6 */
        lo |= otm_bool(json, "faultindicator")      == 1 ? 0x01 : 0;
        lo |= otm_bool(json, "chmodus")             == 1 ? 0x02 : 0;
        lo |= otm_bool(json, "dhwmode")             == 1 ? 0x04 : 0;
        lo |= otm_bool(json, "flamestatus")         == 1 ? 0x08 : 0;
        lo |= otm_bool(json, "coolingactive")       == 1 ? 0x10 : 0;
        lo |= otm_bool(json, "ch2modus")            == 1 ? 0x20 : 0;
        lo |= otm_bool(json, "diagnosticindicator") == 1 ? 0x40 : 0;
        *out = ((uint16_t)hi << 8) | lo;
        return 1; }
    case 1: {                                  /* CH control setpoint (f8.8) */
        if (otm_find(json, "controlsetpoint", v, sizeof(v)) != 0) return 0;
        *out = ot_f88(atof(v)); return 1; }
    case 9: {                                  /* Remote room override setpoint */
        if (otm_find(json, "remoteroomsetpoint", v, sizeof(v)) != 0) return 0;
        *out = ot_f88(atof(v)); return 1; }
    case 14: {                                 /* Max relative modulation level */
        if (otm_find(json, "maxrelmodlvl", v, sizeof(v)) != 0) return 0;
        *out = ot_f88(atof(v)); return 1; }
    case 16: {                                 /* Room setpoint */
        if (otm_find(json, "roomsetpoint", v, sizeof(v)) != 0) return 0;
        *out = ot_f88(atof(v)); return 1; }
    case 17: {                                 /* Relative modulation level */
        if (otm_find(json, "relmodlvl", v, sizeof(v)) != 0) return 0;
        *out = ot_f88(atof(v)); return 1; }
    case 18: {                                 /* CH water pressure */
        if (otm_find(json, "chwaterpressure", v, sizeof(v)) != 0) return 0;
        *out = ot_f88(atof(v)); return 1; }
    case 24: {                                 /* Room temperature */
        if (otm_find(json, "roomtemperature", v, sizeof(v)) != 0) return 0;
        *out = ot_f88(atof(v)); return 1; }
    case 25: {                                 /* Boiler water temperature */
        if (otm_find(json, "boilertemperature", v, sizeof(v)) != 0) return 0;
        *out = ot_f88(atof(v)); return 1; }
    case 26: {                                 /* DHW temperature */
        if (otm_find(json, "dhwtemperature", v, sizeof(v)) != 0) return 0;
        *out = ot_f88(atof(v)); return 1; }
    case 27: {                                 /* Outside temperature */
        if (otm_find(json, "outsidetemperature", v, sizeof(v)) != 0) return 0;
        *out = ot_f88(atof(v)); return 1; }
    case 28: {                                 /* Return water temperature */
        if (otm_find(json, "returnwatertemperature", v, sizeof(v)) != 0) return 0;
        *out = ot_f88(atof(v)); return 1; }
    case 56: {                                 /* DHW setpoint */
        if (otm_find(json, "dhwsetpoint", v, sizeof(v)) != 0) return 0;
        *out = ot_f88(atof(v)); return 1; }
    case 57: {                                 /* Max CH water setpoint */
        if (otm_find(json, "maxchwatersetpoint", v, sizeof(v)) != 0) return 0;
        *out = ot_f88(atof(v)); return 1; }
    case 115: {                                /* OEM diag code */
        if (otm_find(json, "oemdiagnosticcode", v, sizeof(v)) != 0) return 0;
        *out = (uint16_t)atoi(v); return 1; }
    default:
        return 0;                              /* unknown DID — caller sends Unknown-DataId */
    }
}

/* ===================================================================== */
/* BoxTalk publish leg                                                   */
/* ===================================================================== */
static int g_bxt_fd = -1;

static int bxt_connect(void) {
    if (g_bxt_fd >= 0) { close(g_bxt_fd); g_bxt_fd = -1; }
    g_bxt_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_bxt_fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port   = htons(BXT_PORT);
    inet_aton(BXT_HOST, &a.sin_addr);
    if (connect(g_bxt_fd, (struct sockaddr*)&a, sizeof(a)) != 0) {
        close(g_bxt_fd); g_bxt_fd = -1; return -2;
    }
    return 0;
}

static int bxt_send(const char *xml) {
    if (g_bxt_fd < 0) return -1;
    size_t n = strlen(xml);
    if (send(g_bxt_fd, xml, n, MSG_NOSIGNAL) != (ssize_t)n) {
        close(g_bxt_fd); g_bxt_fd = -1; return -1;
    }
    char nul = 0;
    if (send(g_bxt_fd, &nul, 1, MSG_NOSIGNAL) != 1) {
        close(g_bxt_fd); g_bxt_fd = -1; return -1;
    }
    return 0;
}

/* ssdp:connect + ssdp:alive discovery — declare keteladapter-like device
 * with the ThermostatInfo + BoilerInfo services. Without it, hcb_rrd's
 * filter would drop our notifies (toonui takes them unconditionally but
 * the cloud agent etc. need a registered publisher). */
static int bxt_handshake(void) {
    long now = (long)time(NULL); int pid = (int)getpid();
    char buf[2048];
    snprintf(buf, sizeof(buf),
        "<discovery nts=\"ssdp:connect\" uuid=\"%s\" "
        "type=\"urn:schemas-hcb-hae-com:device:keteladapter\" version=\"v\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
        "sessionKey=\"%d-%ld\">\n"
        "\t<device uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:HappBoiler\" "
        "intAddr=\"otgw.thermstat\" version=\"1\">\n"
        "\t<service type=\"ThermostatInfo\" version=\"(null)\"/>\n"
        "\t</device>\n"
        "\t<device uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:HappBoiler\" "
        "intAddr=\"otgw.boiler\" version=\"1\">\n"
        "\t<service type=\"BoilerInfo\" version=\"(null)\"/>\n"
        "\t</device>\n"
        "</discovery>",
        BXT_PUB_UUID, pid, now, UUID_THERMSTAT, UUID_BOILER);
    if (bxt_send(buf) != 0) return -1;
    snprintf(buf, sizeof(buf),
        "<discovery nts=\"ssdp:alive\" uuid=\"%s\" "
        "type=\"urn:schemas-hcb-hae-com:device:keteladapter\" version=\"v\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n"
        "\t<device uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:HappBoiler\" "
        "intAddr=\"otgw.thermstat\" version=\"1\">\n"
        "\t<service type=\"ThermostatInfo\" version=\"(null)\"/>\n"
        "\t</device>\n"
        "\t<device uuid=\"%s\" type=\"urn:schemas-hcb-hae-com:device:HappBoiler\" "
        "intAddr=\"otgw.boiler\" version=\"1\">\n"
        "\t<service type=\"BoilerInfo\" version=\"(null)\"/>\n"
        "\t</device>\n"
        "</discovery>",
        BXT_PUB_UUID, UUID_THERMSTAT, UUID_BOILER);
    return bxt_send(buf);
}

/* hcb_rrd's BoilerChPressure series UUID — verified via strings dump of
 * /HCBv2/var/hcb_rrd/fd71….dat. We have to rebind it from happ_thermstat
 * (which keeps writing zero) to our publisher UUID so the historical
 * pressure chart shows the OTGW value. */
#define RRD_BOILER_CH_PRESSURE_UUID "fd717967-f169-4cf3-8666-4516d9015f6d"

static int bxt_set_rrd_binding(const char *series_uuid, const char *pub_uuid) {
    char buf[640];
    snprintf(buf, sizeof buf,
        "<action class=\"invoke\" uuid=\"%s\" destuuid=\"%s\" serviceid=\"rrdLogger\">\n"
        "    <u:SetRrdDeviceUuid xmlns:u=\"urn:hcb-hae-com:service:rrdLogger:1\">\n"
        "        <deviceUuid>%s</deviceUuid>\n"
        "    </u:SetRrdDeviceUuid>\n"
        "</action>",
        BXT_PUB_UUID, series_uuid, pub_uuid);
    return bxt_send(buf);
}

static int bxt_ensure(void) {
    if (g_bxt_fd >= 0) return 0;
    if (bxt_connect()  != 0) { logmsg("bxt connect: %s", strerror(errno)); return -1; }
    if (bxt_handshake() != 0) { logmsg("bxt handshake failed");
                                close(g_bxt_fd); g_bxt_fd = -1; return -1; }
    /* Rebind the BoilerChPressure RRD series so hcb_rrd records our
     * notifies instead of happ_thermstat's zeroes. Safe to repeat on
     * every reconnect; the action is idempotent. */
    bxt_set_rrd_binding(RRD_BOILER_CH_PRESSURE_UUID, UUID_THERMSTAT);
    logmsg("BoxTalk connected + handshake done + RRD rebind sent");
    return 0;
}

static int bxt_notify(const char *uuid, const char *service,
                      const char *field, double value, int as_int) {
    char buf[768], val[32];
    if (as_int) snprintf(val, sizeof val, "%lld", (long long)value);
    else        snprintf(val, sizeof val, "%.3f", value);
    snprintf(buf, sizeof buf,
        "<notify uuid=\"%s\" serviceid=\"urn:hcb-hae-com:serviceId:%s\">\n"
        "    <%s>%s</%s>\n"
        "</notify>", uuid, service, field, val, field);
    return bxt_send(buf);
}

/* Pull OTGW state once and refresh all known DIDs in the cache. */
static char g_otm_json[16384];

/* Decode an f8.8 fixed-point int back to a float. */
static double f88_to_double(uint16_t v) {
    int16_t s = (int16_t)v;
    return (double)s / 256.0;
}

/* Emit every cached DID that's mapped to a ThermostatInfo / BoilerInfo
 * field. Skipping happens silently when the cache hasn't seen the DID yet
 * (so partial OTGW responses don't cause a flurry of NaN frames). */
static void publish_to_boxtalk(void) {
    if (bxt_ensure() != 0) return;
    uint16_t v;

    /* --- ThermostatInfo --- */
    if (cache_get(18, &v))
        bxt_notify(UUID_THERMSTAT, "ThermostatInfo",
                   "BoilerChPressure", f88_to_double(v), 0);
    if (cache_get(24, &v))
        bxt_notify(UUID_THERMSTAT, "ThermostatInfo",
                   "RoomTemperature", f88_to_double(v), 0);
    if (cache_get(27, &v))
        bxt_notify(UUID_THERMSTAT, "ThermostatInfo",
                   "OutsideTemperature", f88_to_double(v), 0);
    /* Burner state sourced *directly* from OTGW's otmonitor view of the
     * OT bus, not from cache DID 0. Once happ_thermstat writes the
     * master HI byte of DID 0 the bridge marks DID 0 master-owned and
     * poll_otgw stops refreshing the slave LO byte; the cached `flame`
     * bit then stays at whatever it was on the last master-write (=0)
     * even while the boiler is roaring. Reading flamestatus/chmodus/
     * dhwmode straight off otmonitor sidesteps that and gives toonui a
     * burner badge that tracks reality at the 2s poll cadence.
     * Value matches the qt-gui mapping happ_thermstat normally emits:
     *   0=idle, 1=CH burner, 2=hot-water burner. */
    {
        int flame  = otm_bool(g_otm_json, "flamestatus");
        int chmod  = otm_bool(g_otm_json, "chmodus");
        int dhwmod = otm_bool(g_otm_json, "dhwmode");
        int burnerInfo = 0;
        if (flame == 1) {
            if      (dhwmod == 1) burnerInfo = 2;
            else if (chmod  == 1) burnerInfo = 1;
            else                  burnerInfo = 1; /* flame on, no mode bit yet */
        }
        bxt_notify(UUID_THERMSTAT, "ThermostatInfo",
                   "burnerInfo", burnerInfo, 1);
    }

    /* --- BoilerInfo --- */
    if (cache_get(1, &v))
        bxt_notify(UUID_BOILER, "BoilerInfo",
                   "ControlSetpoint", f88_to_double(v), 0);
    if (cache_get(14, &v))
        bxt_notify(UUID_BOILER, "BoilerInfo",
                   "MaxRelativeModulationLevel", f88_to_double(v), 0);
    if (cache_get(17, &v))
        bxt_notify(UUID_BOILER, "BoilerInfo",
                   "RelativeModulationLevel", f88_to_double(v), 0);
    if (cache_get(25, &v))
        bxt_notify(UUID_BOILER, "BoilerInfo",
                   "CurrentBoilerTemperature", f88_to_double(v), 0);
    if (cache_get(26, &v))
        bxt_notify(UUID_BOILER, "BoilerInfo",
                   "DhwTemperature", f88_to_double(v), 0);
    if (cache_get(28, &v))
        bxt_notify(UUID_BOILER, "BoilerInfo",
                   "CurrentBoilerReturnTemperature", f88_to_double(v), 0);
    if (cache_get(56, &v))
        bxt_notify(UUID_BOILER, "BoilerInfo",
                   "DhwSetpoint", f88_to_double(v), 0);
    if (cache_get(57, &v))
        bxt_notify(UUID_BOILER, "BoilerInfo",
                   "MaxChWaterSetpoint", f88_to_double(v), 0);
    if (cache_get(115, &v))
        bxt_notify(UUID_BOILER, "BoilerInfo",
                   "OemDiagCode", (double)v, 1);
}

static void poll_otgw(void) {
    if (http_get(OTGW_HOST, OTGW_PORT, OTMON_PATH,
                 g_otm_json, sizeof(g_otm_json)) <= 0) {
        logmsg("otmonitor GET failed");
        return;
    }
    int updated = 0;
    static const uint8_t known_dids[] = {
        0, 1, 9, 14, 16, 17, 18, 24, 25, 26, 27, 28, 56, 57, 115
    };
    for (size_t i = 0; i < sizeof(known_dids); i++) {
        uint16_t v;
        if (compose_did(known_dids[i], g_otm_json, &v)) {
            cache_set(known_dids[i], v);
            updated++;
        }
    }
    static long n = 0;
    if ((n++ % 30) == 0) {
        logmsg("otgw poll: %d DIDs cached", updated);
        /* every 30th poll, dump cache contents — useful for verification */
        char dump[512]; int o = 0;
        for (size_t i = 0; i < sizeof(known_dids); i++) {
            uint16_t v;
            if (cache_get(known_dids[i], &v))
                o += snprintf(dump + o, sizeof(dump) - o,
                              " DID%d=0x%04x", known_dids[i], v);
        }
        logmsg("cache:%s", dump);
    }

    /* Push the freshly-cached DIDs out on the BoxTalk bus so toonui /
     * hcb_rrd / cloud-agent see real boiler state regardless of what
     * happ_thermstat is doing on its own UUID. */
    publish_to_boxtalk();
}

/* ===================================================================== */
/* Quby frame parser / responder                                         */
/* ===================================================================== */
/* Frame header bytes — high bit = request (master→BA), cleared = response. */
#define HDR_CTRL_REQ   0xC2
#define HDR_CTRL_RSP   0x42
#define HDR_SUB_REQ    0xCB
#define HDR_SUB_RSP    0x4B
#define HDR_OT_REQ     0xCD
#define HDR_OT_RSP     0x4D

/* Build a 7-byte response with our CRC. */
static void build_frame(uint8_t hdr, uint8_t op,
                        uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                        uint8_t out[7]) {
    out[0] = hdr; out[1] = op;
    out[2] = a; out[3] = b; out[4] = c; out[5] = d;
    out[6] = crc8_quby(out, 6);
}

/* Handle one 7-byte master→slave frame, write the response into resp[7].
   Returns 1 if we have a response to send, 0 if we should stay silent. */
static int handle_frame(const uint8_t f[7], uint8_t resp[7]) {
    /* The boot-knock GetVersion (c2 56 ...) is sent WITHOUT a CRC per spec
       §1.4 — accept it regardless of trailing byte. Every other frame must
       pass CRC. */
    int is_boot_knock = (f[0] == HDR_CTRL_REQ && f[1] == 'V');
    if (!is_boot_knock && crc8_quby(f, 6) != f[6]) {
        logmsg("bad CRC on rx frame: %02x %02x %02x %02x %02x %02x crc=%02x exp=%02x",
               f[0], f[1], f[2], f[3], f[4], f[5], f[6], crc8_quby(f, 6));
        return 0;
    }
    switch (f[0]) {
    case HDR_CTRL_REQ:
        /* 'V'=GetVersion, 'Y'=GetParameter, 'J'/'P'=heartbeat. For MVP we
           echo the known-good responses from the 2026-05-15 capture: status
           bytes that happ_thermstat treats as "alive, no fault". */
        if (f[1] == 'J' || f[1] == 'P') {
            /* heartbeat — return the same op with all-zero status */
            build_frame(HDR_CTRL_RSP, f[1], 0, 0, 0, 0, resp);
            return 1;
        }
        if (f[1] == 'V') {                 /* GetVersion (boot-knock) */
            /* Match captured BA response from ttymxc0 capture:
               42 56 41 43 00 25 → HDR=CTRL_RSP, OP='V', 'A','C' (board rev
               AC), 0x00, fw=37. CRC computed. */
            resp[0] = HDR_CTRL_RSP;
            resp[1] = 'V';
            resp[2] = 'A';   resp[3] = 'C';     /* SCB board rev AC */
            resp[4] = 0x00;  resp[5] = 37;      /* fw v37 */
            resp[6] = crc8_quby(resp, 6);
            return 1;
        }
        if (f[1] == 'Y') {                 /* GetParameter(idx in byte 5) */
            /* happ_thermstat reads idx 0..3 at boot to identify the BA;
             * if it sees all zeros it may mark the BA as unrecognised.
             * Values lifted verbatim from the live keteladapter capture
             * (quby_protocol_re.md §3.1):
             *   0 → 00 00 65 99   (product code 0x6599)
             *   1 → 17 00 24 00   (HW 1.7 / SW build 0.24)
             *   2 → 00 00 00 18   (counter / flags 24)
             *   3 → 02 88 45 57   (serial 0x02884557)                */
            static const uint8_t param_table[4][4] = {
                {0x00, 0x00, 0x65, 0x99},
                {0x17, 0x00, 0x24, 0x00},
                {0x00, 0x00, 0x00, 0x18},
                {0x02, 0x88, 0x45, 0x57},
            };
            uint8_t idx = f[5];
            uint8_t pa = 0, pb = 0, pc = 0, pd = 0;
            if (idx < 4) {
                pa = param_table[idx][0];
                pb = param_table[idx][1];
                pc = param_table[idx][2];
                pd = param_table[idx][3];
            }
            build_frame(HDR_CTRL_RSP, 'Y', pa, pb, pc, pd, resp);
            return 1;
        }
        /* unknown ctrl op — silent */
        return 0;

    case HDR_SUB_REQ:
        /* Both 'E'=Enable and 'S'=Subscribe expect a *generic ACK*:
         *   42 41 00 00 00 00 2e
         * per quby_protocol_re.md §3.2. Echoing the opcode back (which
         * we used to do, building HDR_SUB_RSP=0x4B with the request's
         * own op) is what made happ_thermstat decide the BA wasn't
         * speaking the protocol correctly and flip otCommError. */
        build_frame(HDR_CTRL_RSP, 'A', 0, 0, 0, 0, resp);
        return 1;

    case HDR_OT_REQ: {
        /* OpenTherm read/write wrap — frame layout per Quby spec §2.3:
           f[0]=HDR  f[1]=OP(msg-type)  f[2]=A(spare bits / msg-type encoding)
           f[3]=B(DID)  f[4]=C(data-hi)  f[5]=D(data-lo)  f[6]=CRC          */
        uint8_t mtype = f[1];
        uint8_t did   = f[3];
        if (mtype == 0x00) {               /* OT-Read */
            uint16_t v;
            int have = cache_get(did, &v);
            if (!have) {
                /* Synthesize a believable answer for DIDs happ_thermstat
                 * keys off when deciding "OT is alive". Returning
                 * Unknown-DataId (msg-type 7) makes it flip otCommError=1
                 * and refuse to drive the boiler. Returning a valid
                 * Read-Ack with zero data keeps it polling cheerfully. */
                switch (did) {
                case 0:    v = 0x0300; break;  /* Master+Slave status:
                                                  HI = echo of CH+DHW enable
                                                       (matches what master
                                                        last wrote);
                                                  LO = 0 → no fault, not
                                                        heating, no DHW draw,
                                                        no diagnostic flag.
                                                  Setting LO bit 0 (fault)
                                                  is what was tripping
                                                  otCommError. */
                case 3:    v = 0x8304; break;  /* Slave Config:
                                                  hi=0x83 (DHW+CH+modulation+AA),
                                                  lo=0x04 (member-id Intergas —
                                                  the boiler this Toon was
                                                  paired with originally) */
                case 5:    v = 0x0000; break;  /* App-specific fault flags */
                case 6:    v = 0xfff8; break;  /* Remote boiler param flags */
                case 0x7C: v = 0x0220; break;  /* Master OT version 2.2 */
                case 0x7D: v = 0x0103; break;  /* Slave product version */
                case 0x7E: v = 0x0220; break;  /* Master product version */
                case 0x7F: v = 0x0322; break;  /* Slave OT version 3.34 */
                default:   v = 0x0000; break;
                }
                have = 1;
            }
            /* Read response shape per quby_protocol_re.md §3.3:
             *   4d 00 <MT> <DID> <HI> <LO> <CRC>
             *      ▲    ▲
             *      │    └── 0x04 (Read-Ack) when we have data, 0x0A
             *      │        (Unknown-DataId) when we don't.
             *      └── byte 1 echoes the request's op (0x00 = Read-Data),
             *          NOT the response msg-type. Getting this wrong is
             *          what was making happ_thermstat ignore every reply
             *          and trip otCommError. */
            uint8_t mt = have ? 0x04 : 0x0a;
            resp[0] = HDR_OT_RSP; resp[1] = 0x00;
            resp[2] = mt;         resp[3] = did;
            resp[4] = (v >> 8) & 0xff; resp[5] = v & 0xff;
            resp[6] = crc8_quby(resp, 6);
            return 1;
        }
        if (mtype == 0x02) {               /* OT-Write */
            /* Build a proper 32-bit OpenTherm frame:
             *   bit 31     : even parity over the other 31 bits
             *   bits 30-28 : msg-type 001 = Write-Data
             *   bits 27-24 : spare (zero)
             *   bits 23-16 : DataID
             *   bits 15-0  : DataValue
             * The Quby `A` byte (f[2]) holds spare bits in the upper
             * nibble (low nibble carries flag bits the BA tracks but the
             * boiler ignores), so we mask it down to those 4 spare
             * bits and slot them into bits 27-24 of the OT frame. The
             * previous build just copied f[2] verbatim into the OT high
             * byte, leaving msg-type=000 (Read-Data) with broken parity
             * — the boiler never saw a valid Write so chenable stayed
             * Off forever. */
            uint32_t spare = (f[2] >> 4) & 0x0f;
            uint32_t frame = ((uint32_t)0x10 << 24)        /* mtype=Write-Data */
                           | (spare << 24)                 /* spare nibble */
                           | ((uint32_t)did  << 16)
                           | ((uint32_t)f[4] << 8)
                           |  (uint32_t)f[5];
            /* Compute even parity over bits 0..30 and stuff into bit 31. */
            uint32_t v = frame;
            int ones = 0;
            while (v) { ones += (int)(v & 1u); v >>= 1; }
            if (ones & 1) frame |= 0x80000000u;
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "OT=%08X", frame);
            send_otgw_command(cmd);       /* HTTP path — kept for parity */
            forward_write_to_otgw(did, f[4], f[5]);   /* the one that actually
                                                         injects on the wire */
            logmsg("OT-Write fwd to OTGW: %s (DID=%u)", cmd, did);
            /* Write-through: the master will read this DID back almost
             * immediately to verify the value stuck. Mirror the written
             * data into our cache as MASTER-OWNED so the next OTGW poll
             * doesn't clobber it. Without write-through (or with it but
             * letting poll_otgw refresh on top), happ_thermstat sees the
             * readback return the older OTGW value and treats the
             * discrepancy as an OT comm problem (→ otCommError stays
             * at 1 forever). */
            cache_set_master(did, ((uint16_t)f[4] << 8) | f[5]);
            /* Write response is the *generic ACK* (42 41 00 00 00 00 2e)
             * per §3.3. The BA does NOT wait for the OT bus's Write-Ack
             * back from the boiler — the eventual confirmation shows up
             * on the next Read of the same DID. Sending a `4d 05 …`
             * frame here is wrong and was contributing to otCommError. */
            build_frame(HDR_CTRL_RSP, 'A', 0, 0, 0, 0, resp);
            return 1;
        }
        return 0;
    }

    default:
        return 0;
    }
}

/* ===================================================================== */
/* PTY setup + serial config                                             */
/* ===================================================================== */
static int g_pty_master = -1;
static int g_pty_slave_keeper = -1;
static char g_pty_slave[64] = "";

static int open_pty(void) {
    int m, s;
    if (openpty(&m, &s, g_pty_slave, NULL, NULL) != 0) {
        logmsg("openpty: %s", strerror(errno));
        return -1;
    }
    /* Keep the slave fd open as a "keeper" — if we close it, the pty enters
       HUP state and subsequent opens of the slave path return EIO. */
    g_pty_slave_keeper = s;
    /* configure raw mode on master, 9600 8N1 */
    struct termios t;
    if (tcgetattr(m, &t) == 0) {
        cfmakeraw(&t);
        cfsetspeed(&t, B9600);
        tcsetattr(m, TCSANOW, &t);
    }
    /* match on the slave side too */
    if (tcgetattr(s, &t) == 0) {
        cfmakeraw(&t);
        cfsetspeed(&t, B9600);
        tcsetattr(s, TCSANOW, &t);
    }
    return m;
}

/* In passive/bench, expose the slave path via a symlink so a tester can find it. */
static void expose_pty(void) {
    unlink(PTY_LINK);
    if (symlink(g_pty_slave, PTY_LINK) == 0)
        logmsg("pty slave: %s (linked at %s)", g_pty_slave, PTY_LINK);
    else
        logmsg("pty slave: %s (no symlink: %s)", g_pty_slave, strerror(errno));
}

/* Bind-mount the pty slave over /dev/ttymxc0. Active mode only. */
static int bind_mount_tty(void) {
    if (mount(g_pty_slave, TTY_PATH, NULL, MS_BIND, NULL) != 0) {
        logmsg("bind-mount %s -> %s: %s", g_pty_slave, TTY_PATH, strerror(errno));
        return -1;
    }
    logmsg("bind-mounted %s over %s", g_pty_slave, TTY_PATH);
    return 0;
}

/* After bind-mount, any running happ_thermstat still holds an fd to the
 * real kernel UART. Kick it so init respawns it against the new pty.
 * Returns the pid killed, or 0 if none. */
static int kick_thermstat(void) {
    FILE *fp = popen("pidof happ_thermstat 2>/dev/null", "r");
    if (!fp) return 0;
    int pid = 0; fscanf(fp, "%d", &pid); pclose(fp);
    if (pid > 0) {
        if (kill(pid, SIGTERM) == 0)
            logmsg("kicked happ_thermstat pid=%d (will re-open /dev/ttymxc0)", pid);
        else
            logmsg("kick happ_thermstat: %s", strerror(errno));
    }
    return pid;
}

static void unbind_tty(void) {
    if (umount(TTY_PATH) == 0) logmsg("unmounted %s", TTY_PATH);
}

/* ===================================================================== */
/* boot handshake — 256 bytes of 0x6A in two 128-byte bursts              */
/* ===================================================================== */
static void send_boot_sync(int fd) {
    uint8_t pad[128];
    memset(pad, 0x6A, sizeof(pad));
    write(fd, pad, sizeof(pad));
    write(fd, pad, sizeof(pad));
    logmsg("boot sync: sent 256 x 0x6A");
}

/* ===================================================================== */
/* main loop                                                             */
/* ===================================================================== */
static volatile sig_atomic_t g_stop = 0;
static void on_sig(int sig) { (void)sig; g_stop = 1; }

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s -m <passive|bench|active> [options]\n"
        "Modes:\n"
        "  passive   open pty + poll OTGW, log frames, never reply\n"
        "  bench     reply to frames on the pty (no bind-mount)\n"
        "  active    bind-mount pty over /dev/ttymxc0 and serve happ_thermstat\n",
        p);
}

int main(int argc, char **argv) {
    const char *mode = "passive";
    int reply = 0, do_bind = 0;
    int opt;
    while ((opt = getopt(argc, argv, "m:h")) != -1) {
        switch (opt) {
        case 'm': mode = optarg; break;
        case 'h': default: usage(argv[0]); return 1;
        }
    }
    if (!strcmp(mode, "passive"))      { reply = 0; do_bind = 0; }
    else if (!strcmp(mode, "bench"))   { reply = 1; do_bind = 0; }
    else if (!strcmp(mode, "active"))  { reply = 1; do_bind = 1; }
    else { usage(argv[0]); return 1; }

    g_log = fopen(LOGFILE, "a");
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);
    logmsg("starting in %s mode pid=%d", mode, getpid());

    g_pty_master = open_pty();
    if (g_pty_master < 0) return 2;
    expose_pty();
    if (do_bind) {
        if (bind_mount_tty() != 0) return 3;
        /* If happ_thermstat is already running, it holds an fd to the real
           kernel UART. Kick it so init respawns into the new pty. */
        kick_thermstat();
    }

    /* First poll so we have data before any read arrives */
    poll_otgw();
    time_t next_poll = time(NULL) + POLL_INTERVAL_S;

    if (reply) send_boot_sync(g_pty_master);

    uint8_t rbuf[1024]; size_t rlen = 0;
    while (!g_stop) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(g_pty_master, &rfds);
        time_t now = time(NULL);
        struct timeval tv = {
            .tv_sec  = (next_poll > now) ? (next_poll - now) : 0,
            .tv_usec = 100000  /* always wake at least every 100ms */
        };
        int sel = select(g_pty_master + 1, &rfds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(g_pty_master, &rfds)) {
            if (rlen >= sizeof(rbuf)) rlen = 0;  /* overflow guard */
            ssize_t n = read(g_pty_master, rbuf + rlen, sizeof(rbuf) - rlen);
            if (n > 0) {
                rlen += (size_t)n;
                /* drop leading 0x6A sync bytes */
                size_t s = 0;
                while (s < rlen && rbuf[s] == 0x6A) s++;
                if (s) { memmove(rbuf, rbuf + s, rlen - s); rlen -= s; }
                /* parse 7-byte frames */
                while (rlen >= 7) {
                    uint8_t hdr = rbuf[0];
                    if ((hdr & 0x80) == 0 || (hdr != HDR_CTRL_REQ
                        && hdr != HDR_SUB_REQ && hdr != HDR_OT_REQ)) {
                        /* resync — drop one byte */
                        memmove(rbuf, rbuf + 1, --rlen);
                        continue;
                    }
                    logmsg("rx: %02x %02x %02x %02x %02x %02x %02x",
                           rbuf[0], rbuf[1], rbuf[2], rbuf[3],
                           rbuf[4], rbuf[5], rbuf[6]);
                    if (reply) {
                        uint8_t resp[7];
                        if (handle_frame(rbuf, resp)) {
                            write(g_pty_master, resp, 7);
                            logmsg("tx: %02x %02x %02x %02x %02x %02x %02x",
                                   resp[0], resp[1], resp[2], resp[3],
                                   resp[4], resp[5], resp[6]);
                        }
                    }
                    memmove(rbuf, rbuf + 7, rlen - 7);
                    rlen -= 7;
                }
            } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
                /* EIO is normal when no consumer has opened the slave yet —
                   stay quiet, just yield a bit. */
                if (errno != EIO) logmsg("pty read err: %s", strerror(errno));
                struct timespec slp = { 0, 200 * 1000 * 1000 }; /* 200ms */
                nanosleep(&slp, NULL);
            }
        }
        if (time(NULL) >= next_poll) {
            poll_otgw();
            next_poll = time(NULL) + POLL_INTERVAL_S;
        }
    }
    if (do_bind) unbind_tty();
    logmsg("stopping");
    return 0;
}
