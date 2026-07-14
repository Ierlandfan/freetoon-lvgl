#ifndef TOON_P1ESPHOME_H
#define TOON_P1ESPHOME_H

/* Live state from an ESPHome-based P1 reader (Zuidwijk SlimmeLezer / SlimmeLezer+
   and anything else running ESPHome's `dsmr` platform). Updated by p1_thread. */
typedef struct {
    volatile int   connected;
    volatile int   polled;            /* 1 once a poll has been attempted */
    volatile float power_w;           /* net: consumed - produced (negative = export) */
    volatile float consumed_w;        /* Power Consumed  (DSMR power_delivered) */
    volatile float produced_w;        /* Power Produced  (DSMR power_returned) = grid EXPORT,
                                         NOT gross solar production */
    volatile float kwh_import_t1;
    volatile float kwh_import_t2;
    volatile float kwh_export_t1;
    volatile float kwh_export_t2;
    volatile float gas_m3;            /* cumulative */
    volatile float gas_hour_m3;       /* trailing ~60 min, derived from the counter */
} p1esp_state_t;

extern p1esp_state_t p1esp_state;

int p1esphome_start(void);

#endif
