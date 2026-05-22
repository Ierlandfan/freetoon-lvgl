#ifndef TOON_WASTECOLLECTION_H
#define TOON_WASTECOLLECTION_H

/* HVC Groep inzamelkalender REST API.
 * Two-step: 1) postcode+huisnummer → BAG ID; 2) BAG → afvalstromen.
 * IDs we care about (see cyberjunky/home-assistant-hvcgroep):
 *   2 = Restafval  3 = Papier  5 = GFT  6 = Plastic */

#define WASTE_TYPES 4

typedef struct {
    char    label[40];      /* HVC: "Restafval"/"GFT"/... ; ICS: the raw calendar
                               SUMMARY (e.g. "Droge herbruikbare materialen"). */
    char    date[16];       /* "YYYY-MM-DD", empty if none */
} waste_item_t;

typedef struct {
    volatile int connected;
    char         bag_id[24];
    waste_item_t items[WASTE_TYPES];
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

#endif
