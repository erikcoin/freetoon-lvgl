#ifndef TOON_RRD_PUSH_H
#define TOON_RRD_PUSH_H

/* Pushes a single sample into hcb_rrd via setRrdData.
   logger_name = "elec_flow" / "gas_quantity" / etc.
   rra         = "5min" / "5yrhours" / "10yrdays" / ...
   ts          = unix timestamp the sample belongs to
   value       = numeric value (printed as double; hcb_rrd will cast)
   Returns 0 on success. Uses curl in a child process. */
int rrd_push(const char * logger_name, const char * rra,
             long ts, double value);

#endif
