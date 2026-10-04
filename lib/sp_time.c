/* sp_time.c -- libc-backed Time implementations.
 *
 * Sibling to sp_bigint.c / sp_crypto.c. The libc value ops (construct,
 * accessors, shifts) carry no runtime dependency; the formatters
 * (strftime / iso8601 / zone / inspect) return GC-heap strings directly
 * via sp_alloc.h, so the generated TU no longer needs buffer-copying
 * trampolines for them.
 */

#include <ctype.h>
#include "sp_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sp_time.h"
#include "sp_alloc.h"   /* sp_str_dup_external / sp_str_empty for the GC formatters */

/* Time#floor / #ceil / #round, shared by the typed and the boxed receiver so
   the two cannot drift. The no-argument form is ndigits 0: scale is 10^9 and
   every mode lands the value on a whole second. */
sp_Time sp_time_round_to(sp_Time t, int64_t ndigits, int mode) {
  /* CRuby names no number here, and a supported method's message is part of
     what it means to be a subset of it. */
  if (ndigits < 0) sp_raise_cls("ArgumentError", "negative ndigits given");
  if (ndigits >= 9) return t;                 /* full nanosecond resolution */
  int64_t scale = 1;
  for (int64_t k = ndigits; k < 9; k++) scale *= 10;
  int64_t ns = t.tv_nsec;
  if (mode == 0)      ns = ns / scale * scale;
  else if (mode == 1) { if (ns % scale) ns = (ns / scale + 1) * scale; }
  else                ns = (ns + scale / 2) / scale * scale;
  if (ns >= 1000000000) { t.tv_sec += 1; ns -= 1000000000; }
  t.tv_nsec = (int32_t)ns;
  return t;
}

sp_Time sp_time_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (sp_Time){ ts.tv_sec, (int32_t)ts.tv_nsec, 0 };
}

/* A sub-second argument of a whole unit or more belongs in the seconds
   field, the way CRuby normalises it (#3704). */
sp_Time sp_time_norm(sp_Time t) {
  int64_t ns = (int64_t)t.tv_nsec;
  if (ns >= 1000000000LL || ns < 0) {
    int64_t carry = ns / 1000000000LL;
    ns -= carry * 1000000000LL;
    if (ns < 0) { ns += 1000000000LL; carry--; }
    t.tv_sec += carry;
    t.tv_nsec = (int32_t)ns;
  }
  return t;
}
/* Add a sub-second offset that may exceed a whole second (#3704). */
sp_Time sp_time_add_nsec(sp_Time t, int64_t ns) {
  t.tv_sec += ns / 1000000000LL;
  t.tv_nsec = (int32_t)((int64_t)t.tv_nsec + ns % 1000000000LL);
  return sp_time_norm(t);
}
sp_Time sp_time_at_int(int64_t sec) {
  return (sp_Time){ sec, 0, 0 };
}

/* Time.at(Rational): the exact num/den epoch, floored to the nanosecond
   (Time.at(-1r/3) is second -1, nanosecond 666666666). den is positive by
   sp_Rational's normalized-sign invariant. */
sp_Time sp_time_at_div(int64_t num, int64_t den) {
  if (den == 0) sp_raise_cls("ZeroDivisionError", "divided by 0");
  int64_t sec = num / den;
  int64_t rem = num % den;
  if (rem < 0) { sec -= 1; rem += den; }
  /* rem < den, so rem * 1e9 needs more than 64 bits only when den does:
     a 128-bit product where the compiler has one, long double otherwise
     (a 32-bit build; the quotient is below 1e9 either way) */
#if SP_HAVE_INT128
  int64_t ns = (int64_t)(((sp_int128)rem * 1000000000) / den);
#else
  int64_t ns = (int64_t)(((long double)rem * 1000000000.0L) / (long double)den);
#endif
  return (sp_Time){ sec, (int32_t)ns, 0 };
}

/* Exact Float -> (sec, nsec) shift. CRuby converts the Float through to_r,
   so the EXACT binary value of the double decides the nanosecond, floored
   (Time.at(100) - 1.3 has usec 699999, not 700000, because the double
   nearest -1.3 is -1.3000000000000000444...). Reproduce that with integer
   math: decompose the double into mantissa * 2^exp via frexp, widen the
   mantissa-nanoseconds product to 128 bits, and arithmetic-shift (which
   floors negatives) instead of multiplying in double precision. */
static void sp_time_shift_ns(double secs, int64_t base_sec, int32_t base_ns,
                             int64_t *out_sec, int32_t *out_ns) {
  /* CRuby routes the Float through to_r, so a non-finite value raises
     FloatDomainError (Time.at(Float::INFINITY), t + Float::NAN, ...) rather
     than reaching frexp -- where (int64_t)(NaN/Inf * 2^53) would be UB. */
  if (!isfinite(secs))
    sp_raise_cls("FloatDomainError", isnan(secs) ? "NaN" : (secs < 0 ? "-Infinity" : "Infinity"));
  int e;
  double m = frexp(secs, &e);
  int64_t mi = (int64_t)(m * 9007199254740992.0); /* m * 2^53, exact */
  e -= 53;
#if SP_HAVE_INT128
  sp_int128 ns = (sp_int128)mi * 1000000000;
  if (e > 0) ns = (e > 34) ? (ns < 0 ? INT64_MIN : INT64_MAX) : ns << e;
  /* Floor toward -inf (arithmetic shift), matching CRuby's #nsec/#usec, which
     truncate the exact rational. spinel stores nanosecond resolution, so the
     sub-nanosecond bits CRuby keeps for #to_f round-tripping are lost by
     design (see docs/limitations.md). */
  else if (e < 0) ns = (-e > 126) ? (ns < 0 ? -1 : 0) : ns >> -e;
  sp_int128 total = ((sp_int128)base_sec * 1000000000 + base_ns) + ns;
  int64_t sec = (int64_t)(total / 1000000000);
  int64_t rem = (int64_t)(total % 1000000000);
#else
  /* no 128-bit integer (a 32-bit build): the same in long double, whose
     64-bit mantissa on x86 holds the nanosecond count of any date a
     32-bit sp_int can express; the floor is explicit */
  long double nsd = ldexpl((long double)mi * 1000000000.0L, e);
  long double totd = ((long double)base_sec * 1000000000.0L + (long double)base_ns) + floorl(nsd);
  int64_t sec = (int64_t)floorl(totd / 1000000000.0L);
  int64_t rem = (int64_t)(totd - (long double)sec * 1000000000.0L);
#endif
  if (rem < 0) { sec -= 1; rem += 1000000000; }
  *out_sec = sec;
  *out_ns = (int32_t)rem;
}

/* POSIX convention: keep tv_nsec in [0, 1e9). For negative epoch with
   a non-integer fractional part, decrement tv_sec and roll the fraction
   into the positive nsec range -- so Time.at(-0.5).to_i returns -1, not 0. */
sp_Time sp_time_at_float(double epoch) {
  sp_Time r = { 0, 0, 0 };
  sp_time_shift_ns(epoch, 0, 0, &r.tv_sec, &r.tv_nsec);
  return r;
}

/* Time.new(y[,mo[,d[,h[,mi[,s]]]]]) -- local construction. mktime
   interprets the broken-down value in the host local zone and resolves
   DST itself (tm_isdst=-1). The fixed-offset 7-arg form is a separate
   issue. */
/* CRuby validates each civil component against a fixed range before
   normalizing overflow into neighbouring fields (e.g. Feb 30 -> Mar 1 is
   allowed, but mon 13 / mday 32 / hour 25 raise ArgumentError) (#3099). */
static void sp_time_check_args(int64_t mo, int64_t d, int64_t h, int64_t mi, int64_t s) {
  if (mo < 1 || mo > 12) sp_raise_cls("ArgumentError", "mon out of range");
  if (d  < 1 || d  > 31) sp_raise_cls("ArgumentError", "mday out of range");
  if (h  < 0 || h  > 24) sp_raise_cls("ArgumentError", "hour out of range");
  if (mi < 0 || mi > 59) sp_raise_cls("ArgumentError", "min out of range");
  if (s  < 0 || s  > 60) sp_raise_cls("ArgumentError", "sec out of range");
}

static int64_t sp_time_civil_epoch(int64_t y, int64_t mo, int64_t d,
                                   int64_t h, int64_t mi, int64_t s);

/* Not mktime(gmtime(s)) - s: macOS mktime answers -1 for any year before 1900. */
static int32_t sp_time_local_offset(time_t s) {
  struct tm *l = localtime(&s);
  if (!l) return 0;
  return (int32_t)(sp_time_civil_epoch(l->tm_year + 1900, l->tm_mon + 1, l->tm_mday,
                                       l->tm_hour, l->tm_min, l->tm_sec) - (int64_t)s);
}

sp_Time sp_time_new(int64_t y, int64_t mo, int64_t d,
                    int64_t h, int64_t mi, int64_t s) {
  sp_time_check_args(mo, d, h, mi, s);
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_year = (int)y - 1900;
  tm.tm_mon  = (int)mo - 1;
  tm.tm_mday = (int)d;
  tm.tm_hour = (int)h;
  tm.tm_min  = (int)mi;
  tm.tm_sec  = (int)s;
  tm.tm_isdst = -1;
  tm.tm_wday = -1;
  time_t e = mktime(&tm);
  if (e == (time_t)-1 && tm.tm_wday == -1) {
    /* Not the -1 epoch: mktime refused the year (macOS, before 1900); resolve via localtime, twice to settle an offset change. */
    int64_t guess = sp_time_civil_epoch(y, mo, d, h, mi, s);
    int64_t epoch = guess - sp_time_local_offset((time_t)guess);
    epoch = guess - sp_time_local_offset((time_t)epoch);
    return (sp_Time){ epoch, 0, 0 };
  }
  return (sp_Time){ (int64_t)e, 0, 0 };
}

/* Civil (y, mo, d, h, mi, s) -> UTC epoch seconds. Avoid timegm
   (not portable; absent on MSVCRT). Compute the epoch via Howard
   Hinnant's days_from_civil + a manual hour/minute/second add. */
static int64_t sp_time_civil_epoch(int64_t y, int64_t mo, int64_t d,
                                   int64_t h, int64_t mi, int64_t s) {
  int64_t yy = y - (mo <= 2 ? 1 : 0);
  int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
  int64_t yoe = yy - era * 400;
  int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097 + doe - 719468;
  return days * 86400 + h * 3600 + mi * 60 + s;
}

/* Time.utc(y, m, d, h, mi, s) -- UTC construction. */
/* The month argument of the civil constructors accepts an English month name
   as well as a number: Time.utc(2020, "feb", 4). A plain strtoll read those as
   zero and the constructor rejected them (#3703). */
int64_t sp_time_month_arg(const char *s) {
  static const char *const names[12] = {
    "jan", "feb", "mar", "apr", "may", "jun",
    "jul", "aug", "sep", "oct", "nov", "dec" };
  if (!s) return 0;
  if (strlen(s) == 3) {
    char pre[4];
    for (int i = 0; i < 3; i++) {
      char ch = s[i];
      pre[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    pre[3] = '\0';
    for (int m = 0; m < 12; m++)
      if (strcmp(pre, names[m]) == 0) return (int64_t)(m + 1);
  }
  /* anything else is read as an Integer, and rejected the way Integer() would
     be: CRuby takes only the three-letter abbreviations by name */
  return (int64_t)sp_str_to_i_strict(s);
}

sp_Time sp_time_new_utc(int64_t y, int64_t mo, int64_t d,
                        int64_t h, int64_t mi, int64_t s) {
  sp_time_check_args(mo, d, h, mi, s);
  return (sp_Time){ sp_time_civil_epoch(y, mo, d, h, mi, s), 0, 1 };
}

/* A zone argument (`in:`, Time.new's 7th positional, or localtime /
   getlocal's String): CRuby's utc_offset_arg. "UTC" in any case and "Z" are
   UTC; a military letter is a whole-hour offset ("A".."I" +1..+9, "K".."M"
   +10..+12, "N".."Y" -1..-12, no "J"); otherwise "+HH", "+HHMM", "+HHMMSS",
   "+HH:MM" or "+HH:MM:SS", minutes and seconds below 60, where a negative
   zero ("-00:00") is UTC as well. Any other spelling is CRuby's ArgumentError
   naming the forms, and an offset of a day or more its "utc_offset out of
   range" (#3696, #3697, #3698). */
int64_t sp_time_zone_arg_off(const char *z, int *is_utc_out) {
  *is_utc_out = 0;
  if (!z) sp_raise_cls("ArgumentError", "invalid time zone");
  size_t len = strlen(z);
  const char *min = NULL, *sec = NULL;
  int64_t n = 0;
  switch (len) {
  case 1:
    if (z[0] == 'Z') { *is_utc_out = 1; return 0; }
    if (z[0] >= 'A' && z[0] <= 'I') return (int64_t)(z[0] - 'A' + 1) * 3600;
    if (z[0] >= 'K' && z[0] <= 'M') return (int64_t)(z[0] - 'A') * 3600;
    if (z[0] >= 'N' && z[0] <= 'Y') return (int64_t)('M' - z[0]) * 3600;
    goto invalid;
  case 3:
    if ((z[0] | 0x20) == 'u' && (z[1] | 0x20) == 't' && (z[2] | 0x20) == 'c') { *is_utc_out = 1; return 0; }
    break;                                          /* "+HH" */
  case 9: if (z[6] != ':') goto invalid; sec = z + 7; /* fall through: "+HH:MM:SS" */
  case 6: if (z[3] != ':') goto invalid; min = z + 4; break;   /* "+HH:MM" */
  case 7: sec = z + 5;                              /* fall through: "+HHMMSS" */
  case 5: min = z + 3; break;                       /* "+HHMM" */
  default: goto invalid;
  }
  if (sec) {
    if (!isdigit((unsigned char)sec[0]) || !isdigit((unsigned char)sec[1]) || sec[0] > '5') goto invalid;
    n += (sec[0] - '0') * 10 + (sec[1] - '0');
  }
  if (min) {
    if (!isdigit((unsigned char)min[0]) || !isdigit((unsigned char)min[1]) || min[0] > '5') goto invalid;
    n += ((min[0] - '0') * 10 + (min[1] - '0')) * 60;
  }
  if ((z[0] != '+' && z[0] != '-') || !isdigit((unsigned char)z[1]) || !isdigit((unsigned char)z[2])) goto invalid;
  n += (int64_t)((z[1] - '0') * 10 + (z[2] - '0')) * 3600;
  if (z[0] == '-') {
    if (n == 0) { *is_utc_out = 1; return 0; }
    n = -n;
  }
  if (n <= -86400 || n >= 86400) sp_raise_cls("ArgumentError", "utc_offset out of range");
  return n;
invalid:
  sp_raise_cls("ArgumentError", sp_sprintf("\"+HH:MM\", \"-HH:MM\", \"UTC\" or \"A\"..\"I\",\"K\"..\"Z\" expected for utc_offset: %s", z));
  return 0;
}

/* Re-read a Time in the zone the argument names (#3698). */
sp_Time sp_time_in_zone_i(sp_Time t, int64_t off) {
  if (off <= -86400 || off >= 86400)
    sp_raise_cls("ArgumentError", "utc_offset out of range");
  t.is_utc = 2; t.utc_off = (int32_t)off;
  return t;
}
sp_Time sp_time_in_zone_s(sp_Time t, const char *z) {
  int isu = 0;
  int64_t off = sp_time_zone_arg_off(z, &isu);
  if (isu) { t.is_utc = 1; t.utc_off = 0; }
  else { t.is_utc = 2; t.utc_off = (int32_t)off; }
  return t;
}

/* Time.new(y, mo, d, h, mi, s, utc_offset) -- the civil value is read in a
   fixed zone off seconds east of UTC, so the epoch is the UTC epoch of the
   same civil value minus that offset. CRuby bounds the offset to a day. */
sp_Time sp_time_new_off(int64_t y, int64_t mo, int64_t d,
                        int64_t h, int64_t mi, int64_t s, int64_t off) {
  if (off <= -86400 || off >= 86400)
    sp_raise_cls("ArgumentError", "utc_offset out of range");
  sp_time_check_args(mo, d, h, mi, s);
  sp_Time t = { sp_time_civil_epoch(y, mo, d, h, mi, s) - off, 0, 2 };
  t.utc_off = (int32_t)off;
  return t;
}

/* Time.utc/local(y, mo, d, h, mi, s, usec) -- the 7th positional argument is
   microseconds of second. */
sp_Time sp_time_with_usec(sp_Time t, int64_t usec) {
  if (usec < 0 || usec >= 1000000)
    sp_raise_cls("ArgumentError", "subsecx out of range");
  t.tv_nsec = (int32_t)(usec * 1000);
  return t;
}
/* a Float microsecond argument carries into the nanosecond field (#3092). */
sp_Time sp_time_with_usec_f(sp_Time t, double usec) {
  if (usec < 0 || usec >= 1000000)
    sp_raise_cls("ArgumentError", "subsecx out of range");
  t.tv_nsec = (int32_t)(usec * 1000.0);
  return t;
}

/* Time.new(String): the fixed CRuby form "YYYY-MM-DD HH:MM:SS[.frac]" with
   an optional " +HH:MM" / " -HH:MM" / " UTC" zone suffix. A date without a
   time and any other shape raise CRuby's ArgumentError messages; anything
   the grammar does not cover must be loud, never a guessed instant. */
sp_Time sp_time_parse(const char *s) {SP_GC_ROOT_STR(s);
  const char *sp_sprintf(const char *fmt, ...);  /* generated TU */
  int y, mo, d, h, mi, sec, n = 0;
  /* a year alone ("2021", "-44", "+12345") is Time.new(year), as CRuby
     reads it: four digits at least, local midnight of January 1 */
  { const char *q = s + (*s == '+' || *s == '-');
    size_t nd = 0;
    while (q[nd] >= '0' && q[nd] <= '9') nd++;
    if (nd > 0 && q[nd] == 0) {
      if (nd < 4) sp_raise_cls("ArgumentError", sp_sprintf("year must be 4 or more digits: %s", s));
      int64_t yy = 0;
      for (size_t i = 0; i < nd && i < 18; i++) yy = yy * 10 + (q[i] - '0');
      if (*s == '-') yy = -yy;
      return sp_time_new(yy, 1, 1, 0, 0, 0);
    }
  }
  if (sscanf(s, "%4d-%2d-%2d%n", &y, &mo, &d, &n) != 3 || n == 0)
    sp_raise_cls("ArgumentError", sp_sprintf("can't parse: \"%s\"", s));
  const char *p = s + n;
  if (*p == 0)
    sp_raise_cls("ArgumentError", "no time information");
  n = 0;
  if (sscanf(p, " %2d:%2d:%2d%n", &h, &mi, &sec, &n) != 3 || n == 0)
    sp_raise_cls("ArgumentError", sp_sprintf("can't parse: \"%s\"", s));
  p += n;
  int32_t nsec = 0;
  if (*p == '.') {
    p++;
    int digits = 0;
    int64_t frac = 0;
    while (*p >= '0' && *p <= '9' && digits < 9) { frac = frac * 10 + (*p - '0'); p++; digits++; }
    if (digits == 0)
      sp_raise_cls("ArgumentError", sp_sprintf("can't parse: \"%s\"", s));
    while (*p >= '0' && *p <= '9') p++;  /* sub-ns digits are beyond sp_Time */
    while (digits < 9) { frac *= 10; digits++; }
    nsec = (int32_t)frac;
  }
  while (*p == ' ') p++;
  if (*p == 0) {
    /* no zone: host-local, like the civil Time.new */
    sp_Time t = sp_time_new(y, mo, d, h, mi, sec);
    t.tv_nsec = nsec;
    return t;
  }
  if (strcmp(p, "UTC") == 0 || strcmp(p, "Z") == 0) {
    sp_Time t = sp_time_new_utc(y, mo, d, h, mi, sec);
    t.tv_nsec = nsec;
    return t;
  }
  int oh, om;
  n = 0;
  if ((*p == '+' || *p == '-') && sscanf(p + 1, "%2d:%2d%n", &oh, &om, &n) == 2 &&
      n > 0 && p[1 + n] == 0) {
    int64_t off = (int64_t)oh * 3600 + (int64_t)om * 60;
    if (*p == '-') off = -off;
    sp_Time t = sp_time_new_off(y, mo, d, h, mi, sec, off);
    t.tv_nsec = nsec;
    return t;
  }
  sp_raise_cls("ArgumentError", sp_sprintf("can't parse: \"%s\"", s));
}

sp_Time sp_time_utc(sp_Time t) {
  t.is_utc = 1;
  return t;
}

sp_Time sp_time_localtime(sp_Time t) {
  t.is_utc = 0;
  return t;
}
/* Time#getlocal(off)/#localtime(off): reinterpret the instant in a fixed zone
   `off` seconds east of UTC, without changing the underlying epoch (#3093). */
sp_Time sp_time_getlocal_off(sp_Time t, int64_t off) {
  if (off <= -86400 || off >= 86400)
    sp_raise_cls("ArgumentError", "utc_offset out of range");
  t.is_utc = 2;
  t.utc_off = (int32_t)off;
  return t;
}

/* is_utc selects gmtime vs localtime, off is UTC offset in seconds,
   zbuf is the timezone abbreviation (8 bytes). The offset is computed
   (sp_time_local_offset), not read from %z: MSVCRT's %z emits the
   timezone name, not ±HHMM. */
void sp_time_vtm(sp_Time t, struct tm *bd, int32_t *off, char *zbuf) {
  time_t s = (time_t)t.tv_sec;
  if (t.is_utc == 2) {
    /* fixed offset: the civil value is the UTC civil value shifted east */
    time_t sh = s + (time_t)t.utc_off;
    struct tm *g = gmtime(&sh);
    if (g) { *bd = *g; }
else { memset(bd, 0, sizeof(*bd)); }
    if (off) *off = t.utc_off;
    if (zbuf) zbuf[0] = 0;
  }
else if (t.is_utc) {
    struct tm *g = gmtime(&s);
    if (g) { *bd = *g; }
else { memset(bd, 0, sizeof(*bd)); }
    if (off) *off = 0;
    if (zbuf) { zbuf[0]='U'; zbuf[1]='T'; zbuf[2]='C'; zbuf[3]=0; }
  }
else {
    struct tm *l = localtime(&s);
    if (l) { *bd = *l; }
else { memset(bd, 0, sizeof(*bd)); }
    if (off) *off = sp_time_local_offset(s);
    if (zbuf) {
      if (strftime(zbuf, 8, "%Z", bd) == 0) zbuf[0] = 0;
    }
  }
}

int64_t sp_time_year(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)(b.tm_year+1900);}
int64_t sp_time_mon(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)(b.tm_mon+1);}
int64_t sp_time_mday(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)b.tm_mday;}
int64_t sp_time_hour(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)b.tm_hour;}
int64_t sp_time_min(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)b.tm_min;}
int64_t sp_time_sec(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)b.tm_sec;}
int64_t sp_time_wday(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)b.tm_wday;}
int64_t sp_time_yday(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)(b.tm_yday+1);}
int64_t sp_time_isdst(sp_Time t){struct tm b;sp_time_vtm(t,&b,NULL,NULL);return (int64_t)(b.tm_isdst>0?1:0);}
int64_t sp_time_utc_offset(sp_Time t){int32_t o;struct tm b;sp_time_vtm(t,&b,&o,NULL);return (int64_t)o;}

/* Time + Numeric / Time - Numeric. secs may be fractional; the shift is
   exact in the double's binary value (see sp_time_shift_ns). The zone kind
   and offset are inherited from the receiver. */
sp_Time sp_time_add(sp_Time t, double secs) {
  sp_Time r = t;
  sp_time_shift_ns(secs, t.tv_sec, t.tv_nsec, &r.tv_sec, &r.tv_nsec);
  return r;
}

/* strftime returns 0 -- never overruns the buffer -- when the formatted
   result would exceed it, which we surface as "". The 4 KB buffer covers
   any realistic format (CRuby's built-ins are ~25 bytes; this leaves room
   for long literal text or wide fields). A pathological field width
   (`"%1000000000F"`, which CRuby rejects with ERANGE) does not fit and
   yields "" -- a graceful empty string rather than a crash. */
/* The UTC offset (seconds) of a Time, mirroring sp_time_iso8601's manual calc. */
static long sp_time_offset_sec(sp_Time t) {
  if (t.is_utc == 2) return t.utc_off;   /* fixed offset (Time.at in:) */
  if (t.is_utc) return 0;
  return (long)sp_time_local_offset((time_t)t.tv_sec);
}

/* A year as Ruby writes it: zero-padded to four digits with the sign in
   front ("0012", "-0012", "10000"); a positive `width` is the whole field,
   sign included, as in "%4Y" ("-012"). C's %Y is not portable here: glibc
   does not pad it at all, and macOS pads a negative year inside the sign
   ("-012" for "-0012"). Returns the length written, as snprintf does. */
static int sp_time_year_field(char *buf, size_t cap, long yr, int width) {
  const char *sign = yr < 0 ? "-" : "";
  unsigned long mag = yr < 0 ? 0UL - (unsigned long)yr : (unsigned long)yr;
  int digits = width > 0 ? width - (yr < 0) : 4;
  return snprintf(buf, cap, "%s%0*lu", sign, digits > 0 ? digits : 1, mag);
}

static int sp_time_year_str(char *buf, size_t cap, long yr) {
  return sp_time_year_field(buf, cap, yr, -1);
}

/* buf <- the year, then C strftime of `rest` (fields that carry no year).
   Returns the length, or 0 if it did not fit, as strftime does. */
static size_t sp_time_year_then(char *buf, size_t cap, const struct tm *b, const char *rest) {
  int yn = sp_time_year_str(buf, cap, (long)b->tm_year + 1900);
  if (yn < 0 || (size_t)yn >= cap) return 0;
  if (!*rest) return (size_t)yn;
  size_t r = strftime(buf + yn, cap - (size_t)yn, rest, b);
  return r ? (size_t)yn + r : 0;
}

/* Ruby-compatible strftime: C strftime handles the standard directives, but
   Ruby adds %L/%N (subsec), %s (epoch, which C's %s would take through a LOCAL
   mktime), %P (lowercase am/pm), the %:z/%::z colon offsets, and width/flag
   modifiers (%3S, %6N, %10Y). Walk the format, compute those directly, and
   pad; delegate a bare standard directive to strftime. (#2635, #2636) */
const char *sp_time_strftime(sp_Time t, const char *fmt) {SP_GC_ROOT_STR(fmt);
  /* is_utc is a 3-state kind, not a flag: kind 2 is a fixed offset whose
     civil fields are the UTC ones shifted by utc_off. Reading it as a
     boolean sent kind 2 down the gmtime branch, so every field directive
     rendered UTC under a %z that correctly said otherwise. sp_time_vtm
     resolves all three kinds. */
  struct tm tmv;
  sp_time_vtm(t, &tmv, NULL, NULL);
  static char out[8192];
  size_t oi = 0;
  for (const char *p = fmt; *p && oi < sizeof(out) - 128; p++) {
    if (*p != '%') { out[oi++] = *p; continue; }
    const char *tok = p++;
    int upcase = 0, downcase = 0, pad0 = 0, padsp = 0, nopad = 0, colon = 0;
    for (;; p++) {
      if (*p == '^') upcase = 1; else if (*p == '#') downcase = 1;
      else if (*p == '0') pad0 = 1; else if (*p == '_') padsp = 1;
      else if (*p == '-') nopad = 1; else break;
    }
    while (*p == ':') { colon++; p++; }
    int width = -1;
    if (*p >= '0' && *p <= '9') { width = 0; while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0'); }
    /* the E / O locale modifiers select an alternative representation the C
       locale does not have, so the unmodified directive is what they mean (#3705) */
    while ((*p == 'E' || *p == 'O') && p[1]) p++;
    char d = *p;
    /* a format ending in a bare `%` is invalid, not a literal one (#3705) */
    if (!d) sp_raise_cls("ArgumentError", "invalid format");
    char val[128]; val[0] = 0;
    if (d == '%') { val[0] = '%'; val[1] = 0; }
    else if (d == 's') snprintf(val, sizeof val, "%lld", (long long)t.tv_sec);
    else if (d == 'L') {
      /* a width on %L asks for that many fractional digits, not left padding
         of the millisecond count (#3705) */
      int lw = width > 0 ? width : 3;
      char nb[24]; snprintf(nb, sizeof nb, "%09ld", (long)t.tv_nsec);
      if (lw <= 9) { memcpy(val, nb, (size_t)lw); val[lw] = 0; }
      else { strcpy(val, nb); for (int i = 9; i < lw && i < 120; i++) val[i] = '0'; val[lw < 120 ? lw : 120] = 0; }
      width = -1;
    }
    else if (d == 'N') {
      int w = width > 0 ? width : 9;
      char nb[16]; snprintf(nb, sizeof nb, "%09ld", (long)t.tv_nsec);
      if (w <= 9) { memcpy(val, nb, (size_t)w); val[w] = 0; }
      else { strcpy(val, nb); for (int i = 9; i < w && i < 120; i++) val[i] = '0'; val[w < 120 ? w : 120] = 0; }
      width = -1;  /* width consumed by the subsecond precision */
    }
    else if (d == 'z') {
      /* bare %z computed from the receiver's own offset, never C strftime's
         tm_gmtoff -- that field is filled from the tm's construction path and
         varies by platform, and a fixed-offset time (is_utc == 2) has no tm
         representation at all (#2635) */
      if (!colon) {
        long off0 = sp_time_offset_sec(t);
        char sign0 = off0 < 0 ? '-' : '+'; long a0 = off0 < 0 ? -off0 : off0;
        snprintf(val, sizeof val, "%c%02d%02d", sign0, (int)(a0 / 3600), (int)((a0 / 60) % 60));
      }
      else {
        long off = sp_time_offset_sec(t);
        char sign = off < 0 ? '-' : '+'; long a = off < 0 ? -off : off;
        int oh = (int)(a / 3600), om = (int)((a / 60) % 60), os = (int)(a % 60);
        /* %:::z collapses to the minimal precision that loses nothing */
        if (colon >= 3) {
          if (os) snprintf(val, sizeof val, "%c%02d:%02d:%02d", sign, oh, om, os);
          else if (om) snprintf(val, sizeof val, "%c%02d:%02d", sign, oh, om);
          else snprintf(val, sizeof val, "%c%02d", sign, oh);
        }
        else if (colon == 2) snprintf(val, sizeof val, "%c%02d:%02d:%02d", sign, oh, om, os);
        else snprintf(val, sizeof val, "%c%02d:%02d", sign, oh, om);
      }
    }
    else if (d == 'P') { char b2[16]; strftime(b2, sizeof b2, "%p", &tmv); for (char *q = b2; *q; q++) *q = (char)tolower((unsigned char)*q); strcpy(val, b2); }
    /* CRuby names a UTC time "UTC", not the C locale's "GMT"; a fixed-offset
       time has no zone NAME at all, so %Z is empty there (Time#zone is nil). */
    else if (d == 'Z' && t.is_utc) { if (t.is_utc == 1) strcpy(val, "UTC"); else val[0] = 0; }
    /* Ruby's %Y is zero-padded to four digits; C's is not, so a year below
       1000 came out "1" where CRuby writes "0001". Ruby keeps the sign
       outside the padding, so -1 is "-0001". The directives that contain
       the year (%F, %c, %v) take it from the same place, and the century
       and two-digit year round toward minus infinity, as Integer#div and
       #% do: -12 is century -1, year 88. */
    else if (d == 'Y' || d == 'G') {
      long yr = (long)tmv.tm_year + 1900;
      if (d == 'G') {
        /* the ISO 8601 week-based year: C computes it, Ruby formats it */
        char gb[32];
        if (strftime(gb, sizeof gb, "%G", &tmv)) yr = strtol(gb, NULL, 10);
      }
      /* the bare directive pads to four digits; a width, `_` or `-` takes
         the signed number and the padding below sizes it (the sign counts
         toward the width, and zeros go after it) */
      if (width > 0 || padsp || nopad) {
        snprintf(val, sizeof val, "%ld", yr);
        if (width <= 0 && padsp) width = 4 + (yr < 0);
      }
      else sp_time_year_str(val, sizeof val, yr);
    }
    else if (d == 'F') sp_time_year_then(val, sizeof val, &tmv, "-%m-%d");
    else if (d == 'C' || d == 'y' || d == 'x' || d == 'D') {
      long yr = (long)tmv.tm_year + 1900;
      long cen = yr >= 0 ? yr / 100 : -((-yr + 99) / 100);
      long yy = yr - cen * 100;
      if (d == 'C') snprintf(val, sizeof val, "%02ld", cen);
      else if (d == 'y') snprintf(val, sizeof val, "%02ld", yy);
      else snprintf(val, sizeof val, "%02d/%02d/%02ld", tmv.tm_mon + 1, tmv.tm_mday, yy);
    }
    else if (d == 'c') {
      size_t n = strftime(val, sizeof val, "%a %b %e %H:%M:%S ", &tmv);
      if (n) sp_time_year_str(val + n, sizeof val - n, (long)tmv.tm_year + 1900);
    }
    else if (d == 'v') {
      /* "%e-%^b-%4Y": the year four wide, sign included */
      size_t n = strftime(val, sizeof val, "%e-%b-", &tmv);
      for (size_t k = 0; k < n; k++) val[k] = (char)toupper((unsigned char)val[k]);
      if (n) sp_time_year_field(val + n, sizeof val - n, (long)tmv.tm_year + 1900, 4);
    }
    else if (d == 'g') {
      /* the ISO week-based year's last two digits, rounded as %y is */
      char gb[32];
      long gy = strftime(gb, sizeof gb, "%G", &tmv) ? strtol(gb, NULL, 10) : (long)tmv.tm_year + 1900;
      long gc = gy >= 0 ? gy / 100 : -((-gy + 99) / 100);
      snprintf(val, sizeof val, "%02ld", gy - gc * 100);
    }
    else if (strchr("aAbBcCdDeFgGhHIjklmMnprRSTtuUvVwWxXyYzZ", d)) {
      /* a standard Ruby directive: format the bare `%X` (we redo width/case
         ourselves for portability) */
      char f2[3] = { '%', d, 0 };
      strftime(val, sizeof val, f2, &tmv);
    }
    else {
      /* not a Ruby Time#strftime directive (e.g. `%+`): emit the token
         verbatim -- CRuby does, and a platform's C strftime must not interpret
         it (macOS treats `%+` as date(1), glibc leaves it literal). */
      size_t tl = (size_t)(p - tok) + 1;
      if (tl < sizeof val) { memcpy(val, tok, tl); val[tl] = 0; }
      width = -1; upcase = downcase = 0;
    }
    if (upcase) for (char *q = val; *q; q++) *q = (char)toupper((unsigned char)*q);
    /* `%#` changes the field's case as a WHOLE: all-uppercase becomes
       lowercase, anything else becomes uppercase. Per-character swapcase
       instead turned "January" into "jANUARY" where CRuby answers
       "JANUARY" -- only fields that are already uppercase (%p, %Z) agreed. */
    if (downcase) {
      int has_lower = 0;
      for (char *q = val; *q; q++) if (islower((unsigned char)*q)) { has_lower = 1; break; }
      for (char *q = val; *q; q++)
        *q = has_lower ? (char)toupper((unsigned char)*q) : (char)tolower((unsigned char)*q);
    }
    /* the `-` (no-pad) and `_` (space-pad) modifiers rework the default zero
       padding that C strftime already applied to a numeric field (#3090) */
    /* the space-padded fields (%e / %k / %l) take `-` (strip) and `0` (zero
       pad) the same way the zero-padded ones take `-` and `_` (#3705) */
    if ((nopad || pad0) && val[0] == ' ') {
      size_t sp0 = 0; while (val[sp0] == ' ' && val[sp0 + 1] != 0) sp0++;
      if (nopad) memmove(val, val + sp0, strlen(val) - sp0 + 1);
      else for (size_t k = 0; k < sp0; k++) val[k] = '0';
    }
    if ((nopad || padsp) && val[0]) {
      int all_digit = 1;
      for (char *q = val; *q; q++) if (!isdigit((unsigned char)*q)) { all_digit = 0; break; }
      if (all_digit) {
        size_t z = 0; while (val[z] == '0' && val[z + 1] != 0) z++;  /* keep the last digit */
        if (nopad) memmove(val, val + z, strlen(val) - z + 1);
        else for (size_t k = 0; k < z; k++) val[k] = ' ';
      }
    }
    size_t vl = strlen(val), v0 = 0;
    if (width > 0 && !nopad && vl < (size_t)width) {
      char pc = padsp ? ' ' : '0';
      /* zeros go after a sign, as CRuby pads "%10s" of -5 to "-000000005";
         spaces go before it */
      if (pc == '0' && val[0] == '-' && oi < sizeof(out) - 2) { out[oi++] = '-'; v0 = 1; }
      for (size_t k = vl; k < (size_t)width && oi < sizeof(out) - 2; k++) out[oi++] = pc;
    }
    (void)tok;
    for (size_t k = v0; k < vl && oi < sizeof(out) - 2; k++) out[oi++] = val[k];
  }
  out[oi] = 0;
  return sp_str_dup_external(out);
}

/* RFC 3339 zone suffix: "Z" for a UTC time, "+HH:MM" otherwise. `off` is
   the receiver's own offset as sp_time_vtm resolved it, so the fixed-offset
   kind carries its utc_off here instead of being mistaken for UTC. Returns
   the number of bytes appended (0 if they would not fit). */
static size_t sp_time_iso_zone(char *buf, size_t n, size_t cap, sp_Time t, int32_t off) {
  if (t.is_utc == 1) {
    if (n + 1 >= cap) return 0;
    buf[n] = 'Z'; buf[n + 1] = 0;
    return 1;
  }
  if (n + 6 >= cap) return 0;
  char sign = off >= 0 ? '+' : '-';
  long a = off < 0 ? -(long)off : (long)off;
  int oh = (int)(a / 3600), om = (int)((a / 60) % 60);
  buf[n++] = sign;
  buf[n++] = (char)('0' + (oh / 10));
  buf[n++] = (char)('0' + (oh % 10));
  buf[n++] = ':';
  buf[n++] = (char)('0' + (om / 10));
  buf[n++] = (char)('0' + (om % 10));
  buf[n] = 0;
  return 6;
}

/* Time#httpdate (RFC 1123, always GMT) and Time#rfc2822, whose UTC time
   is written -0000 as CRuby's time.rb does. */
const char *sp_time_httpdate(sp_Time t) {
  return sp_time_strftime(sp_time_utc(t), "%a, %d %b %Y %H:%M:%S GMT");
}
const char *sp_time_rfc2822(sp_Time t) {
  return sp_time_strftime(t, t.is_utc == 1 ? "%a, %d %b %Y %H:%M:%S -0000" : "%a, %d %b %Y %H:%M:%S %z");
}

/* RFC 3339 / iso8601. sp_time_vtm resolves the civil fields and the offset
   for all three zone kinds; the suffix is formatted here because MSVCRT's
   %z renders the timezone name rather than ±HHMM. */
const char *sp_time_iso8601(sp_Time t) {
  char buf[64];
  size_t cap = sizeof(buf);
  struct tm b;
  int32_t off;
  sp_time_vtm(t, &b, &off, NULL);
  size_t n = sp_time_year_then(buf, cap, &b, "-%m-%dT%H:%M:%S");
  if (n == 0) return sp_str_empty;
  sp_time_iso_zone(buf, n, cap, t, off);
  return sp_str_dup_external(buf);
}

/* iso8601 / xmlschema with a fraction-digits argument: like sp_time_iso8601
   but inserting `digits` truncated fractional-second places (#3094, #3095). */
const char *sp_time_iso8601_frac(sp_Time t, int64_t digits) {
  if (digits <= 0) return sp_time_iso8601(t);
  if (digits > 50) digits = 50;
  char buf[128];
  size_t cap = sizeof(buf);
  struct tm b;
  int32_t off;
  sp_time_vtm(t, &b, &off, NULL);
  size_t n = sp_time_year_then(buf, cap, &b, "-%m-%dT%H:%M:%S");
  if (n == 0) return sp_str_empty;
  char fb[16]; snprintf(fb, sizeof fb, "%09ld", (long)t.tv_nsec);
  if (n + 1 + (size_t)digits < cap) {
    buf[n++] = '.';
    for (int64_t i = 0; i < digits; i++) buf[n++] = i < 9 ? fb[i] : '0';
    buf[n] = 0;
  }
  sp_time_iso_zone(buf, n, cap, t, off);
  return sp_str_dup_external(buf);
}

const char *sp_time_zone(sp_Time t) {
  /* a fixed-offset Time has no zone NAME, and CRuby answers nil for it rather
     than the empty string the broken-down form leaves behind (#3701) */
  if (t.is_utc == 2) return NULL;
  char buf[8];
  struct tm b;
  sp_time_vtm(t, &b, NULL, buf);
  return sp_str_dup_external(buf);
}

/* Scalar Time inspect. CRuby form: local "YYYY-MM-DD HH:MM:SS +0900",
   UTC "YYYY-MM-DD HH:MM:SS UTC". The poly-box path keeps its own
   sp_Time_inspect; this value-taking variant is for the scalar
   p/puts/to_s codegen path. */
static const char *sp_time_fmt(sp_Time t, int frac) {
  char buf[64];
  size_t cap = sizeof(buf);
  struct tm b;
  int32_t off;
  sp_time_vtm(t, &b, &off, NULL);
  /* the year by hand (sp_time_year_then): Time.utc(1,1,1).to_s is
     "0001-01-01 ..." in CRuby, and C's %Y wrote "1-01-01 ..." */
  size_t n = sp_time_year_then(buf, cap, &b, "-%m-%d %H:%M:%S");
  if (n == 0) {
    snprintf(buf, cap, "Time(%lld)", (long long)t.tv_sec);
    return sp_str_dup_external(buf);
  }
  if (frac && t.tv_nsec != 0 && n + 11 < cap) {
    /* fractional seconds, trailing zeros trimmed (".123456", ".5") */
    n += (size_t)snprintf(buf + n, cap - n, ".%09d", (int)t.tv_nsec);
    while (buf[n - 1] == '0') buf[--n] = 0;
  }
  if (n + 10 < cap) {
    if (t.is_utc == 1) {
      buf[n++]=' '; buf[n++]='U'; buf[n++]='T'; buf[n++]='C'; buf[n]=0;
    }
else {
      char sign = off >= 0 ? '+' : '-';
      long a = off < 0 ? -(long)off : (long)off;
      int oh = (int)(a / 3600);
      int om = (int)((a / 60) % 60);
      int os = (int)(a % 60);
      buf[n++]=' '; buf[n++]=sign;
      buf[n++]=(char)('0'+oh/10); buf[n++]=(char)('0'+oh%10);
      buf[n++]=(char)('0'+om/10); buf[n++]=(char)('0'+om%10);
      if (os) { /* CRuby renders a sub-minute offset as +HHMMSS */
        buf[n++]=(char)('0'+os/10); buf[n++]=(char)('0'+os%10);
      }
      buf[n]=0;
    }
  }
  return sp_str_dup_external(buf);
}

/* Time#inspect renders fractional seconds; Time#to_s does not. */
const char *sp_time_inspect_v(sp_Time t) { return sp_time_fmt(t, 1); }
const char *sp_time_to_s_v(sp_Time t)    { return sp_time_fmt(t, 0); }

/* ---- comparison + shifts (moved from spinel_rt.h; cold) ---- */
int sp_time_cmp(sp_Time a, sp_Time b) {
  if (a.tv_sec < b.tv_sec) return -1;
  if (a.tv_sec > b.tv_sec) return 1;
  if (a.tv_nsec < b.tv_nsec) return -1;
  if (a.tv_nsec > b.tv_nsec) return 1;
  return 0;
}
sp_Time sp_time_add_f(sp_Time t, double secs) {
  return sp_time_add(t, secs);
}
/* Value hash: equal (tv_sec, tv_nsec) pairs must hash equal regardless of
   zone kind, mirroring Time#== which compares the instant only. */
int64_t sp_time_hash(sp_Time t) {
  uint64_t h = (uint64_t)t.tv_sec * 1000000007ULL + (uint64_t)(uint32_t)t.tv_nsec;
  h ^= h >> 33;
  return (int64_t)(h & 0x3fffffffffffffffULL);
}
sp_Time sp_time_add_i(sp_Time t, int64_t secs) {
  sp_Time r = t;
  r.tv_sec = t.tv_sec + (time_t)secs;
  return r;
}
sp_Time sp_time_sub_i(sp_Time t, int64_t secs) {
  sp_Time r = t;
  r.tv_sec = t.tv_sec - (time_t)secs;
  return r;
}
double sp_time_sub_t(sp_Time a, sp_Time b) {
  return sp_time_ns_to_f(a.tv_sec - b.tv_sec, (int64_t)a.tv_nsec - b.tv_nsec);
}
