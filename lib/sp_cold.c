/* Cold, out-of-line helpers extracted from spinel_rt.h.
 *
 * Functions that are large and not on any hot path were `static` in
 * spinel_rt.h, so every translation unit that includes the header (every
 * generated program, plus each lib TU) compiled its own copy. Moving them here
 * -- a single TU that includes the full runtime header -- gives one linked copy
 * and shrinks per-TU compile work. spinel_rt.h keeps an `extern` declaration
 * so all callers still resolve. Only functions that call no program-generated
 * static (sp_sym_to_s / sp_sym_intern / ...) can live here, and each includes
 * only the low-level headers its dependencies need -- NOT spinel_rt.h, which
 * is a monolithic definition header meant to be compiled once (into the
 * generated program), so including it here would multiply-define the runtime. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* statx / STATX_BTIME for File.birthtime on Linux */
#endif
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "sp_alloc.h"   /* sp_str_alloc / sp_str_set_len / sp_raise_cls */
#include "sp_array.h"   /* sp_StrArray for Dir.glob */
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>   /* getpriority for Process.getpriority */
#include <fcntl.h>      /* AT_FDCWD for statx */
#include <errno.h>
#include "sp_time.h"   /* sp_Time for File.mtime */
#include "sp_io.h"     /* sp_file_directory prototype */
#include "sp_str.h"
#include "sp_string.h"
#include "sp_system.h" /* sp_last_status for backtick */
#include "sp_format.h" /* sp_float_to_rational for sp_float_denominator/numerator */
#include <sys/select.h>  /* IO.select and the IO#wait_* readiness family */
#include <sys/socket.h> /* SOCK_STREAM / SOCK_DGRAM for Addrinfo */

/* lib/sp_gc.c. Declared here rather than in sp_gc.h: that header is included
   by every generated TU, so adding to it recompiles the whole suite. */
extern int sp_gc_full_runs;
extern int sp_gc_rem_peak;   /* lib/sp_gc.c: high-water mark of the remembered set */

/* execinfo.h (backtrace_symbols) is a glibc/Apple extension; not all libc
   implementations ship it. Detect availability by the toolchain macros so we
   can guard the header inclusion. Where it is missing we still provide
   no-op struct/macro shims below so the backtrace code compiles unchanged. */
#if defined(__has_include)
#  if __has_include(<execinfo.h>)
#    define HAVE_EXECINFO_H 1
#  endif
#elif defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__)
#  define HAVE_EXECINFO_H 1
#endif

/* Integer#% / Kernel#format "%b"/"%B"/"%o"/"%x"/"%X": non-decimal formatting
   with Ruby's flag, width, precision, and two's-complement-for-negative rules.
   C's printf drops the '+' and ' ' flags on these conversions and has no
   two's-complement form, so none of it can be delegated to libc. */
int sp_fmt_binary(const char *spec, size_t sl, char conv, long long val,
                  char *out, size_t osz) {
  int base = (conv == 'o') ? 8 : (conv == 'x' || conv == 'X') ? 16 : 2;
  int upper = (conv == 'X' || conv == 'B');
  char topd = (char)(base == 8 ? '7' : base == 2 ? '1' : (upper ? 'F' : 'f'));
  /* parse "%<flags><width>.<prec>b" out of spec[0..sl-1] (spec[sl-1] == conv) */
  int f_minus = 0, f_plus = 0, f_space = 0, f_hash = 0, f_zero = 0;
  size_t i = 1;
  for (; i < sl; i++) {
    if (spec[i] == '-') f_minus = 1;
    else if (spec[i] == '+') f_plus = 1;
    else if (spec[i] == ' ') f_space = 1;
    else if (spec[i] == '#') f_hash = 1;
    else if (spec[i] == '0') f_zero = 1;
    else break;
  }
  int width = 0;
  for (; i < sl && spec[i] >= '0' && spec[i] <= '9'; i++) width = (width * 10) + (spec[i] - '0');
  int prec = -1;
  if (i < sl && spec[i] == '.') {
    i++; prec = 0;
    for (; i < sl && spec[i] >= '0' && spec[i] <= '9'; i++) prec = (prec * 10) + (spec[i] - '0');
  }

  int neg = val < 0;
  /* Ruby shows the two's-complement ".." body only for a negative value with no
     sign flag; a + or space flag switches to signed magnitude ("-101"). */
  int twos = neg && !f_plus && !f_space;
  char digits[256]; int dn = 0;
  if (twos) {
    /* the digits that differ from the infinite run of sign digits, plus one
       leading sign digit: -255 in base 16 is "..f01" */
    char t[80]; int tn = 0;
    long long w = val;
    while (w != -1 && tn < (int)sizeof t) {
      long long d = w % base;
      if (d < 0) d += base;
      t[tn++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + (d - 10));
      w = (w - d) / base;
    }
    digits[dn++] = topd;
    while (tn) digits[dn++] = t[--tn];
  }
  else {
    /* signed magnitude: |val| in the base. 0 has no significant digits, so it
       contributes a single '0' only when precision is not 0. */
    unsigned long long mag = neg ? (unsigned long long)(-(val + 1)) + 1 : (unsigned long long)val;
    if (mag == 0) { if (prec != 0) digits[dn++] = '0'; }
    else { char t[80]; int tn = 0;
           while (mag) { int d = (int)(mag % (unsigned)base);
                         t[tn++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + (d - 10));
                         mag /= (unsigned)base; }
           while (tn) digits[dn++] = t[--tn]; }
  }
  /* precision: minimum digit count. The ".." body counts as 2 toward it and pads
     with sign digits; signed magnitude pads with 0. */
  if (prec >= 0) {
    int target = twos ? (prec - 2) : prec;
    char padc = twos ? topd : '0';
    int t2 = target - dn;
    if (t2 > 0) {
      /* clamp to the digits buffer (output is capped at osz anyway) */
      if (t2 + dn >= (int)sizeof(digits)) t2 = (int)sizeof(digits) - dn - 1;
      memmove(digits + t2, digits, (size_t)dn);  /* shift body right */
      memset(digits, padc, (size_t)t2);           /* leading padding */
      dn += t2;
    }
    f_zero = 0;  /* precision disables 0-flag for integer conversions */
  }
  /* assemble sign/prefix + body, then apply width padding */
  char body[300]; int bn = 0;
  char sign = twos ? 0 : (neg ? '-' : (f_plus ? '+' : (f_space ? ' ' : 0)));
  char prefix0 = 0, prefix1 = 0;
  if (f_hash && base == 8) {
    /* octal's alternate form has no letter: it just guarantees a leading 0,
       so it adds nothing to a body that already starts with one (and nothing
       at all to the two's-complement form, where a 0 would misread) */
    if (!twos) {
      if (dn == 0) digits[dn++] = '0';
      else if (digits[0] != '0') prefix0 = '0';
    }
  }
  else if (f_hash && val != 0) {
    prefix0 = '0';
    prefix1 = (base == 2) ? (upper ? 'B' : 'b') : (upper ? 'X' : 'x');
  }
  if (sign) body[bn++] = sign;
  if (prefix0) { body[bn++] = prefix0; if (prefix1) body[bn++] = prefix1; }
  if (twos) { body[bn++] = '.'; body[bn++] = '.'; }
  for (int k = 0; k < dn; k++) body[bn++] = digits[k];

  int o = 0;
  int pad = width - bn;
  if (pad > 0 && !f_minus && f_zero) {
    /* zero-pad: emit sign/prefix/".." first, then fill, then the rest. A two's-
       complement body fills with the sign bit (1); signed magnitude with 0. */
    int head = (sign ? 1 : 0) + (prefix0 ? (prefix1 ? 2 : 1) : 0) + (twos ? 2 : 0);
    char fillc = twos ? topd : '0';
    for (int k = 0; k < head && o < (int)osz; k++) out[o++] = body[k];
    for (int k = 0; k < pad && o < (int)osz; k++) out[o++] = fillc;
    for (int k = head; k < bn && o < (int)osz; k++) out[o++] = body[k];
  }
  else {
    if (pad > 0 && !f_minus) for (int k = 0; k < pad && o < (int)osz; k++) out[o++] = ' ';
    for (int k = 0; k < bn && o < (int)osz; k++) out[o++] = body[k];
    if (pad > 0 && f_minus) for (int k = 0; k < pad && o < (int)osz; k++) out[o++] = ' ';
  }
  return o;
}

/* File.expand_path(path[, base]): absolute, `.`/`..`/`//`-normalized path.
   Depends only on sp_alloc.h + libc; no program-generated symbols. */
const char *sp_file_expand_path(const char *path, const char *base) {
  char raw[8192];
  char cwd[4096];
  const char *home = getenv("HOME");
  if (!home) home = "";
  if (!path) path = "";

  /* The `%.4000s` precision caps each component so the compiler can
     prove the combined output (<= 8001) fits in the 8192 buffer -- this
     silences -Wformat-truncation (an error under the test harness's
     -Werror). 4000 chars/component is well past PATH_MAX, so real paths
     never truncate. */
  if (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
    snprintf(raw, sizeof(raw), "%.4000s%.4000s", home, path + 1);
  }
  else if (path[0] == '/') {
    snprintf(raw, sizeof(raw), "%.4000s", path);
  }
  else {
    char basebuf[8192];
    const char *b;
    if (base && base[0]) {
      if (base[0] == '~' && (base[1] == '\0' || base[1] == '/')) {
        snprintf(basebuf, sizeof(basebuf), "%.4000s%.4000s", home, base + 1);
        b = basebuf;
      }
      else if (base[0] == '/') {
        b = base;
      }
      else {
        if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
        snprintf(basebuf, sizeof(basebuf), "%.4000s/%.4000s", cwd, base);
        b = basebuf;
      }
    }
    else {
      if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
      b = cwd;
    }
    snprintf(raw, sizeof(raw), "%.4000s/%.4000s", b, path);
  }

  /* Normalize: walk segments, collapsing `.`/`..`/`//`. seg_start[k]
     records the output length to roll back to when a `..` pops the
     k-th kept segment. */
  size_t rawlen = strlen(raw);
  char *out = sp_str_alloc(rawlen + 1);
  size_t seg_start[1024];
  int nseg = 0;
  size_t olen = 0;
  out[olen++] = '/';
  const char *p = raw;
  while (*p) {
    if (*p == '/') { p++; continue; }
    const char *q = p;
    while (*q && *q != '/') q++;
    size_t slen = (size_t)(q - p);
    if (slen == 1 && p[0] == '.') {
      /* current dir -- skip */
    }
    else if (slen == 2 && p[0] == '.' && p[1] == '.') {
      if (nseg > 0) { nseg--; olen = seg_start[nseg]; }
    }
    else {
      size_t mark = olen;
      if (olen > 1) out[olen++] = '/';
      memcpy(out + olen, p, slen);
      olen += slen;
      if (nseg < 1024) seg_start[nseg++] = mark;
    }
    p = q;
  }
  out[olen] = 0;
  sp_str_set_len(out, olen);
  return out;
}

/* File.readlink(path): the symlink target as a fresh spinel string (#3005) */
const char *sp_file_readlink(const char *path) {SP_GC_ROOT_STR(path);
  char buf[4096];
  ssize_t n = readlink(path ? path : "", buf, sizeof(buf) - 1);
  if (n < 0) {
    sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" :
                 errno == EINVAL ? "Errno::EINVAL" : "SystemCallError",
                 sp_sprintf("%s @ readlink - %s", strerror(errno), path ? path : ""));
    return "";
  }
  char *out = sp_str_alloc((size_t)n + 1);
  memcpy(out, buf, (size_t)n);
  out[n] = 0;
  sp_str_set_len(out, (size_t)n);
  return out;
}

/* ---- String#to_c parse + Dir.glob (cold; moved from spinel_rt.h) ---- */

/* `exception: false` asks Kernel#Complex / #Rational for nil rather than a
   raise on an unparseable String. The parsers below set this instead of
   raising while it is on; the caller reads it back and answers nil (#3893). */
sp_bool sp_convert_soft = 0;
sp_bool sp_convert_failed = 0;
static sp_Complex sp_str_to_c_impl(const char *s, int strict) {
  double re = 0, im = 0;
  int parsed = 0;
  const char *fin = s;   /* first byte NOT consumed by the parse */
  if (s) {
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    double a = strtod(p, &end);
    if (end != p) {
      parsed = 1;
      /* rational-syntax component "n/d" */
      if (*end == '/') { const char *dp = end + 1; char *de = NULL; double d = strtod(dp, &de); if (de != dp) { a /= d; end = de; } }
      if (*end == 'i') { im = a; fin = end + 1; }
      else {
        re = a;
        const char *q = end;
        double b2 = strtod(q, &end);
        if (end != q) {
          if (*end == '/') { const char *dp = end + 1; char *de = NULL; double d = strtod(dp, &de); if (de != dp) { b2 /= d; end = de; } }
          if (*end == 'i') { im = b2; fin = end + 1; }
          else fin = q;   /* an imaginary number without the 'i' suffix ("1+2") is invalid */
        }
        else if ((*q == '+' || *q == '-') && q[1] == 'i') { im = (*q == '-') ? -1.0 : 1.0; fin = q + 2; }
        else fin = q;        /* "1+" and other incomplete forms leave the operator unconsumed */
      }
    }
    else if (*p == 'i') { im = 1; parsed = 1; fin = p + 1; }
    else if ((*p == '+' || *p == '-') && p[1] == 'i') { im = (*p == '-') ? -1.0 : 1.0; parsed = 1; fin = p + 2; }
  }
  /* Kernel#Complex(str) must consume the whole string (only trailing
     whitespace allowed): an incomplete form like "1+" is invalid, not silently
     (1+0i) (#2617). String#to_c does the opposite -- it reads a leading
     complex and ignores the rest, and answers (0+0i) when nothing parses.
     String#to_r has had that pair (sp_str_to_r / sp_str_to_r_strict) all
     along; to_c had only the strict half, and #to_c was pointed at it, so
     `"abc".to_c` raised where CRuby answers (0+0i). */
  if (parsed && strict) { while (*fin == ' ' || *fin == '\t') fin++; if (*fin != '\0') parsed = 0; }
  if (!parsed) {
    if (!strict) return (sp_Complex){ 0.0, 0.0 };
    if (sp_convert_soft) { sp_convert_failed = 1; return (sp_Complex){ 0.0, 0.0 }; }
    sp_raise_cls("ArgumentError", "invalid value for convert(): ");
  }
  return (sp_Complex){ (sp_float)re, (sp_float)im };
}
/* String#to_c: a leading complex, the rest ignored, (0+0i) when none. */
sp_Complex sp_str_to_c(const char *s) { return sp_str_to_c_impl(s, 0); }
/* Kernel#Complex(String): the whole string or an ArgumentError. */
sp_Complex sp_str_to_c_strict(const char *s) { return sp_str_to_c_impl(s, 1); }

/* FNM_DOTMATCH mode for the walk below: hidden entries (and ".") match a
   non-dot pattern; ".." never does, as CRuby's glob. Set by the _dot wrapper. */
static int sp_glob_dotmatch = 0;

int sp_fnmatch1(const char *pat, const char *str) {
  while (*pat) {
    if (*pat == '*') {
      pat++;
      if (!*pat) return 1;
      while (*str) { if (sp_fnmatch1(pat, str)) return 1; str++; }
      return sp_fnmatch1(pat, str);
    }
else if (*pat == '?') {
      if (!*str) return 0;
      pat++; str++;
    }
else {
      if (*pat != *str) return 0;
      pat++; str++;
    }
  }
  return *str == 0;
}

void sp_dir_glob_rec(const char *fsdir, const char *outprefix,
                            const char *tail, sp_StrArray *a) {
  DIR *d = opendir(fsdir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    const char *name = e->d_name;
    if (name[0] == '.' && name[1] == '.' && name[2] == 0) continue;
    if (!sp_glob_dotmatch && name[0] == '.' && name[1] == 0) continue;
    char fspath[2048], outpath[2048];
    snprintf(fspath, sizeof fspath, "%s/%s", fsdir, name);
    snprintf(outpath, sizeof outpath, "%s%s", outprefix, name);
    if ((sp_glob_dotmatch || !(name[0] == '.' && tail[0] != '.')) && sp_fnmatch1(tail, name)) {
      char *copy = sp_str_alloc(strlen(outpath));
      strcpy(copy, outpath);
      sp_StrArray_push(a, copy);
    }
    if (name[0] != '.') {
      struct stat st;
      if (lstat(fspath, &st) == 0 && S_ISDIR(st.st_mode)) {
        char subprefix[2048];
        snprintf(subprefix, sizeof subprefix, "%s%s/", outprefix, name);
        sp_dir_glob_rec(fspath, subprefix, tail, a);
      }
    }
  }
  closedir(d);
}

sp_StrArray *sp_dir_glob(const char *pattern);
/* Dir.glob(pat, File::FNM_DOTMATCH) (#2828) */
sp_StrArray *sp_dir_glob_dot(const char *pattern) {SP_GC_ROOT_STR(pattern);
  sp_glob_dotmatch = 1;
  sp_StrArray *a = sp_dir_glob(pattern);
  sp_glob_dotmatch = 0;
  return a;
}
sp_StrArray *sp_dir_glob(const char *pattern) {
  /* the pattern is often a fresh interpolation temp, unrooted at the call
     site; the per-match sp_str_allocs below can collect it mid-walk */
  SP_GC_ROOT_STR(pattern);
  sp_StrArray *a = sp_StrArray_new();
  /* every matched entry sp_str_allocs inside the walk below, and enough of
     them trigger a collection mid-build -- root the result like
     sp_dir_entries_impl or it (and its pushed names) get swept under us */
  SP_GC_ROOT(a);
  if (!pattern) return a;
  /* Recursive double-star form: split at the double-star component. Everything
     before it is the output prefix (and, minus the trailing slash, the directory
     to walk); the component after it is the per-directory tail pattern. */
  const char *ss = strstr(pattern, "**");
  if (ss) {
    size_t plen = (size_t)(ss - pattern);
    if (plen >= 1024) return a;
    const char *after = ss + 2;
    if (*after == '/') after++;
    const char *tail = after;
    char outprefix[1024];
    memcpy(outprefix, pattern, plen);
    outprefix[plen] = 0;
    char fsdir[1024];
    if (plen == 0) {
      strcpy(fsdir, ".");
    }
    else {
      memcpy(fsdir, pattern, plen);
      fsdir[plen] = 0;
      if (fsdir[plen - 1] == '/') fsdir[plen - 1] = 0;
      if (fsdir[0] == 0) strcpy(fsdir, "/");
    }
    sp_dir_glob_rec(fsdir, outprefix, tail, a);
    sp_StrArray_sort_bang(a);
    return a;
  }
  const char *slash = strrchr(pattern, '/');
  char dirbuf[1024];
  const char *dirpath;
  const char *base_pat;
  if (slash) {
    size_t dl = (size_t)(slash - pattern);
    if (dl >= sizeof(dirbuf)) return a;
    memcpy(dirbuf, pattern, dl);
    dirbuf[dl] = 0;
    dirpath = (dl == 0) ? "/" : dirbuf;
    base_pat = slash + 1;
  }
else {
    dirpath = ".";
    base_pat = pattern;
  }
  DIR *d = opendir(dirpath);
  if (!d) return a;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    const char *name = e->d_name;
    if (name[0] == '.' && name[1] == '.' && name[2] == 0) continue;
    if (!sp_glob_dotmatch && name[0] == '.' && base_pat[0] != '.') continue;
    if (sp_fnmatch1(base_pat, name)) {
      char full[2048];
      if (slash) snprintf(full, sizeof(full), "%s/%s", dirbuf, name);
      else snprintf(full, sizeof(full), "%s", name);
      char *copy = sp_str_alloc(strlen(full));
      strcpy(copy, full);
      sp_StrArray_push(a, copy);
    }
  }
  closedir(d);
  sp_StrArray_sort_bang(a);
  return a;
}

/* ---- File.read/size/mtime/join/readlines + Math.lgamma (cold) ---- */

const char *sp_file_join(const char **parts, int n) {
  /* CRuby boundary rule (#2785): exactly one separator joins adjacent
     components -- when the accumulated path already ends with '/', the next
     component's leading '/'s are dropped; otherwise one is inserted. */
  /* The parts are read in full before the result is allocated: a part may
     be a String nothing else holds -- a #to_path's fresh answer -- and the
     allocation can collect it. */
  size_t total = 0;
  for (int i = 0; i < n; i++) { if (parts[i]) total += strlen(parts[i]); total++; }
  char *tmp = (char *)malloc(total + 1);
  if (!tmp) sp_oom_die();
  size_t off = 0;
  for (int i = 0; i < n; i++) {
    const char *p = parts[i] ? parts[i] : "";
    if (i > 0) {
      if (off > 0 && tmp[off - 1] == '/') { while (*p == '/') p++; }
      else if (*p != '/') tmp[off++] = '/';
    }
    size_t l = strlen(p);
    memcpy(tmp + off, p, l); off += l;
  }
  char *r = sp_str_alloc((sp_int)off);
  memcpy(r, tmp, off);
  free(tmp);
  r[off] = 0;
  sp_str_set_len(r, off);
  return r;
}

sp_StrArray *sp_file_readlines(const char *path) {SP_GC_ROOT_STR(path);
  sp_StrArray *a = sp_StrArray_new();
  SP_GC_ROOT(a);
  FILE *_fp = fopen(path ? path : "", "r");
  if (!_fp) return a;
  char _buf[4096];
  while (fgets(_buf, (int)sizeof(_buf), _fp)) {
    size_t _l = strlen(_buf);
    char *_r = sp_str_alloc_raw(_l + 1);
    memcpy(_r, _buf, _l + 1);
    sp_StrArray_push(a, _r);
  }
  fclose(_fp);
  return a;
}

sp_StrArray *sp_file_readlines_chomp(const char *path) {SP_GC_ROOT_STR(path);
  sp_StrArray *a = sp_StrArray_new();
  SP_GC_ROOT(a);
  FILE *_fp = fopen(path ? path : "", "r");
  if (!_fp) return a;
  char _buf[4096];
  while (fgets(_buf, (int)sizeof(_buf), _fp)) {
    size_t _l = strlen(_buf);
    if (_l > 0 && _buf[_l-1] == '\n') { _buf[--_l] = '\0'; }
    if (_l > 0 && _buf[_l-1] == '\r') { _buf[--_l] = '\0'; }
    char *_r = sp_str_alloc_raw(_l + 1);
    memcpy(_r, _buf, _l + 1);
    sp_StrArray_push(a, _r);
  }
  fclose(_fp);
  return a;
}

double sp_lgamma_pos(double x) {  /* x > 0 */
  if (x == 1.0 || x == 2.0) return 0.0;
  double corr = 0.0;
  while (x < 12.0) { corr -= log(x); x += 1.0; }
  double inv = 1.0 / x, inv2 = inv * inv;
  /* sum_{k>=1} B_2k / (2k(2k-1) x^(2k-1)) up to the 1/x^11 term */
  double series = (1.0/12.0) + (inv2 * (-(1.0/360.0) + (inv2 * ((1.0/1260.0)
                  + (inv2 * (-(1.0/1680.0) + (inv2 * (1.0/1188.0))))))));
  return corr + ((x - 0.5) * log(x)) - x + (0.5 * log(2.0 * M_PI)) + (series * inv);
}

sp_PolyArray *sp_math_lgamma(double x) {
  int sign = 1; double v;
  if (x > 0.0) {
    v = sp_lgamma_pos(x);
  }
  else if (x == floor(x)) {
    /* pole at every non-positive integer -- detect it directly, since
       sin(M_PI * x) is not exactly 0 there in floating point (#3016).
       gamma approaches -inf from the -0 side, so -0.0 alone reports sign
       -1 (CRuby/C99 lgamma_r) (#3116). */
    v = INFINITY;
    if (x == 0.0 && signbit(x)) sign = -1;
  }
  else {
    double s = sin(M_PI * x);
    if (s < 0.0) sign = -1;
    v = log(M_PI / fabs(s)) - sp_lgamma_pos(1.0 - x);
  }
  sp_PolyArray *r = sp_PolyArray_new(); SP_GC_ROOT(r);
  sp_PolyArray_push(r, sp_box_float(v));
  sp_PolyArray_push(r, sp_box_int(sign));
  return r;
}

/* Read from the stream's current position to EOF.
   The seek size is a HINT, never the length: /proc and /sys entries, FIFOs and
   character devices all report 0 and still yield bytes, and an ordinary file
   can grow between the size query and the read. Sizing the buffer from it and
   stopping there answered "" for the whole of /proc -- silently, since a short
   read is indistinguishable from an empty file (#3411). The hint only picks
   the initial capacity, so the common case is still one allocation and one
   fread. */
const char *sp_slurp_stream(FILE *fp) {
  if (!fp) return &("\xff" "")[1];
  size_t cap = 8192;
  long pos = ftell(fp);
  if (pos >= 0 && fseek(fp, 0, SEEK_END) == 0) {
    long end = ftell(fp);
    if (fseek(fp, pos, SEEK_SET) != 0) { /* unseekable after all: start over */ }
    if (end > pos) cap = (size_t)(end - pos) + 1;
  }
  char *buf = (char *)malloc(cap);
  if (!buf) return &("\xff" "")[1];
  size_t len = 0;
  for (;;) {
    if (len + 1 >= cap) {
      size_t ncap = cap * 2;
      char *nb = (char *)realloc(buf, ncap);
      if (!nb) { free(buf); return &("\xff" "")[1]; }
      buf = nb; cap = ncap;
    }
    size_t got = fread(buf + len, 1, cap - len - 1, fp);
    len += got;
    if (got == 0) break;
  }
  char *r = sp_str_alloc(len);
  if (len) memcpy(r, buf, len);
  r[len] = 0;
  sp_str_set_len(r, len);
  free(buf);
  return r;
}

const char *sp_file_read(const char *path) {SP_GC_ROOT_STR(path);
  if (sp_file_directory(path)) {
    sp_raise_cls("Errno::EISDIR", sp_sprintf("Is a directory @ io_fread - %s", path));
  }
  FILE *f = fopen(path, "r");
  if (!f) {
    sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" : errno == EACCES ? "Errno::EACCES" : "RuntimeError",
                 sp_sprintf("%s @ rb_sysopen - %s", strerror(errno), path));
    return &("\xff" "")[1];
  }
  const char *r = sp_slurp_stream(f);
  fclose(f);
  return r;
}

sp_Time sp_file_atime(const char *path) {SP_GC_ROOT_STR(path);
  if (!path) { sp_raise_cls("TypeError", "no implicit conversion of nil into String"); return (sp_Time){0, 0, 0}; }
  struct stat st;
  if (stat(path, &st) == -1) {
    sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" : "RuntimeError", sp_sprintf("%s @ File.atime - %s", strerror(errno), path));
    return (sp_Time){0, 0, 0};
  }
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
  return (sp_Time){(int64_t)st.st_atimespec.tv_sec, (int32_t)st.st_atimespec.tv_nsec, 0};
#else
  return (sp_Time){(int64_t)st.st_atim.tv_sec, (int32_t)st.st_atim.tv_nsec, 0};
#endif
}
sp_Time sp_file_ctime(const char *path) {SP_GC_ROOT_STR(path);
  if (!path) { sp_raise_cls("TypeError", "no implicit conversion of nil into String"); return (sp_Time){0, 0, 0}; }
  struct stat st;
  if (stat(path, &st) == -1) {
    sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" : "RuntimeError", sp_sprintf("%s @ File.ctime - %s", strerror(errno), path));
    return (sp_Time){0, 0, 0};
  }
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
  return (sp_Time){(int64_t)st.st_ctimespec.tv_sec, (int32_t)st.st_ctimespec.tv_nsec, 0};
#else
  return (sp_Time){(int64_t)st.st_ctim.tv_sec, (int32_t)st.st_ctim.tv_nsec, 0};
#endif
}
sp_Time sp_file_mtime(const char *path) {SP_GC_ROOT_STR(path);
  if (!path) {
    sp_raise_cls("TypeError", "no implicit conversion of nil into String");
    return (sp_Time){0, 0, 0};
  }
  struct stat st;
  if (stat(path, &st) == -1) {
    sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" : "RuntimeError", sp_sprintf("%s @ File.mtime - %s", strerror(errno), path));
    return (sp_Time){0, 0, 0};
  }
#if defined(__APPLE__)
  return (sp_Time){(int64_t)st.st_mtimespec.tv_sec, (int32_t)st.st_mtimespec.tv_nsec, 0};
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
  return (sp_Time){(int64_t)st.st_mtimespec.tv_sec, (int32_t)st.st_mtimespec.tv_nsec, 0};
#else
  /* Linux / others with st_mtim */
  return (sp_Time){(int64_t)st.st_mtim.tv_sec, (int32_t)st.st_mtim.tv_nsec, 0};
#endif
}
sp_Time sp_file_birthtime(const char *path) {SP_GC_ROOT_STR(path);  /* (#2985) */
  if (!path) { sp_raise_cls("TypeError", "no implicit conversion of nil into String"); return (sp_Time){0, 0, 0}; }
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
  struct stat st;
  if (stat(path, &st) == -1) { sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" : "RuntimeError", sp_sprintf("%s @ File.birthtime - %s", strerror(errno), path)); return (sp_Time){0, 0, 0}; }
  return (sp_Time){(int64_t)st.st_birthtimespec.tv_sec, (int32_t)st.st_birthtimespec.tv_nsec, 0};
#elif defined(__linux__) && defined(STATX_BTIME)
  struct statx stx;
  if (statx(AT_FDCWD, path, AT_STATX_SYNC_AS_STAT, STATX_BTIME, &stx) == 0 && (stx.stx_mask & STATX_BTIME))
    return (sp_Time){(int64_t)stx.stx_btime.tv_sec, (int32_t)stx.stx_btime.tv_nsec, 0};
  sp_raise_cls("NotImplementedError", "birthtime() function is unimplemented on this filesystem");
  return (sp_Time){0, 0, 0};
#else
  sp_raise_cls("NotImplementedError", "birthtime() function is unimplemented");
  return (sp_Time){0, 0, 0};
#endif
}
sp_int sp_process_getpriority(sp_int which, sp_int who) {  /* (#3046) */
  errno = 0;
  int r = getpriority((int)which, (id_t)who);
  if (r == -1 && errno != 0)
    sp_raise_cls(errno == EINVAL ? "Errno::EINVAL" : (errno == ESRCH ? "Errno::ESRCH" : "SystemCallError"),
                 strerror(errno));
  return (sp_int)r;
}
sp_IntArray *sp_process_groups(void) {  /* (#3046) */
  sp_IntArray *a = sp_IntArray_new();
  int n = getgroups(0, NULL);
  if (n <= 0) return a;
  gid_t *buf = (gid_t *)malloc(sizeof(gid_t) * (size_t)n);
  if (!buf) return a;
  n = getgroups(n, buf);
  for (int i = 0; i < n; i++) sp_IntArray_push(a, (sp_int)buf[i]);
  free(buf);
  return a;
}

sp_int sp_file_size(const char *path) {SP_GC_ROOT_STR(path);
  if (!path) {
    sp_raise_cls("TypeError", "no implicit conversion of nil into String");
    return 0;
  }
  struct stat st;
  if (stat(path, &st) == -1) {
    int err = errno;  /* capture once: strerror() may clobber errno */
    sp_raise_cls(err == ENOENT ? "Errno::ENOENT" : "RuntimeError", sp_sprintf("%s @ File.size - %s", strerror(err), path));
    return 0;
  }
  /* off_t (typically 64-bit) into sp_int (intptr_t -> 32-bit on a 32-bit
     build): guard the narrowing, as spinel does for int arithmetic. */
  if ((off_t)(sp_int)st.st_size != st.st_size) {
    sp_raise_cls("RangeError", "file size out of range for Integer");
    return 0;
  }
  return (sp_int)st.st_size;
}

sp_IntArray *sp_file_binread_bytes(const char *path) {SP_GC_ROOT_STR(path);
  if (sp_file_directory(path)) {
    sp_raise_cls("Errno::EISDIR", sp_sprintf("Is a directory @ io_fread - %s", path));
  }
  FILE *f = fopen(path, "rb");
  sp_IntArray *a = sp_IntArray_new();
  if (!f) {
    sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" : errno == EACCES ? "Errno::EACCES" : "RuntimeError",
                 sp_sprintf("%s @ rb_sysopen - %s", strerror(errno), path));
    return a;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *buf = (unsigned char *)malloc(sz > 0 ? (size_t)sz : 1);
  if (buf && sz > 0) {
    /* Use fread's actual byte count, not the raw file size — a
       partial read otherwise pushes uninitialized memory. */
    size_t r = fread(buf, 1, (size_t)sz, f);
    for (size_t i = 0; i < r; i++) sp_IntArray_push(a, (sp_int)buf[i]);
  }
  free(buf);
  fclose(f);
  return a;
}

/* ---- more cold helpers: string/File/Dir/poly-combinatorics/misc ---- */

/* String#sum: the byte checksum modulo 2**bits (bits <= 0 or >= 64 leaves the
   sum untruncated, like CRuby). The typed emitter inlines this same loop; the
   boxed receiver reaches it through sp_poly_sum. */
sp_int sp_str_sum_bits(const char *s, sp_int bits) {
  sp_int acc = 0;
  for (const char *p = s ? s : ""; *p; p++) acc += (unsigned char)*p;
  return (bits <= 0 || bits >= 64) ? acc : (acc & ((((sp_int)1) << bits) - 1));
}

const char *sp_str_splice_at(const char *s, sp_int from, sp_int n, const char *val, int range_form) {SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(val);
  if (!s) s = "";
  if (!val) val = "";
  sp_int len = (sp_int)sp_str_length(s);
  if (n < 0) { sp_raise_cls("IndexError", sp_sprintf("negative length %lld", (long long)n)); return s; }
  sp_int from0 = from;
  if (from < 0) from += len;
  if (from < 0 || from > len) {
    if (range_form) sp_raise_cls("RangeError", sp_sprintf("%lld out of range", (long long)from0));
    else sp_raise_cls("IndexError", sp_sprintf("index %lld out of string", (long long)from0));
    return s;
  }
  if (from + n > len) n = len - from;
  return sp_str_concat(sp_str_concat(sp_str_sub_range(s, 0, from), val),
                       sp_str_sub_range(s, from + n, len - from - n));
}

sp_FloatArray *sp_frange_step(sp_FloatRange r, sp_float st) {
  sp_FloatArray *a = sp_FloatArray_new(); SP_GC_ROOT(a);
  if (st == 0) sp_raise_cls("ArgumentError", "step can't be 0");
  sp_float span = r.last - r.first;
  if (span == span && st == st) {   /* not NaN */
    if ((span > 0 && st < 0) || (span < 0 && st > 0)) return a;   /* wrong direction */
  }
  sp_int n = (sp_int)(span / st + (r.excl ? -1e-9 : 1e-9));
  for (sp_int i = 0; i <= n; i++) {
    sp_float v = r.first + (sp_float)i * st;
    if (st > 0) { if (r.excl ? v >= r.last : v > r.last + 1e-9) break; }
    else        { if (r.excl ? v <= r.last : v < r.last - 1e-9) break; }
    sp_FloatArray_push(a, v);
  }
  return a;
}

sp_int sp_poly_cmp_int_arrays(sp_RbVal a, sp_RbVal b, sp_bool *comparable) {
  if (a.tag != SP_TAG_OBJ || b.tag != SP_TAG_OBJ ||
      a.cls_id != SP_BUILTIN_INT_ARRAY || b.cls_id != SP_BUILTIN_INT_ARRAY) { *comparable = FALSE; return 0; }
  sp_IntArray *x = (sp_IntArray *)a.v.p, *y = (sp_IntArray *)b.v.p;
  if (!x || !y) { *comparable = FALSE; return 0; }
  sp_int n = x->len < y->len ? x->len : y->len;
  for (sp_int i = 0; i < n; i++) {
    sp_int xe = x->data[x->start + i], ye = y->data[y->start + i];
    if (xe != ye) { *comparable = TRUE; return xe < ye ? -1 : 1; }
  }
  *comparable = TRUE;
  return (x->len > y->len) - (x->len < y->len);
}

sp_int sp_int_round_half(sp_int v, sp_int nd, int mode) {
  if (nd >= 0) return v;
  sp_int f = 1;
  for (sp_int i = 0; i < -nd; i++) f *= 10;
  sp_int q = v / f, rem = v % f;
  sp_int arem = rem < 0 ? -rem : rem;
  sp_int half = f / 2;
  int up;
  if (arem > half) up = 1;
  else if (arem < half) up = 0;
  else up = mode == 1 ? 1 : mode == 2 ? 0 : (q % 2 != 0);  /* :even ties to even */
  if (up) q += (v < 0 ? -1 : 1);
  return q * f;
}

sp_RbVal sp_poly_replace(sp_RbVal recv, sp_RbVal src) {SP_GC_ROOT_RBVAL(recv);SP_GC_ROOT_RBVAL(src);
  if (recv.tag != SP_TAG_OBJ) return recv;
  /* String#replace on a shared-mutable handle: swap the buffer contents in
     place, from a plain string box or another handle (#3227). */
  if (recv.cls_id == SP_BUILTIN_STRBUF) {
    sp_String *m = (sp_String *)recv.v.p;
    const char *s2 = NULL;
    if (src.tag == SP_TAG_STR) s2 = src.v.s;
    else if (src.tag == SP_TAG_OBJ && src.cls_id == SP_BUILTIN_STRBUF)
      s2 = sp_String_cstr((sp_String *)src.v.p);
    if (s2) sp_String_set_bin(m, s2);
    return recv;
  }
  if (src.tag != SP_TAG_OBJ) return recv;
  if (recv.cls_id == SP_BUILTIN_INT_ARRAY && src.cls_id == SP_BUILTIN_INT_ARRAY)
    sp_IntArray_replace((sp_IntArray *)recv.v.p, (sp_IntArray *)src.v.p);
  else if (recv.cls_id == SP_BUILTIN_FLT_ARRAY && src.cls_id == SP_BUILTIN_FLT_ARRAY)
    sp_FloatArray_replace((sp_FloatArray *)recv.v.p, (sp_FloatArray *)src.v.p);
  else if (recv.cls_id == SP_BUILTIN_STR_ARRAY && src.cls_id == SP_BUILTIN_STR_ARRAY)
    sp_StrArray_replace((sp_StrArray *)recv.v.p, (sp_StrArray *)src.v.p);
  else if (recv.cls_id == SP_BUILTIN_POLY_ARRAY) {
    sp_PolyArray *d = (sp_PolyArray *)recv.v.p;
    d->len = 0;
    switch (src.cls_id) {
      case SP_BUILTIN_INT_ARRAY: { sp_IntArray *s = (sp_IntArray *)src.v.p; for (sp_int i = 0; i < s->len; i++) sp_PolyArray_push(d, sp_box_int(s->data[s->start + i])); break; }
      case SP_BUILTIN_FLT_ARRAY: { sp_FloatArray *s = (sp_FloatArray *)src.v.p; for (sp_int i = 0; i < s->len; i++) sp_PolyArray_push(d, sp_box_float(s->data[i])); break; }
      case SP_BUILTIN_STR_ARRAY: { sp_StrArray *s = (sp_StrArray *)src.v.p; for (sp_int i = 0; i < s->len; i++) sp_PolyArray_push(d, sp_box_str(s->data[i])); break; }
      case SP_BUILTIN_POLY_ARRAY: { sp_PolyArray *s = (sp_PolyArray *)src.v.p; for (sp_int i = 0; i < s->len; i++) sp_PolyArray_push(d, s->data[i]); break; }
      default: break;
    }
  }
  return recv;
}

void sp_poly_combination_recur(sp_PolyArray *src, sp_int start, sp_int k, sp_PolyArray *acc, sp_PolyArray *out) {SP_GC_ROOT(src);SP_GC_ROOT(acc);SP_GC_ROOT(out);
  if (k == 0) {
    sp_PolyArray *cp = sp_PolyArray_new(); SP_GC_ROOT(cp);
    for (sp_int i = 0; i < acc->len; i++) sp_PolyArray_push(cp, acc->data[i]);
    sp_PolyArray_push(out, sp_box_poly_array(cp));
    return;
  }
  for (sp_int i = start; i <= src->len - k; i++) {
    sp_PolyArray_push(acc, src->data[i]);
    sp_poly_combination_recur(src, i + 1, k - 1, acc, out);
    acc->len--;
  }
}

void sp_poly_repeated_combination_recur(sp_PolyArray *src, sp_int start, sp_int k, sp_PolyArray *acc, sp_PolyArray *out) {SP_GC_ROOT(src);SP_GC_ROOT(acc);SP_GC_ROOT(out);
  if (k == 0) {
    sp_PolyArray *cp = sp_PolyArray_new(); SP_GC_ROOT(cp);
    for (sp_int i = 0; i < acc->len; i++) sp_PolyArray_push(cp, acc->data[i]);
    sp_PolyArray_push(out, sp_box_poly_array(cp));
    return;
  }
  for (sp_int i = start; i < src->len; i++) {
    sp_PolyArray_push(acc, src->data[i]);
    sp_poly_repeated_combination_recur(src, i, k - 1, acc, out);
    acc->len--;
  }
}

void sp_poly_permutation_recur(sp_PolyArray *src, sp_int k, sp_IntArray *used, sp_PolyArray *acc, sp_PolyArray *out) {SP_GC_ROOT(src);SP_GC_ROOT(used);SP_GC_ROOT(acc);SP_GC_ROOT(out);
  if (k == 0) {
    sp_PolyArray *cp = sp_PolyArray_new(); SP_GC_ROOT(cp);
    for (sp_int i = 0; i < acc->len; i++) sp_PolyArray_push(cp, acc->data[i]);
    sp_PolyArray_push(out, sp_box_poly_array(cp));
    return;
  }
  for (sp_int i = 0; i < src->len; i++) {
    if (used->data[used->start + i]) continue;
    used->data[used->start + i] = 1;
    sp_PolyArray_push(acc, src->data[i]);
    sp_poly_permutation_recur(src, k - 1, used, acc, out);
    acc->len--;
    used->data[used->start + i] = 0;
  }
}

void sp_poly_repeated_permutation_recur(sp_PolyArray *src, sp_int k, sp_PolyArray *acc, sp_PolyArray *out) {SP_GC_ROOT(src);SP_GC_ROOT(acc);SP_GC_ROOT(out);
  if (k == 0) {
    sp_PolyArray *cp = sp_PolyArray_new(); SP_GC_ROOT(cp);
    for (sp_int i = 0; i < acc->len; i++) sp_PolyArray_push(cp, acc->data[i]);
    sp_PolyArray_push(out, sp_box_poly_array(cp));
    return;
  }
  for (sp_int i = 0; i < src->len; i++) {
    sp_PolyArray_push(acc, src->data[i]);
    sp_poly_repeated_permutation_recur(src, k - 1, acc, out);
    acc->len--;
  }
}

int sp_json_kind(sp_RbVal v) {
  if (v.tag != SP_TAG_OBJ) return 0;
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_FLT_ARRAY:
    case SP_BUILTIN_STR_ARRAY: case SP_BUILTIN_SYM_ARRAY:
    case SP_BUILTIN_POLY_ARRAY: return 1;
    case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_STR_STR_HASH:
    case SP_BUILTIN_INT_STR_HASH: case SP_BUILTIN_STR_POLY_HASH:
    case SP_BUILTIN_SYM_POLY_HASH: case SP_BUILTIN_POLY_POLY_HASH:
    case SP_BUILTIN_INT_INT_HASH:
      return 2;
    default: return 0;
  }
}

sp_bool sp_poly_cbi_p(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ) {
    switch (v.cls_id) {
    case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_STR_STR_HASH:
    case SP_BUILTIN_INT_STR_HASH: case SP_BUILTIN_STR_POLY_HASH:
    case SP_BUILTIN_SYM_POLY_HASH: case SP_BUILTIN_POLY_POLY_HASH:
    case SP_BUILTIN_INT_INT_HASH:
      return FALSE;
    default: break;
    }
  }
  sp_raise_cls("NoMethodError", "undefined method 'compare_by_identity?' for poly");
  return FALSE;
}

/* File.write(path, data): answers the byte count it wrote, measured by the
   same rule that sizes the write, so an embedded NUL in a stringified value
   counts once in both places. */
sp_int sp_file_write(const char *path, const char *data) {SP_GC_ROOT_STR(path);SP_GC_ROOT_STR(data);
  if (sp_file_directory(path)) {
    sp_raise_cls("Errno::EISDIR", sp_sprintf("Is a directory @ rb_sysopen - %s", path));
  }
  FILE *f = fopen(path, "wb");
  if (!f) {
    sp_raise_cls(errno == ENOENT ? "Errno::ENOENT" : errno == EACCES ? "Errno::EACCES" : "RuntimeError",
                 sp_sprintf("%s @ rb_sysopen - %s", strerror(errno), path));
    return 0;
  }
  size_t n = sp_str_byte_len(data);
  /* a short write or a failed close is the write's error, not a count:
     CRuby raises the errno (a full disk is Errno::ENOSPC) */
  size_t w = fwrite(data, 1, n, f);
  int err = (w < n || ferror(f)) ? (errno ? errno : EIO) : 0;
  if (fclose(f) != 0 && !err) err = errno ? errno : EIO;
  if (err) {
    errno = err;
    sp_raise_cls(err == ENOSPC ? "Errno::ENOSPC" : "SystemCallError",
                 sp_sprintf("%s @ rb_sys_fail_on_write - %s", strerror(err), path));
    return 0;
  }
  return (sp_int)w;
}

const char *sp_backtick(const char *cmd) {
  FILE *p = popen(cmd, "r");
  if (!p) { sp_last_status = -1; return sp_str_empty; }
  char *buf = sp_str_alloc_raw(4096);
  size_t n = fread(buf, 1, 4095, p);
  buf[n] = 0;
  int st = pclose(p);
  /* Mirror sp_system_args' $? layout: POSIX pclose returns a wait-status,
     MSVCRT _pclose returns the plain exit code (shift to match). */
  sp_last_status = st;
  return buf;
}

const char *sp_file_basename(const char *path) {SP_GC_ROOT_STR(path);
  /* CRuby: trailing separators are ignored ("/a/b/" -> "b"); an all-separator
     path is "/" (#2784). */
  size_t end = strlen(path);
  while (end > 0 && path[end - 1] == '/') end--;
  if (end == 0) {
    if (path[0] == '/') { char *r = sp_str_alloc(1); r[0] = '/'; r[1] = 0; return r; }
    return sp_str_alloc(0);
  }
  size_t start = end;
  while (start > 0 && path[start - 1] != '/') start--;
  /* sp_gc_mark looks at byte[-1] to distinguish heap strings (`\xfe`)
     from literals (`\xff`). A mid-path pointer has whatever byte came
     before it, so return a fresh sp_str_alloc'd copy with the right marker. */
  size_t n = end - start;
  char *buf = sp_str_alloc((sp_int)n);
  memcpy(buf, path + start, n);
  buf[n] = 0;
  return buf;
}
/* File.basename(path, suffix): ".*" strips the (non-leading) last extension,
   any other suffix strips a literal tail match (#2774). */
const char *sp_file_basename2(const char *path, const char *suffix) {SP_GC_ROOT_STR(path);SP_GC_ROOT_STR(suffix);
  const char *base = sp_file_basename(path);
  size_t n = strlen(base);
  if (suffix && strcmp(suffix, ".*") == 0) {
    const char *dot = strrchr(base, '.');
    if (dot && dot != base) n = (size_t)(dot - base);
  }
  else if (suffix && suffix[0]) {
    size_t sl = strlen(suffix);
    if (n > sl && strcmp(base + n - sl, suffix) == 0) n -= sl;
  }
  if (n == strlen(base)) return base;
  char *r = sp_str_alloc((sp_int)n);
  memcpy(r, base, n); r[n] = 0;
  return r;
}

const char *sp_file_extname(const char *path) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  const char *dot = strrchr(base, '.');
  /* CRuby: leading-dot files (".bashrc") return "". Trailing-dot
     paths ("foo.") keep the dot since Ruby 2.7. */
  if (!dot || dot == base) return sp_str_empty;
  size_t n = strlen(dot);
  char *buf = sp_str_alloc(n);
  memcpy(buf, dot, n + 1);
  return buf;
}

sp_StrArray *sp_dir_entries_impl(const char *path, int children) {
  SP_GC_ROOT_STR(path);
  if (!path) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  DIR *d = opendir(path);
  if (!d) sp_raise_cls("Errno::ENOENT", sp_sprintf("No such file or directory @ dir_initialize - %s", path));
  sp_StrArray *a = sp_StrArray_new();
  SP_GC_ROOT(a);
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    const char *name = e->d_name;
    if (children && name[0] == '.' &&
        (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
    char *copy = sp_str_alloc(strlen(name));
    strcpy(copy, name);
    sp_StrArray_push(a, copy);
  }
  closedir(d);
  sp_StrArray_sort_bang(a);
  return a;
}

/* The rows of a transpose are Arrays; a row that is not is CRuby's TypeError,
   named by the class of what was there. This unit sees the tags alone, so the
   naming is by tag. */
static int sp_transpose_row_p(sp_RbVal rv) {
  return rv.tag == SP_TAG_OBJ &&
         (rv.cls_id == SP_BUILTIN_INT_ARRAY || rv.cls_id == SP_BUILTIN_FLT_ARRAY ||
          rv.cls_id == SP_BUILTIN_STR_ARRAY || rv.cls_id == SP_BUILTIN_SYM_ARRAY ||
          rv.cls_id == SP_BUILTIN_POLY_ARRAY);
}
static const char *sp_transpose_row_class(sp_RbVal rv) {
  switch (rv.tag) {
    case SP_TAG_NIL: return "nil";
    case SP_TAG_BOOL: return rv.v.i ? "true" : "false";
    case SP_TAG_INT: return "Integer";
    case SP_TAG_FLT: return "Float";
    case SP_TAG_STR: return "String";
    case SP_TAG_SYM: return "Symbol";
    default: return "Object";
  }
}
sp_PolyArray *sp_poly_array_transpose(sp_PolyArray *rows) {
  SP_GC_SAVE();
  SP_GC_ROOT(rows);
  if (!rows || rows->len == 0) return sp_PolyArray_new();
  sp_int nrows = rows->len;
  /* Determine column count and element kind from first non-empty row. */
  sp_int ncols = -1;   /* -1 until the first row fixes it; ragged rows raise (#2979) */
  int16_t kind = 0; /* 0=unknown, SP_BUILTIN_INT_ARRAY, SP_BUILTIN_FLT_ARRAY, SP_BUILTIN_STR_ARRAY */
  for (sp_int r = 0; r < nrows; r++) {
    sp_RbVal rv = rows->data[r];
    /* a row that is no Array is CRuby's TypeError, not a row of nothing */
    if (!sp_transpose_row_p(rv))
      sp_raise_cls("TypeError", sp_sprintf("no implicit conversion of %s into Array", sp_transpose_row_class(rv)));
    sp_int rlen = 0;
    if (rv.cls_id == SP_BUILTIN_INT_ARRAY)  { rlen = ((sp_IntArray *)rv.v.p)->len; if(!kind) kind = SP_BUILTIN_INT_ARRAY; }
    else if (rv.cls_id == SP_BUILTIN_FLT_ARRAY) { rlen = ((sp_FloatArray *)rv.v.p)->len; if(!kind) kind = SP_BUILTIN_FLT_ARRAY; }
    else if (rv.cls_id == SP_BUILTIN_STR_ARRAY) { rlen = ((sp_StrArray *)rv.v.p)->len; if(!kind) kind = SP_BUILTIN_STR_ARRAY; }
    else if (rv.cls_id == SP_BUILTIN_POLY_ARRAY) { rlen = ((sp_PolyArray *)rv.v.p)->len; if(!kind) kind = SP_BUILTIN_POLY_ARRAY; }
    if (ncols < 0) ncols = rlen;
    else if (rlen != ncols)
      sp_raise_cls("IndexError", sp_sprintf("element size differs (%lld should be %lld)",
                                            (long long)rlen, (long long)ncols));
  }
  if (ncols < 0) ncols = 0;
  sp_PolyArray *result = sp_PolyArray_new();
  SP_GC_ROOT(result);
  for (sp_int c = 0; c < ncols; c++) {
    sp_RbVal cv = sp_box_nil();
    if (kind == SP_BUILTIN_INT_ARRAY) {
      sp_IntArray *col = sp_IntArray_new();
      SP_GC_ROOT(col);
      for (sp_int r = 0; r < nrows; r++) {
        sp_RbVal rv = rows->data[r];
        sp_int val = SP_INT_NIL;
        if (rv.tag == SP_TAG_OBJ && rv.cls_id == SP_BUILTIN_INT_ARRAY) {
          sp_IntArray *row = (sp_IntArray *)rv.v.p;
          if (c < row->len) val = row->data[c];
        }
        sp_IntArray_push(col, val);
      }
      cv.tag = SP_TAG_OBJ; cv.cls_id = SP_BUILTIN_INT_ARRAY; cv.v.p = col;
    }
else if (kind == SP_BUILTIN_FLT_ARRAY) {
      sp_FloatArray *col = sp_FloatArray_new();
      SP_GC_ROOT(col);
      for (sp_int r = 0; r < nrows; r++) {
        sp_RbVal rv = rows->data[r];
        sp_float val = 0.0;
        if (rv.tag == SP_TAG_OBJ && rv.cls_id == SP_BUILTIN_FLT_ARRAY) {
          sp_FloatArray *row = (sp_FloatArray *)rv.v.p;
          if (c < row->len) val = row->data[c];
        }
        sp_FloatArray_push(col, val);
      }
      cv.tag = SP_TAG_OBJ; cv.cls_id = SP_BUILTIN_FLT_ARRAY; cv.v.p = col;
    }
else if (kind == SP_BUILTIN_STR_ARRAY) {
      sp_StrArray *col = sp_StrArray_new();
      SP_GC_ROOT(col);
      for (sp_int r = 0; r < nrows; r++) {
        sp_RbVal rv = rows->data[r];
        const char *val = sp_str_empty;
        if (rv.tag == SP_TAG_OBJ && rv.cls_id == SP_BUILTIN_STR_ARRAY) {
          sp_StrArray *row = (sp_StrArray *)rv.v.p;
          if (c < row->len && row->data[c]) val = row->data[c];
        }
        sp_StrArray_push(col, val);
      }
      cv.tag = SP_TAG_OBJ; cv.cls_id = SP_BUILTIN_STR_ARRAY; cv.v.p = col;
    }
    else if (kind == SP_BUILTIN_POLY_ARRAY) {
      /* rows are poly arrays (e.g. `map(&:reverse)` yields boxed poly arrays):
         read each element generically, one column a fresh poly array (#2921). */
      sp_PolyArray *col = sp_PolyArray_new();
      SP_GC_ROOT(col);
      for (sp_int r = 0; r < nrows; r++) {
        sp_RbVal rv = rows->data[r];
        sp_RbVal val = sp_box_nil();
        if (rv.tag == SP_TAG_OBJ && rv.cls_id == SP_BUILTIN_POLY_ARRAY) {
          sp_PolyArray *row = (sp_PolyArray *)rv.v.p;
          if (c < row->len) val = row->data[c];
        }
        sp_PolyArray_push(col, val);
      }
      cv.tag = SP_TAG_OBJ; cv.cls_id = SP_BUILTIN_POLY_ARRAY; cv.v.p = col;
    }
    sp_PolyArray_push(result, cv);
  }
  return result;
}

sp_PolyArray *sp_str_chars_poly(const char *s) {SP_GC_ROOT_STR(s);
  sp_PolyArray *a = sp_PolyArray_new();
  SP_GC_ROOT(a);
  if (!s) sp_nil_recv("chars");
  /* to the recorded byte length, not to the first NUL: a NUL is an ordinary
     character and the ones after it are real (#3473) */
  int bin = sp_str_is_binary(s);
  const char *end = s + sp_str_byte_len(s);
  for (const char *p = s; p < end; ) {
    int n = bin ? 1 : sp_utf8_advance(p);
    if (p + n > end) n = (int)(end - p);
    char *c = sp_str_alloc(n); memcpy(c, p, n); c[n] = 0;
    sp_PolyArray_push(a, sp_box_str(c));
    p += n;
  }
  return a;
}

/* ---- Native backtrace formatting (spinel --debug) ----
   Moved from spinel_rt.h. The capture itself (backtrace() at raise time)
   stays in the header next to sp_raise_cls; only the cold symbol->Ruby-frame
   formatting lives here. The two flag globals are defined here so the
   debug-build main() (generated TU) and the header callers share one copy. */
#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#else
/* No execinfo.h: provide no-op shims so the formatting code below compiles
   and links unchanged. backtrace_symbols returns NULL, which the formatter
   treats as "nothing to format" -- the backtrace is simply empty. */
#define backtrace_symbols(buf, n) ((char **)0)
#endif
int sp_bt_enabled = 0;          /* set to 1 by debug-build main() */
const char *sp_bt_srcfile = ""; /* toplevel .rb path, set by debug main() */
static int sp_bt_is_runtime(const char *n) {
  static const char *pfx[] = {
    "int_", "str_", "float_", "sym_", "gc_", "bigint", "sprintf", "raise",
    "exc_", "range", "utf8", "oom", "bt_", "backtrace", "caller", "StrArray",
    "IntArray", "FloatArray", "PtrArray", "PolyArray", "Str", "Int", "Float",
    "Hash", "Range", "Complex", "Rational", "Sym", "alloc", "free", "to_s",
    "dup", "new", "pack", "unpack", "regex", "re_",
    /* arithmetic/runtime helpers that can raise and sit between the raise
       and the user frame (ZeroDivisionError via sp_idiv/sp_imod, etc.) */
    "idiv", "imod", "gcd", "fdiv", "ipow", "iclamp", "div_", "mod_", 0
  };
  for (int i = 0; pfx[i]; i++) {
    size_t l = strlen(pfx[i]);
    if (strncmp(n, pfx[i], l) == 0) return 1;
  }
  return 0;
}

/* Extract the symbol token from a backtrace_symbols line. Two formats:
     - glibc/Linux: "<module>(<symbol>+0x<off>) [0x<addr>]". The symbol is
       empty for unresolved frames (static or stripped fns) -> skip.
     - macOS:       "<idx> <image> <addr> <symbol> + <off>".
   Returns NULL if it isn't a keepable user frame. Detect Linux by the '('
   that delimits the symbol (the macOS format has none). */
static const char *sp_bt_symbol(const char *line) {
  char sym[256];
  const char *lp = strchr(line, '(');
  if (lp) {                                       /* glibc/Linux paren form */
    const char *p = lp + 1;
    const char *end = p;
    while (*end && *end != '+' && *end != ')') end++;
    size_t len = (size_t)(end - p);
    if (len == 0 || len > 250) return 0;          /* unresolved (static/stripped) */
    memcpy(sym, p, len); sym[len] = 0;
  }
else {                                        /* macOS: "<idx> <image> <addr> <symbol> + <off>" */
    /* The symbol is the token just before the " + <off>" delimiter. Parse
       backward from the last " + " rather than forward from "0x" — an image
       path containing "0x" (e.g. /path/0x_proj/bin) would otherwise misparse. */
    const char *plus = 0, *q = line;
    while ((q = strstr(q, " + ")) != 0) { plus = q; q += 3; }
    if (!plus) return 0;
    const char *end = plus;
    const char *p = end;
    while (p > line && p[-1] != ' ') p--;          /* back up to the symbol's start */
    size_t len = (size_t)(end - p);
    if (len == 0 || len > 250) return 0;
    memcpy(sym, p, len); sym[len] = 0;
  }
  if (strcmp(sym, "main") == 0) return strdup("<main>");
  if (strncmp(sym, "sp_", 3) != 0) return 0;     /* skip non-Spinel frames */
  const char *name = sym + 3;
  if (sp_bt_is_runtime(name)) return 0;
  /* De-mangle sp_<Class>_<method> back to Ruby. A Spinel symbol is a path of
     CamelCase class segments (each from a `::`, joined by `_`) followed by the
     method; the method is the first segment that starts lowercase. A literal
     `cls` segment marks a singleton method:
       sp_Helper_cls_boom          -> Helper.boom
       sp_Tep_Url_parse_query      -> Tep::Url#parse_query
       sp_Tep_AuthOAuth2_cls_find  -> Tep::AuthOAuth2.find
       sp_toplevel                 -> toplevel   (top-level method, no class)
     (Method names stay sanitized — e.g. enabled? is enabled_p; reversing that
     needs the emitted name table, a separate refinement.) */
  const char *mstart = 0;   /* first lowercase-starting segment = the method */
  for (const char *p = name; *p; p++) {
    int seg_start = (p == name) || (p[-1] == '_');
    if (seg_start && *p >= 'a' && *p <= 'z') { mstart = p; break; }
  }
  if (!mstart) return strdup(name);            /* all-uppercase: leave as-is */
  if (mstart == name) {                        /* no class path: top-level */
    if (strncmp(name, "cls_", 4) == 0) return strdup(name + 4);  /* top-level singleton */
    return strdup(name);
  }
  char out[256]; size_t o = 0;
  const char *meth; char sep;
  if (strncmp(mstart, "cls_", 4) == 0) { meth = mstart + 4; sep = '.'; }  /* singleton */
  else                                 { meth = mstart;     sep = '#'; }  /* instance */
  for (const char *p = name; p < mstart - 1 && o + 2 < sizeof(out); p++) {
    if (*p == '_') { out[o++] = ':'; out[o++] = ':'; }   /* class-path `_` was a `::` */
    else out[o++] = *p;
  }
  if (o + 1 < sizeof(out)) out[o++] = sep;
  size_t ml = strlen(meth);
  if (o + ml < sizeof(out)) { memcpy(out + o, meth, ml); o += ml; }
  out[o] = 0;
  return strdup(out);
}

sp_StrArray *sp_bt_format(void **buf, int n) {
  sp_StrArray *a = sp_StrArray_new();
  SP_GC_ROOT(a);
  if (!sp_bt_enabled || n <= 0) return a;
  char **syms = backtrace_symbols(buf, n);
  if (!syms) return a;
  const char *src = (sp_bt_srcfile && sp_bt_srcfile[0]) ? sp_bt_srcfile : "(spinel)";
  for (int i = 0; i < n; i++) {
    char *name = (char *)sp_bt_symbol(syms[i]);  /* always strdup'd; free after use */
    if (!name) continue;
    sp_StrArray_push(a, sp_sprintf("%s:in `%s'", src, name));
    free(name);
  }
  free(syms);
  return a;
}

#include <fcntl.h>
#include <sys/file.h>
#include <fnmatch.h>
#include <pwd.h>
#include <sys/wait.h>

/* ---- File / Dir surface ops moved from spinel_rt.h ----
   Path-level libc wrappers (stat family, Dir handle ops, FileTest
   helpers): cold, and every dependency is lib-visible. Prototypes
   first: the bodies keep their original header order, but a few
   helpers were forward-referenced there. */
const char *sp_File_gets_sep(sp_File *f, const char *sep, sp_int limit, sp_bool chomp);
sp_StrArray *sp_File_readlines_sep(sp_File *f, const char *sep, sp_bool chomp);
sp_StrArray *sp_file_readlines_sep(const char *path, const char *sep, sp_bool chomp);
const char *sp_File_readline_sep(sp_File *f, const char *sep, sp_int limit, sp_bool chomp);
const char *sp_File_getc(sp_File *f);
const char *sp_File_readchar(sp_File *f);
sp_int sp_File_getbyte(sp_File *f);
sp_RbVal sp_File_ungetc(sp_File *f, sp_RbVal v);
const char *sp_File_readpartial(sp_File *f, sp_int n);
sp_int sp_File_sysseek(sp_File *f, sp_int off, sp_int whence);
sp_int sp_File_flock(sp_File *f, sp_int op);
sp_int sp_File_fsync(sp_File *f);
sp_RbVal sp_File_putc(sp_File *f, sp_RbVal v);
const char *sp_file_ftype(const char *path);
sp_bool sp_file_readable(const char *path);
sp_bool sp_file_writable(const char *path);
sp_bool sp_file_executable(const char *path);
sp_bool sp_file_readable_real(const char *path);
sp_bool sp_file_writable_real(const char *path);
sp_bool sp_file_executable_real(const char *path);
const char *sp_file_realdirpath(const char *path);
sp_Addrinfo *sp_addrinfo_new(const char *ip, sp_int port, sp_int stype, sp_int is_unix);
const char *sp_addrinfo_inspect(sp_Addrinfo *a);
sp_SockOpt *sp_sockopt_new(sp_int family, sp_int level, sp_int optname, sp_int value);
const char *sp_sockopt_inspect(sp_SockOpt *o);
sp_File *sp_io_for_fd(sp_int fd, const char *mode, sp_bool autoclose);
sp_File *sp_io_wait_events(sp_File *f, double timeout, sp_int kind);
sp_RbVal sp_io_select(sp_PolyArray *rd, sp_PolyArray *wr, sp_PolyArray *er, double timeout);
sp_int sp_file_size_q(const char *path);
sp_bool sp_file_pipe(const char *path);
sp_bool sp_file_identical(const char *a, const char *b);
const char *sp_file_realpath(const char *path);
const char *sp_file_read_len(const char *path, sp_int n);
sp_int sp_file_chmod(sp_int mode, const char *path);
sp_int sp_file_truncate(const char *path, sp_int n);
sp_int sp_file_write_at(const char *path, const char *data, sp_int off);
sp_int sp_file_write_mode(const char *path, const char *data, const char *mode);
sp_File *sp_File_open_flags(const char *path, sp_int fl);
sp_File *sp_File_open_flags_perm(const char *path, sp_int fl, sp_int perm);
void sp_file_stat_scan(void *p);
sp_File *sp_file_stat_handle(const char *path);
sp_int sp_file_stat_mode(const char *path);
sp_bool sp_file_fnmatch(const char *pat, const char *path);
sp_StrArray *sp_file_split(const char *path);
sp_bool sp_file_zero(const char *path);
const char *sp_file_dirname(const char *path);
const char *sp_dir_pwd(void);
sp_int sp_dir_mkdir(const char *path);
sp_int sp_dir_rmdir(const char *path);
sp_int sp_dir_chdir(const char *path);
const char *sp_dir_home(void);
void sp_Dir_fin(void *p);
void sp_Dir_scan(void *p);
sp_Dir *sp_Dir_new(const char *path);
sp_Dir *sp_Dir_for_fd(sp_int fd);
sp_StrArray *sp_Dir_entries_h(sp_Dir *d, sp_int children);
sp_int sp_Dir_fchdir(sp_int fd);
const char *sp_Dir_read(sp_Dir *d);
const char *sp_Dir_path(sp_Dir *d);
sp_RbVal sp_Dir_close(sp_Dir *d);
sp_Dir *sp_Dir_rewind(sp_Dir *d);
sp_int sp_Dir_tell(sp_Dir *d);
sp_Dir *sp_Dir_seek(sp_Dir *d, sp_int pos);
sp_int sp_Dir_fileno(sp_Dir *d);
sp_StrArray *sp_dir_entries(const char *path);
sp_bool sp_dir_empty(const char *path);
const char *sp_dir_home_user(const char *user);
sp_StrArray *sp_dir_children(const char *path);

const char *sp_File_gets_sep(sp_File *f, const char *sep, sp_int limit, sp_bool chomp) {SP_GC_ROOT(f);SP_GC_ROOT_STR(sep);
  if (f && f->is_sock) sp_sock_wait_readable(f);
  if (!f || !f->fp) return NULL;
  size_t sl = sep ? strlen(sep) : 0;
  /* fast path: the default "\n" separator with no limit reads via fgets
     (the byte-wise loop below costs a call per character) */
  if (sl == 1 && sep[0] == '\n' && limit <= 0) {
    /* heap scratch: a 64KB stack local overruns the 64KB fiber stack */
    char *buf = (char *)malloc(65536);
    if (!buf) return NULL;
    if (!fgets(buf, 65536, f->fp)) { free(buf); return NULL; }
    size_t n = strlen(buf);
    if (chomp && n && buf[n - 1] == '\n') n--;
    char *r = sp_str_alloc(n);
    memcpy(r, buf, n); r[n] = 0;
    free(buf);
    sp_str_set_len(r, n);
    f->lineno++;
    return r;
  }
  size_t cap = 256, len = 0;
  char *buf = (char *)malloc(cap);
  if (!buf) return NULL;
  int ch;
  while ((ch = fgetc(f->fp)) != EOF) {
    if (len + 2 > cap) { cap *= 2; char *nb = (char *)realloc(buf, cap); if (!nb) { free(buf); return NULL; } buf = nb; }
    buf[len++] = (char)ch;
    if (sl && len >= sl && memcmp(buf + len - sl, sep, sl) == 0) break;
    if (limit > 0 && (sp_int)len >= limit) break;
  }
  if (len == 0) { free(buf); return NULL; }
  if (chomp && sl && len >= sl && memcmp(buf + len - sl, sep, sl) == 0) len -= sl;
  char *r = sp_str_alloc(len);
  memcpy(r, buf, len); r[len] = 0;
  sp_str_set_len(r, len);
  free(buf);
  f->lineno++;
  return r;
}
sp_StrArray *sp_File_readlines_sep(sp_File *f, const char *sep, sp_bool chomp) {SP_GC_ROOT(f);SP_GC_ROOT_STR(sep);
  sp_StrArray *a = sp_StrArray_new();
  SP_GC_ROOT(a);
  const char *l;
  while ((l = sp_File_gets_sep(f, sep, 0, chomp)) != NULL) sp_StrArray_push(a, l);
  return a;
}
sp_StrArray *sp_file_readlines_sep(const char *path, const char *sep, sp_bool chomp) {SP_GC_ROOT_STR(path);SP_GC_ROOT_STR(sep);
  sp_File *f = sp_File_open(path, "r");
  SP_GC_ROOT(f);
  sp_StrArray *a = sp_File_readlines_sep(f, sep, chomp);
  sp_File_close(f);
  return a;
}
const char *sp_File_readline_sep(sp_File *f, const char *sep, sp_int limit, sp_bool chomp) {SP_GC_ROOT(f);SP_GC_ROOT_STR(sep);
  const char *r = sp_File_gets_sep(f, sep, limit, chomp);
  if (!r) sp_raise_cls("EOFError", "end of file reached");
  return r;
}
const char *sp_File_getc(sp_File *f) {SP_GC_ROOT(f);
  if (!f || !f->fp) return NULL;
  int ch = fgetc(f->fp);
  if (ch == EOF) return NULL;
  int extra = ((ch & 0xE0) == 0xC0) ? 1 : ((ch & 0xF0) == 0xE0) ? 2 : ((ch & 0xF8) == 0xF0) ? 3 : 0;
  char *r = sp_str_alloc((size_t)(1 + extra));
  size_t n = 0;
  r[n++] = (char)ch;
  for (int i = 0; i < extra; i++) {
    int c2 = fgetc(f->fp);
    if (c2 == EOF) break;
    r[n++] = (char)c2;
  }
  r[n] = 0;
  sp_str_set_len(r, n);
  return r;
}
const char *sp_File_readchar(sp_File *f) {SP_GC_ROOT(f);
  const char *r = sp_File_getc(f);
  if (!r) sp_raise_cls("EOFError", "end of file reached");
  return r;
}
sp_int sp_File_getbyte(sp_File *f) {
  if (!f || !f->fp) return SP_INT_NIL;
  int ch = fgetc(f->fp);
  return ch == EOF ? SP_INT_NIL : (sp_int)ch;
}
sp_RbVal sp_File_ungetc(sp_File *f, sp_RbVal v) {
  if (f && f->fp) {
    if (v.tag == SP_TAG_STR && v.v.s && v.v.s[0]) {
      size_t n = sp_str_byte_len(v.v.s);
      for (size_t i = n; i > 0; i--) ungetc((unsigned char)v.v.s[i - 1], f->fp);
    }
    else if (v.tag == SP_TAG_INT) ungetc((int)v.v.i, f->fp);
  }
  return sp_box_nil();
}
sp_int sp_File_sysseek(sp_File *f, sp_int off, sp_int whence) {
  if (!f || !f->fp) return 0;
  fseek(f->fp, (long)off, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET);
  return (sp_int)ftell(f->fp);
}
sp_int sp_File_flock(sp_File *f, sp_int op) {
  if (!f || !f->fp) return 0;
  return flock(fileno(f->fp), (int)op) == 0 ? 0 : 1;
}
sp_int sp_File_fsync(sp_File *f) {
  if (!f || !f->fp) return 0;
  fflush(f->fp);
  fsync(fileno(f->fp));
  return 0;
}
sp_RbVal sp_File_putc(sp_File *f, sp_RbVal v) {
  if (f && f->fp) {
    if (v.tag == SP_TAG_INT) fputc((int)(v.v.i & 0xff), f->fp);
    else if (v.tag == SP_TAG_STR && v.v.s && v.v.s[0]) fputc(v.v.s[0], f->fp);
  }
  return v;
}
const char *sp_file_ftype(const char *path) {SP_GC_ROOT_STR(path);
  struct stat st;
  if (lstat(path ? path : "", &st) != 0)
    sp_raise_cls("Errno::ENOENT",
                 sp_sprintf("No such file or directory @ rb_file_s_ftype - %s", path ? path : ""));
  if (S_ISREG(st.st_mode)) return (&("\xff" "file")[1]);
  if (S_ISDIR(st.st_mode)) return (&("\xff" "directory")[1]);
  if (S_ISLNK(st.st_mode)) return (&("\xff" "link")[1]);
  if (S_ISFIFO(st.st_mode)) return (&("\xff" "fifo")[1]);
  if (S_ISCHR(st.st_mode)) return (&("\xff" "characterSpecial")[1]);
  if (S_ISBLK(st.st_mode)) return (&("\xff" "blockSpecial")[1]);
#ifdef S_ISSOCK
  if (S_ISSOCK(st.st_mode)) return (&("\xff" "socket")[1]);
#endif
  return (&("\xff" "unknown")[1]);
}
/* The `_real?` predicates test the real uid/gid, which is exactly what
   access(2) does; their plain counterparts test the effective ids, which needs
   AT_EACCESS. Routing both through access() would make File.readable? answer
   for the wrong identity in a setuid program. */
static sp_bool sp_file_access_eff(const char *path, int mode) {
  return faccessat(AT_FDCWD, path ? path : "", mode, AT_EACCESS) == 0;
}
sp_bool sp_file_readable(const char *path)   {SP_GC_ROOT_STR(path); return sp_file_access_eff(path, R_OK); }
sp_bool sp_file_writable(const char *path)   {SP_GC_ROOT_STR(path); return sp_file_access_eff(path, W_OK); }
sp_bool sp_file_executable(const char *path) {SP_GC_ROOT_STR(path); return sp_file_access_eff(path, X_OK); }
sp_bool sp_file_readable_real(const char *path)   { return access(path ? path : "", R_OK) == 0; }
sp_bool sp_file_writable_real(const char *path)   { return access(path ? path : "", W_OK) == 0; }
sp_bool sp_file_executable_real(const char *path) { return access(path ? path : "", X_OK) == 0; }
sp_int sp_file_size_q(const char *path) {   /* Integer size, or nil for missing/empty */
  struct stat st;
  if (stat(path ? path : "", &st) != 0 || st.st_size == 0) return SP_INT_NIL;
  return (sp_int)st.st_size;
}
sp_bool sp_file_pipe(const char *path) {
  struct stat st;
  return stat(path ? path : "", &st) == 0 && S_ISFIFO(st.st_mode);
}
sp_bool sp_file_identical(const char *a, const char *b) {
  struct stat sa, sb;
  if (stat(a ? a : "", &sa) != 0 || stat(b ? b : "", &sb) != 0) return 0;
  return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}
const char *sp_file_realpath(const char *path) {SP_GC_ROOT_STR(path);
  char buf[4096];
  if (!realpath(path ? path : "", buf))
    sp_raise_cls("Errno::ENOENT",
                 sp_sprintf("No such file or directory @ realpath_rec - %s", path ? path : ""));
  return sp_sprintf("%s", buf);
}
/* realdirpath resolves every component but the last, so it answers for a name
   that does not exist yet -- where realpath raises Errno::ENOENT. */
const char *sp_file_realdirpath(const char *path) {
  const char *p = path ? path : "";
  char buf[4096];
  if (realpath(p, buf)) return sp_sprintf("%s", buf);
  const char *slash = strrchr(p, '/');
  const char *base = slash ? slash + 1 : p;
  char dir[4096];
  if (!slash) snprintf(dir, sizeof dir, ".");
  else if (slash == p) snprintf(dir, sizeof dir, "/");
  else snprintf(dir, sizeof dir, "%.*s", (int)(slash - p), p);
  if (!realpath(dir, buf))
    sp_raise_cls("Errno::ENOENT",
                 sp_sprintf("No such file or directory @ realpath_rec - %s", dir));
  if (!*base || strcmp(base, ".") == 0) return sp_sprintf("%s", buf);
  if (strcmp(buf, "/") == 0) return sp_sprintf("/%s", base);
  return sp_sprintf("%s/%s", buf, base);
}
sp_bool sp_file_absolute_path_p(const char *path) { return path && path[0] == '/'; }  /* (#2988) */
sp_int sp_file_chown(const char *path, sp_int uid, sp_int gid) {SP_GC_ROOT_STR(path);  /* -1 leaves that id unchanged; returns the path count (#2987) */
  if (chown(path ? path : "", (uid_t)uid, (gid_t)gid) != 0)
    sp_raise_cls("Errno::ENOENT", sp_sprintf("No such file or directory - %s", path ? path : ""));
  return 1;
}
const char *sp_file_read_len(const char *path, sp_int n) {SP_GC_ROOT_STR(path);
  FILE *fp = fopen(path ? path : "", "rb");
  if (!fp)
    sp_raise_cls("Errno::ENOENT",
                 sp_sprintf("No such file or directory @ rb_sysopen - %s", path ? path : ""));
  if (n < 0) n = 0;
  char *r = sp_str_alloc(n);
  size_t got = fread(r, 1, (size_t)n, fp);
  fclose(fp);
  r[got] = 0;
  sp_str_set_len(r, got);
  return r;
}
sp_int sp_file_chmod(sp_int mode, const char *path) {SP_GC_ROOT_STR(path);
  if (chmod(path ? path : "", (mode_t)mode) != 0)
    sp_raise_cls("Errno::ENOENT",
                 sp_sprintf("No such file or directory @ apply2files - %s", path ? path : ""));
  return 1;
}
sp_int sp_file_truncate(const char *path, sp_int n) {SP_GC_ROOT_STR(path);
  if (truncate(path ? path : "", (off_t)n) != 0)
    sp_raise_cls("Errno::ENOENT",
                 sp_sprintf("No such file or directory @ rb_file_s_truncate - %s", path ? path : ""));
  return 0;
}
sp_int sp_file_write_at(const char *path, const char *data, sp_int off) {SP_GC_ROOT_STR(path);SP_GC_ROOT_STR(data);
  int fd = open(path ? path : "", O_WRONLY | O_CREAT, 0666);
  if (fd < 0)
    sp_raise_cls("Errno::ENOENT",
                 sp_sprintf("No such file or directory @ rb_sysopen - %s", path ? path : ""));
  size_t n = sp_str_byte_len(data ? data : sp_str_empty);
  ssize_t w = pwrite(fd, data ? data : "", n, (off_t)off);
  close(fd);
  return w < 0 ? 0 : (sp_int)w;
}
sp_int sp_file_write_mode(const char *path, const char *data, const char *mode) {SP_GC_ROOT_STR(path);SP_GC_ROOT_STR(data);
  FILE *fp = fopen(path ? path : "", mode && mode[0] ? mode : "w");
  if (!fp)
    sp_raise_cls("Errno::ENOENT",
                 sp_sprintf("No such file or directory @ rb_sysopen - %s", path ? path : ""));
  size_t n = sp_str_byte_len(data ? data : sp_str_empty);
  size_t w = fwrite(data ? data : "", 1, n, fp);
  int err = (w < n || ferror(fp)) ? (errno ? errno : EIO) : 0;
  if (fclose(fp) != 0 && !err) err = errno ? errno : EIO;
  if (err) {
    errno = err;
    sp_raise_cls(err == ENOSPC ? "Errno::ENOSPC" : "SystemCallError",
                 sp_sprintf("%s @ rb_sys_fail_on_write - %s", strerror(err), path ? path : ""));
    return 0;
  }
  return (sp_int)w;
}
/* File.open(path, flags, perm): the flag word selects the fdopen mode; the
   permission bits reach open(2) only through this entry, so a created file
   carries the bits the caller asked for rather than 0666. */
static void sp_file_open_raise(const char *path) {
  int e = errno;
  const char *cls = e == ENOENT ? "Errno::ENOENT" : e == EACCES ? "Errno::EACCES"
                  : e == EEXIST ? "Errno::EEXIST" : e == EISDIR ? "Errno::EISDIR" : "SystemCallError";
  sp_raise_cls(cls, sp_sprintf("%s @ rb_sysopen - %s", strerror(e), path ? path : ""));
}
sp_File *sp_File_open_flags_perm(const char *path, sp_int fl, sp_int perm) {SP_GC_ROOT_STR(path);
  if (perm == SP_INT_NIL) perm = 0666;
  int fd = open(path ? path : "", (int)fl | O_CLOEXEC, (mode_t)perm);
  if (fd < 0) sp_file_open_raise(path);
  int acc = (int)fl & O_ACCMODE;
  const char *m = (acc == O_RDONLY) ? "r"
                : (acc == O_WRONLY) ? (((int)fl & O_APPEND) ? "a" : "w")
                : (((int)fl & O_APPEND) ? "a+" : "r+");
  sp_File *f = sp_io_fdopen(fd, m);
  f->path = path;
  return f;
}
sp_File *sp_File_open_flags(const char *path, sp_int fl) {
  return sp_File_open_flags_perm(path, fl, 0666);
}
/* File.open(path, "w", perm): the mode string selects the open(2) flags the
   way fopen(3) reads it -- `+` for read-write, `x` for exclusive creation --
   so the permission bits reach the syscall as they do for the flag-word
   form. The mode is checked before the file is opened, so a bad one raises
   CRuby's ArgumentError and leaves no descriptor behind. A nil perm (SP_INT_NIL)
   is CRuby's default, 0666. */
sp_File *sp_File_open_perm(const char *path, const char *mode, sp_int perm) {SP_GC_ROOT_STR(path);SP_GC_ROOT_STR(mode);
  const char *m = mode && mode[0] ? mode : "r";
  int fl;
  switch (m[0]) {
    case 'r': fl = O_RDONLY; break;
    case 'w': fl = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': fl = O_WRONLY | O_CREAT | O_APPEND; break;
    default:  sp_raise_cls("ArgumentError", sp_sprintf("invalid access mode %s", m)); return NULL;
  }
  for (const char *q = m + 1; *q && *q != ':'; q++) {
    if (*q == '+') fl = (fl & ~O_ACCMODE) | O_RDWR;
    else if (*q == 'x' && m[0] == 'w') fl |= O_EXCL;  /* exclusive create is a w mode only */
    else if (*q != 'b' && *q != 't' && *q != 'e') {
      sp_raise_cls("ArgumentError", sp_sprintf("invalid access mode %s", m)); return NULL;
    }
  }
  if (perm == SP_INT_NIL) perm = 0666;
  int fd = open(path ? path : "", fl | O_CLOEXEC, (mode_t)perm);
  if (fd < 0) sp_file_open_raise(path);
  /* fdopen reads its own mode grammar ("wx+" is write-only to it): hand it
     the access mode the flag word says, as the flags form does */
  int acc = fl & O_ACCMODE;
  const char *fm = (acc == O_RDONLY) ? "r"
                 : (acc == O_WRONLY) ? ((fl & O_APPEND) ? "a" : "w")
                 : ((fl & O_APPEND) ? "a+" : (m[0] == 'w') ? "w+" : "r+");
  sp_File *f = sp_io_fdopen(fd, fm);
  f->path = path;
  f->mode = m;
  return f;
}
void sp_file_stat_scan(void *p) {
  sp_File *f = (sp_File *)p;
  if (f->path) sp_mark_string(f->path);
  if (f->mode) sp_mark_string(f->mode);
}
sp_File *sp_file_stat_handle(const char *path) {SP_GC_ROOT_STR(path);
  struct stat st;
  if (stat(path ? path : "", &st) != 0)
    sp_raise_cls("Errno::ENOENT",
                 sp_sprintf("No such file or directory @ rb_file_s_stat - %s", path ? path : ""));
  sp_File *f = (sp_File *)sp_gc_alloc(sizeof(sp_File), NULL, sp_file_stat_scan);
  f->fp = NULL;
  f->path = sp_sprintf("%s", path ? path : "");
  f->mode = (&("\xff" "stat")[1]);
  f->lineno = 0;
  return f;
}
/* File.lstat / File#lstat: like stat, but describing the link itself when the
   final component is a symlink. The handle records mode "lstat" so the
   accessors below read it with lstat(2). (#2986) */
sp_File *sp_file_lstat_handle(const char *path) {SP_GC_ROOT_STR(path);
  struct stat st;
  if (lstat(path ? path : "", &st) != 0)
    sp_raise_cls("Errno::ENOENT",
                 sp_sprintf("No such file or directory @ rb_file_s_lstat - %s", path ? path : ""));
  sp_File *f = (sp_File *)sp_gc_alloc(sizeof(sp_File), NULL, sp_file_stat_scan);
  f->fp = NULL;
  f->path = sp_sprintf("%s", path ? path : "");
  f->mode = (&("\xff" "lstat")[1]);
  f->lineno = 0;
  return f;
}
/* File::Stat#== is Comparable's, over <=>, which File::Stat defines on the
   modification time: two stats of one unchanged file are equal, and a stat
   handle is never equal to an open stream. Any other pair of handles compares
   by identity (Object#==). A pathless stat (IO#stat on a descriptor handle)
   carries its stream and is fstat(2)ed like its accessors. */
static int sp_stat_handle_p(sp_File *f) {
  return f && f->mode && (strcmp(f->mode, "stat") == 0 || strcmp(f->mode, "lstat") == 0);
}
static int sp_stat_handle_mtime(sp_File *f, struct stat *st) {
  if (f->path && f->path[0])
    return (strcmp(f->mode, "lstat") == 0 ? lstat(f->path, st) : stat(f->path, st)) == 0;
  return f->fp && fstat(fileno(f->fp), st) == 0;
}
sp_bool sp_io_eq(sp_File *a, sp_File *b) {
  if (a == b) return TRUE;
  if (!sp_stat_handle_p(a) || !sp_stat_handle_p(b)) return FALSE;
  struct stat sa, sb;
  if (!sp_stat_handle_mtime(a, &sa) || !sp_stat_handle_mtime(b, &sb)) return FALSE;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
  return sa.st_mtimespec.tv_sec == sb.st_mtimespec.tv_sec && sa.st_mtimespec.tv_nsec == sb.st_mtimespec.tv_nsec;
#else
  return sa.st_mtim.tv_sec == sb.st_mtim.tv_sec && sa.st_mtim.tv_nsec == sb.st_mtim.tv_nsec;
#endif
}
/* True when the handle came from lstat, so a final symlink is not followed. */
sp_bool sp_stat_nofollow(sp_File *f) {
  return f && f->mode && strcmp(f->mode, "lstat") == 0;
}
/* IO#stat. A handle opened from a descriptor (IO.new(fd) / IO.for_fd) has no
   path to stat, so the handle it answers carries the stream instead and the
   accessors below fstat(2) it. */
static int sp_stat_pathless(sp_File *f) {
  return f && f->fp && (!f->path || f->path[0] == 0);
}
sp_File *sp_io_stat_handle(sp_File *f) {SP_GC_ROOT(f);
  if (!f) sp_raise_cls("IOError", "closed stream");
  if (f->path && f->path[0]) return sp_file_stat_handle(f->path);
  struct stat st;
  if (!f->fp || fstat(fileno(f->fp), &st) != 0)
    sp_raise_cls("Errno::EBADF", "Bad file descriptor");
  sp_File *h = (sp_File *)sp_gc_alloc(sizeof(sp_File), NULL, sp_file_stat_scan);
  h->fp = f->fp;
  h->path = sp_str_empty;
  h->mode = (&("\xff" "stat")[1]);
  h->lineno = 0;
  return h;
}
/* One stat(2) field by index, so File::Stat's numeric accessors need one
   runtime entry rather than a dozen. 0=uid 1=gid 2=nlink 3=dev 4=ino
   5=blksize 6=blocks 7=rdev. SP_INT_NIL when the stat itself fails. */
sp_int sp_stat_field(sp_File *f, sp_int which) {SP_GC_ROOT(f);
  struct stat st;
  int r;
  if (sp_stat_pathless(f)) r = fstat(fileno(f->fp), &st);
  else {
    const char *p = (f && f->path) ? f->path : "";
    r = sp_stat_nofollow(f) ? lstat(p, &st) : stat(p, &st);
  }
  if (r != 0) return SP_INT_NIL;
  switch (which) {
    case 0: return (sp_int)st.st_uid;
    case 1: return (sp_int)st.st_gid;
    case 2: return (sp_int)st.st_nlink;
    case 3: return (sp_int)st.st_dev;
    case 4: return (sp_int)st.st_ino;
    case 5: return (sp_int)st.st_blksize;
    case 6: return (sp_int)st.st_blocks;
    default: return (sp_int)st.st_rdev;
  }
}
/* The mode-derived predicates of File::Stat that no path helper covers. Kinds:
   0=pipe? 1=zero? 2=readable? 3=writable? 4=executable? 5=blockdev?
   6=chardev? 7=size? (non-zero size, else nil). */
sp_int sp_stat_pred(sp_File *f, sp_int kind) {SP_GC_ROOT(f);
  struct stat st;
  int r;
  if (sp_stat_pathless(f)) r = fstat(fileno(f->fp), &st);
  else {
    const char *p = (f && f->path) ? f->path : "";
    r = sp_stat_nofollow(f) ? lstat(p, &st) : stat(p, &st);
  }
  if (r != 0) return kind == 7 ? SP_INT_NIL : 0;
  switch (kind) {
    case 0: return S_ISFIFO(st.st_mode) ? 1 : 0;
    case 1: return st.st_size == 0 ? 1 : 0;
    case 2: return (st.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) ? 1 : 0;
    case 3: return (st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) ? 1 : 0;
    case 4: return (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) ? 1 : 0;
    case 5: return S_ISBLK(st.st_mode) ? 1 : 0;
    case 6: return S_ISCHR(st.st_mode) ? 1 : 0;
    default: return st.st_size == 0 ? SP_INT_NIL : (sp_int)st.st_size;
  }
}
sp_int sp_stat_size(sp_File *f) {SP_GC_ROOT(f);
  struct stat st;
  if (sp_stat_pathless(f))
    return fstat(fileno(f->fp), &st) == 0 ? (sp_int)st.st_size : SP_INT_NIL;
  const char *p = (f && f->path) ? f->path : "";
  int r = sp_stat_nofollow(f) ? lstat(p, &st) : stat(p, &st);
  return r == 0 ? (sp_int)st.st_size : SP_INT_NIL;
}
sp_int sp_stat_mode(sp_File *f) {SP_GC_ROOT(f);
  struct stat st;
  if (sp_stat_pathless(f))
    return fstat(fileno(f->fp), &st) == 0 ? (sp_int)st.st_mode : 0;
  const char *p = (f && f->path) ? f->path : "";
  int r = sp_stat_nofollow(f) ? lstat(p, &st) : stat(p, &st);
  return r == 0 ? (sp_int)st.st_mode : 0;
}
const char *sp_stat_ftype(sp_File *f) {SP_GC_ROOT(f);
  if (sp_stat_pathless(f)) {
    struct stat st;
    if (fstat(fileno(f->fp), &st) != 0) return sp_str_empty;
    if (S_ISDIR(st.st_mode))  return (&("\xff" "directory")[1]);
    if (S_ISCHR(st.st_mode))  return (&("\xff" "characterSpecial")[1]);
    if (S_ISBLK(st.st_mode))  return (&("\xff" "blockSpecial")[1]);
    if (S_ISFIFO(st.st_mode)) return (&("\xff" "fifo")[1]);
    if (S_ISSOCK(st.st_mode)) return (&("\xff" "socket")[1]);
    return (&("\xff" "file")[1]);
  }
  /* sp_file_ftype already lstat()s, so it is exactly the lstat answer; a
     following handle resolves the link first. */
  const char *p = (f && f->path) ? f->path : "";
  if (sp_stat_nofollow(f)) return sp_file_ftype(p);
  { const char *rp = sp_file_realpath(p); return sp_file_ftype(rp ? rp : p); }
}
sp_int sp_file_stat_mode(const char *path) {
  struct stat st;
  if (stat(path ? path : "", &st) != 0) return 0;
  return (sp_int)st.st_mode;
}
sp_bool sp_file_fnmatch(const char *pat, const char *path) {
  return fnmatch(pat ? pat : "", path ? path : "", FNM_PATHNAME | FNM_PERIOD) == 0;
}
sp_StrArray *sp_file_split(const char *path) {SP_GC_ROOT_STR(path);
  sp_StrArray *a = sp_StrArray_new();
  SP_GC_ROOT(a);
  sp_StrArray_push(a, sp_file_dirname(path));
  sp_StrArray_push(a, sp_file_basename(path));
  return a;
}
sp_bool sp_file_zero(const char *path) {
  struct stat st;
  if (stat(path ? path : "", &st) != 0) return 0;
  if (S_ISDIR(st.st_mode)) return 0;
  return st.st_size == 0;
}
const char *sp_file_dirname(const char *path) {SP_GC_ROOT_STR(path);
  const char *s = strrchr(path, '/');
  if (!s) { char *r = sp_str_alloc(1); r[0] = '.'; r[1] = 0; return r; }
  if (s == path) { char *r = sp_str_alloc(1); r[0] = '/'; r[1] = 0; return r; }
  size_t n = (size_t)(s - path);
  char *buf = sp_str_alloc(n);
  memcpy(buf, path, n); buf[n] = 0;
  return buf;
}
const char *sp_dir_pwd(void) {
  char tmp[4096];
  if (!getcwd(tmp, sizeof(tmp))) { return sp_str_empty; }
  size_t n = strlen(tmp);
  char *buf = sp_str_alloc(n);
  memcpy(buf, tmp, n + 1);
  return buf;
}
sp_int sp_dir_mkdir(const char *path) {
  return (sp_int)mkdir(path, 0777);
}
sp_int sp_dir_rmdir(const char *path) {
  return (sp_int)rmdir(path);
}
sp_int sp_dir_chdir(const char *path) {
  return (sp_int)chdir(path);
}
const char *sp_dir_home(void) {
  const char *h = getenv("HOME");
  if (!h) return sp_str_empty;
  return sp_str_dup_external(h);
}
void sp_Dir_fin(void *p) { sp_Dir *d = (sp_Dir *)p; if (d->dp) { closedir(d->dp); d->dp = NULL; } }
void sp_Dir_scan(void *p) { sp_Dir *d = (sp_Dir *)p; if (d->path) sp_mark_string(d->path); }
sp_Dir *sp_Dir_new(const char *path) {SP_GC_ROOT_STR(path);
  DIR *dp = opendir(path ? path : "");
  if (!dp)
    sp_raise_cls("Errno::ENOENT",
                 sp_sprintf("No such file or directory @ dir_initialize - %s", path ? path : ""));
  sp_Dir *d = (sp_Dir *)sp_gc_alloc(sizeof(sp_Dir), sp_Dir_fin, sp_Dir_scan);
  d->dp = dp;
  d->path = NULL;   /* set below: the sprintf may GC, and the scan reads path */
  SP_GC_ROOT(d);
  d->path = sp_sprintf("%s", path ? path : "");
  return d;
}
/* Dir.for_fd(fd): take over a descriptor already opened on a directory. The
   handle owns the fd from here, as CRuby's does. */
sp_Dir *sp_Dir_for_fd(sp_int fd) {
  DIR *dp = fd >= 0 ? fdopendir((int)fd) : NULL;
  if (!dp) sp_raise_cls("Errno::EBADF", "Bad file descriptor - fdopendir");
  sp_Dir *d = (sp_Dir *)sp_gc_alloc(sizeof(sp_Dir), sp_Dir_fin, sp_Dir_scan);
  d->dp = dp;
  /* CRuby's Dir.for_fd has no path: the descriptor carries no name. */
  d->path = NULL;
  return d;
}
/* #entries / #children read the OPEN handle: a Dir from Dir.for_fd has no path
   to re-open, and re-opening by path would miss a directory that has since been
   renamed. Rewinds first so the listing is complete however far #read got. */
sp_StrArray *sp_Dir_entries_h(sp_Dir *d, sp_int children) {SP_GC_ROOT(d);
  if (!d || !d->dp) sp_raise_cls("IOError", "closed directory");
  rewinddir(d->dp);
  sp_StrArray *a = sp_StrArray_new();
  SP_GC_ROOT(a);
  struct dirent *e;
  while ((e = readdir(d->dp)) != NULL) {
    if (children && (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)) continue;
    sp_StrArray_push(a, sp_sprintf("%s", e->d_name));
  }
  rewinddir(d->dp);
  return a;
}
sp_int sp_Dir_fchdir(sp_int fd) {
  if (fd < 0 || fchdir((int)fd) != 0)
    sp_raise_cls("Errno::EBADF", "Bad file descriptor - fchdir");
  return 0;
}
const char *sp_Dir_read(sp_Dir *d) {
  if (!d || !d->dp) return NULL;
  struct dirent *e = readdir(d->dp);
  return e ? sp_sprintf("%s", e->d_name) : NULL;
}
/* NULL is Ruby nil in the string convention: a Dir from Dir.for_fd has no path,
   and CRuby answers nil for it rather than an empty string (#3365). */
const char *sp_Dir_path(sp_Dir *d) { return d ? d->path : NULL; }
sp_RbVal sp_Dir_close(sp_Dir *d) { if (d && d->dp) { closedir(d->dp); d->dp = NULL; } return sp_box_nil(); }
sp_Dir *sp_Dir_rewind(sp_Dir *d) { if (d && d->dp) rewinddir(d->dp); return d; }
sp_int sp_Dir_tell(sp_Dir *d) { return d && d->dp ? (sp_int)telldir(d->dp) : 0; }
sp_Dir *sp_Dir_seek(sp_Dir *d, sp_int pos) { if (d && d->dp) seekdir(d->dp, (long)pos); return d; }
sp_int sp_Dir_fileno(sp_Dir *d) { return d && d->dp ? (sp_int)dirfd(d->dp) : -1; }
sp_StrArray *sp_dir_entries(const char *path) {SP_GC_ROOT_STR(path); return sp_dir_entries_impl(path, 0); }
sp_bool sp_dir_empty(const char *path) {SP_GC_ROOT_STR(path);
  struct stat st;
  if (!path || stat(path, &st) != 0)
    sp_raise_cls("Errno::ENOENT",
                 sp_sprintf("No such file or directory @ rb_dir_s_empty_p - %s", path ? path : ""));
  if (!S_ISDIR(st.st_mode)) return FALSE;
  DIR *d = opendir(path);
  if (!d) return FALSE;
  struct dirent *e; sp_bool empty = TRUE;
  while ((e = readdir(d))) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    empty = FALSE; break;
  }
  closedir(d);
  return empty;
}
const char *sp_dir_home_user(const char *user) {SP_GC_ROOT_STR(user);
  if (!user || !*user) return sp_dir_home();
  struct passwd *pw = getpwnam(user);
  if (!pw || !pw->pw_dir)
    sp_raise_cls("ArgumentError", sp_sprintf("user %s doesn't exist", user));
  return sp_str_dup_external(pw->pw_dir);
}
sp_StrArray *sp_dir_children(const char *path) {SP_GC_ROOT_STR(path); return sp_dir_entries_impl(path, 1); }

/* ---- Signal trap machinery + Enumerator cursor/generator ops moved from
   spinel_rt.h ----
   Only tiny TU-defined helpers are linked from here (sp_proc_call, the boxed
   proc constructor, the trap state, the proc argument/return slots): the heavy
   poly/hash/render helpers (sp_enum_items_from, sp_poly_each_elem, ...) stay
   `static` in the generated TU so -O1 still prunes them from programs that
   never use them -- de-externing those measurably bloats every test compile. */
#include <signal.h>
#include "sp_enum.h"
typedef struct sp_Proc sp_Proc;
extern char **environ;
extern sp_RbVal sp_box_proc(void *p);
extern const char *sp_trap_state[];
extern struct sp_Proc *sp_trap_proc[];
extern SP_NORETURN void sp_raise_stop_iteration(sp_RbVal result);
extern SP_TLS sp_RbVal _sp_proc_poly_ret;
extern SP_TLS sp_RbVal _sp_proc_poly_args[];
extern sp_int sp_proc_call(sp_Proc *p, sp_int argc, sp_int *args);
extern const char *sp_signal_signame(sp_int no);
extern SP_COLD int sp_signal_resolve(sp_RbVal sig);

sp_Enumerator *sp_Enumerator_dup(sp_Enumerator *e);
void sp_Enumerator_scan(void *p);
sp_Enumerator *sp_enum_with_src(sp_Enumerator *e, sp_RbVal src, const char *meth);
sp_Enumerator *sp_enum_as_gen(sp_Enumerator *e);
sp_RbVal sp_enum_with_index_value(sp_Enumerator *e);
sp_RbVal sp_enum_with_index_result(sp_Enumerator *e, sp_PolyArray *mapped);
sp_Enumerator *sp_Enumerator_new_from_items(sp_PolyArray *items);
sp_Enumerator *sp_Enumerator_with_index(sp_Enumerator *e, sp_int off);
sp_Enumerator *sp_Enumerator_new_gen(void (*gen)(sp_Fiber *), void *cap, sp_RbVal size);
sp_RbVal sp_enum_gen_pull(sp_Enumerator *e);
sp_RbVal sp_Enumerator_next(sp_Enumerator *e);
sp_RbVal sp_Enumerator_peek(sp_Enumerator *e);
sp_PolyArray *sp_enum_values_wrap(sp_RbVal v);
sp_PolyArray *sp_Enumerator_next_values(sp_Enumerator *e);
sp_PolyArray *sp_Enumerator_peek_values(sp_Enumerator *e);
sp_Enumerator *sp_Enumerator_rewind(sp_Enumerator *e);
sp_RbVal sp_Enumerator_feed(sp_Enumerator *e, sp_RbVal v);
sp_PolyArray *sp_Enumerator_take(sp_Enumerator *e, sp_int n);
sp_PolyArray *sp_Enumerator_to_a(sp_Enumerator *e);
void sp_sig_c_handler(int no);
void sp_sig_exit_dispatch(void);
sp_RbVal sp_signal_trap(sp_RbVal sig, sp_RbVal handler);
sp_int sp_process_kill1(sp_RbVal sig, sp_int pid);
sp_RbVal sp_Enumerator_size(sp_Enumerator *e);

sp_Enumerator *sp_Enumerator_dup(sp_Enumerator *e) {SP_GC_ROOT(e);
  if (!e) return e;
  sp_Enumerator *d = (sp_Enumerator *)sp_gc_alloc(sizeof(sp_Enumerator), NULL, sp_Enumerator_scan);
  *d = *e;
  return d;
}
void sp_Enumerator_scan(void *p) {
  sp_Enumerator *e = (sp_Enumerator *)p;
  if (e->items) sp_gc_mark(e->items);
  if (e->fib) sp_gc_mark(e->fib);
  if (e->gen_cap) sp_gc_mark(e->gen_cap);
  if (e->peeked) sp_mark_rbval(e->peek_val);
  sp_mark_rbval(e->size);
  if (e->has_feed) sp_mark_rbval(e->feed);
  sp_mark_rbval(e->gen_result);
  sp_mark_rbval(e->source);
}
sp_Enumerator *sp_enum_with_src(sp_Enumerator *e, sp_RbVal src, const char *meth) { sp_gc_wb((void*)e);
  e->source = src;
  e->has_src = TRUE;
  e->meth = meth;
  return e;
}
sp_Enumerator *sp_enum_as_gen(sp_Enumerator *e) {
  e->gen_label = TRUE;
  return e;
}
sp_RbVal sp_enum_with_index_value(sp_Enumerator *e) {SP_GC_ROOT(e);
  if (e->meth && (strcmp(e->meth, "each") == 0 || strcmp(e->meth, "each_with_index") == 0))
    return e->source;
  sp_raise_cls("NotImplementedError",
               sp_sprintf("Enumerator#with_index return value for a stored %s enumerator",
                          e->meth ? e->meth : "generator"));
  return sp_box_nil();
}
sp_RbVal sp_enum_with_index_result(sp_Enumerator *e, sp_PolyArray *mapped) {SP_GC_ROOT(e);
  if (e->meth && (strcmp(e->meth, "map") == 0 || strcmp(e->meth, "collect") == 0))
    return sp_box_poly_array(mapped);
  if (e->meth && (strcmp(e->meth, "each") == 0 || strcmp(e->meth, "each_with_index") == 0))
    return e->source;
  sp_raise_cls("NotImplementedError",
               sp_sprintf("Enumerator#with_index return value for a stored %s enumerator",
                          e->meth ? e->meth : "generator"));
  return sp_box_nil();
}
/* `obj.then` / `obj.yield_self` with no block: an enumerator of exactly one
   element, the receiver, which is what CRuby answers (#4028). It renders as
   `#<Enumerator: 5:then>` and reports size 1. */
sp_Enumerator *sp_enum_of_one(sp_RbVal v, const char *meth) {
  SP_GC_ROOT_RBVAL(v);
  sp_PolyArray *a = sp_PolyArray_new();
  SP_GC_ROOT(a);
  sp_PolyArray_push(a, v);
  sp_Enumerator *e = sp_Enumerator_new_from_items(a);
  e->size = sp_box_int(1);
  return sp_enum_with_src(e, v, meth);
}
sp_Enumerator *sp_Enumerator_new_from_items(sp_PolyArray *items) {
  SP_GC_ROOT(items);
  sp_Enumerator *e = (sp_Enumerator *)sp_gc_alloc(sizeof(sp_Enumerator), NULL, sp_Enumerator_scan);
  e->items = items; e->cursor = 0; e->gen = NULL; e->gen_cap = NULL; e->fib = NULL; e->peeked = FALSE; e->size = sp_box_nil(); e->feed = sp_box_nil(); e->has_feed = FALSE; e->gen_result = sp_box_nil(); e->source = sp_box_nil(); e->meth = "each";
  return e;
}
/* Lazy with_index over a generator-backed source (blockless Kernel#loop,
   external enumerators): pull one value at a time and pair it with the
   running index, so an infinite source stays drivable via #next (#3236). */
typedef struct { sp_Enumerator *src; sp_int off; } sp_WiCap;
static void sp_wi_cap_scan(void *p) {
  sp_WiCap *cap = (sp_WiCap *)p;
  if (cap->src) sp_gc_mark(cap->src);
}
static void sp_with_index_gen(sp_Fiber *f) {
  sp_WiCap *cap = (sp_WiCap *)f->user_data;
  sp_Enumerator *s = cap->src;
  sp_int i = cap->off;
  for (;;) {
    sp_RbVal v;
    if (s->gen) {
      if (!s->fib) { s->fib = sp_Fiber_new(s->gen); if (s->gen_cap) { sp_gc_wb((void*)s->fib); s->fib->user_data = s->gen_cap; } }
      if (!sp_Fiber_alive(s->fib)) break;
      v = sp_Fiber_resume(s->fib, sp_box_nil());
      if (!sp_Fiber_alive(s->fib)) break;
    }
    else {
      if (!s->items || s->cursor >= s->items->len) break;
      v = s->items->data[s->cursor++];
    }
    sp_PolyArray *pair = sp_PolyArray_new();
    SP_GC_ROOT(pair);
    sp_PolyArray_push(pair, v);
    sp_PolyArray_push(pair, sp_box_int(i++));
    sp_Fiber_yield(sp_box_poly_array(pair));
  }
}
sp_Enumerator *sp_Enumerator_with_index(sp_Enumerator *e, sp_int off) {
  if (e && e->gen) {
    sp_WiCap *cap = (sp_WiCap *)sp_gc_alloc(sizeof(sp_WiCap), NULL, sp_wi_cap_scan);
    cap->src = e; cap->off = off;
    sp_Enumerator *r = sp_Enumerator_new_gen(sp_with_index_gen, cap, e->size);
    r->source = e->source;
    return r;
  }
  sp_PolyArray *src = e ? e->items : NULL;
  sp_int n = src ? src->len : 0;
  /* Root the source enumerator (a temp `arr.each` is otherwise unreachable): the
     sp_PolyArray_new below can collect it and free src mid-loop -> use-after-free
     of src->data[i] under GC stress. Rooting `e` keeps src alive transitively --
     sp_Enumerator_scan marks e->items and the GC is non-moving. */
  SP_GC_ROOT(e);
  sp_PolyArray *out = sp_PolyArray_new(); SP_GC_ROOT(out);
  for (sp_int i = 0; i < n; i++) {
    sp_PolyArray *pair = sp_PolyArray_new(); SP_GC_ROOT(pair);
    sp_PolyArray_push(pair, src->data[i]);
    sp_PolyArray_push(pair, sp_box_int(off + i));
    sp_PolyArray_push(out, sp_box_poly_array(pair));
  }
  { sp_RbVal src_recv = e ? e->source : sp_box_nil();
    sp_Enumerator *r = sp_Enumerator_new_from_items(out); r->source = src_recv; return r; }
}
sp_Enumerator *sp_Enumerator_new_gen(void (*gen)(sp_Fiber *), void *cap, sp_RbVal size) {SP_GC_ROOT_RBVAL(size);
  sp_Enumerator *e = (sp_Enumerator *)sp_gc_alloc(sizeof(sp_Enumerator), NULL, sp_Enumerator_scan);
  e->items = NULL; e->cursor = 0; e->gen = gen; e->gen_cap = cap; e->fib = NULL; e->peeked = FALSE; e->size = size; e->feed = sp_box_nil(); e->has_feed = FALSE; e->gen_result = sp_box_nil(); e->source = sp_box_nil(); e->meth = "each";
  return e;
}
/* Blockless Kernel#loop: an infinite Enumerator that yields nil forever (#3236).
   The generator is internal; only sp_loop_enum is exposed (spinel_rt.h). */
static void sp_loop_gen(sp_Fiber *f) {
  (void)f;
  for (;;) sp_Fiber_yield(sp_box_nil());
}
sp_Enumerator *sp_loop_enum(void) {
  sp_Enumerator *e = sp_Enumerator_new_gen(sp_loop_gen, NULL, sp_box_nil());
  e->meth = "loop";
  return e;
}

/* IO.copy_stream(src_path, dst_path): stream one file to another, byte count. */
sp_int sp_io_copy_stream(const char *src, const char *dst) {SP_GC_ROOT_STR(src);SP_GC_ROOT_STR(dst);
  FILE *in = fopen(src ? src : "", "rb");
  if (!in)
    sp_raise_cls("Errno::ENOENT",
                 sp_sprintf("No such file or directory @ rb_sysopen - %s", src ? src : ""));
  FILE *out = fopen(dst ? dst : "", "wb");
  if (!out) { fclose(in); sp_raise_cls("Errno::ENOENT",
                 sp_sprintf("No such file or directory @ rb_sysopen - %s", dst ? dst : "")); }
  char buf[8192]; size_t got; sp_int total = 0;
  while ((got = fread(buf, 1, sizeof buf, in)) > 0) { fwrite(buf, 1, got, out); total += (sp_int)got; }
  fclose(in); fclose(out);
  return total;
}

/* Array#combination / permutation over an int array (lib-only; the recursion
   helpers stay file-static, the four entry points are declared in spinel_rt.h). */
static void sp_int_combination_recur(sp_IntArray*src,sp_int start,sp_int k,sp_IntArray*acc,sp_PtrArray*out){SP_GC_ROOT(src);SP_GC_ROOT(acc);SP_GC_ROOT(out);if(k==0){sp_IntArray*cp=sp_IntArray_new();for(sp_int i=0;i<acc->len;i++)sp_IntArray_push(cp,acc->data[acc->start+i]);sp_PtrArray_push(out,cp);return;}for(sp_int i=start;i<=src->len-k;i++){sp_IntArray_push(acc,src->data[src->start+i]);sp_int_combination_recur(src,i+1,k-1,acc,out);acc->len--;}}
sp_PtrArray*sp_IntArray_combination(sp_IntArray*a,sp_int k){SP_GC_ROOT(a);sp_PtrArray*out=sp_PtrArray_new();SP_GC_ROOT(out);if(!a||k<0||k>a->len)return out;sp_IntArray*acc=sp_IntArray_new();SP_GC_ROOT(acc);sp_int_combination_recur(a,0,k,acc,out);return out;}
static void sp_int_repeated_combination_recur(sp_IntArray*src,sp_int start,sp_int k,sp_IntArray*acc,sp_PtrArray*out){SP_GC_ROOT(src);SP_GC_ROOT(acc);SP_GC_ROOT(out);if(k==0){sp_IntArray*cp=sp_IntArray_new();for(sp_int i=0;i<acc->len;i++)sp_IntArray_push(cp,acc->data[acc->start+i]);sp_PtrArray_push(out,cp);return;}for(sp_int i=start;i<src->len;i++){sp_IntArray_push(acc,src->data[src->start+i]);sp_int_repeated_combination_recur(src,i,k-1,acc,out);acc->len--;}}
sp_PtrArray*sp_IntArray_repeated_combination(sp_IntArray*a,sp_int k){SP_GC_ROOT(a);sp_PtrArray*out=sp_PtrArray_new();SP_GC_ROOT(out);if(!a||k<0)return out;sp_IntArray*acc=sp_IntArray_new();SP_GC_ROOT(acc);sp_int_repeated_combination_recur(a,0,k,acc,out);return out;}
static void sp_int_permutation_recur(sp_IntArray*src,sp_int k,sp_IntArray*used,sp_IntArray*acc,sp_PtrArray*out){SP_GC_ROOT(src);SP_GC_ROOT(used);SP_GC_ROOT(acc);SP_GC_ROOT(out);if(k==0){sp_IntArray*cp=sp_IntArray_new();for(sp_int i=0;i<acc->len;i++)sp_IntArray_push(cp,acc->data[acc->start+i]);sp_PtrArray_push(out,cp);return;}for(sp_int i=0;i<src->len;i++){if(used->data[used->start+i])continue;used->data[used->start+i]=1;sp_IntArray_push(acc,src->data[src->start+i]);sp_int_permutation_recur(src,k-1,used,acc,out);acc->len--;used->data[used->start+i]=0;}}
static void sp_int_repeated_permutation_recur(sp_IntArray*src,sp_int k,sp_IntArray*acc,sp_PtrArray*out){SP_GC_ROOT(src);SP_GC_ROOT(acc);SP_GC_ROOT(out);if(k==0){sp_IntArray*cp=sp_IntArray_new();SP_GC_ROOT(cp);for(sp_int i=0;i<acc->len;i++)sp_IntArray_push(cp,acc->data[acc->start+i]);sp_PtrArray_push(out,cp);return;}for(sp_int i=0;i<src->len;i++){sp_IntArray_push(acc,src->data[src->start+i]);sp_int_repeated_permutation_recur(src,k-1,acc,out);acc->len--;}}
sp_PtrArray*sp_IntArray_repeated_permutation(sp_IntArray*a,sp_int k){SP_GC_ROOT(a);sp_PtrArray*out=sp_PtrArray_new();SP_GC_ROOT(out);if(!a||k<0)return out;sp_IntArray*acc=sp_IntArray_new();SP_GC_ROOT(acc);sp_int_repeated_permutation_recur(a,k,acc,out);return out;}
sp_PtrArray*sp_IntArray_permutation(sp_IntArray*a,sp_int k){SP_GC_ROOT(a);sp_PtrArray*out=sp_PtrArray_new();SP_GC_ROOT(out);if(!a||k<0||k>a->len)return out;sp_IntArray*used=sp_IntArray_new();SP_GC_ROOT(used);for(sp_int i=0;i<a->len;i++)sp_IntArray_push(used,0);sp_IntArray*acc=sp_IntArray_new();SP_GC_ROOT(acc);sp_int_permutation_recur(a,k,used,acc,out);return out;}
sp_RbVal sp_enum_gen_pull(sp_Enumerator *e) {SP_GC_ROOT(e); sp_gc_wb((void*)e);
  if (!e->fib) {
    e->fib = sp_Fiber_new(e->gen);
    if (e->gen_cap) { sp_gc_wb((void*)e->fib); e->fib->user_data = e->gen_cap; }
  }
  if (!sp_Fiber_alive(e->fib)) sp_raise_stop_iteration(e->gen_result);
  sp_RbVal feed = e->has_feed ? e->feed : sp_box_nil();
  e->has_feed = FALSE; e->feed = sp_box_nil();   /* consumed by this resume */
  sp_RbVal v = sp_Fiber_resume(e->fib, feed);
  if (!sp_Fiber_alive(e->fib)) { e->gen_result = v; sp_raise_stop_iteration(v); }
  return v;
}
sp_RbVal sp_Enumerator_next(sp_Enumerator *e) {SP_GC_ROOT(e);
  if (e->gen) {
    if (e->peeked) { e->peeked = FALSE; return e->peek_val; }
    return sp_enum_gen_pull(e);
  }
  if (!e->items || e->cursor >= e->items->len) sp_raise_stop_iteration(e->source);
  return e->items->data[e->cursor++];
}
sp_RbVal sp_Enumerator_peek(sp_Enumerator *e) {SP_GC_ROOT(e); sp_gc_wb((void*)e);
  if (e->gen) {
    if (!e->peeked) { e->peek_val = sp_enum_gen_pull(e); e->peeked = TRUE; }
    return e->peek_val;
  }
  if (!e->items || e->cursor >= e->items->len) sp_raise_stop_iteration(e->source);
  return e->items->data[e->cursor];
}
sp_PolyArray *sp_enum_values_wrap(sp_RbVal v) {SP_GC_ROOT_RBVAL(v);
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_POLY_ARRAY) return (sp_PolyArray *)v.v.p;
  sp_PolyArray *a = sp_PolyArray_new(); SP_GC_ROOT(a);
  sp_PolyArray_push(a, v);
  return a;
}
sp_PolyArray *sp_Enumerator_next_values(sp_Enumerator *e) {SP_GC_ROOT(e); return sp_enum_values_wrap(sp_Enumerator_next(e)); }
sp_PolyArray *sp_Enumerator_peek_values(sp_Enumerator *e) {SP_GC_ROOT(e); return sp_enum_values_wrap(sp_Enumerator_peek(e)); }
sp_Enumerator *sp_Enumerator_rewind(sp_Enumerator *e) { sp_gc_wb((void*)e);
  if (!e) return NULL;
  if (e->gen) { e->fib = NULL; e->peeked = FALSE; e->gen_result = sp_box_nil(); }
  else e->cursor = 0;
  e->feed = sp_box_nil(); e->has_feed = FALSE;
  return e;
}
sp_RbVal sp_Enumerator_feed(sp_Enumerator *e, sp_RbVal v) {SP_GC_ROOT_RBVAL(v);SP_GC_ROOT(e); sp_gc_wb((void*)e);
  if (!e) return sp_box_nil();
  if (e->has_feed) sp_raise_cls("TypeError", (&("\xff" "feed value already set")[1]));
  e->feed = v; e->has_feed = TRUE;
  return sp_box_nil();
}
sp_PolyArray *sp_Enumerator_take(sp_Enumerator *e, sp_int n) {SP_GC_ROOT(e);
  sp_PolyArray *r = sp_PolyArray_new();
  SP_GC_ROOT(r);
  if (n <= 0) return r;
  if (e->gen) {
    sp_Fiber *f = sp_Fiber_new(e->gen);
    SP_GC_ROOT(f);
    if (e->gen_cap) { sp_gc_wb((void*)f); f->user_data = e->gen_cap; }
    for (sp_int i = 0; i < n; i++) {
      if (!sp_Fiber_alive(f)) break;
      sp_RbVal v = sp_Fiber_resume(f, sp_box_nil());
      if (!sp_Fiber_alive(f)) break;
      sp_PolyArray_push(r, v);
    }
    return r;
  }
  sp_int lim = e->items ? e->items->len : 0;
  if (n < lim) lim = n;
  for (sp_int i = 0; i < lim; i++) sp_PolyArray_push(r, e->items->data[i]);
  return r;
}
sp_PolyArray *sp_Enumerator_to_a(sp_Enumerator *e) {
  SP_GC_ROOT(e);   /* the allocation below can collect it */
  sp_PolyArray *r = sp_PolyArray_new();
  SP_GC_ROOT(r);
  if (!e) return r;
  if (e->gen) {
    sp_Fiber *f = sp_Fiber_new(e->gen);
    SP_GC_ROOT(f);
    if (e->gen_cap) { sp_gc_wb((void*)f); f->user_data = e->gen_cap; }
    while (sp_Fiber_alive(f)) {
      sp_RbVal v = sp_Fiber_resume(f, sp_box_nil());
      if (!sp_Fiber_alive(f)) break;
      sp_PolyArray_push(r, v);
    }
    return r;
  }
  if (e->items) for (sp_int i = 0; i < e->items->len; i++) sp_PolyArray_push(r, e->items->data[i]);
  return r;
}
void sp_sig_c_handler(int no) {
  sp_Proc *p = (no >= 0 && no < SP_SIG_MAX) ? (sp_Proc *)sp_trap_proc[no] : NULL;
  if (p) {
    sp_int slot = (sp_int)no;
    _sp_proc_poly_args[0] = sp_box_int((sp_int)no);
    sp_proc_call(p, 1, &slot);
  }
}
void sp_sig_exit_dispatch(void) {
  sp_Proc *p = (sp_Proc *)sp_trap_proc[0];
  sp_trap_proc[0] = NULL;   /* run once */
  if (p) { sp_int slot = 0; _sp_proc_poly_args[0] = sp_box_int(0); sp_proc_call(p, 1, &slot); }
}
sp_RbVal sp_signal_trap(sp_RbVal sig, sp_RbVal handler) {SP_GC_ROOT_RBVAL(sig);SP_GC_ROOT_RBVAL(handler);
  int no = sp_signal_resolve(sig);
  if (no == SIGKILL || no == SIGSTOP)
    sp_raise_cls("Errno::EINVAL",
                 sp_sprintf("Invalid argument - SIG%s", sp_signal_signame(no)));
  /* the previous handler: a stored proc or command string; an untouched
     signal reads "DEFAULT", except EXIT whose default handler is nil (#2839) */
  sp_RbVal prev = sp_trap_proc[no] ? sp_box_proc((sp_Proc *)sp_trap_proc[no])
                : sp_trap_state[no] ? sp_box_str(sp_trap_state[no])
                : no == 0 ? sp_box_nil()
                : sp_box_str((&("\xff" "DEFAULT")[1]));
  if (handler.tag == SP_TAG_OBJ && handler.cls_id == SP_BUILTIN_PROC && handler.v.p) {
    sp_trap_proc[no] = (sp_Proc *)handler.v.p;
    sp_trap_state[no] = NULL;
    if (no == 0) { static int armed = 0; if (!armed) { armed = 1; atexit(sp_sig_exit_dispatch); } }
    else signal(no, sp_sig_c_handler);
  }
  else {
    const char *hs = (handler.tag == SP_TAG_STR && handler.v.s) ? handler.v.s : "DEFAULT";
    int ignore = strcmp(hs, "IGNORE") == 0 || strcmp(hs, "SIG_IGN") == 0;
    sp_trap_proc[no] = NULL;
    /* SYSTEM_DEFAULT reads back as itself (#2839); it installs SIG_DFL too.
       An EXIT string command clears the handler, whose read-back is nil. */
    sp_trap_state[no] = no == 0 ? NULL
                      : ignore ? (&("\xff" "IGNORE")[1])
                      : strcmp(hs, "SYSTEM_DEFAULT") == 0 ? (&("\xff" "SYSTEM_DEFAULT")[1])
                      : (&("\xff" "DEFAULT")[1]);
    if (no != 0) signal(no, ignore ? SIG_IGN : SIG_DFL);
  }
  return prev;
}
sp_int sp_process_kill1(sp_RbVal sig, sp_int pid) {SP_GC_ROOT_RBVAL(sig);
  int no = sp_signal_resolve(sig);
  if (kill((pid_t)pid, no) != 0) {
    if (errno == ESRCH) sp_raise_cls("Errno::ESRCH", sp_sprintf("No such process - %lld", (long long)pid));
    if (errno == EPERM) sp_raise_cls("Errno::EPERM", sp_sprintf("Operation not permitted - %lld", (long long)pid));
    sp_raise_cls("Errno::EINVAL", "Invalid argument");
  }
  return 1;
}
sp_RbVal sp_Enumerator_size(sp_Enumerator *e) {SP_GC_ROOT(e);
  if (!e) return sp_box_nil();
  if (e->items) return sp_box_int(e->items->len);
  if (e->size.tag == SP_TAG_OBJ && e->size.cls_id == SP_BUILTIN_PROC) {
    (void)sp_proc_call((sp_Proc *)e->size.v.p, 0, NULL);
    return _sp_proc_poly_ret;
  }
  return e->size;
}

/* ---- ENV core (StrStrHash-backed, #2832/#2842) + GC.stat + String#setbyte
   COW -- relocated from spinel_rt.h. All reach only lib-visible helpers
   (sp_StrStrHash_*, sp_str_alloc/_raw, sp_str_check_mutable in sp_alloc.h,
   SP_HEAP_LOCK/UNLOCK, sp_gc_* counters in sp_gc.h). ---- */
#include "sp_hash.h"

/* ENV.shift: remove and return the first [key, value] pair, or nil */
sp_RbVal sp_env_shift(void) {
  extern char **environ;
  if (!environ || !*environ) return sp_box_nil();
  const char *ent = *environ;
  const char *eq = strchr(ent, '=');
  size_t n = eq ? (size_t)(eq - ent) : strlen(ent);
  char *k = sp_str_alloc(n);
  memcpy(k, ent, n); k[n] = 0;
  SP_GC_ROOT_STR(k);
  const char *v = sp_sprintf("%s", eq ? eq + 1 : "");
  SP_GC_ROOT_STR(v);
  sp_PolyArray *a = sp_PolyArray_new();
  SP_GC_ROOT(a);
  sp_PolyArray_push(a, sp_box_str(k));
  sp_PolyArray_push(a, sp_box_str(v));
  unsetenv(k);
  return sp_box_poly_array(a);
}
sp_int sp_env_size(void) {
  extern char **environ;
  sp_int n = 0;
  for (char **e = environ; e && *e; e++) n++;
  return n;
}
/* A snapshot of the environment as a StrStr hash: ENV's enumeration surface
   (keys/each/select/count{...}/inspect/...) is desugared onto it, so the
   whole Hash machinery serves it (#2742). Keys/values are copied onto the
   GC string heap -- environ storage may move under setenv. */
/* ENV mutators (#2832). Each returns the fresh snapshot so the expression
   value renders like ENV does. */
sp_StrStrHash *sp_env_to_h(void) {
  extern char **environ;
  sp_StrStrHash *h = sp_StrStrHash_new();
  SP_GC_ROOT(h);
  for (char **e = environ; e && *e; e++) {
    const char *eq = strchr(*e, '=');
    if (!eq) continue;
    /* copy the VALUE first and root it: the key below is a fresh unreachable
       heap string until the set, and the value copy may GC (#2842 -- a large
       environment collected mid-loop and swept the just-built key) */
    const char *v = sp_str_dup_external(eq + 1);
    SP_GC_ROOT_STR(v);
    size_t kl = (size_t)(eq - *e);
    char *k = sp_str_alloc_raw(kl + 1);
    memcpy(k, *e, kl); k[kl] = 0;
    sp_StrStrHash_set(h, k, v);
  }
  return h;
}
sp_StrStrHash *sp_env_clear(void) {
  extern char **environ;
  for (;;) {   /* environ shifts under unsetenv: restart until empty */
    char **e = environ;
    if (!e || !*e) break;
    const char *eq = strchr(*e, '=');
    size_t n = eq ? (size_t)(eq - *e) : strlen(*e);
    char k[512];
    if (n >= sizeof k) n = sizeof k - 1;
    memcpy(k, *e, n); k[n] = 0;
    unsetenv(k);
  }
  return sp_env_to_h();
}
/* ENV.update/merge!/replace with a string-pair hash */
sp_StrStrHash *sp_env_update_h(sp_StrStrHash *h, int replace) {
  if (replace) sp_env_clear();
  if (h) {
    SP_GC_ROOT(h);
    for (sp_int i = 0; i < h->len; i++) {
      const char *v = sp_StrStrHash_get(h, h->order[i]);
      if (v) setenv(h->order[i], v, 1); else unsetenv(h->order[i]);
    }
  }
  return sp_env_to_h();
}
/* Keys are spinel rodata literals (SPL: 0xff marker prefix) so the str-hash
   header cache's s[-1] read is in-bounds -- a bare C literal here would
   overread (and could alias a heap marker on some rodata layouts). */
sp_StrIntHash*sp_gc_stat(void){
  /* The string heap (sp_str_heap) is malloc'd separately and deliberately
     excluded from sp_gc_bytes (see sp_str_alloc). Surface its footprint so
     GC.stat can explain "RSS huge but bytes tiny" for string-heavy workloads.
     Prototype: O(n) walk; a production version maintains a running counter.
     The walk holds the heap lock: sp_str_alloc pushes onto this list under
     it from any worker, and an unlocked traversal could read a half-linked
     node. Unlock before building the hash -- sp_gc_alloc takes the same
     (non-recursive) lock. */
  size_t str_bytes=0; sp_int str_count=0;
#ifdef SP_THREADS
  /* Per-worker lists (see sp_alloc.h): each has a single pusher, and only the
     head moves, so a snapshot walk reaches fully-linked nodes -- at worst it
     misses a just-pushed one (benign undercount for an introspection stat). */
  { int nw = sp_active_workers; if (nw < 1) nw = 1; if (nw > SP_MAX_WORKERS) nw = SP_MAX_WORKERS;
    for (int wi = 0; wi < nw; wi++) {
      for(sp_str_hdr*sh=sp_str_wslot[wi].young; sh; sh=sh->next){ str_bytes+=sh->size & SP_STR_SIZE_MASK; str_count++; }
      for(sp_str_hdr*sh=sp_str_wslot[wi].old; sh; sh=sh->next){ str_bytes+=sh->size & SP_STR_SIZE_MASK; str_count++; } } }
#else
  SP_HEAP_LOCK();
  /* Both generations: the split is an internal sweep optimization, so the
     reported footprint has to stay the whole string heap. */
  for(sp_str_hdr*sh=sp_str_heap; sh; sh=sh->next){ str_bytes+=sh->size & SP_STR_SIZE_MASK; str_count++; }
  for(sp_str_hdr*sh=sp_str_old; sh; sh=sh->next){ str_bytes+=sh->size & SP_STR_SIZE_MASK; str_count++; }
  SP_HEAP_UNLOCK();
#endif
  sp_StrIntHash*h=sp_StrIntHash_new();sp_StrIntHash_set(h,SPL("bytes"),(sp_int)SP_GC_CTR_GET(sp_gc_bytes));sp_StrIntHash_set(h,SPL("old_bytes"),(sp_int)sp_gc_old_bytes);sp_StrIntHash_set(h,SPL("threshold"),(sp_int)sp_gc_threshold);sp_StrIntHash_set(h,SPL("cycle"),(sp_int)sp_gc_cycle);sp_StrIntHash_set(h,SPL("full_runs"),(sp_int)sp_gc_full_runs);sp_StrIntHash_set(h,SPL("remembered"),(sp_int)sp_gc_nremembered);sp_StrIntHash_set(h,SPL("remembered_peak"),(sp_int)sp_gc_rem_peak);sp_StrIntHash_set(h,SPL("str_bytes"),(sp_int)str_bytes);sp_StrIntHash_set(h,SPL("str_count"),str_count);return h;}
/* String#setbyte over value-semantics strings: copy-on-write (a literal's
   bytes are static storage). The caller re-binds an lvalue receiver. */
const char *sp_str_setbyte_cow(const char *s, sp_int i, sp_int v) {SP_GC_ROOT_STR(s);
  if (!s) s = "";
  /* an explicitly frozen string (or a frozen-string-literal file's literal,
     both carry the 0xf1 marker) still raises; a HEAP string mutates in
     place so aliases observe the write (CRuby identity semantics); only a
     plain literal -- static storage, marker 0xff -- copies (#2029). */
  sp_str_check_mutable(s);
  /* NUL-safe stored length: strlen would stop at the first NUL byte, making
     every setbyte on a NUL-prefixed buffer (e.g. Array.new(n, 0).pack("C*"))
     raise IndexError. */
  sp_int n = (sp_int)sp_str_byte_len(s);
  if (i < 0) i += n;
  if (i < 0 || i >= n) {
    sp_raise_cls("IndexError", sp_sprintf("index %lld out of string", (long long)i));
    return s;
  }
  {
    unsigned char m = ((const unsigned char *)s)[-1];
    if (m == 0xfe || m == 0xfc) {
      (((sp_str_hdr *)(s - 1)) - 1)->hash = 0;  /* invalidate cached key hash */
      (((sp_str_hdr *)(s - 1)) - 1)->size &= ~SP_STR_SIZE_ASCII7;  /* and the 7-bit answer */
      ((char *)s)[i] = (char)(v & 0xff);
      return s;
    }
    if (m == 0xfd) { ((char *)s)[i] = (char)(v & 0xff); return s; }
  }
  char *r = sp_str_alloc((size_t)n);
  memcpy(r, s, (size_t)n);
  r[n] = 0;
  r[i] = (char)(v & 0xff);
  return r;
}

/* ---- Range#include?/#cover? + Range#to_s -- relocated from spinel_rt.h.
   0 optcarrot uses; reach only sp_range.h's inline core + sp_sprintf
   (resolved at final link against the generated TU). ---- */
#include "sp_range.h"

/* `Range#include?`/`#cover?` on the boxed (SP_TAG_OBJ cls_id
   SP_BUILTIN_RANGE) Range value. The direct sp_Range typed path
   inlines this same check via compile_range_method_expr; poly-recv
   dispatch needs the wrapper so the cls_id arm in
   emit_poly_builtin_dispatch can land on a single C expression. An
   exclusive range stops one short of `last`, so the upper bound is
   `last - excl` (excl is 0 or 1). */
sp_bool sp_range_include(sp_Range *r, sp_int x){SP_GC_ROOT(r);
  /* beginless/endless sentinels (INTPTR_MIN/MAX) clamp one side open */
  if (r->first == INTPTR_MIN || r->last == INTPTR_MAX) {
    if (r->first != INTPTR_MIN && x < r->first) return 0;
    if (r->last != INTPTR_MAX && (r->excl ? x >= r->last : x > r->last)) return 0;
    return 1;
  }
  sp_int lo=sp_range_min_v(*r),hi=sp_range_max_v(*r);
  return sp_range_count(*r)>0 && lo<=x && x<=hi;
}
/* Render a Range for a RangeError message ("-10..1", "1...3", "-10..", "..2"). */
/* Range#inspect: as #to_s, except that a range with NO bound at either end
   names them -- CRuby prints "nil..nil", not ".." (#3670). */
const char *sp_range_inspect(sp_Range r) {
  if (r.first == INTPTR_MIN && r.last == INTPTR_MAX)
    return r.excl ? (&("\xff" "nil...nil")[1]) : (&("\xff" "nil..nil")[1]);
  return sp_range_str(r);
}
const char *sp_range_str(sp_Range r) {
  const char *dots = r.excl ? "..." : "..";
  if (r.first == INTPTR_MIN && r.last == INTPTR_MAX) return dots;
  if (r.first == INTPTR_MIN) return sp_sprintf("%s%lld", dots, (long long)r.last);
  if (r.last == INTPTR_MAX)  return sp_sprintf("%lld%s", (long long)r.first, dots);
  return sp_sprintf("%lld%s%lld", (long long)r.first, dots, (long long)r.last);
}

/* ---- Integer leaf ops (chr/digits/bit_length/bit_range/to_s_base/opt
   variants/pow) -- relocated from spinel_rt.h. All reach only lib-visible
   helpers (sp_str_alloc_raw/sp_str_set_len/sp_int_to_s in sp_alloc.h,
   sp_utf8_encode in sp_str.h, the overflow_p trio now also in sp_alloc.h).
   ---- */

/* Integer#chr: a single byte; CRuby raises RangeError outside 0..255. */
const char*sp_int_chr(sp_int n){
  if(n<0||n>255)sp_raise_cls("RangeError",sp_sprintf("%lld out of char range",(long long)n));
  return sp_plain_char((unsigned char)n);
}
/* Integer#chr(Encoding::UTF_8): encode the codepoint as UTF-8 (1-4 bytes).
   CRuby raises RangeError for a negative/too-large codepoint and for the
   surrogate range, which UTF-8 cannot carry. */
const char*sp_int_chr_utf8(sp_int n){
  if(n<0||n>0x10FFFF)sp_raise_cls("RangeError",sp_sprintf("%lld out of char range",(long long)n));
  if(n>=0xD800&&n<=0xDFFF)sp_raise_cls("RangeError",sp_sprintf("invalid codepoint 0x%llX in UTF-8",(long long)n));
  char*s=sp_str_alloc_raw(5);char*p=s;
  if(n<0x80){*p++=(char)n;}
  else if(n<0x800){*p++=(char)(0xC0|(n>>6));*p++=(char)(0x80|(n&0x3F));}
  else if(n<0x10000){*p++=(char)(0xE0|(n>>12));*p++=(char)(0x80|((n>>6)&0x3F));*p++=(char)(0x80|(n&0x3F));}
  else{*p++=(char)(0xF0|(n>>18));*p++=(char)(0x80|((n>>12)&0x3F));*p++=(char)(0x80|((n>>6)&0x3F));*p++=(char)(0x80|(n&0x3F));}
  *p=0;sp_str_set_len(s,(size_t)(p-s));return s;
}
/* Issue #882: `"hello" << 33` should append the character with
   that codepoint, not the decimal digits. UTF-8 encode (1..4 bytes)
   and return a NUL-terminated string. */
const char *sp_int_codepoint_to_str(sp_int n) {
  /* String#<< / #concat with an out-of-range codepoint raises RangeError,
     matching CRuby ("N out of char range"). */
  if (n < 0 || n > 0x10FFFF) sp_raise_cls("RangeError", sp_sprintf("%lld out of char range", (long long)n));
  char *s = sp_str_alloc_raw(5);
  int len = sp_utf8_encode((uint32_t)n, s);
  s[len] = 0;
  sp_str_set_len(s, (size_t)len);  /* byte_len must be the encoded length, not the alloc */
  return s;
}
/* sp_IntArray lives in sp_array.h (hot core inline) + lib/sp_array.c
   (cold ops). The Integer methods that happen to build an IntArray stay
   here; they call the inline sp_IntArray_new / _push from sp_array.h. */
sp_IntArray*sp_int_digits(sp_int n,sp_int base){if(base<0)sp_raise_cls("ArgumentError","negative radix");if(base<2)sp_raise_cls("ArgumentError",sp_sprintf("invalid radix %lld",(long long)base));if(n<0)sp_raise_cls("Math::DomainError","out of domain");sp_IntArray*a=sp_IntArray_new();if(n==0){sp_IntArray_push(a,0);return a;}while(n>0){sp_IntArray_push(a,n%base);n/=base;}return a;}
/* Integer#bit_length: bits in the two's-complement representation excluding
   the sign bit (a negative n counts the bits of ~n). */
sp_int sp_int_bit_length(sp_int n){unsigned long long x=(n<0)?(unsigned long long)(~n):(unsigned long long)n;sp_int b=0;if(x>=1ULL<<32){b+=32;x>>=32;}if(x>=1ULL<<16){b+=16;x>>=16;}if(x>=1ULL<<8){b+=8;x>>=8;}if(x>=1ULL<<4){b+=4;x>>=4;}if(x>=1ULL<<2){b+=2;x>>=2;}if(x>=1ULL<<1){b+=1;x>>=1;}return b+(sp_int)x;}
sp_int sp_int_bit_range(sp_int n, sp_int start, sp_int len) {
  sp_int shifted;
  if (start >= 0) shifted = (start >= 64) ? (n < 0 ? -1 : 0) : (n >> start);
  else { sp_int s = -start; shifted = (s >= 64) ? 0 : (sp_int)((uint64_t)n << s); }
  uint64_t mask = (len <= 0) ? (len == 0 ? (uint64_t)0 : ~(uint64_t)0)
                             : (len >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << len) - 1));
  return (sp_int)((uint64_t)shifted & mask);
}
/* sp_int_to_s / sp_float_to_s moved to sp_alloc.h (shared so lib/sp_json.c can
   format numbers). String-interpolation of an int slot: a nil sentinel renders
   as the empty string (CRuby interpolates nil as ""), every other value as its
   decimal. */
const char*sp_int_interp(sp_int n){return n==SP_INT_NIL?sp_str_empty:sp_int_to_s(n);}
const char*sp_int_to_s_base(sp_int n,sp_int base){if(base<2||base>36)sp_raise_cls("ArgumentError",sp_sprintf("invalid radix %lld",(long long)base));char*b=sp_str_alloc_raw(72);char tmp[72];int i=0;int neg=0;uint64_t u;if(n<0){neg=1;u=(uint64_t)(-(n+1))+1;}
else{u=(uint64_t)n;}if(u==0){tmp[i++]='0';}
else{while(u>0){sp_int d=u%base;tmp[i++]=d<10?'0'+d:'a'+d-10;u/=base;}}int j=0;if(neg)b[j++]='-';while(i>0)b[j++]=tmp[--i];b[j]=0;sp_str_set_len(b,(size_t)j);return b;}
/* Inspect / to_s for an int? value. CRuby distinguishes the two on
   nil: `nil.to_s` is "" while `nil.inspect` is "nil". For a real
   integer they agree (Integer#to_s and #inspect are both the decimal
   form). Two wrappers keep call-site emit local. */
const char *sp_int_opt_inspect(sp_int v) { return sp_int_is_nil(v) ? "nil" : sp_int_to_s(v); }
const char *sp_int_opt_to_s(sp_int v)    { return sp_int_is_nil(v) ? "" : sp_int_to_s(v); }
sp_int sp_int_pow(sp_int base, sp_int exp) {
  if (exp < 0) sp_raise_cls("RangeError", "negative exponent");
  /* Exact square-and-multiply (the old pow(double) round-trip lost precision
     above 2^53 and saturated on overflow). Overflow follows the +/-/* mode:
     raise by default, wrap under SP_INT_OVERFLOW_MODE_WRAP. */
  sp_int r = 1, b = base;
  while (exp > 0) {
#ifdef SP_INT_OVERFLOW_MODE_WRAP
    if (exp & 1) r = (sp_int)((uintptr_t)r * (uintptr_t)b);
    exp >>= 1;
    if (exp) b = (sp_int)((uintptr_t)b * (uintptr_t)b);
#else
    if (exp & 1) { if (sp_int_mul_overflow_p(r, b, &r)) sp_raise_cls("RangeError", "integer overflow in **"); }
    exp >>= 1;
    if (exp) { if (sp_int_mul_overflow_p(b, b, &b)) sp_raise_cls("RangeError", "integer overflow in **"); }
#endif
  }
  return r;
}

/* ---- ARGV cache + ARGF cold ops -- relocated from spinel_rt.h. sp_argv /
   sp_argf_obj are extern (sp_argf.h), defined by the generated main(). ---- */
#include "sp_argf.h"

sp_StrArray *sp_argv_array_cache = NULL;
sp_StrArray *sp_get_ARGV(void) {
  if (!sp_argv_array_cache) {
    sp_argv_array_cache = sp_StrArray_new();
    for (sp_int i = 0; i < sp_argv.len; i++) sp_StrArray_push(sp_argv_array_cache, sp_argv.data[i]);
  }
  return sp_argv_array_cache;
}
/* Ensure a current readable stream, or return 0 at total end of input. */
int sp_argf_ensure(void) {
  if (sp_argf_obj.cur) return 1;
  if (sp_argv.len == 0) {
    if (sp_argf_obj.started) return 0;
    sp_argf_obj.started = 1; sp_argf_obj.cur = stdin; sp_argf_obj.fname = "-"; return 1;
  }
  while (sp_argf_obj.idx < sp_argv.len) {
    const char *fn = sp_argv.data[sp_argf_obj.idx++];
    sp_argf_obj.started = 1;
    if (fn && fn[0] == '-' && fn[1] == 0) { sp_argf_obj.cur = stdin; sp_argf_obj.fname = "-"; return 1; }
    FILE *f = fn ? fopen(fn, "r") : NULL;
    if (f) { sp_argf_obj.cur = f; sp_argf_obj.fname = fn; return 1; }
  }
  return 0;
}
const char *sp_argf_gets(void) {
  for (;;) {
    if (!sp_argf_ensure()) return NULL;
    char buf[8192];
    if (fgets(buf, sizeof buf, sp_argf_obj.cur)) {
      size_t l = strlen(buf); char *r = sp_str_alloc_raw(l + 1); memcpy(r, buf, l + 1); return r;
    }
    if (sp_argf_obj.cur && sp_argf_obj.cur != stdin) fclose(sp_argf_obj.cur);
    sp_argf_obj.cur = NULL;  /* EOF on this stream; advance on next ensure */
  }
}
const char *sp_argf_read(void) {
  sp_String *s = sp_String_new(""); SP_GC_ROOT(s);
  const char *line;
  while ((line = sp_argf_gets())) sp_String_append(s, line);
  return sp_str_dup(s->data);
}
sp_StrArray *sp_argf_readlines(void) {
  sp_StrArray *a = sp_StrArray_new(); SP_GC_ROOT(a);
  const char *line;
  while ((line = sp_argf_gets())) sp_StrArray_push(a, line);
  return a;
}
const char *sp_argf_filename(void) {
  if (sp_argf_obj.fname) return sp_argf_obj.fname;
  return sp_argv.len > 0 ? sp_argv.data[0] : "-";
}
sp_bool sp_argf_eof(void) { return !sp_argf_ensure(); }

/* ---- Float/String Range value-type ops -- relocated from spinel_rt.h.
   0 optcarrot uses; reach only sp_range.h + lib-visible sp_float_to_s/
   sp_str_inspect/sp_str_eq/sp_StrArray_from_string_range/sp_sprintf. ---- */

/* Float range (1.0..3.0). Endpoints stay sp_float, so cover?/include?/begin/end
   are exact. -HUGE_VAL / +HUGE_VAL are the beginless / endless sentinels. */
sp_FloatRange sp_frange_new(sp_float f, sp_float l, sp_int e) {
  sp_FloatRange r; r.first = f; r.last = l; r.excl = e; r.omitted = 0; return r;
}
/* Same, recording which bound was written as absent rather than infinite. */
sp_FloatRange sp_frange_new_o(sp_float f, sp_float l, sp_int e, sp_int om) {
  sp_FloatRange r; r.first = f; r.last = l; r.excl = e; r.omitted = om; return r;
}
sp_bool sp_frange_cover(sp_FloatRange r, sp_float x) {
  if (r.first != -HUGE_VAL && x < r.first) return 0;
  if (r.last != HUGE_VAL && (r.excl ? x >= r.last : x > r.last)) return 0;
  return 1;
}
sp_bool sp_frange_eq(sp_FloatRange a, sp_FloatRange b) {
  return a.first == b.first && a.last == b.last && a.excl == b.excl;
}
const char *sp_frange_inspect(sp_FloatRange r) {
  /* an OMITTED bound prints as nothing; an explicit infinity prints itself */
  const char *lo = (r.omitted & SP_FRANGE_NO_BEGIN) ? ""
                 : (r.omitted & SP_FRANGE_INT_BEGIN) ? sp_sprintf("%lld", (long long)r.first)
                 : sp_float_to_s(r.first);
  const char *hi = (r.omitted & SP_FRANGE_NO_END) ? ""
                 : (r.omitted & SP_FRANGE_INT_END) ? sp_sprintf("%lld", (long long)r.last)
                 : sp_float_to_s(r.last);
  return sp_sprintf("%s%s%s", lo, r.excl ? "..." : "..", hi);
}
sp_RbVal sp_box_frange(sp_FloatRange v) {
  sp_FloatRange *p = (sp_FloatRange *)sp_gc_alloc(sizeof(sp_FloatRange), NULL, NULL);
  *p = v;
  return sp_box_obj(p, SP_BUILTIN_FLOAT_RANGE);
}
/* Float#max: the exclusive form has no greatest element (Ruby raises). */
sp_float sp_frange_max(sp_FloatRange r) {
  if (r.excl) sp_raise_cls("TypeError", "cannot exclude end value with non Integer begin value");
  return r.last;
}
/* String range ("a".."e"). The endpoints are the value; every traversal
   materializes the element array, which is how a string range behaved before
   it became a value of its own (#3064). */
sp_StrRange sp_srange_new(const char *f, const char *l, sp_int e) {
  sp_StrRange r; r.first = f; r.last = l; r.excl = e; return r;
}
sp_StrArray *sp_srange_to_a(sp_StrRange r) {
  return sp_StrArray_from_string_range(r.first ? r.first : sp_str_empty,
                                       r.last ? r.last : sp_str_empty, r.excl);
}
sp_bool sp_srange_eq(sp_StrRange a, sp_StrRange b) {
  return a.excl == b.excl && sp_str_eq(a.first ? a.first : sp_str_empty, b.first ? b.first : sp_str_empty) &&
         sp_str_eq(a.last ? a.last : sp_str_empty, b.last ? b.last : sp_str_empty);
}
/* #cover? / #=== compare lexicographically, no materialization. */
sp_bool sp_srange_cover(sp_StrRange r, const char *x) {
  if (!x) return 0;
  if (r.first && strcmp(x, r.first) < 0) return 0;
  if (r.last) { int d = strcmp(x, r.last); if (r.excl ? d >= 0 : d > 0) return 0; }
  return 1;
}
const char *sp_srange_to_s(sp_StrRange r) {
  return sp_sprintf("%s%s%s", r.first ? r.first : sp_str_empty,
                    r.excl ? "..." : "..", r.last ? r.last : sp_str_empty);
}
const char *sp_srange_inspect(sp_StrRange r) {
  const char *lo = sp_str_inspect(r.first ? r.first : sp_str_empty);
  const char *hi = sp_str_inspect(r.last ? r.last : sp_str_empty);
  return sp_sprintf("%s%s%s", lo, r.excl ? "..." : "..", hi);
}
sp_RbVal sp_box_srange(sp_StrRange v) {
  sp_StrRange *p = (sp_StrRange *)sp_gc_alloc(sizeof(sp_StrRange), NULL, NULL);
  *p = v;
  return sp_box_obj(p, SP_BUILTIN_STR_RANGE);
}

/* ---- Float leaf ops (opt_inspect/opt_to_s/denominator/numerator/
   to_i_checked) -- relocated from spinel_rt.h. 0 optcarrot uses; reach
   only lib-visible sp_float_is_nil (sp_types.h)/sp_float_to_s (sp_alloc.h)/
   sp_float_to_rational (sp_format.h)/sp_raise_cls/sp_sprintf. ---- */

/* float? (nullable float) counterparts: a non-nil value formats exactly
   like a plain Float (delegates to sp_float_to_s), nil renders "nil"
   (inspect) / "" (to_s). */
const char *sp_float_opt_inspect(sp_float v) { return sp_float_is_nil(v) ? "nil" : sp_float_to_s(v); }
const char *sp_float_opt_to_s(sp_float v)    { return sp_float_is_nil(v) ? "" : sp_float_to_s(v); }
/* Float#numerator / #denominator. A non-finite Float has no rational form, so
   CRuby answers the value itself and 1 rather than converting (#3011). */
sp_int sp_float_denominator(sp_float f) {
  if (isnan(f) || isinf(f)) return 1;
  return sp_float_to_rational(f).den;
}
sp_RbVal sp_float_numerator(sp_float f) {
  if (isnan(f) || isinf(f)) return sp_box_float(f);
  return sp_box_int(sp_float_to_rational(f).num);
}
/* Float#to_i whose integer value escapes int64: CRuby promotes to Bignum;
   until the promotion plan covers statically-int results (#2024), raise
   loudly instead of saturating silently. NaN/Inf raise FloatDomainError. */
sp_int sp_float_to_i_checked(sp_float f) {
  if (isnan(f) || isinf(f)) sp_raise_cls("FloatDomainError", sp_sprintf("%g", f));
  if (f >= 9223372036854775808.0 || f < -9223372036854775808.0)
    sp_raise_cls("RangeError", "float out of Integer range (Bignum promotion pending)");
  return (sp_int)f;
}

/* ---- Box helpers (0 optcarrot uses) -- relocated from spinel_rt.h. ---- */

/* An element read back out of a TYPED array boxes at the runtime read, which
   has no room for the sentinel check the hot path cannot afford. Where analyze
   knows a particular receiver's elements can hold one, it wraps that read in
   this: the correction is paid at the marked site only (#3505). */
sp_RbVal sp_unsentinel(sp_RbVal v) {SP_GC_ROOT_RBVAL(v);
  if (v.tag == SP_TAG_INT && v.v.i == SP_INT_NIL) return sp_box_nil();
  if (v.tag == SP_TAG_FLT && sp_float_is_nil(v.v.f)) return sp_box_nil();
  return v;
}
/* box a sp_Bigint* into a poly slot (heterogeneous container element, or a
   promote-mode overflow result). */
sp_RbVal sp_box_bigint(sp_Bigint *b) { sp_RbVal r; r.tag = SP_TAG_BIGINT; r.cls_id = 0; r.v.p = b; return r; }
sp_RbVal sp_box_encoding(sp_Encoding e) { sp_RbVal r; r.tag = SP_TAG_ENCODING; r.cls_id = 0; r.v.s = sp_encoding_name(e); return r; }
sp_RbVal sp_box_nullable_str(const char *v) { return v ? sp_box_str(v) : sp_box_nil(); }
/* An opaque foreign/FFI pointer: boxed with SP_BUILTIN_FOREIGN_PTR so the
   collector skips it (it is not a sp_gc_alloc allocation). NULL -> nil. */
sp_RbVal sp_box_foreign_ptr(void *p) { return p ? sp_box_obj(p, SP_BUILTIN_FOREIGN_PTR) : sp_box_nil(); }
/* a compiled Regexp value (mrb_regexp_pattern *): untraced, program-lifetime */
sp_RbVal sp_box_regexp(void *p) { return p ? sp_box_obj(p, SP_BUILTIN_REGEX) : sp_box_nil(); }
sp_RbVal sp_box_sym_array(void *p)   { return sp_box_obj(p, SP_BUILTIN_SYM_ARRAY); }
sp_RbVal sp_box_ptr_array(void *p)   { return sp_box_obj(p, SP_BUILTIN_PTR_ARRAY); }
sp_RbVal sp_box_method(void *p)      { return sp_box_obj(p, SP_BUILTIN_METHOD); }
/* Complex / Rational are wider value types (two components); like sp_Range they
   heap-copy when crossing into a poly slot. No internal pointers, so no scan. */
sp_RbVal sp_box_complex(sp_Complex v) {
  sp_Complex *p = (sp_Complex *)sp_gc_alloc(sizeof(sp_Complex), NULL, NULL);
  *p = v;
  return sp_box_obj(p, SP_BUILTIN_COMPLEX);
}
sp_RbVal sp_box_rational(sp_Rational v) {
  sp_Rational *p = (sp_Rational *)sp_gc_alloc(sizeof(sp_Rational), NULL, NULL);
  *p = v;
  return sp_box_obj(p, SP_BUILTIN_RATIONAL);
}
/* Same heap-box rationale as sp_Range: sp_Time is 12+ bytes (tv_sec +
   tv_nsec), wider than sp_RbVal's 8-byte union. No internal pointers
   so no scanner is needed. */
sp_RbVal sp_box_time(sp_Time v) {
  sp_Time *p = (sp_Time *)sp_gc_alloc(sizeof(sp_Time), NULL, NULL);
  *p = v;
  return sp_box_obj(p, SP_BUILTIN_TIME);
}
/* Boxing for the unboxed sp_Tms value (a rescue expression merges it into a
   nullable slot, #3132): heap-copy like sp_box_frange. */
sp_RbVal sp_box_tms(sp_Tms v) {
  sp_Tms *p = (sp_Tms *)sp_gc_alloc(sizeof(sp_Tms), NULL, NULL);
  *p = v;
  return sp_box_obj(p, SP_BUILTIN_TMS);
}
sp_RbVal sp_box_openstruct(sp_OpenStruct *o){ return sp_box_obj(o, SP_BUILTIN_OPENSTRUCT); }

/* ---- More cold String/StrArray ops -- relocated from spinel_rt.h. ---- */
#include "sp_re.h"   /* mrb_regexp_pattern/re_exec for sp_str_re_match_p_at */

/* respond_to? on a poly value that turns out to hold a BUILTIN. The compile
   time fold emits a cls_id check against the user classes defining the name,
   which necessarily answers false for a builtin member of the union -- yet
   Array really does respond to :each. Answer the core builtin surface here,
   keyed off the runtime class name so every Array variant shares one list.
   Deliberately conservative: an unlisted name answers false rather than
   guessing, which is also what an undispatchable method would do. */
sp_bool sp_str_in_list(const char *m, const char *const *list) {
  for (int i = 0; list[i]; i++) if (strcmp(m, list[i]) == 0) return 1;
  return 0;
}
/* String#index / #rindex return a boxed nil for not-found, boxed
   int for found. Issue #532: typed-int slot can't represent CRuby's
   nil-vs-real-index distinction in-band; widening the result type
   to sp_RbVal at the call site lets `pos.nil?` and `puts pos.inspect`
   work via the standard poly-tag dispatch. The -1 sentinel comes
   from the underlying sp_str_*_index helpers; we widen here at the
   boxing layer so existing call sites that want the raw int still
   work via `sp_str_index` directly. */
sp_RbVal sp_str_index_poly(const char *s, const char *sub) {SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(sub); sp_int n = sp_str_index(s, sub); return n < 0 ? sp_box_nil() : sp_box_int(n); }
sp_RbVal sp_str_index_from_poly(const char *s, const char *sub, sp_int start) {SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(sub); sp_int n = sp_str_index_from(s, sub, start); return n < 0 ? sp_box_nil() : sp_box_int(n); }
sp_RbVal sp_str_rindex_poly(const char *s, const char *sub) {SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(sub); sp_int n = sp_str_rindex(s, sub); return n < 0 ? sp_box_nil() : sp_box_int(n); }
sp_PolyArray *sp_str_lines_poly(const char *s) {SP_GC_ROOT_STR(s);
  sp_StrArray *ls = sp_str_lines(s); SP_GC_ROOT(ls);
  sp_PolyArray *a = sp_PolyArray_new(); SP_GC_ROOT(a);
  if (ls) {
    sp_int len = sp_StrArray_length(ls);
    for (sp_int i = 0; i < len; i++) {
      sp_PolyArray_push(a, sp_box_str(sp_StrArray_get(ls, i)));
    }
  }
  return a;
}
/* String#match?(/re/, pos) — pos is a codepoint index (CRuby semantics),
   unlike Regexp#match?(str, pos) which uses byte offset. Convert the
   codepoint index to a byte offset before dispatching to re_exec. */
sp_bool sp_str_re_match_p_at(mrb_regexp_pattern *pat, const char *str, sp_int cpos) {SP_GC_ROOT_STR(str);
  sp_int cl = sp_str_length(str);
  if (cpos < 0) cpos += cl;
  if (cpos < 0 || cpos > cl) return FALSE;
  size_t boff = sp_utf8_byte_offset(str, cpos);
  int64_t slen = (int64_t)strlen(str);
  int caps[2];
  return re_exec(pat, str, slen, (sp_int)boff, caps, 2, 0) > 0;
}
/* Issue #910: sub(string, hash) — literal-substring pattern
   with a hash replacement. Replaces only the first match. */
const char *sp_str_sub_str_str_hash(const char *str, const char *pat, sp_StrStrHash *h) {SP_GC_ROOT_STR(pat);SP_GC_ROOT(h);SP_GC_ROOT_STR(str);
  if (!str || !pat) return str;
  const char *found = strstr(str, pat);
  if (!found) return str;
  size_t before = (size_t)(found - str);
  size_t plen = strlen(pat);
  const char *rep = (h && sp_StrStrHash_has_key(h, pat)) ? sp_StrStrHash_get(h, pat) : "";
  size_t rlen = strlen(rep);
  size_t rest = strlen(str) - before - plen;
  size_t total = before + rlen + rest;
  char *out = sp_str_alloc_raw(total + 1);
  memcpy(out, str, before);
  memcpy(out + before, rep, rlen);
  memcpy(out + before + rlen, found + plen, rest);
  out[total] = 0;
  return out;
}
/* Array#sum with a String initial value: concatenation fold ("abc" from
   ["a","b","c"].sum("")), CRuby's + on each element. */
const char *sp_StrArray_sum_str(sp_StrArray *a, const char *init) {SP_GC_ROOT(a);SP_GC_ROOT_STR(init);
  size_t n = init ? strlen(init) : 0;
  if (a) for (sp_int i = 0; i < a->len; i++) if (a->data[i]) n += strlen(a->data[i]);
  char *r = sp_str_alloc(n); size_t o = 0;
  if (init) { memcpy(r, init, strlen(init)); o = strlen(init); }
  if (a) for (sp_int i = 0; i < a->len; i++) if (a->data[i]) { size_t l = strlen(a->data[i]); memcpy(r + o, a->data[i], l); o += l; }
  r[o] = 0; sp_str_set_len(r, o); return r;
}
sp_RbVal sp_StrArray_uniq_bangq(sp_StrArray *a) {SP_GC_ROOT(a);
  if (!a) return sp_box_nil();
  sp_int n = a->len;
  sp_StrArray_uniq_bang(a);
  return a->len != n ? sp_box_str_array(a) : sp_box_nil();
}
/* Array#join for float arrays -- each element via the Ruby-faithful
   sp_float_to_s ("1.0", not "1"). Mirrors sp_IntArray_join exactly: build in a
   malloc buffer, return an sp_str_alloc'd copy. (Not sp_String#data, whose owner
   isn't GC-rooted across the return.) sp_float_to_s's result is copied
   immediately, before the next call can reuse its buffer. */
sp_bool sp_StrArray_eq(sp_StrArray*a,sp_StrArray*b){SP_GC_ROOT(a);SP_GC_ROOT(b);if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(sp_int i=0;i<a->len;i++)if(!sp_str_eq(a->data[i],b->data[i]))return FALSE;return TRUE;}

/* ---- Complex ops / class-frozen bitmap -- relocated from spinel_rt.h. ---- */

/* An Integer-classed (fl bit clear) whole component boxes as an Integer;
   anything else keeps the Float class. The INTPTR guard mirrors
   sp_complex_mag: casting an out-of-range double to sp_int is UB. */
sp_RbVal sp_complex_comp_v(sp_float v, int is_f) {
  if (!is_f && v >= -(sp_float)INTPTR_MAX && v <= (sp_float)INTPTR_MAX && v == (sp_float)(sp_int)v)
    return sp_box_int((sp_int)v);
  return sp_box_float(v);
}
/* CRuby Complex#abs: Integer only via the zero-component shortcut (|other|)
   on an all-Integer complex; hypot is always a Float. #abs2 is Integer iff
   both components are Integer-classed. */
sp_RbVal sp_complex_abs_v(sp_Complex a) {
  if (a.fl == 0 && a.im == 0) return sp_complex_comp_v(a.re < 0 ? -a.re : a.re, 0);
  if (a.fl == 0 && a.re == 0) return sp_complex_comp_v(a.im < 0 ? -a.im : a.im, 0);
  return sp_box_float(sp_complex_abs(a));
}
sp_RbVal sp_complex_abs2_v(sp_Complex a) {
  sp_float v = sp_complex_abs2(a);
  if (a.fl == 0) return sp_complex_comp_v(v, 0);
  return sp_box_float(v);
}
unsigned char sp_class_frozen_map[4096];
void sp_class_freeze_id(sp_int cls_id) {
  sp_int ix = cls_id >= 0 ? cls_id : (3900 - cls_id);
  if (ix >= 0 && ix < 4096) sp_class_frozen_map[ix] = 1;
}
sp_bool sp_class_frozen_id(sp_int cls_id) {
  sp_int ix = cls_id >= 0 ? cls_id : (3900 - cls_id);
  return (ix >= 0 && ix < 4096) ? (sp_bool)sp_class_frozen_map[ix] : 0;
}

/* ---- Round helpers / typed-array frozen-check / IO.pipe / sysopen --
   relocated from spinel_rt.h. ---- */

/* Float#round(half:) tie-breaking: :even is banker's rounding (rint under
   the default FE_TONEAREST), :down rounds ties toward zero. (:up is the
   plain round().) */
double sp_round_half_even(double x) { return rint(x); }
double sp_round_half_down(double x) { return x >= 0 ? ceil(x - 0.5) : floor(x + 0.5); }
/* Frozen flag of a builtin array, matching what sp_*Array_splice check (the
   struct field, which the promote path would otherwise bypass by building a new
   array, and which lets us check frozen up front before any GC root is live). */
int sp_typed_arr_frozen(sp_RbVal v) {
  switch (v.cls_id) {
    case SP_BUILTIN_INT_ARRAY: return ((sp_IntArray *)v.v.p)->frozen;
    case SP_BUILTIN_FLT_ARRAY: return ((sp_FloatArray *)v.v.p)->frozen;
    case SP_BUILTIN_STR_ARRAY: return ((sp_StrArray *)v.v.p)->frozen;
    case SP_BUILTIN_POLY_ARRAY: return ((sp_PolyArray *)v.v.p)->frozen;
    default: return 0;
  }
}
/* IO.pipe -> [reader, writer] handles (#2815) */
sp_PolyArray *sp_io_pipe(void) {
  int fds[2];
  if (sp_io_make_pipe(fds) != 0) sp_raise_cls("IOError", "pipe failed");
  sp_PolyArray *a = sp_PolyArray_new();
  SP_GC_ROOT(a);
  sp_PolyArray_push(a, sp_box_obj(sp_io_fdopen(fds[0], "r"), SP_BUILTIN_IO));
  sp_PolyArray_push(a, sp_box_obj(sp_io_fdopen(fds[1], "w"), SP_BUILTIN_IO));
  return a;
}
/* IO.for_fd(fd, mode): wrap a descriptor the program already owns. autoclose
   false leaves the fd open when the handle is collected, which is the point of
   the call -- the fd usually belongs to something else. */
sp_File *sp_io_for_fd(sp_int fd, const char *mode, sp_bool autoclose) {SP_GC_ROOT_STR(mode);
  if (fd < 0 || fcntl((int)fd, F_GETFD) < 0)
    sp_raise_cls("Errno::EBADF", "Bad file descriptor");
  /* When the user did not pass a mode (mode is NULL or ""), CRuby picks a
     mode that matches the fd's actual open flags via a "first fdopen
     that does not fail" fallback. Mirror that: probe the fd's flags and
     pass the closest fopen mode, so wrapping a write-only fd works
     without forcing the caller to spell the mode. */
  const char *m;
  if (mode && *mode) {
    m = mode;
  } else {
    int fl = fcntl((int)fd, F_GETFL);
    if (fl < 0) m = "r";
    else if ((fl & O_ACCMODE) == O_WRONLY) m = "w";
    else if ((fl & O_ACCMODE) == O_RDWR)   m = "r+";
    else                                   m = "r";
  }
  sp_File *f = sp_io_fdopen_ex((int)fd, m, 0);
  if (f && !autoclose) f->no_autoclose = 1;
  return f;
}

/* The readiness family and IO.select both run on select(2), which is what
   CRuby's IO.select is: its exceptfds set is the only portable spelling of
   "priority". poll(2)'s POLLPRI is not -- macOS raises it for ordinary readable
   data on a pipe, so a pipe carrying "x" reported itself priority-readable.
   `kind` is 0 read / 1 write / 2 priority / 3 read|write, kept numeric so the
   generated TU does not have to pull in the flag headers. A negative timeout
   blocks. Each answers the handle itself when ready and nil on timeout. */
static void sp_io_sel_timeout(double timeout, struct timeval *tv, struct timeval **tvp) {
  if (timeout < 0) { *tvp = NULL; return; }
  tv->tv_sec = (time_t)timeout;
  tv->tv_usec = (suseconds_t)((timeout - (double)tv->tv_sec) * 1000000.0);
  *tvp = tv;
}
sp_File *sp_io_wait_events(sp_File *f, double timeout, sp_int kind) {SP_GC_ROOT(f);
  if (!f || !f->fp) sp_raise_cls("IOError", "closed stream");
  int fd = fileno(f->fp);
  if (fd < 0 || fd >= FD_SETSIZE) sp_raise_cls("IOError", "file descriptor out of range");
  fd_set rs, ws, es;
  FD_ZERO(&rs); FD_ZERO(&ws); FD_ZERO(&es);
  if (kind == 0 || kind == 3) FD_SET(fd, &rs);
  if (kind == 1 || kind == 3) FD_SET(fd, &ws);
  if (kind == 2) FD_SET(fd, &es);
  struct timeval tv, *tvp;
  sp_io_sel_timeout(timeout, &tv, &tvp);
  int n;
  do { n = select(fd + 1, &rs, &ws, &es, tvp); } while (n < 0 && errno == EINTR);
  if (n < 0) sp_raise_cls("IOError", "select failed");
  return n > 0 ? f : NULL;
}

/* IO.select(read, write, error, timeout) -> [ready_read, ready_write,
   ready_error], or nil when the timeout expires first. A nil array stands for
   "watch nothing", so all three nil is just a sleep. */
sp_File *(*sp_user_to_io_hook)(sp_RbVal) = NULL;
static sp_File *sp_select_io_of(sp_RbVal v) {
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_IO && v.v.p) return (sp_File *)v.v.p;
  /* A user object that answers #to_io names the handle to wait on -- CRuby
     waits on wrappers this way, and a TLS socket is one. */
  if (v.tag == SP_TAG_OBJ && v.cls_id >= 0 && sp_user_to_io_hook) {
    sp_File *f = sp_user_to_io_hook(v);
    if (f) return f;
  }
  sp_raise_cls("TypeError",
               sp_sprintf("no implicit conversion of %s into IO",
                          (v.tag == SP_TAG_OBJ && v.cls_id >= 0 && sp_obj_cls_name_fn)
                            ? sp_obj_cls_name_fn(v.cls_id) : "nil"));
  return NULL;
}
sp_RbVal sp_io_select(sp_PolyArray *rd, sp_PolyArray *wr, sp_PolyArray *er, double timeout) {
  sp_PolyArray *src[3] = { rd, wr, er };
  fd_set sets[3];
  int maxfd = -1;
  for (int g = 0; g < 3; g++) {
    FD_ZERO(&sets[g]);
    sp_int n = src[g] ? src[g]->len : 0;
    for (sp_int i = 0; i < n; i++) {
      sp_File *f = sp_select_io_of(src[g]->data[i]);
      int fd = (f && f->fp) ? fileno(f->fp) : -1;
      if (fd < 0 || fd >= FD_SETSIZE) sp_raise_cls("IOError", "file descriptor out of range");
      FD_SET(fd, &sets[g]);
      if (fd > maxfd) maxfd = fd;
    }
  }
  struct timeval tv, *tvp;
  sp_io_sel_timeout(timeout, &tv, &tvp);
  int n;
  do { n = select(maxfd + 1, &sets[0], &sets[1], &sets[2], tvp); } while (n < 0 && errno == EINTR);
  if (n < 0) sp_raise_cls("IOError", "select failed");
  if (n == 0) return sp_box_nil();
  sp_PolyArray *out = sp_PolyArray_new();
  SP_GC_ROOT(out);
  for (int g = 0; g < 3; g++) {
    sp_PolyArray *part = sp_PolyArray_new();
    sp_int cnt = src[g] ? src[g]->len : 0;
    for (sp_int i = 0; i < cnt; i++) {
      sp_File *f = sp_select_io_of(src[g]->data[i]);
      if (f && f->fp && FD_ISSET(fileno(f->fp), &sets[g]))
        sp_PolyArray_push(part, src[g]->data[i]);
    }
    sp_PolyArray_push(out, sp_box_poly_array(part));
  }
  return sp_box_poly_array(out);
}

sp_int sp_io_sysopen(const char *path, sp_int flags, sp_int perm) {SP_GC_ROOT_STR(path);
  /* flags are CRuby's File::Constants (O_* on this platform); 0 is O_RDONLY.
     They were dropped at codegen and every sysopen was O_RDONLY -- a FIFO
     opened for writing hung waiting for a writer of its own (#4206). */
  int fd = open(path ? path : "", (int)flags, (mode_t)(perm ? perm : 0666));
  if (fd < 0) {
    const char *cls = errno == ENOENT ? "Errno::ENOENT"
                    : errno == ENXIO  ? "Errno::ENXIO"
                    : errno == EACCES ? "Errno::EACCES"
                    : errno == EEXIST ? "Errno::EEXIST"
                    : errno == EINVAL ? "Errno::EINVAL" : "SystemCallError";
    sp_raise_cls(cls, sp_sprintf("%s @ rb_sysopen - %s", strerror(errno), path ? path : ""));
  }
  return (sp_int)fd;
}

/* ---- Kernel#sleep -- relocated from spinel_rt.h. 0 optcarrot uses;
   sp_sched_sleep already lib-visible (sp_sched.h, included transitively
   via sp_io.h/sp_time.h -> check explicitly below). ---- */
#include "sp_sched.h"

/* Kernel#sleep with sub-second precision. Argument is seconds as a
   double so `sleep(0.5)` actually waits 500ms; the legacy `sleep((unsigned)0.5)`
   cast truncated to 0 and returned immediately. POSIX uses
   nanosleep(); Windows uses Sleep() (milliseconds). Negative or NaN
   inputs no-op. */
void sp_sleep(sp_float s) {
  if (!(s > 0.0)) return;
#ifdef SP_THREADS
  /* Scheduler-aware: park the green thread and free its OS worker for others; a
     monitor thread wakes it after the duration (see lib/sp_sched.c). */
  sp_sched_sleep((double)s);
#else
  struct timespec req;
  req.tv_sec = (time_t)s;
  req.tv_nsec = (long)((s - (double)req.tv_sec) * 1e9);
  if (req.tv_nsec < 0) req.tv_nsec = 0;
  if (req.tv_nsec >= 1000000000L) req.tv_nsec = 999999999L;
  while (nanosleep(&req, &req) == -1 && errno == EINTR) {}
#endif
}

/* ---- BigRational box/scan/format ops -- relocated from spinel_rt.h.
   0 optcarrot uses. ---- */
#include "sp_str.h"   /* sp_str_concat for brat_to_s/inspect */

void sp_brat_scan(void *p) {
  sp_BigRational *r = (sp_BigRational *)p;
  if (r->num) sp_gc_mark(r->num);
  if (r->den) sp_gc_mark(r->den);
}
/* Construct a reduced big Rational: normalize the sign onto the numerator and
   divide out the gcd. den must be non-zero (callers pass a literal or a checked
   value). */
sp_RbVal sp_box_brat(sp_Bigint *num, sp_Bigint *den) {
  if (sp_bigint_sign(den) < 0) { num = sp_bigint_sub(sp_bigint_new_int(0), num); den = sp_bigint_sub(sp_bigint_new_int(0), den); }
  sp_Bigint *g = sp_bigint_gcd(num, den);
  if (sp_bigint_sign(g) != 0) { num = sp_bigint_div(num, g); den = sp_bigint_div(den, g); }
  sp_BigRational *p = (sp_BigRational *)sp_gc_alloc(sizeof(sp_BigRational), NULL, sp_brat_scan);
  p->num = num; p->den = den;
  return sp_box_obj(p, SP_BUILTIN_BIG_RATIONAL);
}
/* Lift a bignum (or an int) to a big Rational num/1. */
sp_RbVal sp_brat_from_bigint(sp_Bigint *n) { return sp_box_brat(n, sp_bigint_new_int(1)); }
void sp_addrinfo_scan(void *p) {
  sp_Addrinfo *a = (sp_Addrinfo *)p;
  if (a->ip) sp_mark_string((void *)a->ip);
  if (a->afname) sp_mark_string((void *)a->afname);
}
sp_RbVal sp_box_addrinfo(sp_Addrinfo *v) { return sp_box_obj(v, SP_BUILTIN_ADDRINFO); }
sp_RbVal sp_box_sockopt(sp_SockOpt *v) { return sp_box_obj(v, SP_BUILTIN_SOCKOPT); }
sp_SockOpt *sp_sockopt_new(sp_int family, sp_int level, sp_int optname, sp_int value) {
  sp_SockOpt *o = (sp_SockOpt *)sp_gc_alloc(sizeof(sp_SockOpt), NULL, NULL);
  o->family = family; o->level = level; o->optname = optname; o->value = value;
  return o;
}
const char *sp_sockopt_inspect(sp_SockOpt *o) {SP_GC_ROOT(o);
  if (!o) return sp_sprintf("nil");
  return sp_sprintf("#<Socket::Option: INET %lld %lld %lld>",
                    (long long)o->level, (long long)o->optname, (long long)o->value);
}
/* Addrinfo.tcp / .udp / .ip / .unix, and the endpoint behind #local_address /
   #remote_address. `stype` is SOCK_STREAM / SOCK_DGRAM, 0 for a bare address. */
sp_Addrinfo *sp_addrinfo_new(const char *ip, sp_int port, sp_int stype, sp_int is_unix) {SP_GC_ROOT_STR(ip);
  sp_Addrinfo *a = (sp_Addrinfo *)sp_gc_alloc(sizeof(sp_Addrinfo), NULL, sp_addrinfo_scan);
  a->ip = NULL; a->afname = NULL;
  SP_GC_ROOT(a);
  a->ip = sp_sprintf("%s", ip ? ip : "");
  int v6 = !is_unix && ip && strchr(ip, ':') != NULL;
  a->afname = sp_sprintf("%s", is_unix ? "AF_UNIX" : v6 ? "AF_INET6" : "AF_INET");
  a->afamily = is_unix ? AF_UNIX : v6 ? AF_INET6 : AF_INET;
  a->port = port;
  a->socktype = stype;
  a->protocol = 0;
  return a;
}
const char *sp_addrinfo_inspect(sp_Addrinfo *a) {SP_GC_ROOT(a);
  if (!a) return sp_sprintf("nil");
  if (strcmp(a->afname, "AF_UNIX") == 0) return sp_sprintf("#<Addrinfo: %s SOCK_STREAM>", a->ip);
  const char *st = a->socktype == SOCK_DGRAM ? " UDP"
                 : a->socktype == SOCK_STREAM ? " TCP" : "";
  if (strcmp(a->afname, "AF_INET6") == 0)
    return sp_sprintf("#<Addrinfo: [%s]:%lld%s>", a->ip, (long long)a->port, st);
  return sp_sprintf("#<Addrinfo: %s:%lld%s>", a->ip, (long long)a->port, st);
}
const char *sp_brat_to_s(sp_BigRational *r) {SP_GC_ROOT(r);
  const char *ns = sp_bigint_to_s(r->num), *ds = sp_bigint_to_s(r->den);
  return sp_str_concat(sp_str_concat(ns, SPL("/")), ds);
}
const char *sp_brat_inspect(sp_BigRational *r) {SP_GC_ROOT(r);
  return sp_str_concat(sp_str_concat(SPL("("), sp_brat_to_s(r)), SPL(")"));
}
sp_float sp_brat_to_f(sp_BigRational *r) {SP_GC_ROOT(r);
  return sp_bigint_to_double(r->num) / sp_bigint_to_double(r->den);
}

/* ---- Marshal.dump/load helpers -- relocated from spinel_rt.h. 0 optcarrot
   uses. ---- */

/* Marshal implementation moved to lib/sp_marshal.c. These small wrappers give
   the standalone serializer construction primitives that need spinel_rt.h
   types; sp_re_init (codegen) installs them into sp_marshal_v along with the
   generated sym_intern / obj_dump / obj_load. */
sp_RbVal sp_marv_arr_new(void) { return sp_box_poly_array(sp_PolyArray_new()); }
void sp_marv_arr_push(sp_RbVal a, sp_RbVal v) {SP_GC_ROOT_RBVAL(a);SP_GC_ROOT_RBVAL(v); sp_PolyArray_push((sp_PolyArray *)a.v.p, v); }
sp_RbVal sp_marv_box_complex(sp_float re, sp_float im) { sp_Complex c; c.re = re; c.im = im; return sp_box_complex(c); }
sp_RbVal sp_marv_box_rational(sp_int n, sp_int d) { return sp_box_rational(sp_rational_new(n, d)); }
void sp_marv_raise(const char *cls, const char *msg) {SP_GC_ROOT_STR(msg); sp_raise_cls(cls, msg); }

/* ---- Regexp gsub/sub-with-Hash + Signal/Interrupt exception ctors --
   relocated from spinel_rt.h. 0 optcarrot uses. ---- */
#include "sp_exc.h"

/* String#gsub(regex, hash) — per-match hash lookup form. CRuby's
 * semantics: each matched substring is looked up as a key in the
 * hash; the value (if present) is the replacement, otherwise the
 * matched substring is dropped (CRuby returns "", not the match).
 * Used by html_escape / json_escape idioms (gsub(/[&<>]/, ESCAPES)). */
const char *sp_re_gsub_str_str_hash(mrb_regexp_pattern *pat, const char *str, sp_StrStrHash *h) {SP_GC_ROOT_STR(str);SP_GC_ROOT(h);
  int64_t slen = (int64_t)strlen(str);
 /* malloc scratch (realloc-safe); exact-sized string emitted below. */
  size_t cap = (slen * 2) + 64; char *out = (char *)malloc(cap); size_t olen = 0;
  int64_t pos = 0; int caps[64];
  while (pos <= slen) {
    int n = re_exec(pat, str, slen, pos, caps, 64, 0);
    if (n <= 0 || caps[0] < 0) break;
    size_t before = caps[0] - pos;
    int mlen = caps[1] - caps[0];
    /* Lay a 0xff (rodata-literal) marker byte right before the transient key
       so sp_str_hash's s[-1] read is in-bounds and routes to the plain
       (non-caching) path -- this buffer has no sp_str_hdr to cache into. */
    char keybuf[65]; keybuf[0] = (char)0xff;
    char *kbuf = mlen + 1 < (int)sizeof(keybuf) ? keybuf : (char *)malloc(mlen + 2);
    if (kbuf != keybuf) kbuf[0] = (char)0xff;
    char *key = kbuf + 1;
    memcpy(key, str + caps[0], mlen);
    key[mlen] = 0;
    /* A miss takes the hash's DEFAULT, not the empty string: CRuby looks the
       match up with #[], so `Hash.new("?")` substitutes "?" (#3555). The
       default is nil for a plain hash, which renders empty as before. */
    const char *rep = sp_StrStrHash_get(h, key);
    if (!rep) rep = "";
    size_t rlen = strlen(rep);
    if (olen + before + rlen >= cap) { cap = ((olen + before + rlen) * 2) + 64; out = (char *)realloc(out, cap); }
    memcpy(out + olen, str + pos, before); olen += before;
    memcpy(out + olen, rep, rlen); olen += rlen;
    if (kbuf != keybuf) free(kbuf);
    if (caps[0] == caps[1]) {
 /* Zero-width match: keep the source char at this position and step
    past it (see sp_re_gsub for the rationale). */
      if (caps[1] < slen) {
        if (olen + 1 >= cap) { cap = (olen * 2) + 64; out = (char *)realloc(out, cap); }
        out[olen++] = str[caps[1]];
      }
      pos = caps[1] + 1;
    }
else {
      pos = caps[1];
    }
  }
  if (pos < slen) {
    size_t rest = slen - pos;
    if (olen + rest >= cap) { cap = olen + rest + 1; out = (char *)realloc(out, cap); }
    memcpy(out + olen, str + pos, rest); olen += rest;
  }
  char *res = sp_str_alloc(olen);
  memcpy(res, out, olen);
  free(out);
  return res;
}
/* Issue #910: sub(regex, hash) — same lookup semantics as
   sp_re_gsub_str_str_hash but only the first match. */
const char *sp_re_sub_str_str_hash(mrb_regexp_pattern *pat, const char *str, sp_StrStrHash *h) {SP_GC_ROOT(h);SP_GC_ROOT_STR(str);
  int64_t slen = (int64_t)strlen(str);
  int caps[64];
  int n = re_exec(pat, str, slen, 0, caps, 64, 0);
  if (n <= 0 || caps[0] < 0) return str;
  int mlen = caps[1] - caps[0];
  /* 0xff marker before the transient key: keeps sp_str_hash's s[-1] read
     in-bounds and on the non-caching path (no sp_str_hdr behind this buffer). */
  char keybuf[65]; keybuf[0] = (char)0xff;
  char *kbuf = mlen + 1 < (int)sizeof(keybuf) ? keybuf : (char *)malloc(mlen + 2);
  if (kbuf != keybuf) kbuf[0] = (char)0xff;
  char *key = kbuf + 1;
  memcpy(key, str + caps[0], mlen);
  key[mlen] = 0;
  /* a missing key answers the hash's DEFAULT, which is what Hash.new("?")
     exists for; hard-coding "" dropped it (#3824) */
  const char *rep = h ? sp_StrStrHash_get(h, key) : "";
  if (!rep) rep = "";
  size_t rlen = strlen(rep);
  size_t rest = slen - caps[1];
  size_t total = caps[0] + rlen + rest;
  char *out = sp_str_alloc_raw(total + 1);
  memcpy(out, str, caps[0]);
  memcpy(out + caps[0], rep, rlen);
  memcpy(out + caps[0] + rlen, str + caps[1], rest);
  out[total] = 0;
  if (kbuf != keybuf) free(kbuf);
  return out;
}
/* msg: an explicit second argument overrides the SIG<name> message (only the
   Integer-signal form accepts one, matching CRuby); NULL keeps the default. */
sp_Exception *sp_signal_exc_new_m(sp_RbVal sig, const char *msg) {SP_GC_ROOT_RBVAL(sig);SP_GC_ROOT_STR(msg);
  if (msg && sig.tag != SP_TAG_INT)
    sp_raise_cls("ArgumentError", "wrong number of arguments");
  int no = sp_signal_resolve(sig);
  const char *nm = sp_signal_signame(no);
  sp_Exception *e = sp_exc_new("SignalException",
                               msg ? msg : sp_sprintf("SIG%s", nm ? nm : "?"));
  SP_GC_ROOT(e);
  e->xkey = sp_box_int((sp_int)no);
  return e;
}
sp_Exception *sp_signal_exc_new(sp_RbVal sig) {SP_GC_ROOT_RBVAL(sig);
  return sp_signal_exc_new_m(sig, NULL);
}
sp_Exception *sp_interrupt_new(const char *msg) {SP_GC_ROOT_STR(msg);
  sp_Exception *e = sp_exc_new("Interrupt", (msg && msg[0]) ? msg : "Interrupt");
  SP_GC_ROOT(e);
  e->xkey = sp_box_int((sp_int)SIGINT);
  return e;
}

/* ---- FFI array data / array-kind length / sp_Class unbox -- relocated
   from spinel_rt.h. 0 optcarrot uses. ---- */

/* FFI array hand-off from a POLY slot: dispatch on the RUNTIME storage kind.
   A poly value may hold any array variant -- an int array that poly-collapsed
   still boxes an sp_IntArray (cls_id INT_ARRAY), and reinterpreting its .v.p
   as sp_PolyArray* read garbage lengths and marshalled NULL (the toy LoRA
   flatline). nil passes as NULL (the C idiom for an absent array); any other
   runtime kind raises loudly rather than silently handing the callee NULL. */
const int64_t *sp_ffi_int_array_data(sp_RbVal v) {SP_GC_ROOT_RBVAL(v);
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_INT_ARRAY)
    return sp_IntArray_ffi_data((sp_IntArray *)v.v.p);
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_POLY_ARRAY)
    return sp_PolyArray_ffi_int_data((sp_PolyArray *)v.v.p);
  if (v.tag == SP_TAG_NIL) return (const int64_t *)0;
  sp_raise_cls("TypeError", "no implicit conversion into an FFI :int_array");
  return (const int64_t *)0;  /* unreached */
}
const double *sp_ffi_float_array_data(sp_RbVal v) {SP_GC_ROOT_RBVAL(v);
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_FLT_ARRAY)
    return sp_FloatArray_ffi_data((sp_FloatArray *)v.v.p);
  if (v.tag == SP_TAG_OBJ && v.cls_id == SP_BUILTIN_POLY_ARRAY)
    return sp_PolyArray_ffi_float_data((sp_PolyArray *)v.v.p);
  if (v.tag == SP_TAG_NIL) return (const double *)0;
  sp_raise_cls("TypeError", "no implicit conversion into an FFI :float_array");
  return (const double *)0;  /* unreached */
}
/* FFI array hand-off. Concrete arrays expose their element storage zero-copy
   (sp_int/sp_float are int64/double on 64-bit targets). A poly_array can't
   be punned -- its ->data is sp_RbVal[] (boxed) -- so unbox element-wise into
   a fresh GC-tracked buffer (sp_gc_alloc_nogc: no collection mid-build, so a
   sibling array arg's buffer can't be swept; freed at a later GC). */
const int64_t *sp_PolyArray_ffi_int_data(sp_PolyArray *a) {SP_GC_ROOT(a);
  if (!a || a->len <= 0) return (const int64_t *)0;
  int64_t *buf = (int64_t *)sp_gc_alloc_nogc((size_t)a->len * sizeof(int64_t), NULL, NULL);
  for (sp_int i = 0; i < a->len; i++) buf[i] = (int64_t)a->data[i].v.i;
  return buf;
}
const double *sp_PolyArray_ffi_float_data(sp_PolyArray *a) {SP_GC_ROOT(a);
  if (!a || a->len <= 0) return (const double *)0;
  double *buf = (double *)sp_gc_alloc_nogc((size_t)a->len * sizeof(double), NULL, NULL);
  for (sp_int i = 0; i < a->len; i++) buf[i] = (double)a->data[i].v.f;
  return buf;
}
/* Element count of an array-kind value, or -1 if `el` is not an array (a
   non-object, a user object, a hash, etc.). Lets assoc/rassoc skip non-array
   and too-short pairs without indexing them, so a `nil` search key cannot
   spuriously match an out-of-bounds (nil) read. */
sp_int sp_array_kind_len(sp_RbVal el) {
  if (el.tag != SP_TAG_OBJ || !el.v.p) return -1;
  switch (el.cls_id) {
    case SP_BUILTIN_INT_ARRAY:
    case SP_BUILTIN_SYM_ARRAY:  return ((sp_IntArray *)el.v.p)->len;
    case SP_BUILTIN_FLT_ARRAY:  return ((sp_FloatArray *)el.v.p)->len;
    case SP_BUILTIN_STR_ARRAY:  return ((sp_StrArray *)el.v.p)->len;
    case SP_BUILTIN_POLY_ARRAY: return ((sp_PolyArray *)el.v.p)->len;
    default: return -1;
  }
}
sp_Class sp_unbox_class(sp_RbVal v) {
  if (v.tag != SP_TAG_CLASS) return SP_CLASS_NIL;
  if (v.cls_id == SP_CLASS_BY_NAME) { sp_Class c = {-1, v.v.s}; return c; }
  { sp_Class c = {(sp_int)v.cls_id}; return c; }
}

/* Arithmetic reached an int slot still holding the nil sentinel -- a container
   read that missed. That value is nil, so CRuby's NoMethodError is the answer,
   not a computation on INTPTR_MIN. */
SP_NORETURN void sp_raise_nil_int_op(sp_int a, sp_int b, const char *op) {SP_GC_ROOT_STR(op);
  (void)b;
  if (a == SP_INT_NIL)
    sp_raise_cls("NoMethodError", sp_sprintf("undefined method '%s' for nil", op));
  /* nil on the RIGHT is the coercion failure CRuby reports from Integer#+ */
  sp_raise_cls("TypeError", "nil can't be coerced into Integer");
}

/* `Queue#freeze` raises rather than freezing: a frozen queue could never be
   pushed to again, so Ruby refuses it outright. Names the receiver the way
   CRuby's message does, so a SizedQueue reports as one. */
SP_NORETURN void sp_raise_cannot_freeze(const char *cls, void *p) {
  sp_raise_cls("TypeError",
               sp_sprintf("cannot freeze #<%s:0x%016llx>", cls ? cls : "Object",
                          (unsigned long long)(uintptr_t)p));
}
