/* sp_time.h -- Time value type + libc-backed accessors / formatters.
 *
 * The whole of spinel's Time implementation lives in lib/sp_time.c
 * (compiled into libspinel_rt.a), including the formatters, which now
 * return GC-heap strings directly (sp_time.c includes sp_alloc.h) -- so
 * the generated TU calls them straight, with no buffer-copying
 * trampoline in spinel_rt.h.
 *
 * The value ops (constructors, accessors, shifts) use int64_t / double
 * directly, keeping that part decoupled from the runtime's typedefs.
 */
#ifndef SP_TIME_H
#define SP_TIME_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* is_utc is a 3-state zone kind: 0 = host-local zone, 1 = UTC, 2 = a fixed
   utc_off-seconds offset (Time.new's 7th argument). utc_off is meaningful
   only for kind 2; the positional (sp_Time){sec, ns, kind} initializers used
   throughout leave it zero. */
typedef struct { int64_t tv_sec; int32_t tv_nsec; int8_t is_utc; int32_t utc_off; } sp_Time;
/* Seconds as a Float the way CRuby's Time#to_f and Time#- make them: the
   whole nanosecond count converted, then divided by 1e9. That count is not
   always a double, so the answer can sit just below the decimal value
   (1790544731.561 reads 1790544731.5609999), and an epoch-milliseconds
   `(t.to_f * 1000).to_i` depends on it (#5211). Past Fixnum's reach
   (2**62 ns) CRuby rounds the exact quotient, which the split sum is the
   nearest to here. */
static inline double sp_time_ns_to_f(int64_t sec, int64_t nsec) {
  if (sec > -4611686018LL && sec < 4611686018LL)
    return (double)(sec * 1000000000LL + nsec) / 1e9;
  return (double)sec + (double)nsec / 1e9;
}

/* The Time a Time subclass instance starts with, inside the instance, before
   its `new`, `at` or `now` sets it: the epoch, local (Time.allocate's). */
static inline void sp_Time_init_embedded(sp_Time *t) { *t = (sp_Time){0, 0, 0, 0}; }

/* Constructors */
sp_Time sp_timev_now(void);
sp_Time sp_timev_at_int(int64_t sec);
sp_Time sp_timev_norm(sp_Time t);   /* carry a whole second out of tv_nsec */
sp_Time sp_timev_add_nsec(sp_Time t, int64_t ns);
int64_t sp_time_zone_arg_off(const char *z, int *is_utc_out);
sp_Time sp_timev_in_zone_i(sp_Time t, int64_t off);
sp_Time sp_timev_in_zone_s(sp_Time t, const char *z);
sp_Time sp_timev_at_float(double epoch);
sp_Time sp_timev_at_div(int64_t num, int64_t den);
sp_Time sp_timev_parse(const char *s);
/* require "time"'s strict class parsers */
sp_Time sp_timev_s_httpdate(const char *s);
sp_Time sp_timev_s_rfc2822(const char *s);
sp_Time sp_timev_s_xmlschema(const char *s);
/* and Time.parse, for the numeric ISO 8601 forms */
sp_Time sp_timev_s_parse(const char *s);
sp_Time sp_timev_new(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi, int64_t s);
int64_t sp_time_month_arg(const char *s);
sp_Time sp_timev_new_utc(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi, int64_t s);
sp_Time sp_timev_new_off(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi, int64_t s, int64_t off);
sp_Time sp_timev_with_usec(sp_Time t, int64_t usec);
sp_Time sp_timev_with_usec_f(sp_Time t, double usec);
sp_Time sp_timev_utc(sp_Time t);
sp_Time sp_timev_localtime(sp_Time t);

/* Broken-down resolver: is_utc selects gmtime vs localtime, off is the
   UTC offset in seconds, zbuf is the timezone abbreviation (8 bytes). */
void sp_timev_vtm(sp_Time t, struct tm *bd, int32_t *off, char *zbuf);

/* Field accessors */
int64_t sp_timev_year(sp_Time t);
int64_t sp_timev_mon(sp_Time t);
int64_t sp_timev_mday(sp_Time t);
int64_t sp_timev_hour(sp_Time t);
int64_t sp_timev_min(sp_Time t);
int64_t sp_timev_sec(sp_Time t);
int64_t sp_timev_wday(sp_Time t);
int64_t sp_timev_yday(sp_Time t);
int64_t sp_timev_isdst(sp_Time t);
int64_t sp_timev_utc_offset(sp_Time t);
/* Time#floor / #ceil / #round: `ndigits` decimal places of the subsecond part,
   with mode 0 = floor, 1 = ceil, 2 = round-half-up. ndigits 0 is the no-argument
   form (whole seconds) and >= 9 keeps the full nanosecond resolution; a carry
   past a second bumps tv_sec. A negative ndigits is CRuby's ArgumentError. */
sp_Time sp_timev_round_to(sp_Time t, int64_t ndigits, int mode);

/* Time + Numeric / Time - Numeric. secs may be fractional. */
sp_Time sp_timev_add(sp_Time t, double secs);

/* Comparison + integer/float shifts + Time-Time difference (cold value ops). */
int sp_timev_cmp(sp_Time a, sp_Time b);
int64_t sp_timev_hash(sp_Time t);
sp_Time sp_timev_add_f(sp_Time t, double secs);
sp_Time sp_timev_add_i(sp_Time t, int64_t secs);
sp_Time sp_timev_sub_i(sp_Time t, int64_t secs);
double sp_timev_sub_t(sp_Time a, sp_Time b);

/* Formatters: return a GC-heap string (from sp_alloc.h), "" on failure. */
const char *sp_timev_strftime(sp_Time t, const char *fmt);
const char *sp_timev_iso8601(sp_Time t);
const char *sp_timev_httpdate(sp_Time t);
const char *sp_timev_rfc2822(sp_Time t);
const char *sp_timev_iso8601_frac(sp_Time t, int64_t digits);
sp_Time sp_timev_getlocal_off(sp_Time t, int64_t off);
const char *sp_timev_zone(sp_Time t);
const char *sp_timev_inspect_v(sp_Time t);  /* renders fractional seconds */
const char *sp_timev_to_s_v(sp_Time t);     /* whole seconds only (Time#to_s) */

/* A Time is a reference, as in CRuby: localtime / utc / gmtime change the
   object every holder sees. The sp_timev_ functions above compute on the
   value; the sp_time_ functions below take and answer GC-heap Times, a new
   one for every answer that is a Time, and are what the generated code
   calls. The _bang forms change the Time in place and answer it. */
sp_Time *sp_time_ref(sp_Time v);
sp_Time *sp_time_dup(const sp_Time *t);
sp_Time *sp_time_utc_bang(sp_Time *t);
sp_Time *sp_time_localtime_bang(sp_Time *t);
sp_Time *sp_time_getlocal_off_bang(sp_Time *t, int64_t off);
/* one of the fixed-format parsers, handed a Symbol's name */
sp_Time *sp_time_s_parse_sym(sp_Time *(*parse)(const char *), const char *name);
sp_Time *sp_time_in_zone_s_bang(sp_Time *t, const char *z);
sp_Time *sp_time_now(void);
sp_Time *sp_time_at_int(int64_t sec);
sp_Time *sp_time_norm(const sp_Time *t);
sp_Time *sp_time_add_nsec(const sp_Time *t, int64_t ns);
sp_Time *sp_time_in_zone_i(const sp_Time *t, int64_t off);
sp_Time *sp_time_in_zone_s(const sp_Time *t, const char *z);
sp_Time *sp_time_at_float(double epoch);
sp_Time *sp_time_at_div(int64_t num, int64_t den);
sp_Time *sp_time_parse(const char *s);
sp_Time *sp_time_s_httpdate(const char *s);
sp_Time *sp_time_s_rfc2822(const char *s);
sp_Time *sp_time_s_xmlschema(const char *s);
sp_Time *sp_time_s_parse(const char *s);
sp_Time *sp_time_new(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi, int64_t s);
sp_Time *sp_time_new_utc(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi, int64_t s);
sp_Time *sp_time_new_off(int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi, int64_t s, int64_t off);
sp_Time *sp_time_with_usec(const sp_Time *t, int64_t usec);
sp_Time *sp_time_with_usec_f(const sp_Time *t, double usec);
sp_Time *sp_time_utc(const sp_Time *t);
sp_Time *sp_time_localtime(const sp_Time *t);
void sp_time_vtm(const sp_Time *t, struct tm *bd, int32_t *off, char *zbuf);
int64_t sp_time_year(const sp_Time *t);
int64_t sp_time_mon(const sp_Time *t);
int64_t sp_time_mday(const sp_Time *t);
int64_t sp_time_hour(const sp_Time *t);
int64_t sp_time_min(const sp_Time *t);
int64_t sp_time_sec(const sp_Time *t);
int64_t sp_time_wday(const sp_Time *t);
int64_t sp_time_yday(const sp_Time *t);
int64_t sp_time_isdst(const sp_Time *t);
int64_t sp_time_utc_offset(const sp_Time *t);
sp_Time *sp_time_round_to(const sp_Time *t, int64_t ndigits, int mode);
sp_Time *sp_time_add(const sp_Time *t, double secs);
int sp_time_cmp(const sp_Time *a, const sp_Time *b);
int64_t sp_time_hash(const sp_Time *t);
sp_Time *sp_time_add_f(const sp_Time *t, double secs);
sp_Time *sp_time_add_i(const sp_Time *t, int64_t secs);
sp_Time *sp_time_sub_i(const sp_Time *t, int64_t secs);
double sp_time_sub_t(const sp_Time *a, const sp_Time *b);
const char *sp_time_strftime(const sp_Time *t, const char *fmt);
const char *sp_time_iso8601(const sp_Time *t);
const char *sp_time_httpdate(const sp_Time *t);
const char *sp_time_rfc2822(const sp_Time *t);
const char *sp_time_iso8601_frac(const sp_Time *t, int64_t digits);
sp_Time *sp_time_getlocal_off(const sp_Time *t, int64_t off);
const char *sp_time_zone(const sp_Time *t);
const char *sp_time_inspect_v(const sp_Time *t);
const char *sp_time_to_s_v(const sp_Time *t);


#endif
