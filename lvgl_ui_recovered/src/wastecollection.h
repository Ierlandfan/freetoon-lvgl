#ifndef TOON_WASTECOLLECTION_H
#define TOON_WASTECOLLECTION_H

#include <stddef.h>

/* HVC Groep inzamelkalender REST API.
 * Two-step: 1) postcode+huisnummer → BAG ID; 2) BAG → afvalstromen.
 * IDs we care about (see cyberjunky/home-assistant-hvcgroep):
 *   2 = Restafval  3 = Papier  5 = GFT  6 = Plastic */

#define WASTE_TYPES 4
#define WASTE_MAX_EVENTS 128

typedef struct {
    char    label[40];      /* HVC: "Restafval"/"GFT"/... ; ICS: the raw calendar
                               SUMMARY (e.g. "Droge herbruikbare materialen"). */
    char    date[16];       /* "YYYY-MM-DD", empty if none */
} waste_item_t;

typedef struct {
    char    date[16];       /* "YYYY-MM-DD" */
    char    label[40];
} waste_event_t;

typedef struct {
    volatile int connected;
    volatile int wake_fetch;   /* set to 1 to trigger an immediate re-fetch */
    char         bag_id[24];
    waste_item_t items[WASTE_TYPES];
    waste_event_t events[WASTE_MAX_EVENTS];
    int          event_count;
} waste_state_t;

extern waste_state_t waste_state;

int waste_start(void);

/* Returns the soonest upcoming pickup date and a comma-joined types list.
   `out_date` is empty if none scheduled. */
void waste_next_pickup(char * out_date, int dsz, char * out_labels, int lsz);

/* Soonest two distinct pickup dates plus their "+"-joined type lists.
   Returns the number filled (0..2). */
typedef struct {
    char date[16];
    char labels[40];
} waste_pickup_t;
int waste_next_2_pickups(waste_pickup_t * out1, waste_pickup_t * out2);

/* Up to max_n soonest upcoming pickups within `lead_days`, with a per-type
   pickup-day cutoff (Plastic until 21:00, others until 16:00). Single source
   of truth for the home tile and dim screen. Returns count filled. */
int waste_next_n_windowed(int lead_days, waste_pickup_t * out, int max_n);

/* Whole days from today to an ISO "YYYY-MM-DD" date (negative if past). */
long waste_days_until(const char * date);

/* Checks if a waste pickup on `date` is currently within the active dim-screen display window:
 * from (lead_days) days before pickup at `alert_hour`:00, until pickup day at `alert_hour`:00.
 * If lead_days <= 0, filter is off (always active until pickup day at `alert_hour`:00).
 * Returns:
 *   1 = currently active (within window)
 *   0 = upcoming, but not yet in window (too early)
 *  -1 = expired (already past pickup day at `alert_hour`:00)
 */
int waste_dim_window_status(const char * date, int lead_days, int alert_hour);

/* Finds the soonest non-expired pickup and, if it falls within the dim warning window,
 * fills `out` with its date and "+"-joined labels and returns 1. Returns 0 if none active. */
int waste_get_dim_pickup(waste_pickup_t * out, int lead_days, int alert_hour);

/* Shorten waste category label for compact display (e.g. dim screen).
 * "Groente/Fruit/Tuinafval" -> "GFT"
 * "Plastic/Metaal/Drankpakken" -> "PMD"
 * "Papier en karton" -> "Papier"
 * Handles composite labels with "+" (e.g. "GFT + PMD"). */
void waste_shorten_label(const char * in, char * out, size_t outsz);

#endif
