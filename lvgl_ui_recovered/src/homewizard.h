#ifndef TOON_HOMEWIZARD_H
#define TOON_HOMEWIZARD_H

/* Live state from the two HomeWizard devices on VLAN99.
   Updated by homewizard_thread (poll every 5s). */
typedef struct {
    volatile int    connected_p1;
    volatile float  power_w;          /* active_power_w (signed; negative = export) */
    volatile float  kwh_import_t1;    /* total_power_import_t1_kwh */
    volatile float  kwh_import_t2;    /* total_power_import_t2_kwh */
    volatile float  kwh_import_total; /* total_power_import_kwh */
    volatile float  kwh_export_total; /* total_power_export_kwh */
    volatile int    tariff;           /* active_tariff: 1 or 2 */
    volatile float  gas_m3;           /* total_gas_m3 */
    volatile float  voltage_l1_v;
    volatile float  current_l1_a;

    volatile int    connected_water;
    volatile float  water_total_m3;   /* total_liter_m3 (cumulative m³) */
    volatile float  water_lpm;        /* active_liter_lpm */
} hw_state_t;

extern hw_state_t hw_state;

int homewizard_start(void);

#endif
