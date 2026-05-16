/*
 * Pushes samples into hcb_rrd's setRrdData HTTP endpoint.
 * Format mirrors what the TSC toonWater plugin uses:
 *   GET /hcb_rrd?action=setRrdData&loggerName=X&rra=Y&samples=URLEnc{"TS":VAL}
 */
#include "rrd_push.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rrd_push(const char * logger_name, const char * rra,
             long ts, double value) {
    if (!logger_name || !rra) return -1;
    /* JSON body: {"<ts>":<value>}. Build then URL-encode the special chars. */
    char json[96];
    snprintf(json, sizeof(json), "{\"%ld\":%.3f}", ts, value);

    char enc[256];
    char * dst = enc;
    for (const char * p = json; *p && dst < enc + sizeof(enc) - 4; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= '0' && c <= '9') || c == '.' || c == '-') {
            *dst++ = (char)c;
        } else if (c == '{') { memcpy(dst, "%7B", 3); dst += 3; }
        else if (c == '}')   { memcpy(dst, "%7D", 3); dst += 3; }
        else if (c == '"')   { memcpy(dst, "%22", 3); dst += 3; }
        else if (c == ':')   { memcpy(dst, "%3A", 3); dst += 3; }
        else                 { *dst++ = '_'; }
    }
    *dst = 0;

    char cmd[640];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/curl -s --max-time 3 "
        "'http://localhost:10080/hcb_rrd?action=setRrdData&loggerName=%s&rra=%s&samples=%s' >/dev/null 2>&1",
        logger_name, rra, enc);
    int rc = system(cmd);
    return (rc == 0) ? 0 : -1;
}
