/* spinel: the single-binary Spinel compiler *and* driver.
 *
 * One executable does the whole job, with no shell wrapper and no chain of
 * helper binaries (the legacy parse -> analyze -> codegen pipeline collapsed
 * into this process). It:
 *   1. parses the .rb (the Prism walk fills the in-memory node table),
 *   2. infers types and emits C (codegen_program),
 *   3. invokes the C compiler (cc) to link the generated C against
 *      libspinel_rt.a into a native binary, and
 *   4. optionally runs that binary (-E).
 *
 * Usage:
 *   spinel app.rb               compile to ./app
 *   spinel app.rb -o myapp      compile to ./myapp
 *   spinel app.rb -c            emit C only (app.c)
 *   spinel app.rb -S            print C to stdout
 *   spinel -e 'puts 42'         compile inline source (default name: a)
 *   spinel -E app.rb a b c      compile to a temp dir, run, ARGV=[a,b,c]
 */
#include "node_table.h"
#include "value_pos.h"
#include "implied_calls.h"
#include "implicit_calls.h"
#include "spinel_rev.h"
#include "codegen.h"
#include "analyze.h"
#include "repr.h"
#include "share.h"
#include "csplit.h"
#include "unit_cache.h"
#include "decide.h"
#include "method_table_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#if defined(__APPLE__)
  #include <mach-o/dyld.h>
#endif
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include "timing.h"

extern int g_no_root_elision;
extern int g_no_root_frame;
extern int g_opt_level;
extern int g_strict_require_cli;
extern int g_dump_load_graph;
extern int g_load_model_graph;
extern int sp_parse_reported;
/* RUBY_DESCRIPTION for the compiled program, in the `ruby -v` shape:
   "spinel <RUBY_VERSION> (<release> revision <rev>)" (codegen appends the
   platform). The version word is the one RUBY_VERSION and
   RUBY_ENGINE_VERSION report, so a script that reads the version out of the
   description gets the same answer; release and revision tell builds apart. */
extern const char *g_ruby_description;
extern int g_inline_hot;
extern int g_no_write_barrier;
extern const char *g_ext_init_name;
extern const char *g_ext_entries;
extern const char *g_ext_classes;
extern const char *g_ext_target;
extern const char *g_ext_feature;
extern char *g_ext_header_text;
extern char *g_ext_shim_text;
void unit_incremental_set(int on);   /* codegen_util.c: --incremental's unit pragmas */
/* Native Windows builds its programs differently, over the lib/win32 shim,
   and says how in a header of its own (SPINEL_PLATFORM_CFLAGS / _LIBS go on
   every program's command line, "@LIB@" in them spelled as the runtime's lib
   directory); the defaults are POSIX's. */
#if defined(_WIN32)
#include "sp_win32_driver.h"
#endif
#ifndef SPINEL_DEFAULT_CC
#define SPINEL_DEFAULT_CC "cc"
#endif
#ifndef SPINEL_EXE_SUFFIX
#define SPINEL_EXE_SUFFIX ""
#endif
#define PATH_SEP '/'
#define EXE_SUFFIX SPINEL_EXE_SUFFIX

/* Defined in spinel_parse.c (compiled with -DSPINEL_PARSE_AS_LIB). */
NodeTable *sp_parse_file(const char *source_file, const char *argv0);

/* ---------- tiny growable string ---------- */
typedef struct { char *p; size_t len, cap; } Str;
static void s_reserve(Str *s, size_t extra) {
  if (s->len + extra + 1 > s->cap) {
    s->cap = (s->len + extra + 1) * 2;
    s->p = realloc(s->p, s->cap);
  }
}
static void s_add(Str *s, const char *t) {
  size_t n = strlen(t);
  s_reserve(s, n);
  memcpy(s->p + s->len, t, n);
  s->len += n;
  s->p[s->len] = '\0';
}
/* Append a shell-quoted token followed by a space (paths may contain spaces). */
/* Copy `in` with a `_mt` variant marker removed from before its extension, so
   sp_x.o and sp_x_mt.o compare as the same package object. */
static void s_strip_mt(const char *in, char *out, size_t cap) {
  snprintf(out, cap, "%s", in ? in : "");
  char *dot = strrchr(out, '.');
  if (!dot || (size_t)(dot - out) < 3) return;
  if (strncmp(dot - 3, "_mt", 3) != 0) return;
  memmove(dot - 3, dot, strlen(dot) + 1);
}

static void s_add_arg(Str *s, const char *t) {
  s_add(s, "\"");
  s_add(s, t);
  s_add(s, "\" ");
}

/* --print-build writes the ingredients spinel is authoritative about, one per
   line as `kind value`. The consumer is a program, and the previous spelling
   of this printed the whole cc command line instead -- which meant `spin pack`
   had to carry a quote-aware shell splitter and a token classifier to recover
   the categories the command had just been assembled from. Naming a compiler
   was the other half of the mistake: which cc compiles the C is the caller's
   decision (a cross toolchain, say), and none of spinel's business. What IS
   spinel's business is the defines, the include paths, the libraries and the
   inputs, because those have to agree with how the runtime archive was built.
   Optimisation, warnings, diagnostics and section GC are deliberately absent:
   they are how THIS build was run, not what the program requires. */
static void bi_put(Str *bi, const char *kind, const char *val) {
  if (!val || !*val) return;
  s_add(bi, kind); s_add(bi, " "); s_add(bi, val); s_add(bi, "\n");
}
static void bi_put_toks(Str *bi, const char *kind, const char *toks) {
  if (!toks || !*toks) return;
  char *d = strdup(toks);
  if (!d) return;
  for (char *t = strtok(d, " \t"); t; t = strtok(NULL, " \t")) bi_put(bi, kind, t);
  free(d);
}

/* The host libraries a native program needs beyond libc and libm.
   OpenBSD's backtrace(3), which the runtime archive calls, is
   libexecinfo's rather than libc's. String#crypt is libc crypt(3), linked
   only when the program calls it: Darwin's and OpenBSD's libc have it,
   with no libcrypt, and elsewhere a linker without a default --as-needed
   (Fedora's) otherwise made libcrypt a dependency of every binary (#6674). */
static void add_host_libs(Str *cmd, Str *bi, int uses_crypt) {
#if defined(__OpenBSD__)
  s_add(cmd, "-lexecinfo ");
  bi_put(bi, "lib", "-lexecinfo");
#elif !defined(__APPLE__)
  if (uses_crypt) {
    s_add(cmd, "-lcrypt ");
    bi_put(bi, "lib", "-lcrypt");
  }
#endif
  (void)uses_crypt;
}

#if defined(_WIN32)
/* The platform's own flags (SPINEL_PLATFORM_CFLAGS / _LIBS) onto the command
   line, "@LIB@" spelled as the runtime's lib directory (quoted: it may hold a
   space), and into the --print-build report by kind. */
static void add_platform_flags(Str *cmd, Str *bi, const char *flags, const char *lib_dir) {
  if (!flags || !*flags) return;
  char *d = strdup(flags);
  if (!d) return;
  for (char *t = strtok(d, " "); t; t = strtok(NULL, " ")) {
    char tok[4200];
    const char *at = strstr(t, "@LIB@");
    if (at) snprintf(tok, sizeof tok, "%.*s%s%s", (int)(at - t), t, lib_dir, at + 5);
    else snprintf(tok, sizeof tok, "%s", t);
    if ((tok[0] == '-' && (tok[1] == 'I' || tok[1] == 'L')) && at) {
      char q[4300]; snprintf(q, sizeof q, "-%c\"%s\" ", tok[1], tok + 2); s_add(cmd, q);
    }
    else { s_add(cmd, tok); s_add(cmd, " "); }
    bi_put(bi, tok[1] == 'I' ? "include" : tok[1] == 'D' ? "define" :
               (tok[1] == 'l' || tok[1] == 'L' || tok[1] == 'W' || !strcmp(tok, "-static")) ? "lib" : "cflag",
           tok[1] == 'I' ? tok + 2 : tok);
  }
  free(d);
}
#endif

static void set_env(const char *k, const char *v) {
  setenv(k, v, 1);
}

static int file_exists(const char *p) {
  FILE *f = fopen(p, "rb");
  if (f) { fclose(f); return 1; }
  return 0;
}

static int proc_id(void) {
  return (int)getpid();
}

static const char *temp_dir(void) {
  const char *t = getenv("TMPDIR");
  if (!t) t = "/tmp";
  return t;
}

/* Build "<tmpdir>/spinel_<tag>_<pid>_<n><ext>" into `out`. */
static void make_temp_path(char *out, size_t sz, const char *tag, int n, const char *ext) {
  snprintf(out, sz, "%s%cspinel_%s_%d_%d%s", temp_dir(), PATH_SEP, tag, proc_id(), n, ext);
}

/* Directory containing this executable (no trailing separator). */
static void exe_dir(const char *argv0, char *out, size_t outsz) {
  char buf[4096];
  buf[0] = '\0';
#if defined(__APPLE__)
  uint32_t bsz = (uint32_t)sizeof buf;
  if (_NSGetExecutablePath(buf, &bsz) != 0) buf[0] = '\0';
#else
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n > 0) buf[n] = '\0'; else buf[0] = '\0';
#endif
  if (!buf[0]) {
    /* Fall back to argv0 (resolve when it carries a path). */
    if (!realpath(argv0, buf)) { strncpy(buf, argv0, sizeof buf - 1); buf[sizeof buf - 1] = '\0'; }
  }
  else {
    /* Canonicalize: _NSGetExecutablePath returns the path spinel was invoked
       through WITHOUT following symlinks, so the installed tree's
       /usr/local/bin/spinel symlink resolved lib/ against bin/ and the link
       line pointed at bin/lib/libspinel_rt.a. (/proc/self/exe is already
       fully resolved; realpath is a no-op on that branch.) */
    char rp[4096];
    if (realpath(buf, rp)) { strncpy(buf, rp, sizeof buf - 1); buf[sizeof buf - 1] = '\0'; }
  }
  char *slash = strrchr(buf, '/');
  if (slash) *slash = '\0';
  else strcpy(buf, ".");
  strncpy(out, buf, outsz - 1);
  out[outsz - 1] = '\0';
}

/* Resolve the lib/ directory: a dev tree builds the binary at build/spinel
   (lib at ../lib); an installed tree drops it next to lib/. */
static void resolve_lib_dir(const char *argv0, char *out, size_t outsz) {
  char dir[4096], cand[4096];
  exe_dir(argv0, dir, sizeof dir);
  snprintf(cand, sizeof cand, "%s%clib%clibspinel_rt.a", dir, PATH_SEP, PATH_SEP);
  if (file_exists(cand)) { snprintf(out, outsz, "%s%clib", dir, PATH_SEP); return; }
  snprintf(cand, sizeof cand, "%s%c..%clib%clibspinel_rt.a", dir, PATH_SEP, PATH_SEP, PATH_SEP);
  if (file_exists(cand)) { snprintf(out, outsz, "%s%c..%clib", dir, PATH_SEP, PATH_SEP); return; }
  /* Last resort: assume sibling lib/ (may not exist; cc will report). */
  snprintf(out, outsz, "%s%clib", dir, PATH_SEP);
}

/* Append the FFI link/cflag markers the codegen embedded in the C source.
   Each marker line is a C comment of the form  PREFIX <flags> SP/  where
   PREFIX is e.g. the SPINEL_LINK opener; `prefix` includes that opener. */
static void scrape_ffi_markers(const char *csrc, const char *prefix, Str *out) {
  size_t plen = strlen(prefix);
  const char *p = csrc;
  while (p && *p) {
    const char *eol = strchr(p, '\n');
    size_t llen = eol ? (size_t)(eol - p) : strlen(p);
    if (llen > plen && strncmp(p, prefix, plen) == 0) {
      /* strip the trailing space + comment close */
      const char *body = p + plen;
      size_t blen = llen - plen;
      while (blen > 0 && (body[blen-1] == ' ' || body[blen-1] == '/' || body[blen-1] == '*')) blen--;
      char tmp[1024];
      if (blen >= sizeof tmp) blen = sizeof tmp - 1;
      memcpy(tmp, body, blen);
      tmp[blen] = '\0';
      s_add(out, tmp);
      s_add(out, " ");
    }
    if (!eol) break;
    p = eol + 1;
  }
}

/* The banner every emitted translation unit opens with. spin reads it to keep
   spinel's own output out of a package's carried C (#4362); the guard below
   reads it to keep that output from landing ON a source in the first place. */
#define SP_EMIT_BANNER "/* Generated by Spinel AOT compiler */"

/* Whether the regular file at path opens with spinel's banner: 1 it does,
   0 it does not, -1 there is no regular file to read. */
static int spinel_output(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  char head[sizeof SP_EMIT_BANNER];
  size_t n = fread(head, 1, sizeof head - 1, f);
  fclose(f);
  head[n] = 0;
  return sp_streq(head, SP_EMIT_BANNER);
}

/* Refuse to write C over a file spinel did not write. `spinel app.rb -c -o
   sp_json.c` inside a package destroys the source and leaves the generated
   unit under its name, and nothing downstream can undo that: the compile then
   fails on a wall of duplicate symbols naming no file, and the source is gone.
   Re-emitting over spinel's OWN output is the ordinary case and stays silent.
   `--force` is the way through for anyone who means it. Only a regular file
   is looked into, as refuse_log_overwrite does: `-o /dev/stdout` into a pipe
   opened the pipe to read it and waited for a writer that is this process. */
static int g_force_overwrite = 0;
static int refuse_overwrite(const char *path) {
  if (g_force_overwrite) return 0;
  /* -1: nothing there to lose; 1: our own previous output */
  if (spinel_output(path) != 0) return 0;
  fprintf(stderr,
          "spinel: refusing to overwrite '%s': it is not spinel's output.\n"
          "spinel:   emitting there would replace that file with a translation\n"
          "spinel:   unit and lose what is in it. Choose another -o path, or\n"
          "spinel:   pass --force if you mean to replace it.\n", path);
  return 1;
}

/* The decisions log is emptied before the source is read, so the same care
   comes first there: `spinel --decisions-log=app.rb app.rb` would leave
   nothing to compile. A log is lines of `kind@site` (src/decide.c), a kind
   being lowercase words joined by hyphens (nn-read, root-frame); a file
   that opens with anything else is not replaced.

   Only a regular file is looked into. A pipe, a FIFO or /dev/stdout has
   nothing to lose, and opening one to read it would wait for a writer that
   is this process. */
static int refuse_log_overwrite(const char *path) {
  if (g_force_overwrite || !path || !*path) return 0;
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) return 0;
  /* one that cannot be read cannot be shown to be a log */
  char head[64];
  size_t n = 0;
  FILE *f = fopen(path, "rb");
  if (f) { n = fread(head, 1, sizeof head - 1, f); fclose(f); }
  head[n] = 0;
  size_t k = strspn(head, "abcdefghijklmnopqrstuvwxyz-");
  if (k > 2 && head[k] == '@' && head[0] != '-' && head[k - 1] != '-' && memchr(head, '-', k)) return 0;
  fprintf(stderr,
          "spinel: refusing to overwrite '%s': it is not a decisions log.\n"
          "spinel:   Choose another --decisions-log path, or pass --force if\n"
          "spinel:   you mean to replace it.\n", path);
  return 1;
}

/* Whether the C compiler spells things clang's way. The two spellings of the
   caret suppression are not interchangeable -- gcc takes
   -fno-diagnostics-show-caret and refuses clang's -fno-caret-diagnostics, and
   clang the reverse -- and an unknown -f flag is an error on both rather than
   the warning an unknown -W is, so there is no spelling to pass blind. Asked
   once: the answer cannot change inside one run, and the question costs a
   process. */
/* --jobs=N (#4847 C1): preprocess the generated unit, split it into N parts
   (csplit.c), compile the parts in parallel and link them with the rest of
   the command. `cmd` is the whole single-unit command; the source sits at
   [src_at, src_end) in it, the compile flags before it and the link inputs
   after it. Returns 0 when the binary is built; anything else leaves the
   caller to run the single-unit command, whose diagnostics name the .rb. */
/* the generated unit's size from which a build splits unasked */
#define SPLIT_AUTO_BYTES (4L << 20)

/* --split-parts=M: the parts the split cuts, apart from the compiles that
   run at once (--jobs); 0 or below --jobs is --jobs. More parts than slots
   go through the slots as a queue, the biggest first, so the slowest part
   does not run alone at the end of the build (#10325). */
static int g_split_parts = 0;

/* the order to start parts in: by preprocessed size, biggest first (ties
   keep the part order) */
static off_t *cc_part_bytes;
static int cc_part_cmp(const void *a, const void *b) {
  int x = *(const int *)a, y = *(const int *)b;
  if (cc_part_bytes[x] != cc_part_bytes[y]) return cc_part_bytes[x] < cc_part_bytes[y] ? 1 : -1;
  return x - y;
}

static int cc_split_build(const char *cmd, size_t src_at, size_t src_end,
                          const char *c_path, int jobs) {
  if (jobs > 64) jobs = 64;
  int nparts = g_split_parts > jobs ? g_split_parts : jobs;
  if (nparts > 256) nparts = 256;
  /* the scratch goes where TMPDIR says, /tmp by default: a large program's
     preprocessed unit and parts take several GB (#9267) */
  const char *tmpd = getenv("TMPDIR");
  char dir[4096];
  snprintf(dir, sizeof dir, "%s/spinel_split_XXXXXX", tmpd && *tmpd ? tmpd : "/tmp");
  if (!mkdtemp(dir)) return -1;
  char flags[16384];
  if (src_at >= sizeof flags) return -1;
  memcpy(flags, cmd, src_at); flags[src_at] = 0;
  char pre[4200]; snprintf(pre, sizeof pre, "%s/pre.i", dir);
  size_t cl = strlen(flags) + strlen(c_path) + strlen(pre) + 64;
  char *c1 = malloc(cl);
  /* -E keeps the line markers: csplit.c replays them, so the parts know which
     lines are system headers and which are the .rb (#8579) */
  snprintf(c1, cl, "%s -E '%s' -o '%s'", flags, c_path, pre);
  double tm_pre = sp_timing_now();
  int rc = system(c1);
  free(c1);
  sp_timing_end(tm_pre, "cc_preprocess", "");
  double tm_split = sp_timing_now();
  char (*parts)[4096] = malloc(sizeof(*parts) * (size_t)nparts);
  char hdr[4200];
  if (rc == 0 && c_split(pre, dir, nparts, parts, hdr, sizeof hdr) != nparts) rc = -1;
  sp_timing_end(tm_split, "cc_split", "");
  double tm_cc = sp_timing_now();
  /* the parts through `jobs` slots, the biggest first */
  int *order = malloc(sizeof(int) * (size_t)nparts);
  off_t *bytes = calloc((size_t)nparts, sizeof(off_t));
  pid_t *pids = calloc((size_t)nparts, sizeof(pid_t));
  if (!order || !bytes || !pids) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int k = 0; k < nparts; k++) {
    struct stat pst;
    order[k] = k;
    if (rc == 0 && stat(parts[k], &pst) == 0) bytes[k] = pst.st_size;
  }
  cc_part_bytes = bytes;
  qsort(order, (size_t)nparts, sizeof order[0], cc_part_cmp);
  int next = 0, running = 0;
  while (rc == 0 && (next < nparts || running > 0)) {
    while (rc == 0 && next < nparts && running < jobs) {
      int k = order[next++];
      size_t ql = strlen(flags) + 2 * strlen(parts[k]) + 64;
      char *q = malloc(ql);
      snprintf(q, ql, "%s -c '%s' -o '%s.o'", flags, parts[k], parts[k]);
      pid_t p = fork();
      if (p == 0) { execl("/bin/sh", "sh", "-c", q, (char *)NULL); _exit(127); }
      free(q);
      if (p < 0) { rc = -1; break; }
      pids[k] = p; running++;
    }
    if (running == 0) break;
    int st = 0;
    pid_t done = wait(&st);
    if (done < 0) { rc = -1; break; }
    for (int k = 0; k < nparts; k++)
      if (pids[k] == done) { pids[k] = 0; running--; if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) rc = -1; break; }
  }
  /* (a failure stops starting parts; the ones still running are waited for) */
  for (int k = 0; k < nparts; k++) if (pids[k] > 0) { int st = 0; waitpid(pids[k], &st, 0); }
  free(pids); free(order); free(bytes);
  { char ex[48]; snprintf(ex, sizeof ex, " jobs=%d parts=%d", jobs, nparts); sp_timing_end(tm_cc, "cc_compile", ex); }
  double tm_lk = sp_timing_now();
  if (rc == 0) {
    size_t ll = strlen(cmd) + (size_t)nparts * 4200 + 16;
    char *lk = malloc(ll);
    char *lf = c_link_flags(flags);
    size_t w = (size_t)snprintf(lk, ll, "%s", lf);
    free(lf);
    for (int k = 0; k < nparts; k++) w += (size_t)snprintf(lk + w, ll - w, "'%s.o' ", parts[k]);
    snprintf(lk + w, ll - w, "%s", cmd + src_end);
    rc = system(lk);
    free(lk);
  }
  sp_timing_end(tm_lk, "cc_link", "");
  /* the pieces are scratch either way */
  char rmq[4200]; snprintf(rmq, sizeof rmq, "rm -rf '%s'", dir);
  if (!getenv("SPINEL_KEEP_SPLIT")) { int rr = system(rmq); (void)rr; }
  else fprintf(stderr, "spinel: split kept at %s\n", dir);
  free(parts);
  return rc;
}

static int cc_is_clang(const char *cc_cmd) {
  static int answer = -1;
  if (answer >= 0) return answer;
  answer = 0;
  char q[1024];
  snprintf(q, sizeof q, "%s --version 2>/dev/null", cc_cmd);
  FILE *fp = popen(q, "r");
  if (fp) {
    char line[512] = {0};
    if (fgets(line, sizeof line, fp) && strstr(line, "clang")) answer = 1;
    pclose(fp);
  }
  return answer;
}

static void usage(void) {
  fprintf(stderr,
    "Spinel AOT Compiler\n\n"
    "Usage: spinel app.rb              - compile to ./app\n"
    "       spinel app.rb -o myapp     - compile to ./myapp\n"
    "       spinel app.rb -c           - generate app.c only\n"
    "       spinel app.rb -S           - print C to stdout\n"
    "       spinel app.rb --print-build - print the build ingredients, compile nothing\n"
    "       spinel -e 'puts 42'        - compile inline source\n"
    "       spinel -E app.rb a b c     - compile + run with ARGV=[a, b, c]\n\n"
    "Options:\n"
    "  -o FILE     Output file\n"
    "  --link ARG  Extra link input (object/archive/-lLIB); repeatable\n"
    "  -v, --version  Print the compiler version and build revision\n"
    "  -c          C source only (don't compile)\n"
    "  --force     overwrite an -o path (with -c) or a --decisions-log path\n"
    "              that spinel did not write\n"
    "  -I DIR      Add a feature search root for `require \"name\"` (like ruby -I)\n"
    "  --strict-require  Refuse an unresolvable require instead of warning\n"
    "  --emit-rbs  Dump inferred type signatures as RBS (-> app.rbs), no binary\n"
    "  --emit-types Dump per-position inferred types + diagnostics as JSON\n"
    "  --warn-widen  Warn, at the slot, for each parameter or return that\n"
    "              widened to untyped (the boxed slow path)\n"
    "  --check-stores  Warn, at the store, for each value written raw into a\n"
    "              C slot of another C type (a compiler self-check)\n"
    "  --emit-symbol-map  Dump emitted-symbol -> Ruby-name map as JSON, no binary\n"
    "  -S          Print C to stdout\n"
    "  -E          Run the compiled binary; leftover args become its ARGV\n"
    "  -O LEVEL    Optimization level (default: 2)\n"
    "  -g          Add debug info (-g) + #line, leaving -O as-is (at -O2 an inlined\n"
    "              method has no frame in Exception#backtrace; --debug keeps them)\n"
    "  --timing    Report on stderr how long each compile phase took (the\n"
    "              analysis, its passes, the C generation, the cc steps);\n"
    "              the same as SP_TIMING=1 (docs/profiling.md)\n"
    "  --profile   Profiling build: -O2 -g -fno-omit-frame-pointer, unstripped,\n"
    "              and writes <out>.symbols.json (perf record -g ready)\n"
    "  --debug     Debug build: step through the .rb in gdb/lldb (#line, -g -O0)\n"
    "  --no-line-map  Suppress #line directives\n"
    "  --defer-refusals  Build anyway: a refused statement raises\n"
    "               NotImplementedError when reached (a refusal outside any\n"
    "               statement of a method, when the method is called)\n"
    "  --ext-init NAME    Emit a host-callable library: NAME() replaces main\n"
    "  --ext-entry M.m,.. Export these module methods (with -c; writes a .h contract)\n"
    "  --ext-class C,..   Export the instance methods of these classes (or `all`)\n"
    "               by name: sp_ext_send and sp_ext_methods in the contract\n"
    "  --ext cruby        Also generate the CRuby extension shim (<out>_ext.c)\n"
    "  --no-inline-hot  Do not force small leaf methods inline (default: do).\n"
    "                 Forcing is worth a sixth of optcarrot's frame rate and\n"
    "                 costs up to twice the C compile time\n"
    "  --decisions-log=FILE  Write the key of each optimization decision that\n"
    "              would be a miscompile if its check were wrong, one per line\n"
    "  --decisions=FILE  Take only the decisions FILE lists (an empty file:\n"
    "              none); `spinel bisect` searches the list for a wrong answer\n"
    "  --cc=CMD    C compiler (default: cc)\n"
    "  --jobs=N    compile the generated C as N units in parallel\n"
    "              (default: split only a unit of 4 MB or more, and no wasm32-wasi\n"
    "              module; --jobs=1 never)\n"
    "  --split-parts=M  With a split: cut M parts and compile them N at a time,\n"
    "              the biggest first (default: M = N)\n"
    "  --incremental  Development build: compile the C in units, reuse the cached\n"
    "              objects of units whose text did not change (not with -S, --emit-*)\n"
    "  --unit-bytes=N[K|M]  With --incremental: the target size of one unit\n"
    "              (default 256K); another size shares no cached object\n"
    "  --cache-stats  With --incremental: print the cache hits and misses\n"
    "              on stderr after the build\n"
    "  --target=wasm32-wasi  Build a WebAssembly module for a WASI host with the\n"
    "              wasi-sdk at $WASI_SDK (default /opt/wasi-sdk); Integer is 32-bit,\n"
    "              and Fiber, Thread, processes and sockets are not available there\n"
    "  --exceptions=abort  With --target=wasm32-wasi, for a host without wasm\n"
    "              exception handling: a raise traps, and rescue, retry, catch and a\n"
    "              break or return that leaves a block through a frame are refused\n"
    "  --gc=none   No garbage collector: memory is allocated and never reclaimed,\n"
    "              for a short run under a host's memory cap (also --no-gc;\n"
    "              --gc=on is the default). The collector leaves the build, and\n"
    "              the runtime linked is the one built without its bookkeeping\n"
    "              (lib/libspinel_rt_nogc.a)\n"
    "  -e STR      Inline Ruby source (repeatable; joined with newlines)\n"
    "  --rbs DIR   Seed analyzer with RBS signatures from DIR (advisory)\n"
    "  --no-inline-rbs  Ignore inline RBS comments (`#: ...`, `# @rbs ...`)\n"
    "              in the program (default: apply them; docs/inline-rbs.md)\n"
    "  --int-overflow=MODE  Int +/-/* overflow handling (default: raise)\n"
    "  --dump-ast  Print the text AST and exit (debug)\n"
    "  --dump-load-graph  Print the files the program loads and what each require\n"
    "              resolves to, and exit (debug)\n"
    "  --load-model=MODEL  inline (default): required files are spliced in;\n"
    "              graph: each file is its own root and a require a load node\n"
    "              (a require that is a top-level statement, for now)\n"
    "  --dump-value-positions  Print the value positions of each method body and of\n"
    "              the program, and exit (debug)\n"
    "  --check-value-pos  Check the value-position iterator's limits and exit (debug)\n"
    "  --dump-implied-calls  Print the methods CRuby calls for each node of the program\n"
    "              that its text does not name, and exit (debug)\n"
    "  --method-table-check  Print the reads of the static method tables made outside\n"
    "              call_target, and the answers that are the table's last definition (debug)\n");
}

/* --dump-ast (1), --dump-value-positions (2), --check-value-pos (3),
   --dump-implied-calls (5) and --dump-load-graph (4, which the parse prints as it resolves the requires)
   work on the parsed table and stop; the mode a flag names, or 0. */
static int table_mode_of(const char *a) {
  if (sp_streq(a, "--dump-ast")) return 1;
  if (sp_streq(a, "--dump-value-positions")) return 2;
  if (sp_streq(a, "--dump-implied-calls")) return 5;
  if (sp_streq(a, "--dump-load-graph")) return g_dump_load_graph = 4;
  return sp_streq(a, "--check-value-pos") ? 3 : 0;
}

/* The flags the parse reads: `--strict-require` refuses an unresolvable
   require instead of warning past it -- inside a resolved dependency set the
   universe is known, so a require that resolves to nothing is a bug; `spin
   build` has always compiled this way through SPINEL_STRICT_REQUIRE, and `spin
   flags` needs to hand the same gate to a caller driving the compiler itself
   (#4105). `--load-model=inline|graph` picks how required files are loaded:
   spliced into one program (the default), or each kept as its own root with
   a load node per require (#9898; requires that are top-level statements, for now). 1 when `a`
   is one of them. */
static int parse_flag(const char *a) {
  if (sp_streq(a, "--strict-require")) return g_strict_require_cli = 1;
  if (sp_streq(a, "--load-model=inline")) { g_load_model_graph = 0; return 1; }
  if (sp_streq(a, "--load-model=graph")) return g_load_model_graph = 1;
  return 0;
}
/* The dumps of an analysis answer, printed once the analysis has it:
   --dump-repr (the representations, as the compile settles them) and
   --dump-load-record (the load phase's points, after the scope walk). 1
   when `a` is one of them. */
static int analysis_dump_flag(const char *a) {
  if (sp_streq(a, "--dump-repr")) return g_dump_repr = 1;
  if (sp_streq(a, "--dump-implicit")) return g_dump_implicit = 1;
  return sp_streq(a, "--dump-load-record") ? (g_dump_load_record = 1) : 0;
}
/* --rbs: the seed file the extractor wrote, removed however the compile
   ends -- a return from main, or a dump flag's exit from inside the
   analysis (--dump-load-record, --dump-traits, --dump-repr) */
static void rbs_seed_remove(void) {
  if (g_opt_rbs_seed) remove(g_opt_rbs_seed);
}
static int run_table_mode(const NodeTable *nt, int mode) {
  /* the lowering rows the graph matched on each call's parse, against the
     node's (analyze_util.c); a report on stderr */
  if (getenv("SPINEL_LOWER_ROWS_CHECK")) lower_rows_check(nt);
  if (mode == 1) nt_dump(nt, stdout);
  else if (mode == 2) value_pos_dump(nt, stdout);
  else if (mode == 3) return value_pos_check();
  else if (mode == 5) implied_calls_dump(nt, stdout);
  return 0;
}

#ifdef SP_WORK_COUNT
static void work_report(void) { fprintf(stderr, "spinel-work: %llu\n", g_nt_work); }
#endif

/* The runtime archive a program links. Thread/Mutex/Queue/... take the
   -DSP_THREADS variant, a C extension the one carrying the bridge, and
   --gc=none (codegen's SPINEL_GC_NONE marker) the one built with
   -DSP_GC_NONE, whose own functions register no root and run no barrier,
   since no collection reads them (#10699). The extension archives have no
   such variant: a program with both links the collecting one, and only the
   program's own TU is without the bookkeeping. */
static const char *runtime_archive(const char *csrc, int threads, int wasi, int abort_exceptions, int *cext, int *gc_none) {
  *cext = strstr(csrc, "/* SPINEL_USES_CEXT */") != NULL;
  *gc_none = strstr(csrc, "/* SPINEL_GC_NONE */") != NULL;
  if (*cext && (wasi || abort_exceptions)) {
    fprintf(stderr, "spinel: C extensions currently require the native exception runtime\n");
    return NULL;
  }
  if (*cext) return threads ? "libspinel_cext_rt_mt.a" : "libspinel_cext_rt.a";
  if (*gc_none) return threads ? "libspinel_rt_mt_nogc.a" : "libspinel_rt_nogc.a";
  return threads ? "libspinel_rt_mt.a" : "libspinel_rt.a";
}

static void runtime_includes(Str *cmd, Str *bi, const char *lib_dir, int uses_cext) {
  char tmp[8192];
  snprintf(tmp, sizeof tmp, "-I\"%s\" -I\"%s%cregexp\" ", lib_dir, lib_dir, PATH_SEP); s_add(cmd, tmp);
  if (uses_cext) {
    snprintf(tmp, sizeof tmp, "-I\"%s%c..%cinclude\" ", lib_dir, PATH_SEP, PATH_SEP); s_add(cmd, tmp);
    { char ci[4096]; snprintf(ci, sizeof ci, "%s%c..%cinclude", lib_dir, PATH_SEP, PATH_SEP); bi_put(bi, "include", ci); }
  }
  bi_put(bi, "include", lib_dir);
  { char rgi[4096]; snprintf(rgi, sizeof rgi, "%s%cregexp", lib_dir, PATH_SEP); bi_put(bi, "include", rgi); }
}

/* The C compiler is half of what a spinel build is, so name it too: a bug
   report that turns out to be a codegen/compiler interaction is otherwise
   missing the other half. Reports whatever `--cc=` selected, when that
   flag came first; a compiler that cannot be run leaves the line as it
   was rather than saying anything about it. */
static int print_version(const char *cc_cmd) {
  char ccv[512] = {0};
  char ccq[1024];
  /* Ask the preprocessor what the compiler is rather than `--version`,
     whose first word is the name it was invoked by: `cc --version` says
     "cc (Ubuntu ...) 13.3.0" for gcc, which names the symlink, not the
     compiler. __clang_version__ / __GNUC__ say which family and which
     version; the invocation name is added only when it differs. */
  snprintf(ccq, sizeof ccq, "%s -E -dM -x c /dev/null 2>/dev/null", cc_cmd);
  FILE *fp = popen(ccq, "r");
  if (fp) {
    char line[512], clangv[128] = {0};
    int gmaj = -1, gmin = 0, gpat = 0, apple = 0;
    while (fgets(line, sizeof line, fp)) {
      static const char cvk[] = "#define __clang_version__ \"";
      if (!strncmp(line, cvk, sizeof cvk - 1)) {
        char *q = strchr(line + sizeof cvk - 1, '"'); if (q) *q = '\0';
        snprintf(clangv, sizeof clangv, "%s", line + sizeof cvk - 1);
        char *sp = strchr(clangv, ' '); if (sp) *sp = '\0';   /* "18.1.3 (1ubuntu1)" -> "18.1.3" */
      }
      else if (!strncmp(line, "#define __apple_build_version__ ", 32)) apple = 1;
      else if (!strncmp(line, "#define __GNUC__ ", 17)) gmaj = atoi(line + 17);
      else if (!strncmp(line, "#define __GNUC_MINOR__ ", 23)) gmin = atoi(line + 23);
      else if (!strncmp(line, "#define __GNUC_PATCHLEVEL__ ", 28)) gpat = atoi(line + 28);
    }
    pclose(fp);
    const char *family = clangv[0] ? "clang" : gmaj >= 0 ? "gcc" : NULL;
    if (family) {
      const char *base = strrchr(cc_cmd, '/'); base = base ? base + 1 : cc_cmd;
      int named = !strncmp(base, family, strlen(family));
      /* Apple's clang numbers itself differently from LLVM's; say which */
      if (clangv[0]) snprintf(ccv, sizeof ccv, "%s%s %s%s%s%s", apple ? "apple " : "", family, clangv, named ? "" : " (", named ? "" : base, named ? "" : ")");
      else snprintf(ccv, sizeof ccv, "%s %d.%d.%d%s%s%s", family, gmaj, gmin, gpat, named ? "" : " (", named ? "" : base, named ? "" : ")");
    }
  }
  if (!ccv[0]) {   /* a compiler the preprocessor probe cannot read: its own first line */
    snprintf(ccq, sizeof ccq, "%s --version 2>/dev/null", cc_cmd);
    fp = popen(ccq, "r");
    if (fp) {
      if (fgets(ccv, sizeof ccv, fp)) {
        char *nl = strchr(ccv, '\n');
        if (nl) *nl = '\0';
      }
      pclose(fp);
    }
  }
  /* The release name first, since it is what a person reads; the build
     revision in parentheses is what tells two builds of one release
     apart, and tools/spin.rb's spinel_rev reads it from there as the
     toolchain identity for its probe records. */
  if (ccv[0]) printf("spinel %s (%s) [%s]\n", SPINEL_RELEASE, SPINEL_BUILD_REV, ccv);
  else printf("spinel %s (%s)\n", SPINEL_RELEASE, SPINEL_BUILD_REV);
  return 0;
}

/* --incremental and the two flags that only mean something with it. The
   native step reads them where it builds, so they live here rather than in
   main's locals. */
static struct {
  int on, cache_stats;
  size_t unit_bytes;     /* --unit-bytes=N[K|M]: marker-free bytes per unit */
  const char *needs;     /* the companion flag given, for its refusal without --incremental */
  const char *bad;       /* a --unit-bytes= that is not a size */
} inc_flags = { 0, 0, 256 * 1024, NULL, NULL };
/* 1 when a is one of the three (a malformed size is kept for
   incremental_begin to refuse), 0 otherwise */
static int incremental_opt(const char *a) {
  if (sp_streq(a, "--incremental")) inc_flags.on = 1;
  else if (sp_streq(a, "--cache-stats")) { inc_flags.cache_stats = 1; inc_flags.needs = "--cache-stats"; }
  else if (!strncmp(a, "--unit-bytes=", 13)) {
    char *e = NULL;
    unsigned long long v = (a[13] >= '0' && a[13] <= '9') ? strtoull(a + 13, &e, 10) : 0;
    int sh = e && (*e == 'K' || *e == 'k') ? 10 : e && (*e == 'M' || *e == 'm') ? 20 : 0;
    if (e && sh) e++;
    if (!e || *e || v == 0 || v > (1ULL << 40) >> sh) inc_flags.bad = a + 13;
    else inc_flags.unit_bytes = (size_t)(v << sh);
    inc_flags.needs = "--unit-bytes";
  }
  else return 0;
  return 1;
}
/* After option parsing, before the frontend runs, so a refused combination
   leaves nothing half built: the modes that print or emit something other
   than a binary do not combine with --incremental. Otherwise turns the
   unit pragmas on. 1: refused (said why), 0: go on. */
static int incremental_begin(int stdout_mode, int print_build, int emit_rbs, int emit_types, int emit_symbol_map) {
  if (inc_flags.bad) {
    fprintf(stderr, "spinel: --unit-bytes expects a size such as 256K or 1M, got '%s'\n", inc_flags.bad);
    return 1;
  }
  if (!inc_flags.on) {
    if (inc_flags.needs) fprintf(stderr, "spinel: %s needs --incremental\n", inc_flags.needs);
    return inc_flags.needs != NULL;
  }
  const char *with = stdout_mode ? "-S" : print_build ? "--print-build" : emit_rbs ? "--emit-rbs"
                   : emit_types ? "--emit-types" : emit_symbol_map ? "--emit-symbol-map"
                   : g_ext_init_name ? "--ext-init" : g_ext_entries ? "--ext-entry" : g_ext_target ? "--ext"
                   : NULL;
  if (with) {
    fprintf(stderr, "spinel: --incremental cannot be combined with %s\n", with);
    return 1;
  }
  unit_incremental_set(1);
  return 0;
}

static int dir_is_empty(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return 0;
  struct dirent *e;
  int empty = 1;
  while (empty && (e = readdir(d)) != NULL)
    empty = strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0;
  closedir(d);
  return empty;
}

/* -c --incremental writes a directory, and refuse_overwrite's care applies
   to it: a regular file there, or a directory holding something other than
   a previous unit set (no units.txt of spinel's, and no program.c of
   spinel's, which a declined split leaves), is not written into without
   --force; nor is one whose program.c spinel did not write, since the
   unit set's program.c replaces it. --force replaces a file, but writes
   into a directory beside what is in it.
   A missing directory is created, one level. 1: refused (said why). */
static int units_dir_refused(const char *dir) {
  struct stat st;
  int exists = stat(dir, &st) == 0, ours = 1;
  if (exists && !S_ISDIR(st.st_mode)) ours = 0;
  else if (exists) {
    char m[4200], p[4200];
    snprintf(m, sizeof m, "%s/units.txt", dir);
    snprintf(p, sizeof p, "%s/program.c", dir);
    int prog = spinel_output(p);
    ours = dir_is_empty(dir) || ((c_split_units_manifest(m) || prog == 1) && prog != 0);
  }
  if (!ours && !g_force_overwrite) {
    if (S_ISDIR(st.st_mode))
      fprintf(stderr,
              "spinel: refusing to write into '%s': it is not spinel's output.\n"
              "spinel:   writing the unit set there would mix it with what is in\n"
              "spinel:   it. Choose another -o path, or pass --force if you mean\n"
              "spinel:   to write into it; what is there stays.\n", dir);
    else
      fprintf(stderr,
              "spinel: refusing to overwrite '%s': it is not spinel's output.\n"
              "spinel:   the unit set is a directory and would replace that\n"
              "spinel:   file. Choose another -o path, or pass --force if you\n"
              "spinel:   mean to replace it.\n", dir);
    return 1;
  }
  if (exists && !S_ISDIR(st.st_mode)) { remove(dir); exists = 0; }
  if (!exists && mkdir(dir, 0777) != 0) {
    fprintf(stderr, "spinel: cannot create %s: %s\n", dir, strerror(errno));
    return 1;
  }
  return 0;
}

/* -c --incremental: the generated C went to a temp path and the cc command
   was assembled as for a build, so that the units are preprocessed with the
   build's own flags; the unit set is written to -o DIR (default
   <basename>.units) instead of compiling. main's exit status. */
static int incremental_write_dir(const char *cmd, size_t src_at, size_t src_end, const char *c_path,
                                 const char *output, const char *basename, int want_g, const char *cc_cmd) {
  char dir[4096];
  snprintf(dir, sizeof dir, "%s%s", output ? output : basename, output ? "" : ".units");
  int rc = units_dir_refused(dir);
  if (!rc) {
    IncrementalOpts o = { inc_flags.unit_bytes, inc_flags.cache_stats, want_g, cc_cmd, dir };
    rc = cc_incremental_build(cmd, src_at, src_end, c_path, 0, &o) != 0;
  }
  remove(c_path);
  return rc;
}

/* The native step: the single unit, the --jobs split of a large one, or
   with --incremental the unit cache (unit_cache.c). A split or an
   incremental build that fails leaves the single unit to run, unless its
   STRICT variable says the failure is the answer (for tests of the split
   itself). main's cc status. */
static int cc_native_build(const char *cmd, size_t src_at, size_t src_end, const char *c_path,
                           int cc_jobs, int target_wasi, int want_g, const char *cc_cmd) {
  int cc_rc = -1;
  if (inc_flags.on) {
    IncrementalOpts o = { inc_flags.unit_bytes, inc_flags.cache_stats, want_g, cc_cmd, NULL };
    cc_rc = cc_incremental_build(cmd, src_at, src_end, c_path, cc_jobs, &o);
    if (cc_rc != 0 && !getenv("SPINEL_INCREMENTAL_STRICT")) cc_rc = system(cmd);
    return cc_rc;
  }
  /* Unasked, only a large unit is split: a part cannot inline what another
     part defines, which cost optcarrot's 0.6 MB unit ~10% of its speed, and
     run time comes before build time. A unit this large is where cc's one
     core is the build (campfire's 10 MB: 77 s whole, 36 s split). A
     wasm32-wasi module splits only when --jobs asks: it was never split
     before, and a module that got slower unasked would say nothing why. */
  if (cc_jobs == 0) {
    struct stat cst;
    cc_jobs = 1;
    if (!target_wasi && stat(c_path, &cst) == 0 && cst.st_size >= SPLIT_AUTO_BYTES) {
      long nc = sysconf(_SC_NPROCESSORS_ONLN);
      cc_jobs = nc > 16 ? 16 : nc > 1 ? (int)nc : 1;
    }
  }
  if (cc_jobs > 1)
    cc_rc = cc_split_build(cmd, src_at, src_end, c_path, cc_jobs);
  if (cc_rc != 0 && !(cc_jobs > 1 && getenv("SPINEL_SPLIT_STRICT")))
    cc_rc = system(cmd);   /* the single unit, or the fallback */
  return cc_rc;
}

/* A pointer of the wrong type in generated code is a miscompile, not a style
   question: it reads one struct through another's layout, which segfaults or
   answers garbage (a Symbol key dereferenced as a char *, #3975; an Integer
   element bound to a string handle, #3971). Both showed up only as a warning
   in the build log, easy to scroll past. Fail the build instead, so the
   emitter's own bug surfaces here rather than at run time.
   A call to a function no declaration names is the same miscompile: C
   takes it to return int, so a pointer result loses its upper half on a
   64-bit target. Spinel declares every function it calls itself, so only
   an ffi_source fragment can make one -- calling an ffi_func by its C
   name, which the prologue declares under a private name (docs/FFI.md).
   Under -Wno-all neither cc said so, and the program crashed at run time. */
static void miscompile_errors(Str *cmd) {
  s_add(cmd, "-Werror=incompatible-pointer-types -Werror=int-conversion "
             "-Werror=implicit-function-declaration ");
}

/* The value of an --ext option: the next argument, which is a name and so
   never starts with '-'. A missing one stops the run, where taking the
   next flag as the value lost that flag and failed later for a reason that
   named nothing. */
static const char *ext_option_value(int argc, char **argv, int *i, const char *flag) {
  if (*i + 1 >= argc || argv[*i + 1][0] == '-') {
    fprintf(stderr, "spinel: %s needs a value\n", flag);
    exit(1);
  }
  *i += 2;
  return argv[*i - 1];
}

int main(int argc, char **argv) {
#ifdef SP_WORK_COUNT
  atexit(work_report);
#endif
#if defined(_WIN32)
  SPINEL_PLATFORM_INIT();
#endif
  /* `spinel diff FILE.rb ...`, `spinel bisect FILE.rb ...`: the companion
     tool beside the compiler runs it (tools/diff.rb, built to
     bin/spinel-diff; tools/bisect.rb likewise); the arguments pass through
     untouched, its exit status is the answer. */
  if (argc >= 2 && (sp_streq(argv[1], "diff") || sp_streq(argv[1], "bisect"))) {
    char dir[4096];
    exe_dir(argv[0], dir, sizeof dir);
    char tool[4200];
    snprintf(tool, sizeof tool, "%s/spinel-%s", dir, argv[1]);
    char self[4200];
    snprintf(self, sizeof self, "%s/spinel", dir);
    setenv("SPINEL", self, 0);
    argv[1] = tool;
    execv(tool, argv + 1);
    fprintf(stderr, "spinel: cannot run %s (build it with `make tools`)\n", tool);
    return 4;
  }
  const char *source = NULL;
  const char *output = NULL;
  /* Every --link consumes an argv slot, so argc bounds the list. A fixed
     array dropped the entries past its end without a word: a program whose
     packages carry more than 64 objects linked without the last of them
     (and without the allocator flag spin appends after them). */
  const char **link_extra = malloc(sizeof(const char *) * (size_t)(argc > 0 ? argc : 1));
  int n_link_extra = 0;
  const char *cc_cmd = SPINEL_DEFAULT_CC;
  const char *opt_level = "2";
  /* --target=wasm32-wasi: the program is a WebAssembly module for a WASI
     host (wasmtime, Node's WASI, a browser shim), built by the wasi-sdk's
     clang against the runtime archive in lib/wasm32-wasi/. */
  int target_wasi = 0;
  /* --exceptions=abort (wasm32-wasi only): a runtime without setjmp/longjmp,
     for a host that has no exception-handling proposal. A raise traps; the
     constructs that need a frame to catch it are refused (see lib/sp_setjmp.h). */
  int exc_abort = 0;
  const char *int_overflow = "raise";
  const char *rbs_dir = NULL;
  extern int sp_inline_rbs_enabled, sp_inline_rbs_failed;
  int c_only = 0, stdout_mode = 0, run_mode = 0, dump_ast = 0;
  int print_build = 0;   /* --print-build: emit the build ingredients, run nothing */
  int cc_jobs = 0;       /* --jobs=N: compile the C as N units in parallel (#4847); 0 = auto */
  int emit_rbs = 0, emit_types = 0, emit_symbol_map = 0;
  int debug = 0, line_map = 1, want_g = 0, profile = 0, warn_widen = 0, check_stores = 0;
  /* Accumulated -e source and the program ARGV after the -E boundary. */
  Str eval_src = {0};
  int eval_used = 0;
  char **run_args = NULL;
  int n_run_args = 0;

  g_ruby_description = "spinel " SP_RUBY_VERSION " (" SPINEL_RELEASE " revision " SPINEL_BUILD_REV ")";

  for (int i = 1; i < argc; ) {
    const char *a = argv[i];
    if (!strncmp(a, "--source=", 9))      { source = a + 9; i++; }
    else if (!strncmp(a, "--output=", 9)) { output = a + 9; i++; }
    else if (!strncmp(a, "--cc=", 5))     { cc_cmd = a + 5; i++; }
    else if (!strncmp(a, "--jobs=", 7))   { cc_jobs = atoi(a + 7); i++; }
    else if (!strncmp(a, "--split-parts=", 14)) { g_split_parts = atoi(a + 14); i++; }
    else if (incremental_opt(a))           { i++; }
    else if (!strncmp(a, "--target=", 9)) {
      if (sp_streq(a + 9, "wasm32-wasi")) target_wasi = 1;
      else if (sp_streq(a + 9, "native")) target_wasi = 0;
      else { fprintf(stderr, "spinel: unknown --target %s (wasm32-wasi or native)\n", a + 9); return 1; }
      i++;
    }
    else if (!strncmp(a, "--gc=", 5) || sp_streq(a, "--no-gc")) {
      if (sp_streq(a, "--no-gc") || sp_streq(a + 5, "none")) g_gc_none = 1;
      else if (sp_streq(a + 5, "on")) g_gc_none = 0;
      else { fprintf(stderr, "spinel: unknown --gc %s (on or none)\n", a + 5); return 1; }
      i++;
    }
    else if (!strncmp(a, "--exceptions=", 13)) {
      if (sp_streq(a + 13, "abort")) exc_abort = 1;
      else if (sp_streq(a + 13, "unwind")) exc_abort = 0;
      else { fprintf(stderr, "spinel: unknown --exceptions %s (abort or unwind)\n", a + 13); return 1; }
      i++;
    }
    else if (!strncmp(a, "--rbs=", 6))    { rbs_dir = a + 6; i++; }
    else if (sp_streq(a, "--rbs"))         { if (++i < argc) rbs_dir = argv[i]; i++; }
    else if (sp_streq(a, "--no-inline-rbs")) { sp_inline_rbs_enabled = 0; i++; }
    else if (sp_streq(a, "--disable=frozen-string-literal") ||
             sp_streq(a, "--disable-frozen-string-literal")) {
      fprintf(stderr, "spinel: --disable=frozen-string-literal is not "
                      "supported: string literals are always frozen\n");
      return 1;
    }
    else if (sp_streq(a, "--enable=frozen-string-literal") ||
             sp_streq(a, "--enable-frozen-string-literal")) {
      i++;   /* the default; accepted for CRuby command-line compatibility */
    }
    else if (!strncmp(a, "--int-overflow=", 15)) { int_overflow = a + 15; i++; }
    else if (sp_streq(a, "--int-overflow")) { if (++i < argc) int_overflow = argv[i]; i++; }
    else if (sp_streq(a, "-o"))            { if (++i < argc) output = argv[i]; i++; }
    else if (sp_streq(a, "--link"))        { if (++i < argc) link_extra[n_link_extra++] = argv[i]; i++; }
    else if (sp_streq(a, "-O"))            { if (++i < argc) opt_level = argv[i]; i++; }
    /* `-O2`, the spelling every C compiler takes and the one a build script
       reaches for. Only the separated form was matched, so the joined one
       fell through to the unknown-flag arm below and was DISCARDED: a build
       asking for -O0 silently got the -O2 default, and nothing said so. */
    else if (!strncmp(a, "-O", 2) && a[2])  { opt_level = a + 2; i++; }
    else if (sp_streq(a, "--debug"))       { debug = 1; opt_level = "0"; want_g = 1; i++; }
    else if (sp_streq(a, "-g"))            { debug = 1; want_g = 1; i++; }
    else if (sp_streq(a, "--profile"))     { profile = 1; want_g = 1; i++; }
    else if (sp_streq(a, "--line-map"))    { line_map = 1; i++; }
    else if (sp_streq(a, "--no-line-map")) { line_map = 0; i++; }
    else if (sp_streq(a, "--defer-refusals")) { g_opt_defer_refusals = 1; i++; }
    else if (sp_streq(a, "--warn-widen"))  { warn_widen = 1; i++; }
    else if (sp_streq(a, "--check-stores")) { check_stores = 1; i++; }
    /* keep every GC root, so a suspected miscompile can be bisected against
       the same binary rather than against a different build. */
    else if (sp_streq(a, "--no-root-elision")) { g_no_root_elision = 1; i++; }
    /* keep the per-root registration (one cleanup-attribute root per local)
       instead of the per-function root frame: the same bisecting hatch. */
    else if (sp_streq(a, "--no-root-frame")) { g_no_root_frame = 1; i++; }
    else if (sp_streq(a, "--force")) { g_force_overwrite = 1; i++; }
    else if (sp_streq(a, "--inline-hot"))    { g_inline_hot = 1; i++; }
    /* Compile without forcing the small leaf methods inline: a smaller binary
       and a faster C compile, at the cost of a call per hot-loop method. */
    else if (sp_streq(a, "--no-inline-hot")) { g_inline_hot = 0; i++; }
    else if (sp_streq(a, "--no-write-barrier")) { g_no_write_barrier = 1; i++; }
    /* The same bisecting hatch, one decision at a time (src/decide.c): take
       only the listed decisions, or write down the ones taken. They travel
       in the environment so that a compile some other tool starts, `spinel
       diff` or an oracle script, is restricted the same way. */
    else if (!strncmp(a, "--decisions=", 12) || !strncmp(a, "--decisions-log=", 16)) {
      int log = a[11] == '-';
      const char *path = a + (log ? 16 : 12);
      /* no name is not no restriction: it would compile unrestricted and
         be read as the answer under the list */
      if (!*path) { fprintf(stderr, "spinel: %s needs a file\n", log ? "--decisions-log=" : "--decisions="); return 1; }
      set_env(log ? "SPINEL_DECISIONS_LOG" : "SPINEL_DECISIONS", path);
      i++;
    }
    /* Library emission for host extensions (docs/internals/ext-design.md):
       --ext-init names the host-callable init function (emitted in place of
       main), --ext-entry designates the exported methods. */
    else if (sp_streq(a, "--ext-init"))  { g_ext_init_name = ext_option_value(argc, argv, &i, a); }
    else if (sp_streq(a, "--ext-entry")) { g_ext_entries = ext_option_value(argc, argv, &i, a); }
    else if (sp_streq(a, "--ext-class")) { g_ext_classes = ext_option_value(argc, argv, &i, a); }
    else if (sp_streq(a, "--ext") && i + 1 < argc) { g_ext_target = argv[i + 1]; i += 2; }
    else if (parse_flag(a))                { i++; }
    else if (sp_streq(a, "-c"))            { c_only = 1; i++; }
    else if (sp_streq(a, "--timing"))      { set_env("SP_TIMING", "1"); i++; }
    else if (sp_streq(a, "-I"))            { if (++i < argc) sp_add_feature_root(argv[i]); i++; }
    else if (!strncmp(a, "-I", 2) && a[2]) { sp_add_feature_root(a + 2); i++; }
    else if (sp_streq(a, "-S"))            { stdout_mode = 1; i++; }
    /* every String slot is the handle, the only representation: the
       option that chose it is accepted and ignored */
    else if (sp_streq(a, "--string-repr=handle")) { i++; }
    else if (sp_streq(a, "--print-build")) { print_build = 1; i++; }
    else if (sp_streq(a, "-E"))            { run_mode = 1; i++; }
    else if (sp_streq(a, "--emit-rbs"))    { emit_rbs = 1; i++; }
    else if (sp_streq(a, "--emit-types"))  { emit_types = 1; i++; }
    else if (sp_streq(a, "--plan-check"))  { g_plan_check = 1; i++; }
    else if (sp_streq(a, "--method-table-check")) { g_method_table_check = 1; i++; }
    else if (sp_streq(a, "--arm-check"))   { g_arm_check = 1; i++; }
    else if (sp_streq(a, "--repr-check"))  { g_repr_check = 1; i++; }
    else if (analysis_dump_flag(a))        { i++; }
    else if (sp_streq(a, "--check-traits")) { g_check_traits = 1; i++; }
#ifdef SP_BOP_SHARE_CHECK
    else if (sp_streq(a, "--check-bop-share")) return builtin_ops_share_check() ? 1 : 0;
#endif
    else if (sp_streq(a, "--check-bop-arity")) return builtin_ops_arity_check() ? 1 : 0;
    else if (sp_streq(a, "--check-bop-callbacks")) return builtin_ops_callbacks_check() ? 1 : 0;
    else if (sp_streq(a, "--dump-traits"))  { g_dump_traits = 1; i++; }
    else if (sp_streq(a, "--emit-symbol-map")) { emit_symbol_map = 1; i++; }
    else if (table_mode_of(a))             { dump_ast = table_mode_of(a); i++; }
    else if (sp_streq(a, "-h") || sp_streq(a, "--help")) { usage(); return 0; }
    /* `-v` is the `ruby -v` spelling, so a tool that asks every Ruby it
       drives for its version the same way (ruby/ruby-bench does) gets
       an answer instead of the usage text. */
    else if (sp_streq(a, "--version") || sp_streq(a, "-v")) return print_version(cc_cmd);
    else if (sp_streq(a, "-e")) {
      if (++i < argc) {
        if (eval_used) s_add(&eval_src, "\n");
        s_add(&eval_src, argv[i]);
        eval_used = 1;
      }
      i++;
    }
    else if (sp_streq(a, "--")) {
      i++;
      if (!source && i < argc) { source = argv[i]; i++; }
      if (run_mode) { run_args = &argv[i]; n_run_args = argc - i; }
      break;
    }
    else if (a[0] != '-') {
      /* A positional: the source path. In run mode it ends flag parsing and
         the rest of the line becomes the program's ARGV. */
      if (!source) {
        source = a;
        i++;
        if (run_mode) { run_args = &argv[i]; n_run_args = argc - i; break; }
      }
      else {
        i++;  /* extra positionals are ignored in compile mode */
      }
    }
    else {
      /* Unknown flag. In run mode it is the ARGV boundary (the program may
         take its own flags, and `--` ends ours); in compile mode it is a
         mistake, and silence about it builds something other than what was
         asked for. A mistyped `--int-overflow=promot` produced a default
         raise build with no diagnostic at all -- the compiler answering a
         question nobody asked. */
      if (run_mode) { run_args = &argv[i]; n_run_args = argc - i; break; }
      fprintf(stderr, "spinel: unknown option '%s'\n", a);
      return 1;
    }
  }

  /* -e: materialize the inline source as a temp .rb. */
  char eval_path[4096] = {0};
  if (eval_used) {
    make_temp_path(eval_path, sizeof eval_path, "eval", 0, ".rb");
    if (!write_text_file(eval_path, eval_src.p ? eval_src.p : "")) return 1;
    source = eval_path;
  }
  free(eval_src.p);

  if (!source) { usage(); return 1; }
#if defined(_WIN32)
  source = SPINEL_PLATFORM_PATH(source);
  if (output) output = SPINEL_PLATFORM_PATH(output);
#endif
  /* -O0 / -O1 (and --debug) build the TU unoptimised: the generated program
     asks for larger fiber stacks (#4496). -Os / -Og and anything else keep
     the -O2 assumption. */
  g_opt_level = (opt_level[0] == '0' || opt_level[0] == '1') ? opt_level[0] - '0' : 2;
  if (!file_exists(source)) { fprintf(stderr, "spinel: %s: No such file\n", source); return 1; }

  /* Mode-conflict checks mirror the old driver. */
  if (run_mode && (output || c_only || stdout_mode)) {
    fprintf(stderr, "spinel: -E (run) cannot be combined with -o/-c/-S\n");
    return 2;
  }
  if (incremental_begin(stdout_mode, print_build, emit_rbs, emit_types, emit_symbol_map)) return 1;

  const char *ov_define = NULL;
  if (sp_streq(int_overflow, "raise"))   ov_define = "-DSP_INT_OVERFLOW_MODE_RAISE";
  else if (sp_streq(int_overflow, "wrap"))    ov_define = "-DSP_INT_OVERFLOW_MODE_WRAP";
  else if (sp_streq(int_overflow, "promote")) ov_define = "-DSP_INT_OVERFLOW_MODE_PROMOTE";
  else { fprintf(stderr, "spinel: --int-overflow expects raise|wrap|promote, got '%s'\n", int_overflow); return 2; }
  /* Let the analyzer widen accumulating int locals to bigint more freely in
     promote mode (block-iteration loops, not just `while`). See analyze.h. */
  g_promote_mode = sp_streq(int_overflow, "promote");
  g_wrap_mode = sp_streq(int_overflow, "wrap");
  g_traits_promote = g_promote_mode;
  if (exc_abort && !target_wasi) {
    fprintf(stderr, "spinel: --exceptions=abort is for --target=wasm32-wasi\n");
    return 1;
  }
  g_exc_abort = exc_abort;

  /* Base name for default output paths. */
  char basename[1024];
  if (eval_used) {
    strcpy(basename, "a");
  }
  else {
    const char *b = source;
    const char *sl = strrchr(source, '/');
    if (sl) b = sl + 1;
    strncpy(basename, b, sizeof basename - 1);
    basename[sizeof basename - 1] = '\0';
    char *dot = strstr(basename, ".rb");
    if (dot && dot[3] == '\0') *dot = '\0';
  }

  /* --rbs: extract advisory seeds via the sibling spinel_rbs_extract binary
     and hand them to the analyzer through g_opt_rbs_seed. An empty result is a
     silent no-op; a missing extractor is warned about, since --rbs was asked for
     explicitly and the seeds would otherwise vanish without a trace (#1792), and
     a file the extractor cannot parse stops the build (#11323). */
  char seed_path[4096] = {0};
  if (rbs_dir && *rbs_dir) {
    char dir[4096], extractor[4096];
    exe_dir(argv[0], dir, sizeof dir);
    snprintf(extractor, sizeof extractor, "%s%cspinel_rbs_extract%s", dir, PATH_SEP, EXE_SUFFIX);
    if (!file_exists(extractor)) {
      fprintf(stderr, "spinel: warning: --rbs %s requested but spinel_rbs_extract "
              "was not found beside the compiler; .rbs type seeds are unavailable "
              "(rebuild with `make deps`, or reinstall a toolchain that ships it)\n", rbs_dir);
    }
    if (file_exists(extractor)) {
      make_temp_path(seed_path, sizeof seed_path, "seed", 0, ".txt");
      Str cmd = {0};
      s_add_arg(&cmd, extractor);
      /* each seed preceded by its .rbs position, which a disagreement with
         an inline RBS annotation names */
      if (sp_inline_rbs_enabled) s_add_arg(&cmd, "--positions");
      s_add_arg(&cmd, rbs_dir);
      s_add(&cmd, "> ");
      s_add_arg(&cmd, seed_path);
      int rc = system(cmd.p);
      free(cmd.p);
      /* a signature file that did not parse (or could not be read) loses
         every signature in it, the ones that would stop the build among
         them: the build stops instead of going on without them (#11323) */
      if (rc != 0) {
        remove(seed_path);
        fprintf(stderr, "spinel: --rbs %s: a signature file above did not parse or could not be read, "
                "so its signatures would be lost; fix it or move it out of %s\n", rbs_dir, rbs_dir);
        return 1;
      }
      if (file_exists(seed_path)) {
        g_opt_rbs_seed = strdup(seed_path);
        atexit(rbs_seed_remove);
      }
      else {
        remove(seed_path);
        seed_path[0] = '\0';
      }
    }
  }

  /* Source mapping: codegen emits #line under --debug or --line-map.
     --debug/-g use the fuller debug path; otherwise line-map (on by
     default) just adds #line. The parser stamps node positions in every
     compile (g_opt_positions): the analysis reads them, and a node a rewrite
     made has none (cplan_nil's unset temp), so --no-line-map suppresses only
     the #line directives and decides what the default compile decides. The
     emit modes that need positions force the debug mode below. */
  if (debug) g_opt_debug = 1;
  else if (line_map) g_opt_line_map = 1;
  g_opt_positions = 1;
  /* --warn-widen places each warning at its slot, which needs the parser's
     positions: forced past --no-line-map, as --emit-types forces them. */
  if (warn_widen) { g_opt_line_map = 1; g_opt_warn_widen = 1; }
  /* --check-stores reports each store at its Ruby line, so it needs them too */
  if (check_stores) { g_opt_line_map = 1; g_opt_check_stores = 1; }
  /* A decision key names its site by position: forced the same way. */
  if (refuse_log_overwrite(getenv("SPINEL_DECISIONS_LOG"))) return 1;
  if (decide_setup()) g_opt_line_map = 1;

  /* Analyze-only emit modes write their artifact from inside codegen_program
     and produce an empty translation unit; the output path goes with them. */
  char emit_out[4096];
  if (emit_rbs) {
    snprintf(emit_out, sizeof emit_out, "%s", output ? output : basename);
    if (!output) { strncat(emit_out, ".rbs", sizeof emit_out - strlen(emit_out) - 1); }
    g_opt_emit_rbs = strdup(emit_out);
  }
  else if (emit_types) {
    snprintf(emit_out, sizeof emit_out, "%s", output ? output : basename);
    if (!output) { strncat(emit_out, ".types.json", sizeof emit_out - strlen(emit_out) - 1); }
    /* The positions the JSON carries need the parser's line map, not the
       debug build: --debug also turns codegen's g_debug on, which drops
       `static`/always_inline from every method and switches the backtrace
       substrate on, so the C of `--emit-types -S` was the debug compile's,
       not what -S alone emits. Forced past --no-line-map, since the JSON
       is nothing without positions. */
    g_opt_line_map = 1;
    g_opt_emit_types = strdup(emit_out);
    /* `--emit-types -o x.json -S`: the JSON and the C of the same compile,
       in one run rather than two (a consumer showing both, rubys/spinel-ide) */
    if (stdout_mode) g_opt_emit_types_keep_c = 1;
  }
  else if (emit_symbol_map) {
    snprintf(emit_out, sizeof emit_out, "%s", output ? output : basename);
    if (!output) { strncat(emit_out, ".symbols.json", sizeof emit_out - strlen(emit_out) - 1); }
    g_opt_debug = 1;
    g_opt_emit_symbol_map = strdup(emit_out);
  }
  if (profile) {
    /* --profile also writes the symbol map next to the binary (offline
       symbolization input for perf tooling); codegen falls through to the
       real build after writing it (#1336). */
    char psym[4096];
    snprintf(psym, sizeof psym, "%s.symbols.json", output ? output : basename);
    g_opt_profile_symbol_map = strdup(psym);
  }

  /* ---------- pipeline: parse -> AST -> codegen ---------- */
  /* The target's integer width is the pointer width of the C compiler that
     builds the program: this binary's own unless --cc named another (a
     32-bit build on a 64-bit host, `--cc='cc -m32'`), which is asked. */
  extern int sp_target_int_bits;
  int target_i386 = 0;
  /* wasm32-wasi: the C compiler is the wasi-sdk's clang unless --cc named
     one, found through $WASI_SDK (or WASI_SDK_PATH, the sdk's own name for
     it), else /opt/wasi-sdk, where its installer puts it. */
  char wasi_cc[4096];
  if (target_wasi && sp_streq(cc_cmd, "cc")) {
    const char *sdk = getenv("WASI_SDK");
    if (!sdk || !*sdk) sdk = getenv("WASI_SDK_PATH");
    if (!sdk || !*sdk) sdk = "/opt/wasi-sdk";
    snprintf(wasi_cc, sizeof wasi_cc, "%s/bin/clang", sdk);
    if (!file_exists(wasi_cc)) {
      fprintf(stderr, "spinel: --target=wasm32-wasi needs the wasi-sdk: %s is not there (set WASI_SDK to where it is)\n", wasi_cc);
      return 1;
    }
    cc_cmd = wasi_cc;
  }
  /* A 32-bit spinel on a 64-bit host (`make CC='cc -m32'`) ships a 32-bit
     runtime archive beside itself, and the default `cc` there is 64-bit: the
     program is told -m32, so the toolchain it saw is the one it uses. */
  const char *cc_width_flag = "";
  if (!sp_streq(cc_cmd, "cc") || sizeof(void *) == 4) {
    char ccq[1024];
    snprintf(ccq, sizeof ccq, "%s -E -dM -x c /dev/null 2>/dev/null", cc_cmd);
    FILE *fp = popen(ccq, "r");
    if (fp) {
      char line[512];
      while (fgets(line, sizeof line, fp)) {
        static const char k[] = "#define __SIZEOF_POINTER__ ";
        if (!strncmp(line, k, sizeof k - 1)) { int b = atoi(line + sizeof k - 1) * 8; if (b == 32 || b == 64) sp_target_int_bits = b; }
        if (!strncmp(line, "#define __i386__ ", 17)) target_i386 = 1;
      }
      pclose(fp);
    }
    if (sp_streq(cc_cmd, "cc") && sp_target_int_bits == 64) {
      cc_width_flag = "-m32 ";
      sp_target_int_bits = 32;
#if defined(__i386__)
      target_i386 = 1;
#endif
    }
  }
#if defined(__i386__)
  else target_i386 = 1;
#endif
  double tm_front = sp_timing_now();
  NodeTable *nt = sp_parse_file(source, argv[0]);
  if (eval_path[0]) remove(eval_path);
  if (!nt) {
    /* an inline RBS error has been reported at the annotation */
    if (!sp_inline_rbs_failed && !sp_parse_reported) fprintf(stderr, "spinel: parse failed for '%s'\n", source);
    return 1;
  }

  if (dump_ast) { int rc = run_table_mode(nt, dump_ast); nt_free(nt); return rc; }
  sp_timing_end(tm_front, "frontend", "");

  /* the CRuby shim's feature name (Init_<feature>, the require name) derives
     from the output basename; codegen needs it while generating the shim */
  char ext_feat[256];
  if (g_ext_target && output) {
    const char *bs = strrchr(output, '/');
    bs = bs ? bs + 1 : output;
    size_t fl = 0;
    for (const char *p = bs; *p && fl < sizeof ext_feat - 1; p++) {
      if (*p == '.') break;
      ext_feat[fl++] = *p;
    }
    ext_feat[fl] = 0;
    if (fl) g_ext_feature = ext_feat;
  }
  double tm_cg = sp_timing_now();
  char *csrc = codegen_program(nt);
  sp_timing_end(tm_cg, "codegen_program", "");
  nt_free(nt);
  decide_write_log();
  if (!csrc) { fprintf(stderr, "spinel: codegen failed\n"); return 1; }

  /* Emit modes already wrote their file; codegen returned empty C, unless
     --emit-types was asked for the C on stdout as well. */
  if (emit_rbs || emit_types || emit_symbol_map) {
    fprintf(stderr, "Wrote %s\n", emit_out);
    if (emit_types && stdout_mode) fputs(csrc, stdout);
    free(csrc);
    return 0;
  }

  if (stdout_mode) { fputs(csrc, stdout); free(csrc); return 0; }

  if ((g_ext_init_name || g_ext_entries || g_ext_classes) && !c_only) {
    fprintf(stderr, "spinel: --ext-init/--ext-entry emit a LIBRARY: pass -c "
                    "(the host's build compiles and links the result)\n");
    free(csrc);
    return 1;
  }
  if ((g_ext_entries || g_ext_classes) && !g_ext_init_name) {
    fprintf(stderr, "spinel: --ext-entry and --ext-class need --ext-init (the host must "
                    "initialize the runtime before calling an entry)\n");
    free(csrc);
    return 1;
  }
  if (g_ext_target && !sp_streq(g_ext_target, "cruby")) {
    fprintf(stderr, "spinel: --ext %s: only `cruby` is generated in v1 "
                    "(the Layer-1 library serves any other host by hand)\n",
            g_ext_target);
    free(csrc);
    return 1;
  }

  /* --exceptions=abort: what leaves through a frame by longjmp is refused,
     as the analysis refused rescue, retry and catch: a break out of a block
     that is not spliced in, a return out of a proc, a Fiber's switch. The
     runtime's text names each (sp_brk_throw, sp_proc_return, sp_Fiber_).
     Before the C is written: `-c` refuses as the build does. */
  if (exc_abort) {
    static const struct { const char *tok, *what; } nl[] = {
      { "sp_brk_throw(", "a break out of a block that is not spliced into its method" },
      { "sp_proc_return(", "a return out of a proc" },
      { "sp_Fiber_", "Fiber" },
    };
    for (size_t k = 0; k < sizeof nl / sizeof nl[0]; k++)
      if (strstr(csrc, nl[k].tok)) {
        fprintf(stderr, "spinel: %s needs exception handling, which --exceptions=abort leaves out\n", nl[k].what);
        free(csrc);
        return 1;
      }
  }
  /* Decide where the generated C goes. */
  char c_path[4096];
  int c_is_temp = 0;
  if (c_only && !inc_flags.on) {
    snprintf(c_path, sizeof c_path, "%s", output ? output : basename);
    if (!output) strncat(c_path, ".c", sizeof c_path - strlen(c_path) - 1);
  }
  else {
    make_temp_path(c_path, sizeof c_path, "out", 0, ".c");
    c_is_temp = 1;
  }
  if (c_only && !inc_flags.on && refuse_overwrite(c_path)) { free(csrc); return 1; }
  double tm_wr = sp_timing_now();
  if (!write_text_file(c_path, csrc)) { free(csrc); return 1; }
  sp_timing_end(tm_wr, "write_c", "");

  if (c_only && !inc_flags.on) {
    /* With --print-build as well, the C is written and the ingredient report
       follows from this same run: `spin pack` used to invoke the compiler
       twice, once for the C and once for the report, and the second run
       repeated the whole front end (parse, analyze, codegen: minutes on a
       large tree, half of a CI job) to print a few lines that were already
       known at the end of the first (#4456). The cc step below is skipped
       either way, so the fall-through is the report and nothing more. */
    fprintf(stderr, "Wrote %s\n", c_path);
    if (g_ext_init_name && g_ext_header_text) {
      char h_path[4096];
      snprintf(h_path, sizeof h_path, "%s", c_path);
      size_t hl = strlen(h_path);
      if (hl > 2 && sp_streq(h_path + hl - 2, ".c")) h_path[hl - 1] = 'h';
      else strncat(h_path, ".h", sizeof h_path - hl - 1);
      if (!write_text_file(h_path, g_ext_header_text)) { free(csrc); return 1; }
      fprintf(stderr, "Wrote %s\n", h_path);
      if (g_ext_shim_text) {
        char s_path[4096];
        size_t cl = strlen(c_path);
        snprintf(s_path, sizeof s_path, "%.*s_ext.c",
                 (int)(cl > 2 && sp_streq(c_path + cl - 2, ".c") ? cl - 2 : cl), c_path);
        if (!write_text_file(s_path, g_ext_shim_text)) { free(csrc); return 1; }
        fprintf(stderr, "Wrote %s\n", s_path);
      }
    }
    if (!print_build) { free(csrc); return 0; }
  }

  /* ---------- link: cc <generated C> -> native binary ---------- */
  char lib_dir[4096];
  resolve_lib_dir(argv[0], lib_dir, sizeof lib_dir);

  Str ffi_links = {0}, ffi_cflags = {0}, native_objs = {0};
  scrape_ffi_markers(csrc, "/* SPINEL_LINK: ", &ffi_links);
  scrape_ffi_markers(csrc, "/* SPINEL_CFLAGS: ", &ffi_cflags);
  /* native_obj (Path B): root-relative carried-C object paths to link, one
     package's C linked only when its require appeared. Resolved against the
     compiler's base dir (beside lib/ and packages/) at the link step below. */
  scrape_ffi_markers(csrc, "/* SPINEL_LINK_OBJ: ", &native_objs);
  /* A program that uses Thread/Mutex/Queue/... links the -DSP_THREADS runtime
     variant (libspinel_rt_mt.a) plus -lpthread; everything else links the
     byte-identical single-threaded archive. See codegen's SPINEL_USES_THREADS
     marker and the two-variant build in the Makefile. */
  int uses_threads = strstr(csrc, "/* SPINEL_USES_THREADS */") != NULL;
  /* A frame past the fiber stack crashes on the guard page rather than
     overflowing detectably, and always_inline bypasses the C compiler's own
     large-frame brake -- ask for the warning back (#3913). */
  int fiber_frame_guard = strstr(csrc, "/* SPINEL_FIBER_FRAME_GUARD */") != NULL;
  int uses_crypt = strstr(csrc, "sp_str_crypt(") != NULL;  /* String#crypt: link -lcrypt */
  int uses_cext, gc_none;
  const char *rt_lib = runtime_archive(csrc, uses_threads, target_wasi, exc_abort, &uses_cext, &gc_none);
  free(csrc);
  if (!rt_lib) { if (c_is_temp) remove(c_path); return 1; }
  /* wasm has no threads without SharedArrayBuffer and no stack switching
     without asyncify: the runtime archive there is the single-threaded one,
     and a program that reaches for Thread is refused here, where the reason
     can be said, rather than at the link. */
  if (target_wasi) {
    if (uses_threads) {
      fprintf(stderr, "spinel: Thread, Mutex, Queue and ConditionVariable are not available on wasm32-wasi\n");
      if (c_is_temp) remove(c_path);
      return 1;
    }
    rt_lib = exc_abort ? (gc_none ? "wasm32-wasi/libspinel_rt_noexc_nogc.a" : "wasm32-wasi/libspinel_rt_noexc.a")
                       : (gc_none ? "wasm32-wasi/libspinel_rt_nogc.a" : "wasm32-wasi/libspinel_rt.a");
    if (run_mode) {
      fprintf(stderr, "spinel: -E cannot run a wasm32-wasi module; build it and run it with wasmtime\n");
      if (c_is_temp) remove(c_path);
      return 1;
    }
  }

  /* Output binary path: -E uses a temp so the cwd stays clean. */
  char bin_path[4096];
  int bin_is_temp = 0;
  if (run_mode) {
    char rdir[4096];
    snprintf(rdir, sizeof rdir, "run_%s", basename);
    make_temp_path(bin_path, sizeof bin_path, rdir, 0, EXE_SUFFIX);
    bin_is_temp = 1;
  }
  else {
    snprintf(bin_path, sizeof bin_path, "%s%s", output ? output : basename, output ? "" : target_wasi ? ".wasm" : EXE_SUFFIX);
  }

  Str cmd = {0};
  /* Filled alongside cmd, one ingredient at a time, so the two cannot say
     different things: a category line is written at the same statement that
     puts the flag on the command line. */
  Str bi = {0};
  char tmp[8192];
  s_add(&cmd, cc_cmd);
  s_add(&cmd, " ");
  s_add(&cmd, cc_width_flag);
  snprintf(tmp, sizeof tmp, "-O%s ", opt_level); s_add(&cmd, tmp);
  s_add(&cmd, "-Wno-all -ffunction-sections -fdata-sections ");
  /* A Float `a * b - c` is two IEEE operations in Ruby, each rounded to
     double. clang's default (-ffp-contract=on; gcc's is =fast) fuses the
     pair written in one C expression into a single fused multiply-add
     that rounds once, and the program answers a double a few ULP from
     CRuby's with nothing to say so. common.mk's SEC_FLAGS carries the same
     flag for the runtime and the test harness. */
  s_add(&cmd, "-ffp-contract=off ");
  bi_put(&bi, "cflag", "-ffp-contract=off");
  /* Functions and loop heads on a cache line. A hot loop's speed otherwise
     depends on where the linker happened to put it, which moves with any
     unrelated change to the program: optcarrot swung ~5% between builds of
     the same C. Aligned, it is +4.7% (1260 -> 1319 fps), the benchmark set
     is 0.8% faster on the geometric mean, campfire's req/s is unchanged,
     and the binary is 2% larger. Both gcc and clang take the spelling; a
     wasm module has no cache line to aim at. */
  if (!target_wasi) {
    s_add(&cmd, "-falign-functions=64 -falign-loops=64 ");
    bi_put(&bi, "cflag", "-falign-functions=64"); bi_put(&bi, "cflag", "-falign-loops=64");
  }
  /* A 32-bit target gets what common.mk gives the runtime there: 64-bit
     time_t and file offsets, and SSE arithmetic on i386 (the x87 unit rounds
     every intermediate at 80 bits, and 3.7.round(1) came out 3.8). */
  if (cc_width_flag[0]) bi_put(&bi, "cflag", "-m32");
  if (sp_target_int_bits == 32 && !target_wasi) {
    s_add(&cmd, "-D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64 ");
    bi_put(&bi, "define", "-D_TIME_BITS=64"); bi_put(&bi, "define", "-D_FILE_OFFSET_BITS=64");
    if (target_i386) { s_add(&cmd, "-msse2 -mfpmath=sse "); bi_put(&bi, "cflag", "-msse2"); bi_put(&bi, "cflag", "-mfpmath=sse"); }
  }
  miscompile_errors(&cmd);
  /* The error list is spinel's output, not cc's: the same flag assembly
     remaps every location onto the Ruby line and promotes two pointer
     warnings to errors. cc's own default cap (20 for clang) truncated that
     list with a "too many errors" line AFTER twenty had scrolled past, so a
     build's remaining work read 30-40% smaller than it was. Lift the cap --
     0 means unlimited in both -- in each compiler's own spelling; an unknown
     -f flag is an error, not a warning (#4152). */
  s_add(&cmd, cc_is_clang(cc_cmd) ? "-ferror-limit=0 " : "-fmax-errors=0 ");
  /* With #line on, cc reports against the .rb -- but only the LINE is remapped.
     The column is the C one, and a whole Ruby line is emitted as one C line, so
     it routinely lands hundreds of columns past the end of the source line it
     is printed under. cc then pads its caret line out to that column, so every
     error costs a line of whitespace as wide as the C expression was long.
     Ask for no column at all: `repro.rb:12: error: ...` claims only what is
     true (#4097). The caret goes with it -- cc draws it from the same column,
     so it points at nothing and pads to that width whatever the location says;
     the source-quote line is cc's only way to carry it and goes too, leaving
     the file and line the message already names.

     -fno-show-column is a flag both take. The caret needs asking for
     separately on BOTH, in each one's own spelling: gcc drops the quote line
     with the column only where #line has remapped the location, since it then
     has no file it can read the source line out of, and every other
     diagnostic -- a note from a header, a line of the generated C that no
     #line covers -- still draws one from the generated file, where a line is
     as wide as a whole Ruby statement. The two spellings are not
     interchangeable and an unknown -f flag is an error rather than a warning,
     so which one to pass is a question, and cc_is_clang() is where it is put
     (#4097 passed gcc's to clang and left the build broken for every clang,
     macOS CI included). */
  if (debug || line_map) {
    s_add(&cmd, "-fno-show-column ");
    s_add(&cmd, cc_is_clang(cc_cmd) ? "-fno-caret-diagnostics "
                                    : "-fno-diagnostics-show-caret ");
  }
  if (fiber_frame_guard) s_add(&cmd, "-Wframe-larger-than=65536 ");
  runtime_includes(&cmd, &bi, lib_dir, uses_cext);
  /* wasm32-wasi: what common.mk gives the runtime archive there (see its
     WASI_CFLAGS): lib/wasi's stand-ins for the POSIX headers wasi-libc
     leaves out, wasi-libc's emulations of signals, mmap and process clocks,
     and setjmp/longjmp through the standard exception-handling instructions
     (the C compiler's default is the legacy encoding, which the engines
     refuse). */
  if (target_wasi) {
    snprintf(tmp, sizeof tmp, "-I\"%s%cwasi\" ", lib_dir, PATH_SEP); s_add(&cmd, tmp);
    { char wi[4096]; snprintf(wi, sizeof wi, "%s%cwasi", lib_dir, PATH_SEP); bi_put(&bi, "include", wi); }
    static const char *const wdefs[] = { "-D_WASI_EMULATED_SIGNAL", "-D_WASI_EMULATED_PROCESS_CLOCKS", "-D_WASI_EMULATED_GETPID", "-D_WASI_EMULATED_MMAN" };
    for (size_t wi = 0; wi < sizeof wdefs / sizeof wdefs[0]; wi++) { s_add(&cmd, wdefs[wi]); s_add(&cmd, " "); bi_put(&bi, "define", wdefs[wi]); }
    static const char *const wcfl[] = { "-mllvm", "-wasm-enable-sjlj", "-mllvm", "-wasm-use-legacy-eh=false" };
    /* --exceptions=abort: no sjlj lowering (it is what needs the host's
       exception handling) and the runtime's abort form of setjmp/longjmp */
    if (!exc_abort)
      for (size_t wi = 0; wi < sizeof wcfl / sizeof wcfl[0]; wi++) { s_add(&cmd, wcfl[wi]); s_add(&cmd, " "); bi_put(&bi, "cflag", wcfl[wi]); }
    if (exc_abort) { s_add(&cmd, "-DSP_EXC_ABORT "); bi_put(&bi, "define", "-DSP_EXC_ABORT"); }
  }
#if defined(_WIN32)
  if (!target_wasi) add_platform_flags(&cmd, &bi, SPINEL_PLATFORM_CFLAGS, lib_dir);
#endif
  /* Compile the generated TU with the same threading define as the mt runtime
     archive it links, so the per-worker SP_TLS globals (sp_gc_roots, ...) it
     references through the runtime headers get the matching thread-local
     storage class. initial-exec keeps those reads a single segment load. */
  if (uses_threads) {
    s_add(&cmd, "-DSP_THREADS -ftls-model=initial-exec ");
    bi_put(&bi, "define", "-DSP_THREADS");
    /* Not an optimisation the caller may drop: the generated TU and the mt
       archive have to agree on the storage class of the per-worker globals. */
    bi_put(&bi, "cflag", "-ftls-model=initial-exec");
  }
  if (ffi_cflags.p) s_add(&cmd, ffi_cflags.p);
  bi_put_toks(&bi, "cflag", ffi_cflags.p);
  /* Every flag that changes how the generated C compiles goes before the
     source: a split build (cc_split_build) compiles its parts with the flags
     in front of the source only, and one placed after it reached the link
     alone. The overflow mode there left each part with the runtime header's
     raise-mode Integer arithmetic, so an --int-overflow=promote program of a
     few MB raised RangeError where it should have made a Bignum. */
  s_add(&cmd, ov_define); s_add(&cmd, " ");
  if (want_g) s_add(&cmd, "-g ");
  if (profile) s_add(&cmd, "-fno-omit-frame-pointer ");
  size_t cc_src_at = cmd.p ? strlen(cmd.p) : 0;   /* the flags before the source */
  s_add_arg(&cmd, c_path);
  size_t cc_src_end = cmd.p ? strlen(cmd.p) : 0;
  bi_put(&bi, "source", c_path);
  /* --link objects/archives sit between the generated TU and the runtime
     archive: they reference sp_ runtime symbols, and ld resolves left to
     right. --link -l flags go after -lm below, where DSOs belong. */
  for (int li = 0; li < n_link_extra; li++) {
    if (strncmp(link_extra[li], "-l", 2) == 0) continue;
    /* A threaded program needs the package's threaded object for the same
       reason the runtime archive has one: an object built without
       -DSP_THREADS reads the runtime's per-worker globals as non-TLS and the
       link fails. A package that carries both variants names the threaded one
       `<stem>_mt.<ext>`; take it when it is there, and the plain one when it
       is not. Without this only the bundled packages -- whose objects arrive
       through native_obj, where the same rule already applied -- could be
       linked into a threaded program (#4032). */
    const char *in = link_extra[li];
    char mtv[4096]; mtv[0] = 0;
    if (uses_threads) {
      const char *dot = strrchr(in, '.');
      const char *slash = strrchr(in, '/');
      if (dot && (!slash || dot > slash) &&
          (strcmp(dot, ".o") == 0 || strcmp(dot, ".a") == 0)) {
        size_t stem = (size_t)(dot - in);
        if (stem < 6 || strncmp(dot - 3, "_mt", 3) != 0)
          snprintf(mtv, sizeof mtv, "%.*s_mt%s", (int)stem, in, dot);
      }
    }
    { const char *chosen = (mtv[0] && access(mtv, F_OK) == 0) ? mtv : in;
      s_add_arg(&cmd, chosen);
      bi_put(&bi, "link", chosen); }
  }
  /* native_obj carried-C objects: resolve each root-relative path against the
     base dir (lib_dir minus its trailing "/lib", where packages/ also lives)
     and add as a link input, before the runtime archive. Try the dev-tree
     "<base>/../" fallback like the require resolver. Same left-to-right rule:
     they reference sp_ runtime symbols resolved from the archive that follows. */
  if (native_objs.p && native_objs.p[0]) {
    int blen = (int)strlen(lib_dir);
    if (blen >= 4 && strcmp(lib_dir + blen - 4, "/lib") == 0) blen -= 4;
    char *toks = strdup(native_objs.p);
    for (char *t = strtok(toks, " "); t; t = strtok(NULL, " ")) {
      /* Dedup against --link inputs: spin links a package's carried C from its
         own cache while the package also declares it via native_obj -- both on
         the link line is a multiple-definition error. The same basename in an
         explicit --link object means it is already covered. */
      const char *tb = strrchr(t, '/'); tb = tb ? tb + 1 : t;
      int dup = 0;
      for (int li = 0; li < n_link_extra && !dup; li++) {
        const char *lb = strrchr(link_extra[li], '/');
        lb = lb ? lb + 1 : link_extra[li];
        /* compare with the `_mt` variant marker removed on both sides: the
           same package object can arrive as sp_x.o here and sp_x_mt.o there,
           and both on the link line is a multiple definition */
        char ab[256], bb[256];
        s_strip_mt(lb, ab, sizeof ab);
        s_strip_mt(tb, bb, sizeof bb);
        if (strcmp(ab, bb) == 0) dup = 1;
      }
      if (dup) continue;
      char op[4096];
      /* A threaded program's TU and runtime archive are built with
         -DSP_THREADS, which makes the runtime's per-worker globals
         thread-local; a package object compiled without it references them as
         non-TLS and the link fails. Prefer the `_mt` variant, falling back to
         the plain one for a package (or an installed tree) that has none. */
      char tmt[4096]; tmt[0] = 0;
      if (uses_threads || target_wasi) {
        /* a wasm32-wasi program links the package's wasm object, `<stem>_wasi.o`
           (the Makefile's wasm-rt target builds the bundled ones) */
        size_t tl = strlen(t);
        if (tl > 2 && strcmp(t + tl - 2, ".o") == 0)
          snprintf(tmt, sizeof tmt, "%.*s%s.o", (int)(tl - 2), t, uses_threads ? "_mt" : exc_abort ? "_wasi_noexc" : "_wasi");
      }
      const char *cands[2] = { tmt[0] ? tmt : t, tmt[0] ? t : NULL };
      int placed = 0;
      for (int ci = 0; ci < 2 && !placed; ci++) {
        if (!cands[ci]) break;
        snprintf(op, sizeof op, "%.*s/%s", blen, lib_dir, cands[ci]);
        if (access(op, F_OK) != 0)
          snprintf(op, sizeof op, "%.*s/../%s", blen, lib_dir, cands[ci]);
        if (access(op, F_OK) == 0) placed = 1;
      }
      s_add_arg(&cmd, op);
      bi_put(&bi, "link", op);
    }
    free(toks);
  }
  snprintf(tmp, sizeof tmp, "\"%s%c%s\" ", lib_dir, PATH_SEP, rt_lib); s_add(&cmd, tmp);
  { char rtp[4096]; snprintf(rtp, sizeof rtp, "%s%c%s", lib_dir, PATH_SEP, rt_lib); bi_put(&bi, "runtime", rtp); }
  /* -lm AFTER the archive: ld processes inputs left to right and (with the
     GNU default --as-needed) drops a DSO no preceding input references.
     sp_format.o pulls in sqrt/sin/cos, so libm must follow libspinel_rt.a. */
  s_add(&cmd, "-lm ");
  bi_put(&bi, "lib", "-lm");
  if (target_wasi) {
    /* The main thread's stack is linear memory too; 8 MB is the native
       default. A linker flag, so it follows the source: a split build
       compiles each part with the flags before it, where clang warns that
       a -Wl is unused. */
    s_add(&cmd, "-Wl,-z,stack-size=8388608 "); bi_put(&bi, "lib", "-Wl,-z,stack-size=8388608");
    /* the emulations the defines above asked for, and sjlj's runtime */
    static const char *const wlibs[] = { "-lsetjmp", "-lwasi-emulated-signal", "-lwasi-emulated-process-clocks", "-lwasi-emulated-getpid", "-lwasi-emulated-mman" };
    /* --exceptions=abort leaves out the first, -lsetjmp */
    for (size_t wi = exc_abort ? 1 : 0; wi < sizeof wlibs / sizeof wlibs[0]; wi++) { s_add(&cmd, wlibs[wi]); s_add(&cmd, " "); bi_put(&bi, "lib", wlibs[wi]); }
  }
  else add_host_libs(&cmd, &bi, uses_crypt);
  for (int li = 0; li < n_link_extra; li++)
    if (strncmp(link_extra[li], "-l", 2) == 0) {
      s_add(&cmd, link_extra[li]); s_add(&cmd, " ");
      bi_put(&bi, "lib", link_extra[li]);
    }
  if (uses_threads) { s_add(&cmd, "-lpthread "); bi_put(&bi, "lib", "-lpthread"); }
#if defined(_WIN32)
  if (!target_wasi) add_platform_flags(&cmd, &bi, SPINEL_PLATFORM_LIBS, lib_dir);
#endif
  bi_put(&bi, "define", ov_define);
#if !defined(__APPLE__)
  if (debug && !target_wasi) s_add(&cmd, "-rdynamic ");  /* ELF: name user frames in backtraces */
#endif
  /* An ffi_lib name whose archive already arrived as an explicit --link
     input (spin's [[build]] artifacts) is satisfied: also emitting the -l
     flag would make ld search the system paths for a library that only
     exists in the build cache. A --link basename lib<name>.(a|so) counts as
     providing -l<name>, so one package source serves both worlds -- the
     Makefile's -L via ffi_cflags and spin's absolute --link paths. */
  /* The OpenSSL library directory the build probed on a host whose
     OpenSSL is keg-only (Homebrew): the package's -lssl/-lcrypto resolve
     there, and an ELF binary finds it again at run time (#7191). */
  if (ffi_links.p && SPINEL_OPENSSL_LIBDIR[0] &&
      (strstr(ffi_links.p, "-lssl") || strstr(ffi_links.p, "-lcrypto"))) {
    char ld[1100];
    snprintf(ld, sizeof ld, "-L%s", SPINEL_OPENSSL_LIBDIR);
    s_add_arg(&cmd, ld); s_add(&cmd, " "); bi_put(&bi, "lib", ld);
#if !defined(__APPLE__)
    if (!target_wasi) {
      snprintf(ld, sizeof ld, "-Wl,-rpath,%s", SPINEL_OPENSSL_LIBDIR);
      s_add_arg(&cmd, ld); s_add(&cmd, " "); bi_put(&bi, "lib", ld);
    }
#endif
  }
  if (ffi_links.p) {
    char *ltoks = strdup(ffi_links.p);
    for (char *t = strtok(ltoks, " "); t; t = strtok(NULL, " ")) {
      int covered = 0;
      if (strncmp(t, "-l", 2) == 0) {
        char want_a[512], want_so[512];
        snprintf(want_a, sizeof want_a, "lib%s.a", t + 2);
        snprintf(want_so, sizeof want_so, "lib%s.so", t + 2);
        for (int li = 0; li < n_link_extra && !covered; li++) {
          const char *lb = strrchr(link_extra[li], '/');
          lb = lb ? lb + 1 : link_extra[li];
          if (strcmp(lb, want_a) == 0 || strcmp(lb, want_so) == 0) covered = 1;
        }
      }
      if (!covered) { s_add(&cmd, t); s_add(&cmd, " "); bi_put(&bi, "lib", t); }
    }
    free(ltoks);
  }
  /* The section GC flag is the LINKER's, not the host's: a wasm module is
     linked by wasm-ld whatever the host, and lld takes --gc-sections only
     (ld64's -dead_strip is an unknown argument to it, #4519). */
#if defined(__APPLE__)
  if (target_wasi) s_add(&cmd, "-Wl,--gc-sections ");
  else s_add(&cmd, "-Wl,-dead_strip ");
#else
  s_add(&cmd, "-Wl,--gc-sections ");
#endif
  s_add(&cmd, "-o ");
  s_add_arg(&cmd, bin_path);
  free(ffi_links.p);
  free(ffi_cflags.p);

  /* `--print-build`: say what the program REQUIRES instead of building it. A
     build recipe generated elsewhere -- `spin pack`'s Makefile -- is derived
     from these lines rather than from a second copy of the same knowledge. A
     copy drifts, and a drifted copy breaks only on the machine the pack was
     sent to, which is the least diagnosable place for it. What is deliberately
     NOT here is the compiler and how this build was run: a recipient
     cross-compiling for another target picks their own cc and their own
     optimisation, and the ingredients below are what has to survive that. */
  if (print_build) {
    fputs(bi.p ? bi.p : "", stdout); free(cmd.p); free(bi.p);
    if (c_is_temp) remove(c_path);   /* the report was the point; the C was not asked for */
    return 0;
  }
  free(bi.p);
  int cc_rc = -1;
  /* -c --incremental: the unit set is the output; nothing is compiled */
  if (c_only && inc_flags.on) { cc_rc = incremental_write_dir(cmd.p, cc_src_at, cc_src_end, c_path, output, basename, want_g, cc_cmd); free(cmd.p); return cc_rc; }
  double tm_ccall = sp_timing_now();
  cc_rc = cc_native_build(cmd.p, cc_src_at, cc_src_end, c_path, cc_jobs, target_wasi, want_g, cc_cmd);
  sp_timing_end(tm_ccall, "cc_total", "");
  free(cmd.p);
  if (cc_rc != 0) {
    /* Keep the generated C and say where: cc's diagnostic points into that
       file, and the one build where it is the only thing that can explain the
       failure was the one build that threw it away (#4096). */
    fprintf(stderr, "spinel: C compilation failed\n");
    if (c_is_temp)
      fprintf(stderr, "spinel: the generated C is kept at %s\n", c_path);
    if (bin_is_temp) remove(bin_path);
    return 1;
  }
  if (c_is_temp) remove(c_path);

  if (!run_mode) {
    fprintf(stderr, "%s -> %s\n", source, bin_path);
    return 0;
  }

  /* ---------- run: hand off to the freshly compiled binary ---------- */
  Str run = {0};
  s_add_arg(&run, bin_path);
  for (int k = 0; k < n_run_args; k++) s_add_arg(&run, run_args[k]);
  int run_rc = system(run.p);
  free(run.p);
  remove(bin_path);
  if (WIFEXITED(run_rc)) return WEXITSTATUS(run_rc);
  return run_rc ? 1 : 0;
}
