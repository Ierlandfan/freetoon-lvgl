#ifndef TOON_SCHEDULE_H
#define TOON_SCHEDULE_H

/* Single weekly_recurring schedule entry. */
typedef struct {
    int target_state;   /* 0=Comfort 1=Home 2=Sleep 3=Away */
    int start_min;      /* 0..59 */
    int start_hour;     /* 0..23 */
    int start_day;      /* 0..6 (Toon convention: 0 = Monday) */
    int end_min;
    int end_hour;
    int end_day;
} schedule_entry_t;

#define SCHEDULE_MAX 64
extern schedule_entry_t schedule_entries[SCHEDULE_MAX];
extern int schedule_count;

/* Fetch current schedule from hcb_config via HTTP, parse into the
   schedule_entries array. Returns 0 on success. */
int schedule_load(void);

/* Write current schedule_entries back. Returns 0 on success. */
int schedule_save(void);

/* Append a new entry — used by the day-edit UI. Returns 0 if accepted. */
int schedule_add(const schedule_entry_t * e);

/* Remove entry at index. */
int schedule_remove(int idx);

/* Update entry at index. */
int schedule_replace(int idx, const schedule_entry_t * e);

/* Returns hex color for a given target_state for consistent UI coloring. */
unsigned schedule_state_color(int state);
const char * schedule_state_name(int state);
const char * schedule_day_short(int day);   /* "Mon", "Tue", ... */

/* Returns 0..3 = the preset the schedule says is active right now (the
 * most-recent past start_time, wrapping the week). -1 if no entries. */
int schedule_program_now(void);

#endif
