/* spinel: the single-binary Spinel compiler *and* driver.
 *
 * One executable does the whole job, with no shell wrapper and no chain of
 * helper binaries (the legacy parse -> analyze -> codegen pipeline collapsed
 * into this process). It:
 *   1. parses the .rb (Prism walk -> text AST -> in-memory node table),
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
#include "spinel_rev.h"
#include "codegen.h"
#include "analyze.h"
#include "csplit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#if defined(__APPLE__)
  #include <mach-o/dyld.h>
#endif
#include <sys/wait.h>
#include <sys/stat.h>

extern int g_no_root_elision;
extern int g_no_root_frame;
extern int g_opt_level;
extern int g_require_gate_cli;
extern int g_inline_hot;
extern int g_no_write_barrier;
extern const char *g_ext_init_name;
extern const char *g_ext_entries;
extern const char *g_ext_target;
extern const char *g_ext_feature;
extern char *g_ext_header_text;
extern char *g_ext_shim_text;
#define PATH_SEP '/'
#define EXE_SUFFIX ""

/* Defined in spinel_parse.c (compiled with -DSPINEL_PARSE_AS_LIB). */
char *sp_parse_file_to_text(const char *source_file, const char *argv0);

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

/* Refuse to write C over a file spinel did not write. `spinel app.rb -c -o
   sp_json.c` inside a package destroys the source and leaves the generated
   unit under its name, and nothing downstream can undo that: the compile then
   fails on a wall of duplicate symbols naming no file, and the source is gone.
   Re-emitting over spinel's OWN output is the ordinary case and stays silent.
   `--force` is the way through for anyone who means it. */
static int g_force_overwrite = 0;
static int refuse_overwrite(const char *path) {
  if (g_force_overwrite) return 0;
  FILE *f = fopen(path, "rb");
  if (!f) return 0;                      /* new file: nothing to lose */
  char head[sizeof SP_EMIT_BANNER];
  size_t n = fread(head, 1, sizeof head - 1, f);
  fclose(f);
  head[n] = 0;
  if (sp_streq(head, SP_EMIT_BANNER)) return 0;   /* our own previous output */
  fprintf(stderr,
          "spinel: refusing to overwrite '%s': it is not spinel's output.\n"
          "spinel:   emitting there would replace that file with a translation\n"
          "spinel:   unit and lose what is in it. Choose another -o path, or\n"
          "spinel:   pass --force if you mean to replace it.\n", path);
  return 1;
}

static int write_text_file(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "spinel: cannot write '%s'\n", path); return 0; }
  fputs(text, f);
  fclose(f);
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

static int cc_split_build(const char *cmd, size_t src_at, size_t src_end,
                          const char *c_path, int jobs) {
  if (jobs > 64) jobs = 64;
  char dir[] = "/tmp/spinel_split_XXXXXX";
  if (!mkdtemp(dir)) return -1;
  char flags[16384];
  if (src_at >= sizeof flags) return -1;
  memcpy(flags, cmd, src_at); flags[src_at] = 0;
  char pre[4200]; snprintf(pre, sizeof pre, "%s/pre.i", dir);
  size_t cl = strlen(flags) + strlen(c_path) + strlen(pre) + 64;
  char *c1 = malloc(cl);
  snprintf(c1, cl, "%s -E -P '%s' -o '%s'", flags, c_path, pre);
  int rc = system(c1);
  free(c1);
  char (*parts)[4096] = malloc(sizeof(*parts) * (size_t)jobs);
  char hdr[4200];
  if (rc == 0 && c_split(pre, dir, jobs, parts, hdr, sizeof hdr) != jobs) rc = -1;
  pid_t *pids = calloc((size_t)jobs, sizeof(pid_t));
  for (int k = 0; rc == 0 && k < jobs; k++) {
    size_t ql = strlen(flags) + 2 * strlen(parts[k]) + 64;
    char *q = malloc(ql);
    snprintf(q, ql, "%s -c '%s' -o '%s.o'", flags, parts[k], parts[k]);
    pid_t p = fork();
    if (p == 0) { execl("/bin/sh", "sh", "-c", q, (char *)NULL); _exit(127); }
    free(q);
    if (p < 0) { rc = -1; break; }
    pids[k] = p;
  }
  for (int k = 0; k < jobs; k++) {
    if (pids[k] <= 0) continue;
    int st = 0;
    if (waitpid(pids[k], &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) rc = -1;
  }
  free(pids);
  if (rc == 0) {
    size_t ll = strlen(cmd) + (size_t)jobs * 4200 + 16;
    char *lk = malloc(ll);
    size_t w = 0;
    memcpy(lk, cmd, src_at); w = src_at;
    for (int k = 0; k < jobs; k++) w += (size_t)snprintf(lk + w, ll - w, "'%s.o' ", parts[k]);
    snprintf(lk + w, ll - w, "%s", cmd + src_end);
    rc = system(lk);
    free(lk);
  }
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
    "  --version   Print the compiler build revision\n"
    "  -c          C source only (don't compile)\n"
    "  --force     with -c, overwrite an -o path spinel did not write\n"
    "  -I DIR      Add a feature search root for `require \"name\"` (like ruby -I)\n"
    "  --require-gate  Refuse an unresolvable require instead of warning\n"
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
    "  -g          Add debug info (-g) + #line, leaving -O as-is\n"
    "  --profile   Profiling build: -O2 -g -fno-omit-frame-pointer, unstripped,\n"
    "              and writes <out>.symbols.json (perf record -g ready)\n"
    "  --debug     Debug build: step through the .rb in gdb/lldb (#line, -g -O0)\n"
    "  --no-line-map  Suppress #line directives\n"
    "  --defer-refusals  Build anyway: a refused method raises NotImplementedError\n"
    "               when called, a refused top-level or class-body statement\n"
    "               when reached\n"
    "  --ext-init NAME    Emit a host-callable library: NAME() replaces main\n"
    "  --ext-entry M.m,.. Export these module methods (with -c; writes a .h contract)\n"
    "  --ext cruby        Also generate the CRuby extension shim (<out>_ext.c)\n"
    "  --no-inline-hot  Do not force small leaf methods inline (default: do).\n"
    "                 Forcing is worth a sixth of optcarrot's frame rate and\n"
    "                 costs up to twice the C compile time\n"
    "  --cc=CMD    C compiler (default: cc)\n"
    "  --jobs=N    compile the generated C as N units in parallel\n"
    "              (default: split only a unit of 4 MB or more; --jobs=1 never)\n"
    "  --target=wasm32-wasi  Build a WebAssembly module for a WASI host with the\n"
    "              wasi-sdk at $WASI_SDK (default /opt/wasi-sdk); Integer is 32-bit,\n"
    "              and Fiber, Thread, processes and sockets are not available there\n"
    "  -e STR      Inline Ruby source (repeatable; joined with newlines)\n"
    "  --rbs DIR   Seed analyzer with RBS signatures from DIR (advisory)\n"
    "  --int-overflow=MODE  Int +/-/* overflow handling (default: raise)\n"
    "  --dump-ast  Print the text AST and exit (debug)\n");
}

#ifdef SP_WORK_COUNT
static void work_report(void) { fprintf(stderr, "spinel-work: %llu\n", g_nt_work); }
#endif

int main(int argc, char **argv) {
#ifdef SP_WORK_COUNT
  atexit(work_report);
#endif
  /* `spinel diff FILE.rb ...`: the companion tool beside the compiler runs
     it (tools/diff.rb, built to bin/spinel-diff); the arguments pass through
     untouched, its exit status is the answer. */
  if (argc >= 2 && sp_streq(argv[1], "diff")) {
    char dir[4096];
    exe_dir(argv[0], dir, sizeof dir);
    char tool[4200];
    snprintf(tool, sizeof tool, "%s/spinel-diff", dir);
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
  const char *cc_cmd = "cc";
  const char *opt_level = "2";
  /* --target=wasm32-wasi: the program is a WebAssembly module for a WASI
     host (wasmtime, Node's WASI, a browser shim), built by the wasi-sdk's
     clang against the runtime archive in lib/wasm32-wasi/. */
  int target_wasi = 0;
  const char *int_overflow = "raise";
  const char *rbs_dir = NULL;
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

  for (int i = 1; i < argc; ) {
    const char *a = argv[i];
    if (!strncmp(a, "--source=", 9))      { source = a + 9; i++; }
    else if (!strncmp(a, "--output=", 9)) { output = a + 9; i++; }
    else if (!strncmp(a, "--cc=", 5))     { cc_cmd = a + 5; i++; }
    else if (!strncmp(a, "--jobs=", 7))   { cc_jobs = atoi(a + 7); i++; }
    else if (!strncmp(a, "--target=", 9)) {
      if (sp_streq(a + 9, "wasm32-wasi")) target_wasi = 1;
      else if (sp_streq(a + 9, "native")) target_wasi = 0;
      else { fprintf(stderr, "spinel: unknown --target %s (wasm32-wasi or native)\n", a + 9); return 1; }
      i++;
    }
    else if (!strncmp(a, "--rbs=", 6))    { rbs_dir = a + 6; i++; }
    else if (sp_streq(a, "--rbs"))         { if (++i < argc) rbs_dir = argv[i]; i++; }
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
    else if (sp_streq(a, "--defer-refusals")) { set_env("SPINEL_DEFER_REFUSALS", "1"); i++; }
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
    /* Library emission for host extensions (docs/internals/ext-design.md):
       --ext-init names the host-callable init function (emitted in place of
       main), --ext-entry designates the exported methods. */
    else if (sp_streq(a, "--ext-init") && i + 1 < argc) { g_ext_init_name = argv[i + 1]; i += 2; }
    else if (sp_streq(a, "--ext-entry") && i + 1 < argc) { g_ext_entries = argv[i + 1]; i += 2; }
    else if (sp_streq(a, "--ext") && i + 1 < argc) { g_ext_target = argv[i + 1]; i += 2; }
    /* Refuse an unresolvable require instead of warning past it. Inside a
       resolved dependency set the universe is known, so a require that
       resolves to nothing is a bug; `spin build` has always compiled this way
       through SPINEL_REQUIRE_GATE, and `spin flags` needs to hand the same
       gate to a caller driving the compiler itself (#4105). */
    else if (sp_streq(a, "--require-gate")) { g_require_gate_cli = 1; i++; }
    else if (sp_streq(a, "-c"))            { c_only = 1; i++; }
    else if (sp_streq(a, "-I"))            { if (++i < argc) sp_add_feature_root(argv[i]); i++; }
    else if (!strncmp(a, "-I", 2) && a[2]) { sp_add_feature_root(a + 2); i++; }
    else if (sp_streq(a, "-S"))            { stdout_mode = 1; i++; }
    else if (sp_streq(a, "--print-build")) { print_build = 1; i++; }
    else if (sp_streq(a, "-E"))            { run_mode = 1; i++; }
    else if (sp_streq(a, "--emit-rbs"))    { emit_rbs = 1; i++; }
    else if (sp_streq(a, "--emit-types"))  { emit_types = 1; i++; }
    else if (sp_streq(a, "--emit-symbol-map")) { emit_symbol_map = 1; i++; }
    else if (sp_streq(a, "--dump-ast"))    { dump_ast = 1; i++; }
    else if (sp_streq(a, "-h") || sp_streq(a, "--help")) { usage(); return 0; }
    /* The C compiler is half of what a spinel build is, so name it too: a bug
       report that turns out to be a codegen/compiler interaction is otherwise
       missing the other half. Reports whatever `--cc=` selected, when that
       flag came first; a compiler that cannot be run leaves the line as it
       was rather than saying anything about it. */
    else if (sp_streq(a, "--version")) {
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

  const char *ov_define = NULL;
  if (sp_streq(int_overflow, "raise"))   ov_define = "-DSP_INT_OVERFLOW_MODE_RAISE";
  else if (sp_streq(int_overflow, "wrap"))    ov_define = "-DSP_INT_OVERFLOW_MODE_WRAP";
  else if (sp_streq(int_overflow, "promote")) ov_define = "-DSP_INT_OVERFLOW_MODE_PROMOTE";
  else { fprintf(stderr, "spinel: --int-overflow expects raise|wrap|promote, got '%s'\n", int_overflow); return 2; }
  /* Let the analyzer widen accumulating int locals to bigint more freely in
     promote mode (block-iteration loops, not just `while`). See analyze.h. */
  g_promote_mode = sp_streq(int_overflow, "promote");

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
     and hand them to the analyzer through SPINEL_RBS_SEED. An empty result is a
     silent no-op; a missing extractor is warned about, since --rbs was asked for
     explicitly and the seeds would otherwise vanish without a trace (#1792). */
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
      s_add_arg(&cmd, rbs_dir);
      s_add(&cmd, "> ");
      s_add_arg(&cmd, seed_path);
      int rc = system(cmd.p);
      free(cmd.p);
      if (rc == 0 && file_exists(seed_path)) {
        set_env("SPINEL_RBS_SEED", seed_path);
      }
      else {
        remove(seed_path);
        seed_path[0] = '\0';
      }
    }
  }

  /* Source mapping: the parser stamps node positions and codegen emits #line
     when SPINEL_DEBUG / SPINEL_LINE_MAP is set. --debug/-g use the fuller
     debug path; otherwise line-map (on by default) just adds #line. The emit
     modes that need positions force SPINEL_DEBUG below. */
  if (debug) set_env("SPINEL_DEBUG", "1");
  else if (line_map) set_env("SPINEL_LINE_MAP", "1");
  /* --warn-widen places each warning at its slot, which needs the parser's
     positions: forced past --no-line-map, as --emit-types forces them. */
  if (warn_widen) { set_env("SPINEL_LINE_MAP", "1"); set_env("SPINEL_WARN_WIDEN", "1"); }
  /* --check-stores reports each store at its Ruby line, so it needs them too */
  if (check_stores) { set_env("SPINEL_LINE_MAP", "1"); set_env("SPINEL_CHECK_STORES", "1"); }

  /* Analyze-only emit modes write their artifact from inside codegen_program
     and produce an empty translation unit; route the output path via env. */
  char emit_out[4096];
  if (emit_rbs) {
    snprintf(emit_out, sizeof emit_out, "%s", output ? output : basename);
    if (!output) { strncat(emit_out, ".rbs", sizeof emit_out - strlen(emit_out) - 1); }
    set_env("SPINEL_EMIT_RBS", emit_out);
  }
  else if (emit_types) {
    snprintf(emit_out, sizeof emit_out, "%s", output ? output : basename);
    if (!output) { strncat(emit_out, ".types.json", sizeof emit_out - strlen(emit_out) - 1); }
    /* The positions the JSON carries need the parser's line map, not the
       debug build: SPINEL_DEBUG also turns codegen's g_debug on, which drops
       `static`/always_inline from every method and switches the backtrace
       substrate on, so the C of `--emit-types -S` was the debug compile's,
       not what -S alone emits. Forced past --no-line-map, since the JSON
       is nothing without positions. */
    set_env("SPINEL_LINE_MAP", "1");
    set_env("SPINEL_EMIT_TYPES", emit_out);
    /* `--emit-types -o x.json -S`: the JSON and the C of the same compile,
       in one run rather than two (a consumer showing both, rubys/spinel-ide) */
    if (stdout_mode) set_env("SPINEL_EMIT_TYPES_KEEP_C", "1");
  }
  else if (emit_symbol_map) {
    snprintf(emit_out, sizeof emit_out, "%s", output ? output : basename);
    if (!output) { strncat(emit_out, ".symbols.json", sizeof emit_out - strlen(emit_out) - 1); }
    set_env("SPINEL_DEBUG", "1");
    set_env("SPINEL_EMIT_SYMBOL_MAP", emit_out);
  }
  if (profile) {
    /* --profile also writes the symbol map next to the binary (offline
       symbolization input for perf tooling); codegen falls through to the
       real build after writing it (#1336). */
    char psym[4096];
    snprintf(psym, sizeof psym, "%s.symbols.json", output ? output : basename);
    set_env("SPINEL_PROFILE_SYMBOL_MAP", psym);
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
  char *text = sp_parse_file_to_text(source, argv[0]);
  if (eval_path[0]) remove(eval_path);
  if (!text) { fprintf(stderr, "spinel: parse failed for '%s'\n", source); if (seed_path[0]) remove(seed_path); return 1; }

  if (dump_ast) { fputs(text, stdout); free(text); if (seed_path[0]) remove(seed_path); return 0; }

  NodeTable *nt = nt_load_text(text);
  free(text);
  if (!nt) { fprintf(stderr, "spinel: failed to load AST\n"); if (seed_path[0]) remove(seed_path); return 1; }

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
  char *csrc = codegen_program(nt);
  nt_free(nt);
  if (seed_path[0]) remove(seed_path);
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

  if ((g_ext_init_name || g_ext_entries) && !c_only) {
    fprintf(stderr, "spinel: --ext-init/--ext-entry emit a LIBRARY: pass -c "
                    "(the host's build compiles and links the result)\n");
    free(csrc);
    return 1;
  }
  if (g_ext_entries && !g_ext_init_name) {
    fprintf(stderr, "spinel: --ext-entry needs --ext-init (the host must "
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

  /* Decide where the generated C goes. */
  char c_path[4096];
  int c_is_temp = 0;
  if (c_only) {
    snprintf(c_path, sizeof c_path, "%s", output ? output : basename);
    if (!output) strncat(c_path, ".c", sizeof c_path - strlen(c_path) - 1);
  }
  else {
    make_temp_path(c_path, sizeof c_path, "out", 0, ".c");
    c_is_temp = 1;
  }
  if (c_only && refuse_overwrite(c_path)) { free(csrc); return 1; }
  if (!write_text_file(c_path, csrc)) { free(csrc); return 1; }

  if (c_only) {
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
  const char *rt_lib = uses_threads ? "libspinel_rt_mt.a" : "libspinel_rt.a";
  free(csrc);
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
    rt_lib = "wasm32-wasi/libspinel_rt.a";
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
  /* A pointer of the wrong type in generated code is a miscompile, not a style
     question: it reads one struct through another's layout, which segfaults or
     answers garbage (a Symbol key dereferenced as a char *, #3975; an Integer
     element bound to a string handle, #3971). Both showed up only as a warning
     in the build log, easy to scroll past. Fail the build instead, so the
     emitter's own bug surfaces here rather than at run time. */
  s_add(&cmd, "-Werror=incompatible-pointer-types -Werror=int-conversion ");
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
  snprintf(tmp, sizeof tmp, "-I\"%s\" -I\"%s%cregexp\" ", lib_dir, lib_dir, PATH_SEP); s_add(&cmd, tmp);
  bi_put(&bi, "include", lib_dir);
  { char rgi[4096]; snprintf(rgi, sizeof rgi, "%s%cregexp", lib_dir, PATH_SEP); bi_put(&bi, "include", rgi); }
  /* wasm32-wasi: what common.mk gives the runtime archive there (see its
     WASI_CFLAGS): lib/wasi's stand-ins for the POSIX headers wasi-libc
     leaves out, wasi-libc's emulations of signals, mmap and process clocks,
     and setjmp/longjmp through the standard exception-handling instructions
     (the C compiler's default is the legacy encoding, which the engines
     refuse). The main thread's stack is linear memory too; 8 MB is the
     native default. */
  if (target_wasi) {
    snprintf(tmp, sizeof tmp, "-I\"%s%cwasi\" ", lib_dir, PATH_SEP); s_add(&cmd, tmp);
    { char wi[4096]; snprintf(wi, sizeof wi, "%s%cwasi", lib_dir, PATH_SEP); bi_put(&bi, "include", wi); }
    static const char *const wdefs[] = { "-D_WASI_EMULATED_SIGNAL", "-D_WASI_EMULATED_PROCESS_CLOCKS", "-D_WASI_EMULATED_GETPID", "-D_WASI_EMULATED_MMAN" };
    for (size_t wi = 0; wi < sizeof wdefs / sizeof wdefs[0]; wi++) { s_add(&cmd, wdefs[wi]); s_add(&cmd, " "); bi_put(&bi, "define", wdefs[wi]); }
    static const char *const wcfl[] = { "-mllvm", "-wasm-enable-sjlj", "-mllvm", "-wasm-use-legacy-eh=false", "-Wl,-z,stack-size=8388608" };
    for (size_t wi = 0; wi < sizeof wcfl / sizeof wcfl[0]; wi++) { s_add(&cmd, wcfl[wi]); s_add(&cmd, " "); bi_put(&bi, "cflag", wcfl[wi]); }
  }
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
          snprintf(tmt, sizeof tmt, "%.*s%s.o", (int)(tl - 2), t, uses_threads ? "_mt" : "_wasi");
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
    /* the emulations the defines above asked for, and sjlj's runtime */
    static const char *const wlibs[] = { "-lsetjmp", "-lwasi-emulated-signal", "-lwasi-emulated-process-clocks", "-lwasi-emulated-getpid", "-lwasi-emulated-mman" };
    for (size_t wi = 0; wi < sizeof wlibs / sizeof wlibs[0]; wi++) { s_add(&cmd, wlibs[wi]); s_add(&cmd, " "); bi_put(&bi, "lib", wlibs[wi]); }
  }
#if !defined(__APPLE__)
  /* String#crypt = libc crypt(3), linked only when the program calls it:
     a linker without a default --as-needed (Fedora's) otherwise made
     libcrypt a dependency of every binary (#6674) */
  else if (uses_crypt) {
    s_add(&cmd, "-lcrypt ");
    bi_put(&bi, "lib", "-lcrypt");
  }
#endif
  for (int li = 0; li < n_link_extra; li++)
    if (strncmp(link_extra[li], "-l", 2) == 0) {
      s_add(&cmd, link_extra[li]); s_add(&cmd, " ");
      bi_put(&bi, "lib", link_extra[li]);
    }
  if (uses_threads) { s_add(&cmd, "-lpthread "); bi_put(&bi, "lib", "-lpthread"); }
  s_add(&cmd, ov_define); s_add(&cmd, " ");
  bi_put(&bi, "define", ov_define);
  if (want_g) s_add(&cmd, "-g ");
  if (profile) s_add(&cmd, "-fno-omit-frame-pointer ");
#if !defined(__APPLE__)
  if (debug && !target_wasi) s_add(&cmd, "-rdynamic ");  /* ELF: name user frames in backtraces */
#endif
  /* An ffi_lib name whose archive already arrived as an explicit --link
     input (spin's [[build]] artifacts) is satisfied: also emitting the -l
     flag would make ld search the system paths for a library that only
     exists in the build cache. A --link basename lib<name>.(a|so) counts as
     providing -l<name>, so one package source serves both worlds -- the
     Makefile's -L via ffi_cflags and spin's absolute --link paths. */
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
  /* Unasked, only a large unit is split: a part cannot inline what another
     part defines, which cost optcarrot's 0.6 MB unit ~10% of its speed, and
     run time comes before build time. A unit this large is where cc's one
     core is the build (campfire's 10 MB: 77 s whole, 36 s split). */
  if (cc_jobs == 0) {
    struct stat cst;
    cc_jobs = 1;
    if (stat(c_path, &cst) == 0 && cst.st_size >= SPLIT_AUTO_BYTES) {
      long nc = sysconf(_SC_NPROCESSORS_ONLN);
      cc_jobs = nc > 16 ? 16 : nc > 1 ? (int)nc : 1;
    }
  }
  if (cc_jobs > 1 && !target_wasi)
    cc_rc = cc_split_build(cmd.p, cc_src_at, cc_src_end, c_path, cc_jobs);
  /* SPINEL_SPLIT_STRICT: a split that fails is the failure (for tests of the
     split itself); otherwise the single unit is the fallback */
  if (cc_rc != 0 && !(cc_jobs > 1 && getenv("SPINEL_SPLIT_STRICT")))
    cc_rc = system(cmd.p);   /* the single unit, or the fallback */
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
