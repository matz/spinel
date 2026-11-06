/* Splitting one generated translation unit into several (#4847 C1).

   A large program's generated C is one file, and cc compiles one file on one
   core: campfire's 10 MB took ~50 s while eleven cores sat idle. c_split
   takes the PREPROCESSED unit (cc -E) and writes a shared header plus N
   part files that compile in parallel and link to the same program:

   - types, typedefs, extern declarations and `static inline` functions go to
     the header, as they were, in their original order;
   - every other function keeps one definition, in one part, and loses
     `static`; the header declares it where the definition stood;
   - every file-scope variable is defined once, in part 0, without `static`;
     the header declares it `extern` where the definition stood.

   The header keeps the unit's own order, with each definition replaced by its
   declaration, so everything a later item names is declared before it, as it
   was in the single file. An inline function with a `static` local keeps one
   copy of that local only if it has one definition, so it is demoted to a
   plain function. Anything this reader does not recognise makes it decline
   (returns -1), and the caller compiles the single unit as before.

   The unit keeps the preprocessor's line markers (`# N "file" flags`), and
   each item goes out under the marker in effect where it stood, so a part
   still says which lines came from a system header and which from the .rb.
   Without them every warning cc silences in system headers fires on the
   SDK's declarations in every part (#8579: Apple clang's
   -Wnullability-completeness, 10,000 lines of log for a five-line program),
   and an error in a part names the part instead of the program. The
   markers' include-stack flags (1 and 2) are not replayed, only the
   system-header ones (3 and 4): the items leave the unit's order, and a
   stack that does not match is worth a warning of its own.

   c_split_units is the other reader of the same items, for --incremental.
   There the parts are not for
   parallelism but for a cache of objects, so a part's text has to stay the
   same when the program changes elsewhere. It cuts at the `#pragma spinel
   unit <tag> <group>` lines codegen writes under that flag only, where each
   method, constructor, dispatcher and main begins. The items before the
   return from `#include "spinel_rt.h"` are the runtime region, which goes to
   sp_rt.h and to rt units that every program built with the same flags
   shares; the rest is the program region, which goes to sp_program.h and to
   units packed by a jump consistent hash of the group or the item's family,
   so that growing the program moves few items between units. Every program
   name a unit uses but does not define reaches it through a declaration
   written into that unit, not through the shared header, so adding a method
   changes only the units that define and call it. A pragma written where a
   method began and then abandoned (an EMIT_COLLECT_UNIT re-emission) leaves
   its proc segments in the text under a tag no body uses: they become dead
   external functions that the link's --gc-sections drops. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include "csplit.h"
#include "sha256.h"

typedef enum { IT_HDR, IT_FN, IT_VAR, IT_PROTO, IT_MARK, IT_UNIT } ItemKind;
typedef struct {
  size_t st, en;        /* [st, en) in the text */
  ItemKind kind;
  size_t body;          /* IT_FN: offset of the body's `{`; IT_UNIT: the
                           pragma's index in the UnitPragma table */
  char name[128];
  int keep_inline;      /* IT_FN: stays `static inline` in the header */
  size_t mfile;         /* the line marker in effect at st: its "file" (with
                           the quotes), or 0 when the unit has none */
  size_t mflen;
  int msys;             /* its system-header flags: bit 0 = 3, bit 1 = 4 */
  int mflags;           /* its include flags: bit 0 = 1 (the entry into an
                           include), bit 1 = 2 (the return from one) */
  long mline;           /* the line st is on, as the marker counts */
  int unit;             /* the unit tag in effect at st (an index into the
                           --incremental split's tag table), or -1 (untagged) */
  int region;           /* 0 = the runtime region, 1 = the program region */
  int home;             /* the unit the item is written to, or -1 (header or
                           declaration table only) */
  int drop;             /* a variable's occurrence that is not its definition */
} Item;

/* `#pragma spinel unit <tag> <group>`, which an --incremental build's
   codegen writes where a method, constructor, dispatcher or main begins
   (unit_begin_in in codegen_util.c). The sizes are codegen's UNIT_TAG_MAX:
   a longer word is not one codegen wrote, and declines. */
#define CS_TAG_MAX 192
typedef struct { char tag[CS_TAG_MAX]; char group[CS_TAG_MAX]; } UnitPragma;

/* Is t[i..] a line marker `# N "file" ...`? (a `#` at the start of a line) */
static int is_marker(const char *t, size_t i, size_t n) {
  if (i >= n || t[i] != '#') return 0;
  i++;
  while (i < n && (t[i] == ' ' || t[i] == '\t')) i++;
  return i < n && t[i] >= '0' && t[i] <= '9';
}

static size_t skip_lit(const char *t, size_t n, size_t i);

/* Read the marker at t[i..] (is_marker said yes) into the current position:
   the file and flags it names, and the line the NEXT line has. Returns the
   offset of the newline ending the marker line (or n). */
static size_t read_marker(const char *t, size_t i, size_t n, size_t *mfile, size_t *mflen, int *msys, int *mflags, long *ln) {
  size_t e = i;
  while (e < n && t[e] != '\n') e++;
  size_t p = i + 1;
  while (p < e && (t[p] == ' ' || t[p] == '\t')) p++;
  long v = 0;
  while (p < e && t[p] >= '0' && t[p] <= '9') v = v * 10 + (t[p++] - '0');
  while (p < e && t[p] == ' ') p++;
  if (p < e && t[p] == '"') {
    size_t q = skip_lit(t, e, p);
    *mfile = p; *mflen = q - p;
    int sys = 0, fl = 0;
    for (size_t r = q; r < e; r++) {
      if (t[r] == '3' && (r + 1 >= e || t[r + 1] == ' ')) sys |= 1;
      else if (t[r] == '4' && (r + 1 >= e || t[r + 1] == ' ')) sys |= 2;
      else if (t[r] == '1' && (r + 1 >= e || t[r + 1] == ' ')) fl |= 1;
      else if (t[r] == '2' && (r + 1 >= e || t[r + 1] == ' ')) fl |= 2;
    }
    *msys = sys; *mflags = fl;
  }
  *ln = v;
  return e;
}

/* Where each item's text goes, with the marker its last line left in
   effect, so a marker is written only where the next item would otherwise
   be claimed by the wrong file or line.
   A system header's items go to a file of their own, included where they
   stood (out_sync). Apple clang's -Wnullability-completeness is decided per
   file, not per marker: once a file holds one pointer with a nullability
   qualifier (the SDK's), every pointer of the file outside a system header
   is warned about, the program's included, and the parts of a large program
   wrote hundreds of MB of those warnings, most of the cc time (#8579). */
typedef struct {
  FILE *f;              /* where the next item goes: main, or side */
  FILE *main, *side;    /* side: the system-header file now open, or NULL */
  const char *dir, *tag;
  int nside, bad;
  int noside;           /* a unit: system-header items stay inline, flagged */
  size_t mfile, mflen;
  int msys;
  long line;            /* the line the next output line claims, or -1 */
} Out;

static void out_init(Out *o, FILE *f, const char *dir, const char *tag) {
  memset(o, 0, sizeof *o);
  o->f = o->main = f; o->dir = dir; o->tag = tag; o->line = -1;
}

/* Close the system-header file now open: the next item goes to main again */
static void out_side_end(Out *o) {
  if (!o->side) return;
  if (ferror(o->side) | (fclose(o->side) != 0)) o->bad = 1;
  o->side = NULL; o->f = o->main;
  o->mfile = 0; o->line = -1;
}

static void out_sync(Out *o, const char *t, const Item *x) {
  if (!x->mfile) return;   /* a unit without markers: nothing to keep */
  if (x->msys && !o->side && !o->bad && !o->noside) {
    char nm[64], path[4200];
    snprintf(nm, sizeof nm, "sp_sys_%s%d.h", o->tag, o->nside++);
    snprintf(path, sizeof path, "%s/%s", o->dir, nm);
    o->side = fopen(path, "wb");
    if (!o->side) o->bad = 1;
    else {
      fprintf(o->main, "#include \"%s\"\n", nm);
      o->f = o->side; o->mfile = 0; o->line = -1;
    }
  }
  else if (!x->msys && o->side) out_side_end(o);
  if (o->mfile && o->mflen == x->mflen && memcmp(t + o->mfile, t + x->mfile, x->mflen) == 0 &&
      o->msys == x->msys && o->line == x->mline)
    return;
  fprintf(o->f, "# %ld %.*s%s%s\n", x->mline, (int)x->mflen, t + x->mfile,
          (x->msys & 1) ? " 3" : "", (x->msys & 2) ? " 4" : "");
  o->mfile = x->mfile; o->mflen = x->mflen; o->msys = x->msys;
  o->line = x->mline;
}

/* After writing [st, en) of the text plus `extra` newlines: the output's
   line count moved by the range's newlines, unless the range holds a marker
   of its own (a marker inside a declaration, as the preprocessor writes
   where it dropped more than eight blank lines), which puts the count out of
   reach; -1 then makes the next item write its marker. */
static void out_advance(Out *o, const char *t, size_t st, size_t en, int extra) {
  if (o->line < 0) return;
  long nl = 0;
  for (size_t i = st; i < en; i++) {
    if (t[i] != '\n') continue;
    nl++;
    if (is_marker(t, i + 1, en)) { o->line = -1; return; }
  }
  o->line += nl + extra;
}

static int is_idch(char ch) {
  return ch == '_' || (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '$';
}

/* skip a string or char literal starting at t[i]; returns the index after it */
static size_t skip_lit(const char *t, size_t n, size_t i) {
  char q = t[i++];
  while (i < n && t[i] != q) { if (t[i] == '\\') i++; i++; }
  return i < n ? i + 1 : n;
}

/* Does [st, en) contain the word `w` outside literals? */
static int has_word(const char *t, size_t st, size_t en, const char *w) {
  size_t wl = strlen(w);
  for (size_t i = st; i < en; ) {
    char ch = t[i];
    if (ch == '"' || ch == '\'') { i = skip_lit(t, en, i); continue; }
    if (is_idch(ch)) {
      size_t s = i;
      while (i < en && is_idch(t[i])) i++;
      if (i - s == wl && memcmp(t + s, w, wl) == 0) return 1;
      continue;
    }
    i++;
  }
  return 0;
}

static size_t skip_group(const char *t, size_t i, size_t en) {
  while (i < en && (t[i] == ' ' || t[i] == '\n' || t[i] == '\t')) i++;
  if (i >= en || t[i] != '(') return i;
  for (int d = 0; i < en; i++) {
    if (t[i] == '"' || t[i] == '\'') { i = skip_lit(t, en, i) - 1; continue; }
    if (t[i] == '(') d++;
    else if (t[i] == ')' && --d == 0) return i + 1;
  }
  return i;
}

/* The first `(` at depth 0 in [st, en) that is not an attribute's or a
   __typeof__'s, or `en`. */
static size_t first_paren(const char *t, size_t st, size_t en) {
  for (size_t i = st; i < en; ) {
    char ch = t[i];
    if (ch == '"' || ch == '\'') { i = skip_lit(t, en, i); continue; }
    if (is_idch(ch)) {
      size_t s = i;
      while (i < en && is_idch(t[i])) i++;
      size_t wl = i - s;
      int skip = (wl == 13 && memcmp(t + s, "__attribute__", 13) == 0) ||
                 (wl == 10 && memcmp(t + s, "__typeof__", 10) == 0) ||
                 (wl == 6 && memcmp(t + s, "typeof", 6) == 0) ||
                 (wl == 7 && memcmp(t + s, "__asm__", 7) == 0) ||
                 (wl == 8 && memcmp(t + s, "_Alignas", 8) == 0);
      if (skip) i = skip_group(t, i, en);
      continue;
    }
    if (ch == '(') return i;
    if (ch == '{' || ch == '=') return en;
    i++;
  }
  return en;
}

/* the identifier ending just before `p` (skipping spaces) */
static void ident_before(const char *t, size_t lo, size_t p, char *out, size_t osz) {
  out[0] = 0;
  while (p > lo && (t[p - 1] == ' ' || t[p - 1] == '\n' || t[p - 1] == '\t')) p--;
  size_t e = p;
  while (p > lo && is_idch(t[p - 1])) p--;
  size_t l = e - p;
  if (l == 0 || l >= osz) return;
  memcpy(out, t + p, l); out[l] = 0;
}

/* Does the word at [w, w+wl) introduce a trailing attribute group? */
static int is_attr_word(const char *w, size_t wl) {
  return (wl == 13 && memcmp(w, "__attribute__", 13) == 0) ||
         (wl == 7 && memcmp(w, "__asm__", 7) == 0) ||
         (wl == 5 && memcmp(w, "__asm", 5) == 0) ||
         (wl == 3 && memcmp(w, "asm", 3) == 0);
}

/* The end of [st, en) with any trailing attribute groups -- `__attribute__
   ((...))`, `__asm__ (...)` -- and the whitespace around them dropped, so
   what is left ends at the declaration's own last character. The system
   headers a program reaches through put a trailing attribute in both places
   this reader decides something from the last character: macOS spells a
   constant `static const uuid_t UUID_NULL __attribute__ ((unused)) = {...}`
   (<uuid/uuid.h>, via <pwd.h>) and a tag `struct __darwin_arm_sve_z_state
   {...} __attribute__((aligned(4)))` (<mach/machine/_structs.h>). Scanned
   forwards, so a `(` inside a string literal is not a paren (#4847). */
static size_t trim_trailing_attrs(const char *t, size_t st, size_t en) {
  size_t keep = st;
  for (size_t i = st; i < en; ) {
    char ch = t[i];
    if (ch == '"' || ch == '\'') { i = skip_lit(t, en, i); keep = i; continue; }
    /* a directive line inside the declaration (a #pragma that a _Pragma in
       a macro left between `}` and `;`, SLIST_HEAD in the macOS 27 SDK) */
    if (ch == '#') { while (i < en && t[i] != '\n') i++; continue; }
    if (!is_idch(ch)) {
      i++;
      if (ch != ' ' && ch != '\n' && ch != '\t' && ch != '\r') keep = i;
      continue;
    }
    size_t s = i;
    while (i < en && is_idch(t[i])) i++;
    if (is_attr_word(t + s, i - s)) {
      i = skip_group(t, i, en);
      continue;   /* an attribute is not the declaration's own content */
    }
    keep = i;
  }
  return keep;
}

/* The item's leading words, past __extension__. */
static int starts_with_word(const char *t, size_t st, size_t en, const char *w) {
  size_t i = st;
  for (;;) {
    while (i < en && (t[i] == ' ' || t[i] == '\n' || t[i] == '\t')) i++;
    size_t s = i;
    while (i < en && is_idch(t[i])) i++;
    if (i - s == 13 && memcmp(t + s, "__extension__", 13) == 0) continue;
    size_t wl = strlen(w);
    return i - s == wl && memcmp(t + s, w, wl) == 0;
  }
}

/* Emit [st, en) without the storage words `static` (and, with no_inline, the
   inline words), outside literals. */
static void put_stripped(FILE *f, const char *t, size_t st, size_t en, int no_inline) {
  for (size_t i = st; i < en; ) {
    char ch = t[i];
    if (ch == '"' || ch == '\'') { size_t j = skip_lit(t, en, i); fwrite(t + i, 1, j - i, f); i = j; continue; }
    if (is_idch(ch)) {
      size_t s = i;
      while (i < en && is_idch(t[i])) i++;
      size_t wl = i - s;
      int drop = (wl == 6 && memcmp(t + s, "static", 6) == 0);
      if (no_inline)
        drop = drop || (wl == 6 && memcmp(t + s, "inline", 6) == 0) ||
               (wl == 10 && memcmp(t + s, "__inline__", 10) == 0) ||
               (wl == 8 && memcmp(t + s, "__inline", 8) == 0);
      if (no_inline && wl == 13 && memcmp(t + s, "always_inline", 13) == 0) {
        fputs("noinline", f);   /* keep the attribute list well-formed */
        continue;
      }
      if (no_inline && wl == 16 && memcmp(t + s, "SP_ALWAYS_INLINE", 16) == 0) {
        fputs("SP_NOINLINE", f);   /* the sp_compat.h spelling of the same */
        continue;
      }
      if (!drop) fwrite(t + s, 1, wl, f);
      continue;
    }
    fputc(ch, f); i++;
  }
}

/* A declaration's text up to its top-level `=` (the initializer's start), or
   its end. Declines (-1) when it declares more than one name. */
static long decl_cut(const char *t, size_t st, size_t en) {
  int d = 0;
  for (size_t i = st; i < en; ) {
    char ch = t[i];
    if (ch == '"' || ch == '\'') { i = skip_lit(t, en, i); continue; }
    if (ch == '(' || ch == '[' || ch == '{') d++;
    else if (ch == ')' || ch == ']' || ch == '}') d--;
    else if (d == 0 && ch == '=') return (long)i;
    else if (d == 0 && ch == ',') return -1;
    i++;
  }
  return (long)en;
}

/* the first top-level `,` in [st, en), or en */
static size_t top_comma(const char *t, size_t st, size_t en) {
  int d = 0;
  for (size_t i = st; i < en; ) {
    char ch = t[i];
    if (ch == '"' || ch == '\'') { i = skip_lit(t, en, i); continue; }
    if (ch == '(' || ch == '[' || ch == '{') d++;
    else if (ch == ')' || ch == ']' || ch == '}') d--;
    else if (d == 0 && ch == ',') return i;
    i++;
  }
  return en;
}

/* the top-level `=` in [st, en), or en */
static size_t top_eq(const char *t, size_t st, size_t en) {
  int d = 0;
  for (size_t i = st; i < en; ) {
    char ch = t[i];
    if (ch == '"' || ch == '\'') { i = skip_lit(t, en, i); continue; }
    if (ch == '(' || ch == '[' || ch == '{') d++;
    else if (ch == ')' || ch == ']' || ch == '}') d--;
    else if (d == 0 && ch == '=') return i;
    i++;
  }
  return en;
}

/* A file-scope variable declaration [st, en) (ending in `;`), possibly of
   several names: each becomes `extern` in the header and one definition in
   the part, the shared specifiers repeated for every declarator. */
static void put_vars(FILE *h, FILE *part, const char *t, size_t st, size_t en) {
  size_t end = en - 1;   /* before the `;` */
  size_t c1 = top_comma(t, st, end);
  if (c1 >= end) {   /* one declarator, whatever its shape */
    /* An untagged struct or union type is a different type in every
       declaration that spells it: the header's extern and the part's
       definition would disagree. Tag it, define it in the header, and have
       the part name it by the tag. */
    size_t eq = top_eq(t, st, end);
    for (size_t q = st; q < eq; ) {
      if (!is_idch(t[q])) { q++; continue; }
      size_t ws = q;
      while (q < eq && is_idch(t[q])) q++;
      size_t wl = q - ws;
      if (!((wl == 6 && memcmp(t + ws, "struct", 6) == 0) || (wl == 5 && memcmp(t + ws, "union", 5) == 0))) continue;
      size_t r = q;
      while (r < eq && (t[r] == ' ' || t[r] == '\n')) r++;
      if (r >= eq || t[r] != '{') break;   /* tagged: nothing to do */
      int bd = 0; size_t bc = r;
      for (; bc < eq; bc++) { if (t[bc] == '{') bd++; else if (t[bc] == '}' && --bd == 0) break; }
      if (bc >= eq) break;
      static int anon_n = 0;
      int k = anon_n++;
      fputs("extern ", h);
      put_stripped(h, t, st, q, 0); fprintf(h, " sp__csplit_anon%d ", k);
      put_stripped(h, t, r, eq, 0); fputs(";\n", h);
      put_stripped(part, t, st, q, 0); fprintf(part, " sp__csplit_anon%d ", k);
      put_stripped(part, t, bc + 1, en, 0); fputc('\n', part);
      return;
    }
    fputs("extern ", h);
    put_stripped(h, t, st, top_eq(t, st, end), 0);
    fputs(";\n", h);
    put_stripped(part, t, st, en, 0);
    fputc('\n', part);
    return;
  }
  /* the first declarator starts at its name, or at the `*`/`(` before it */
  size_t nm_end = top_eq(t, st, c1);
  { int dd = 0; for (size_t q = st; q < nm_end; q++) {
      if (t[q] == '(') dd++; else if (t[q] == ')') dd--;
      else if (t[q] == '[' && dd == 0) { nm_end = q; break; } } }
  while (nm_end > st && (t[nm_end - 1] == ' ' || t[nm_end - 1] == '\n')) nm_end--;
  size_t ds = nm_end;
  while (ds > st && is_idch(t[ds - 1])) ds--;
  while (ds > st && (t[ds - 1] == '*' || t[ds - 1] == ' ' || t[ds - 1] == '(')) {
    if (t[ds - 1] == ' ') { size_t b = ds - 1; while (b > st && t[b - 1] == ' ') b--; if (b > st && (t[b - 1] == '*' || t[b - 1] == '(')) { ds = b; continue; } break; }
    ds--;
  }
  size_t ps = ds;   /* the specifiers are [st, ps) */
  size_t a = ds;
  for (;;) {
    size_t b = top_comma(t, a, end);
    size_t e = top_eq(t, a, b);
    fputs("extern ", h);
    put_stripped(h, t, st, ps, 0); fputc(' ', h);
    fwrite(t + a, 1, e - a, h); fputs(";\n", h);
    put_stripped(part, t, st, ps, 0); fputc(' ', part);
    fwrite(t + a, 1, b - a, part); fputs(";\n", part);
    if (b >= end) break;
    a = b + 1;
  }
}

/* A function's share of the parts' work: its bytes without the line markers
   and blank lines, which the preprocessor writes in proportion to the
   statements (one marker per statement from the .rb) rather than to the
   code. Counted with them, a statement-dense main body outweighed the rest
   and the parts came out uneven, one of them near empty. */
static size_t fn_weight(const char *t, size_t st, size_t en) {
  size_t w = 0;
  for (size_t i = st; i < en; ) {
    size_t e = i;
    while (e < en && t[e] != '\n') e++;
    if (e > i && !is_marker(t, i, en)) w += e - i + 1;
    i = e + 1;
  }
  return w;
}

static int cmpstr(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Read one word of a unit pragma ([A-Za-z0-9_], as unit_sanitize leaves
   it) at t[*i..en) into out. 0 when there is none or it is too long. */
static int pragma_word(const char *t, size_t *i, size_t en, char *out) {
  size_t p = *i;
  while (p < en && (t[p] == ' ' || t[p] == '\t')) p++;
  size_t s = p;
  while (p < en && (t[p] == '_' || (t[p] >= '0' && t[p] <= '9') || (t[p] >= 'a' && t[p] <= 'z') || (t[p] >= 'A' && t[p] <= 'Z'))) p++;
  if (p == s || p - s >= CS_TAG_MAX) return 0;
  memcpy(out, t + s, p - s); out[p - s] = 0;
  *i = p;
  return 1;
}

/* Is the directive line [st, en) a unit pragma? 1 (read into *u) when it is
   `#pragma spinel unit <tag> <group>`, 0 when it is some other directive
   (a system header's `#pragma GCC ...` stays a header item), -1 when it is
   a unit pragma of another shape: the reader would not know which unit the
   items after it belong to. */
static int unit_pragma(const char *t, size_t st, size_t en, UnitPragma *u) {
  static const char *const lead[3] = { "pragma", "spinel", "unit" };
  size_t i = st + 1;
  for (int w = 0; w < 3; w++) {
    while (i < en && (t[i] == ' ' || t[i] == '\t')) i++;
    size_t s = i, wl = strlen(lead[w]);
    while (i < en && is_idch(t[i])) i++;
    if (i - s != wl || memcmp(t + s, lead[w], wl) != 0) return 0;
  }
  if (!pragma_word(t, &i, en, u->tag) || !pragma_word(t, &i, en, u->group)) return -1;
  while (i < en && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r')) i++;
  return i == en ? 1 : -1;
}

/* The item lexer: [0, n) of the preprocessed text as a list of items in text
   order (a function, a declaration, a directive line or a line marker each),
   with the line marker in effect at each, and each function's keep_inline
   resolved against the prototypes. Shared by c_split and the --incremental
   split, so both read the text the same way. 0, or -1 when the text holds
   something this reader does not read (with SPINEL_SPLIT_DEBUG saying
   where). *prags: the unit pragmas, which IT_UNIT items index. */
static int cs_lex(const char *t, size_t n, Item **items, int *nitems, UnitPragma **prags, int *nprags) {
  Item *it = NULL; int nit = 0, cap = 0;
  UnitPragma *up = NULL; int nup = 0, upcap = 0;
  int ok = 1;
  size_t i = 0;
  while (i < n && ok) {
    while (i < n && (t[i] == ' ' || t[i] == '\n' || t[i] == '\t' || t[i] == '\r')) i++;
    if (i >= n) break;
    /* an empty declaration declares nothing (a macro that expands to
       nothing before its `;`, sys/socket.h in the macOS 27 SDK) */
    if (t[i] == ';') { i++; continue; }
    if (nit == cap) { cap = cap ? cap * 2 : 4096; it = realloc(it, sizeof *it * (size_t)cap); }
    Item *x = &it[nit];
    memset(x, 0, sizeof *x);
    x->st = i;
    x->unit = -1; x->home = -1;
    if (t[i] == '#') {   /* a line marker, or a directive line cc -E kept (#pragma) */
      int mark = is_marker(t, i, n);
      while (i < n && t[i] != '\n') i++;
      x->en = i; x->kind = mark ? IT_MARK : IT_HDR;
      if (!mark) {
        if (nup == upcap) { upcap = upcap ? upcap * 2 : 256; up = realloc(up, sizeof *up * (size_t)upcap); }
        int u = unit_pragma(t, x->st, x->en, &up[nup]);
        if (u < 0) { ok = 0; break; }
        if (u) { x->kind = IT_UNIT; x->body = (size_t)nup++; }
      }
      nit++;
      continue;
    }
    int d = 0;
    size_t en = 0, body = 0;
    for (; i < n; ) {
      char ch = t[i];
      if (ch == '"' || ch == '\'') { i = skip_lit(t, n, i); continue; }
      if (ch == '(' || ch == '[') d++;
      else if (ch == ')' || ch == ']') d--;
      else if (ch == '{') {
        if (d == 0 && !body) {
          size_t p = i;
          while (p > x->st && (t[p - 1] == ' ' || t[p - 1] == '\n' || t[p - 1] == '\t')) p--;
          if (p > x->st && t[p - 1] == ')' && first_paren(t, x->st, i) < i &&
              !starts_with_word(t, x->st, i, "typedef") && !starts_with_word(t, x->st, i, "struct") &&
              !starts_with_word(t, x->st, i, "union") && !starts_with_word(t, x->st, i, "enum")) {
            /* a function body: through its matching brace */
            body = i;
            int bd = 0;
            for (; i < n; ) {
              char c2 = t[i];
              if (c2 == '"' || c2 == '\'') { i = skip_lit(t, n, i); continue; }
              if (c2 == '{') bd++;
              else if (c2 == '}' && --bd == 0) { i++; break; }
              i++;
            }
            en = i;
            break;
          }
        }
        d++;
      }
      else if (ch == '}') d--;
      else if (ch == ';' && d == 0) { i++; en = i; break; }
      i++;
    }
    if (!en) { ok = 0; break; }
    x->en = en;
    x->kind = IT_PROTO;   /* a declaration, sorted below, unless named here */
    if (body) {
      x->kind = IT_FN; x->body = body;
      size_t fp = first_paren(t, x->st, body);
      ident_before(t, x->st, fp, x->name, sizeof x->name);
      if (!x->name[0]) { ok = 0; break; }
      int inl = has_word(t, x->st, fp, "inline") || has_word(t, x->st, fp, "__inline__") ||
                has_word(t, x->st, fp, "__inline") || has_word(t, x->st, body, "always_inline") ||
                has_word(t, x->st, body, "SP_ALWAYS_INLINE");
      /* a static local needs the single definition; 2 marks it */
      x->keep_inline = has_word(t, body, en, "static") ? 2 : inl;
    }
    else if (starts_with_word(t, x->st, en, "typedef") || starts_with_word(t, x->st, en, "extern") ||
             starts_with_word(t, x->st, en, "_Static_assert")) {
      x->kind = IT_HDR;
    }
    else if (starts_with_word(t, x->st, en, "struct") || starts_with_word(t, x->st, en, "union") ||
             starts_with_word(t, x->st, en, "enum")) {
      /* `struct X { ... };` or `struct X;` declares a tag; anything with a
         declarator after it (`struct X x;`) is a variable */
      size_t p = trim_trailing_attrs(t, x->st, en - 1);   /* before the `;` */
      int words = 0;
      for (size_t q = x->st; q < en; ) {
        if (t[q] == '"' || t[q] == '\'') { q = skip_lit(t, en, q); continue; }
        if (is_idch(t[q])) { words++; while (q < en && is_idch(t[q])) q++; }
        else q++;
      }
      if ((p > x->st && t[p - 1] == '}') || words <= 2) x->kind = IT_HDR;
      else x->kind = IT_PROTO;   /* sorted below: a prototype or a variable */
    }
    if (!body && x->kind == IT_PROTO) {
      size_t fp = first_paren(t, x->st, en);
      size_t q = fp + 1;
      while (q < en && (t[q] == ' ' || t[q] == '\n')) q++;
      long cut = decl_cut(t, x->st, en);
      if (fp < en && (cut < 0 || (size_t)cut == en) && t[q] != '*') x->kind = IT_PROTO;
      else x->kind = IT_VAR;
    }
    if (x->kind == IT_PROTO) {
      size_t fp = first_paren(t, x->st, en);
      ident_before(t, x->st, fp, x->name, sizeof x->name);
      /* a prototype that says inline makes the function inline wherever it
         is defined (keep_inline here: "declared inline") */
      x->keep_inline = has_word(t, x->st, en, "inline") || has_word(t, x->st, en, "__inline__") ||
                       has_word(t, x->st, en, "__inline") || has_word(t, x->st, en, "always_inline") ||
                       has_word(t, x->st, en, "SP_ALWAYS_INLINE");
    }
    if (x->kind == IT_VAR && !x->name[0]) {
      long cut = decl_cut(t, x->st, en);
      if (cut < 0) cut = (long)top_comma(t, x->st, en);   /* the first of several */
      /* the declarator's name: the last identifier before `[`, `=` or `;` */
      size_t lim = (size_t)cut;
      int dd = 0;
      for (size_t q = x->st; q < lim; q++) {
        if (t[q] == '(' ) dd++;
        else if (t[q] == ')') dd--;
        else if (t[q] == '[' && dd == 0) { lim = q; break; }
      }
      if (lim > x->st && t[lim - 1] == ';') lim--;
      /* a pointer to function names itself inside `(*name)` */
      for (size_t q = x->st; q + 1 < lim; q++) {
        if (t[q] != '(' || t[q + 1] != '*') continue;
        size_t r = q + 2;
        while (r < lim && (t[r] == ' ' || t[r] == '*')) r++;
        size_t e = r;
        while (e < lim && is_idch(t[e])) e++;
        if (e > r && e - r < sizeof x->name) { memcpy(x->name, t + r, e - r); x->name[e - r] = 0; }
        break;
      }
      if (!x->name[0]) ident_before(t, x->st, trim_trailing_attrs(t, x->st, lim), x->name, sizeof x->name);
      if (!x->name[0]) { ok = 0; break; }
    }
    nit++;
  }
  if (!ok) {
    free(it); free(up); return -1;
  }

  /* The line marker in effect at each item's start: one walk over the text,
     counting lines and reading each marker where it stands (a marker names
     the line that follows it). */
  {
    size_t mfile = 0, mflen = 0, pos = 0;
    int msys = 0, mflags = 0;
    long ln = 1;
    if (is_marker(t, 0, n)) { pos = read_marker(t, 0, n, &mfile, &mflen, &msys, &mflags, &ln); if (pos < n) pos++; }
    for (int k = 0; k < nit; k++) {
      while (pos < it[k].st) {
        if (t[pos] == '\n') {
          ln++; pos++;
          if (is_marker(t, pos, n)) { pos = read_marker(t, pos, n, &mfile, &mflen, &msys, &mflags, &ln); if (pos < n) pos++; }
          continue;
        }
        pos++;
      }
      it[k].mfile = mfile; it[k].mflen = mflen; it[k].msys = msys; it[k].mflags = mflags; it[k].mline = ln;
    }
  }

  /* A function is inline when its definition or any prototype says so,
     unless a static local needs its one definition. */
  {
    int np2 = 0;
    const char **pin = malloc(sizeof(char *) * (size_t)(nit + 1));
    for (int k = 0; k < nit; k++) if (it[k].kind == IT_PROTO && it[k].keep_inline) pin[np2++] = it[k].name;
    qsort(pin, (size_t)np2, sizeof *pin, cmpstr);
    for (int k = 0; k < nit; k++) {
      if (it[k].kind != IT_FN) continue;
      if (it[k].keep_inline == 2) { it[k].keep_inline = 0; continue; }
      const char *key = it[k].name;
      if (!it[k].keep_inline && bsearch(&key, pin, (size_t)np2, sizeof *pin, cmpstr)) it[k].keep_inline = 1;
    }
    free(pin);
  }
  *items = it; *nitems = nit; *prags = up; *nprags = nup;
  return 0;
}

int c_split(const char *pre_path, const char *out_dir, int nparts,
            char (*part_paths)[4096], char *hdr_path, size_t hdr_sz) {
  FILE *in = fopen(pre_path, "rb");
  if (!in) return -1;
  fseek(in, 0, SEEK_END);
  long fl = ftell(in);
  fseek(in, 0, SEEK_SET);
  if (fl <= 0) { fclose(in); return -1; }
  char *t = malloc((size_t)fl + 1);
  size_t n = fread(t, 1, (size_t)fl, in);
  fclose(in);
  t[n] = 0;

  Item *it = NULL; int nit = 0;
  UnitPragma *up = NULL; int nup = 0;
  if (cs_lex(t, n, &it, &nit, &up, &nup) != 0) { free(t); return -1; }
  free(up);   /* the parts are cut by size, not at the unit pragmas */
  int ok = 1;
  /* A name kept static inline keeps its prototypes as written; every other
     function's prototypes lose `static` and any inline word. */
  int nfn = 0; size_t fn_bytes = 0;
  for (int k = 0; k < nit; k++)
    if (it[k].kind == IT_FN && !it[k].keep_inline) { nfn++; fn_bytes += fn_weight(t, it[k].st, it[k].en); }
  /* sorted names of the inline-kept functions, for the prototype rule */
  int ninl = 0;
  const char **inl = malloc(sizeof(char *) * (size_t)(nit + 1));
  for (int k = 0; k < nit; k++) if (it[k].kind == IT_FN && it[k].keep_inline) inl[ninl++] = it[k].name;
  qsort(inl, (size_t)ninl, sizeof *inl, cmpstr);

  snprintf(hdr_path, hdr_sz, "%s/sp_split.h", out_dir);
  FILE *h = fopen(hdr_path, "wb");
  if (!h) { free(t); free(it); free(inl); return -1; }
  FILE **pf = calloc((size_t)nparts, sizeof(FILE *));
  for (int p = 0; p < nparts; p++) {
    snprintf(part_paths[p], 4096, "%s/sp_part%d.c", out_dir, p);
    pf[p] = fopen(part_paths[p], "wb");
    if (!pf[p]) { ok = 0; break; }
    fprintf(pf[p], "#include \"sp_split.h\"\n");
  }
  if (!ok) {
    fclose(h);
    for (int p = 0; p < nparts; p++) if (pf[p]) fclose(pf[p]);
    free(pf); free(t); free(it); free(inl);
    return -1;
  }
  size_t per = fn_bytes / (size_t)nparts + 1, acc = 0;
  int cur = 0;
  Out oh;
  out_init(&oh, h, out_dir, "h");
  Out *op = calloc((size_t)nparts, sizeof(Out));
  char (*ptag)[16] = calloc((size_t)nparts, sizeof *ptag);
  for (int p = 0; p < nparts; p++) {
    snprintf(ptag[p], sizeof ptag[p], "p%d_", p);
    out_init(&op[p], pf[p], out_dir, ptag[p]);
  }
  for (int k = 0; k < nit; k++) {
    Item *x = &it[k];
    switch (x->kind) {
      case IT_MARK:
        break;   /* replayed where the items it covered go out */
      case IT_UNIT:
        break;   /* a cut for the --incremental split only */
      case IT_HDR:
        out_sync(&oh, t, x);
        fwrite(t + x->st, 1, x->en - x->st, oh.f); fputc('\n', oh.f);
        out_advance(&oh, t, x->st, x->en, 1);
        break;
      case IT_PROTO: {
        const char *key = x->name;
        int keep = bsearch(&key, inl, (size_t)ninl, sizeof *inl, cmpstr) != NULL;
        out_sync(&oh, t, x);
        if (keep) fwrite(t + x->st, 1, x->en - x->st, oh.f);
        else put_stripped(oh.f, t, x->st, x->en, 1);
        fputc('\n', oh.f);
        out_advance(&oh, t, x->st, x->en, 1);
        break;
      }
      case IT_VAR:
        out_sync(&oh, t, x);
        out_sync(&op[0], t, x);
        put_vars(oh.f, op[0].f, t, x->st, x->en);
        oh.line = -1; op[0].line = -1;   /* rewritten: the line count is not the text's */
        break;
      case IT_FN:
        if (x->keep_inline) {
          out_sync(&oh, t, x);
          fwrite(t + x->st, 1, x->en - x->st, oh.f); fputc('\n', oh.f);
          out_advance(&oh, t, x->st, x->en, 1);
        }
        else {
          out_sync(&oh, t, x);
          put_stripped(oh.f, t, x->st, x->body, 1);
          fputs(";\n", oh.f);
          out_advance(&oh, t, x->st, x->body, 1);
          /* the storage words go from the declaration only: the body's own
             `static` locals stay static */
          out_sync(&op[cur], t, x);
          put_stripped(op[cur].f, t, x->st, x->body, 1);
          fwrite(t + x->body, 1, x->en - x->body, op[cur].f);
          fputc('\n', op[cur].f);
          out_advance(&op[cur], t, x->st, x->en, 1);
          acc += fn_weight(t, x->st, x->en);
          if (acc >= per * (size_t)(cur + 1) && cur < nparts - 1) cur++;
        }
        break;
    }
  }
  /* a write that failed (a full disk) leaves a truncated header or part,
     which would compile into errors that name no cause: the split is
     abandoned instead, saying why (#9267) */
  out_side_end(&oh);
  int bad = oh.bad | ferror(h) | (fclose(h) != 0);
  const char *bad_path = bad ? hdr_path : NULL;
  for (int p = 0; p < nparts; p++) {
    out_side_end(&op[p]);
    if ((op[p].bad | ferror(pf[p]) | (fclose(pf[p]) != 0)) && !bad_path) bad_path = part_paths[p];
  }
  free(op); free(ptag); free(pf); free(t); free(it); free(inl);
  if (bad_path) {
    fprintf(stderr, "spinel: writing %s failed (%s); compiling the C as one unit instead\n", bad_path, strerror(errno));
    return -1;
  }
  return nparts;
}

/* ---------- the --incremental split ----------

   c_split cuts by size into a fixed number of parts; c_split_units cuts at
   the `#pragma spinel unit <tag> <group>` lines an --incremental build's
   codegen writes, so that a unit's text depends on the methods in it and on
   little else, and an edit recompiles one unit out of a cache of objects.
   The unit's own items are written whole; everything another unit needs from
   it is declared by a shared header or by the unit's import list:

   - the runtime region (every item before the return from
     `#include "spinel_rt.h"`) goes to sp_rt.h as c_split's header would
     have it, and its non-inline functions and variables to rt units
     packed in text order. It does not depend on the program, so those
     units are shared by every program built with the same flags;
   - the program region's untagged types, typedefs and inline helpers go to
     sp_program.h; its methods, procs, literals and variables are each
     defined in one unit, and declared by a table of prototypes and externs
     from which every unit takes the entries whose names it uses;
   - a tag's items (all of its segments, including a capture struct's
     typedef, which cannot be declared elsewhere) stay in one unit. Groups
     (a class in a file) are packed into units by a jump consistent hash,
     so the unit set moves little when the program grows; a group over the
     size target gets units of its own, by tag.

   Every unit is named by the SHA-256 of its text without the line markers
   (`-g` keeps them), so the same text compiles under the same name in every
   build. A method named like `_fzl_<hex>` or `sp_pd_<hex>` puts that name
   in a pragma line, where codegen's text scanners may take it for a literal
   reference: the worst case is one unused literal definition.
   SPINEL_UNIT_DROP_DECL=<name> (internal, for the gate) leaves that one
   declaration out of the table, to prove a missing declaration is a compile
   error rather than an object. */

static uint64_t fnv1a64_n(const char *s, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 1099511628211ULL;
  return h;
}

static uint64_t fnv1a64(const char *s) {
  return fnv1a64_n(s, strlen(s));
}

/* Lamping and Veach's jump consistent hash: 0..nb-1. When nb grows by one,
   only about 1/nb of the keys move, so a program crossing the next multiple
   of the unit size does not reshuffle every unit. */
static int jump_hash(uint64_t key, int nb) {
  int64_t b = -1, j = 0;
  while (j < nb) {
    b = j;
    key = key * 2862933555777941757ULL + 1;
    j = (int64_t)(((uint64_t)(b + 1) << 31) / ((key >> 33) + 1));
  }
  return (int)b;
}

/* A name -> index map. The keys are not copied: they are item names,
   pragma words and the declaration table's own copies, which outlive it. */
typedef struct {
  const char **key;
  size_t *klen;
  int *val;
  size_t cap, n;
} NameMap;

static int nm_find(const NameMap *m, const char *s, size_t l) {
  if (!m->cap) return -1;
  size_t i = (size_t)fnv1a64_n(s, l) & (m->cap - 1);
  for (; m->key[i]; i = (i + 1) & (m->cap - 1))
    if (m->klen[i] == l && memcmp(m->key[i], s, l) == 0) return m->val[i];
  return -1;
}

static void nm_put(NameMap *m, const char *s, int v) {
  if ((m->n + 1) * 2 > m->cap) {
    NameMap g = { NULL, NULL, NULL, m->cap ? m->cap * 2 : 1024, 0 };
    g.key = calloc(g.cap, sizeof *g.key);
    g.klen = calloc(g.cap, sizeof *g.klen);
    g.val = calloc(g.cap, sizeof *g.val);
    for (size_t i = 0; i < m->cap; i++) if (m->key[i]) nm_put(&g, m->key[i], m->val[i]);
    free(m->key); free(m->klen); free(m->val);
    *m = g;
  }
  size_t l = strlen(s);
  size_t i = (size_t)fnv1a64_n(s, l) & (m->cap - 1);
  for (; m->key[i]; i = (i + 1) & (m->cap - 1))
    if (m->klen[i] == l && memcmp(m->key[i], s, l) == 0) { m->val[i] = v; return; }
  m->key[i] = s; m->klen[i] = l; m->val[i] = v; m->n++;
}

static void nm_free(NameMap *m) {
  free(m->key); free(m->klen); free(m->val);
  memset(m, 0, sizeof *m);
}

/* A declaration-table entry: what a unit that names `name` but does not
   hold its first declaration needs to compile. Entries are added in text
   order of their first declaration, so an entry's index is its order. */
enum { DK_FN, DK_PROTO, DK_VAR };
typedef struct {
  char *name;           /* malloc'd: the table grows, and the map keeps the key */
  int kind;
  int item;             /* the item its text comes from */
  int k;                /* DK_VAR: the declarator of that item */
  int pinned;           /* written into sp_program.h ahead of an inline helper */
  int stamp;            /* the last name scan that met it */
} CsDecl;

/* A unit: its place in link order and the items it defines. */
enum { UK_RT, UK_PROGRAM, UK_TAGGED };
typedef struct {
  char label[CS_LABEL_MAX];
  int kind;
  char *groups;         /* tagged: its groups, comma separated; else NULL */
  int nitems, *items;   /* item indices, in text order */
} CsUnit;

typedef struct {
  const char *t; size_t n;
  Item *it; int nit;
  UnitPragma *up; int nup;
  const CSplitOpts *opts;
  const char *dir;
  /* tags and their groups, from the pragmas */
  int *ptag;            /* per pragma: its tag, or -1 for a `prog prog` reset */
  int ntag, ngroup;
  int *tag_group;
  NameMap tags, groups;
  /* functions kept static inline, and each variable's defining occurrence */
  NameMap inl, vars;
  /* the declaration table */
  CsDecl *decl; int ndecl, dcap;
  NameMap dmap;
  int stamp;
  CsUnit *u; int nu;
} Split;

/* The program's line markers name the generated C by the path cc -E was
   given, a temp name with the pid in it. Every marker naming that file is
   rewritten to "spinel_program.c" before anything is read, so no pid reaches
   a header or a unit (it would be in a -g unit's hash and in its DWARF).
   NULL when the text does not open with a line marker. */
static char *cs_canon_markers(const char *t, size_t n, size_t *nout) {
  static const char canon[] = "\"spinel_program.c\"";
  size_t cl = sizeof canon - 1;
  if (!is_marker(t, 0, n)) return NULL;
  size_t mf = 0, mfl = 0; int sys = 0, ret = 0; long ln = 0;
  read_marker(t, 0, n, &mf, &mfl, &sys, &ret, &ln);
  if (!mfl) return NULL;
  size_t hits = 0;
  for (size_t i = 0; i < n; ) {
    size_t e = i;
    while (e < n && t[e] != '\n') e++;
    if (is_marker(t, i, e)) {
      size_t f = 0, fl = 0; int s2 = 0, r2 = 0; long l2 = 0;
      read_marker(t, i, e, &f, &fl, &s2, &r2, &l2);
      if (fl == mfl && memcmp(t + f, t + mf, mfl) == 0) hits++;
    }
    i = e + 1;
  }
  char *o = malloc(n + hits * cl + 1);
  size_t w = 0;
  for (size_t i = 0; i < n; ) {
    size_t e = i;
    while (e < n && t[e] != '\n') e++;
    size_t f = 0, fl = 0;
    if (is_marker(t, i, e)) {
      int s2 = 0, r2 = 0; long l2 = 0;
      read_marker(t, i, e, &f, &fl, &s2, &r2, &l2);
    }
    if (fl == mfl && fl && memcmp(t + f, t + mf, mfl) == 0) {
      memcpy(o + w, t + i, f - i); w += f - i;
      memcpy(o + w, canon, cl); w += cl;
      memcpy(o + w, t + f + fl, e - f - fl); w += e - f - fl;
    }
    else { memcpy(o + w, t + i, e - i); w += e - i; }
    if (e < n) o[w++] = '\n';
    i = e + 1;
  }
  o[w] = 0;
  *nout = w;
  return o;
}

/* Does the marker item name the runtime header, `.../spinel_rt.h`? */
static int names_rt_header(const Item *x, const char *t) {
  static const char tail[] = "spinel_rt.h\"";
  size_t tl = sizeof tail - 1;
  if (x->mflen < tl + 1 || memcmp(t + x->mfile + x->mflen - tl, tail, tl) != 0) return 0;
  char c = t[x->mfile + x->mflen - tl - 1];
  return c == '"' || c == '/';
}

/* The runtime region is every item before the return from
   `#include "spinel_rt.h"`: the first marker that names the main file (the
   file of the text's first marker) with flag 2 after the entry into
   spinel_rt.h (flag 1). The entry is looked for first because clang, unlike
   gcc, returns to the main file with flag 2 once already, from its
   <built-in> and <command line> block, before the include. Nothing but
   comments and defines precede that include in the generated C, and
   everything after its return is the program's, whatever file a marker
   names. -1 when either marker is missing. */
static int cs_regions(Item *it, int nit, const char *t) {
  int k0 = -1, ke = -1, kb = -1;
  for (int k = 0; k < nit && k0 < 0; k++) if (it[k].kind == IT_MARK && it[k].mflen) k0 = k;
  if (k0 < 0) return -1;
  for (int k = k0 + 1; k < nit && ke < 0; k++)
    if (it[k].kind == IT_MARK && (it[k].mflags & 1) && names_rt_header(&it[k], t)) ke = k;
  if (ke < 0) return -1;
  for (int k = ke + 1; k < nit && kb < 0; k++)
    if (it[k].kind == IT_MARK && (it[k].mflags & 2) && it[k].mflen == it[k0].mflen &&
        memcmp(t + it[k].mfile, t + it[k0].mfile, it[k0].mflen) == 0)
      kb = k;
  if (kb < 0) return -1;
  for (int k = 0; k < nit; k++) it[k].region = k >= kb;
  return 0;
}

/* Each pragma names a tag and the group it is packed with; every program
   item after it carries that tag, until the next pragma or a `prog prog`
   reset. The runtime region holds no pragma and is never tagged. */
static void cs_tags(Split *S) {
  S->ptag = malloc(sizeof(int) * (size_t)(S->nup + 1));
  S->tag_group = malloc(sizeof(int) * (size_t)(S->nup + 1));
  for (int p = 0; p < S->nup; p++) {
    UnitPragma *u = &S->up[p];
    if (strcmp(u->tag, "prog") == 0 && strcmp(u->group, "prog") == 0) { S->ptag[p] = -1; continue; }
    int tg = nm_find(&S->tags, u->tag, strlen(u->tag));
    if (tg < 0) {
      tg = S->ntag++;
      nm_put(&S->tags, u->tag, tg);
      int g = nm_find(&S->groups, u->group, strlen(u->group));
      if (g < 0) { g = S->ngroup++; nm_put(&S->groups, u->group, g); }
      S->tag_group[tg] = g;
    }
    S->ptag[p] = tg;
  }
  int cur = -1;
  for (int k = 0; k < S->nit; k++) {
    Item *x = &S->it[k];
    if (x->kind == IT_UNIT) { cur = S->ptag[x->body]; continue; }
    x->unit = x->region ? cur : -1;
  }
}

/* The names that stay static inline in a header: an untagged inline
   function's. A tagged one (an inline-hinted method, which --incremental
   does not emit, so only defensively) is demoted to a plain function in its
   unit, since the other units that call it cannot see its body. */
static void cs_inline_names(Split *S) {
  for (int k = 0; k < S->nit; k++) {
    Item *x = &S->it[k];
    if (x->kind != IT_FN || !x->keep_inline) continue;
    if (x->unit >= 0) { x->keep_inline = 0; continue; }
    nm_put(&S->inl, x->name, k);
  }
}

static int is_inline_name(const Split *S, const char *name) {
  return nm_find(&S->inl, name, strlen(name)) >= 0;
}

/* The shared specifiers of a declaration of several names end where the
   first declarator starts: at its name, or at the `*`/`(` before it. c1 is
   the first top-level comma. (put_vars' reading, for the unit split.) */
static size_t var_specs_end(const char *t, size_t st, size_t c1) {
  size_t nm_end = top_eq(t, st, c1);
  { int dd = 0; for (size_t q = st; q < nm_end; q++) {
      if (t[q] == '(') dd++; else if (t[q] == ')') dd--;
      else if (t[q] == '[' && dd == 0) { nm_end = q; break; } } }
  while (nm_end > st && (t[nm_end - 1] == ' ' || t[nm_end - 1] == '\n')) nm_end--;
  size_t ds = nm_end;
  while (ds > st && is_idch(t[ds - 1])) ds--;
  while (ds > st && (t[ds - 1] == '*' || t[ds - 1] == ' ' || t[ds - 1] == '(')) {
    if (t[ds - 1] == ' ') { size_t b = ds - 1; while (b > st && t[b - 1] == ' ') b--; if (b > st && (t[b - 1] == '*' || t[b - 1] == '(')) { ds = b; continue; } break; }
    ds--;
  }
  return ds;
}

/* The k-th declarator [*a, *b) of a declaration of several names, with its
   initialiser; 0 when there is no k-th. */
static int var_declarator(const char *t, size_t st, size_t en, int k, size_t *a, size_t *b) {
  size_t end = en - 1, c1 = top_comma(t, st, end);
  size_t p = var_specs_end(t, st, c1);
  for (int j = 0; ; j++) {
    size_t q = top_comma(t, p, end);
    if (j == k) { *a = p; *b = q; return 1; }
    if (q >= end) return 0;
    p = q + 1;
  }
}

/* The name a declarator [a, b) declares: the last identifier outside any
   (), [] or {} before its initialiser, or the one inside `(*name)` for a
   pointer to function. A struct body's members and an array's size are
   inside brackets, so `struct { char d[3]; } x` names x. */
static void declarator_name(const char *t, size_t a, size_t b, char *out, size_t osz) {
  size_t lim = trim_trailing_attrs(t, a, top_eq(t, a, b));
  size_t ls = 0, le = 0;
  int d = 0;
  out[0] = 0;
  for (size_t q = a; q < lim; ) {
    char ch = t[q];
    if (ch == '"' || ch == '\'') { q = skip_lit(t, lim, q); continue; }
    if (is_idch(ch)) {
      size_t s = q;
      while (q < lim && is_idch(t[q])) q++;
      if (d == 0 && !(t[s] >= '0' && t[s] <= '9')) { ls = s; le = q; }
      continue;
    }
    if (ch == '(' && d == 0 && q + 1 < lim && t[q + 1] == '*') {
      /* `(*name)`, `(*const name[])`: a pointer to function is named by
         the last identifier directly inside the parentheses */
      size_t fs = 0, fe = 0, r = q + 1;
      for (int pd = 1; r < lim && pd > 0; ) {
        char c2 = t[r];
        if (is_idch(c2)) {
          size_t s = r;
          while (r < lim && is_idch(t[r])) r++;
          if (pd == 1) { fs = s; fe = r; }
          continue;
        }
        if (c2 == '(' || c2 == '[') pd++;
        else if (c2 == ')' || c2 == ']') pd--;
        r++;
      }
      if (fe > fs) { ls = fs; le = fe; break; }
    }
    if (ch == '(' || ch == '[' || ch == '{') d++;
    else if (ch == ')' || ch == ']' || ch == '}') d--;
    q++;
  }
  if (le > ls && le - ls < osz) { memcpy(out, t + ls, le - ls); out[le - ls] = 0; }
}

/* Does the variable declaration [st, en) initialise anything? */
static int var_has_init(const char *t, size_t st, size_t en) {
  return top_eq(t, st, en - 1) < en - 1;
}

/* A variable is defined once, at its defining occurrence: the first
   occurrence with an initialiser, else the first. Every other occurrence is
   dropped, which keeps a tentative `static T *x;` in one unit and its
   `= NULL` in another (SP_POOL_DECLARE before SP_POOL_DEFINE) from defining
   x twice. vars maps a name to its defining occurrence. */
static void cs_vars(Split *S) {
  for (int k = 0; k < S->nit; k++) {
    Item *x = &S->it[k];
    if (x->kind != IT_VAR) continue;
    /* the lexer's name stops at the first `[`, which may be a member's in
       an anonymous struct; here the name keys the definition */
    size_t a = x->st, b = top_comma(S->t, x->st, x->en - 1);
    if (b < x->en - 1) var_declarator(S->t, x->st, x->en, 0, &a, &b);
    char nm[128];
    declarator_name(S->t, a, b, nm, sizeof nm);
    if (nm[0]) memcpy(x->name, nm, sizeof nm);
    int d = nm_find(&S->vars, x->name, strlen(x->name));
    if (d < 0) nm_put(&S->vars, x->name, k);
    else if (!var_has_init(S->t, S->it[d].st, S->it[d].en) && var_has_init(S->t, x->st, x->en))
      nm_put(&S->vars, x->name, k);
  }
  for (int k = 0; k < S->nit; k++) {
    Item *x = &S->it[k];
    if (x->kind == IT_VAR) x->drop = nm_find(&S->vars, x->name, strlen(x->name)) != k;
  }
}

/* A one-declarator variable whose type is an untagged struct or union:
   q just past the `struct`/`union` word, r at its `{`, bc at its `}`. 0 when
   the type is anything else. */
static int var_anon(const char *t, size_t st, size_t end, size_t *q, size_t *r, size_t *bc) {
  size_t eq = top_eq(t, st, end);
  for (size_t p = st; p < eq; ) {
    if (!is_idch(t[p])) { p++; continue; }
    size_t ws = p;
    while (p < eq && is_idch(t[p])) p++;
    size_t wl = p - ws;
    if (!((wl == 6 && memcmp(t + ws, "struct", 6) == 0) || (wl == 5 && memcmp(t + ws, "union", 5) == 0))) continue;
    size_t s = p;
    while (s < eq && (t[s] == ' ' || t[s] == '\n')) s++;
    if (s >= eq || t[s] != '{') return 0;   /* tagged */
    int bd = 0; size_t c = s;
    for (; c < eq; c++) { if (t[c] == '{') bd++; else if (t[c] == '}' && --bd == 0) break; }
    if (c >= eq) return 0;
    *q = p; *r = s; *bc = c;
    return 1;
  }
  return 0;
}

/* The `extern` declaration of the k-th name of the variable declaration
   [st, en), for a header or a declaration-table entry. An untagged struct
   or union is a different type in every declaration that spells it, so the
   extern and the definition (put_var_def) both name it
   `sp__anon_<variable>`: a tag from the name, not from a counter, keeps the
   text independent of what else the split wrote before. */
static void put_var_decl(FILE *f, const char *t, size_t st, size_t en, int k, const char *name) {
  size_t end = en - 1, q, r, bc, a, b;
  if (top_comma(t, st, end) >= end) {
    fputs("extern ", f);
    if (var_anon(t, st, end, &q, &r, &bc)) {
      put_stripped(f, t, st, q, 0); fprintf(f, " sp__anon_%s ", name);
      put_stripped(f, t, r, top_eq(t, st, end), 0);
    }
    else put_stripped(f, t, st, top_eq(t, st, end), 0);
    fputs(";\n", f);
    return;
  }
  if (!var_declarator(t, st, en, k, &a, &b)) return;
  size_t ps = var_specs_end(t, st, top_comma(t, st, end));
  fputs("extern ", f);
  put_stripped(f, t, st, ps, 0); fputc(' ', f);
  fwrite(t + a, 1, top_eq(t, a, b) - a, f); fputs(";\n", f);
}

/* The definition of every name of the variable declaration [st, en),
   without `static`. */
static void put_var_def(FILE *f, const char *t, size_t st, size_t en, const char *name) {
  size_t end = en - 1, q, r, bc, a, b;
  if (top_comma(t, st, end) >= end) {
    if (var_anon(t, st, end, &q, &r, &bc)) {
      put_stripped(f, t, st, q, 0); fprintf(f, " sp__anon_%s ", name);
      put_stripped(f, t, bc + 1, en, 0);
    }
    else put_stripped(f, t, st, en, 0);
    fputc('\n', f);
    return;
  }
  size_t ps = var_specs_end(t, st, top_comma(t, st, end));
  for (int k = 0; var_declarator(t, st, en, k, &a, &b); k++) {
    put_stripped(f, t, st, ps, 0); fputc(' ', f);
    fwrite(t + a, 1, b - a, f); fputs(";\n", f);
  }
}

/* A function's text with each `_sp_gcr_<N>` renamed `_sp_gcr_<k>`, k
   counting from 1 in order of first appearance. SP_GC_ROOT numbers its
   guards with __COUNTER__, which runs across the whole translation unit:
   unrenamed, one root added near the top would change the text of every
   function after it, and every unit would miss. */
static void put_body(FILE *f, const char *t, size_t st, size_t en) {
  size_t *seen = NULL, *seenl = NULL;
  int ns = 0, cap = 0;
  for (size_t i = st; i < en; ) {
    char ch = t[i];
    if (ch == '"' || ch == '\'') { size_t j = skip_lit(t, en, i); fwrite(t + i, 1, j - i, f); i = j; continue; }
    if (!is_idch(ch)) { fputc(ch, f); i++; continue; }
    size_t s = i;
    while (i < en && is_idch(t[i])) i++;
    size_t d = s + 8;
    int gcr = i > d && memcmp(t + s, "_sp_gcr_", 8) == 0;
    for (size_t p = d; gcr && p < i; p++) if (t[p] < '0' || t[p] > '9') gcr = 0;
    if (!gcr) { fwrite(t + s, 1, i - s, f); continue; }
    int k = 0;
    while (k < ns && !(seenl[k] == i - d && memcmp(t + seen[k], t + d, i - d) == 0)) k++;
    if (k == ns) {
      if (ns == cap) { cap = cap ? cap * 2 : 16; seen = realloc(seen, sizeof *seen * (size_t)cap); seenl = realloc(seenl, sizeof *seenl * (size_t)cap); }
      seen[ns] = d; seenl[ns] = i - d; ns++;
    }
    fprintf(f, "_sp_gcr_%d", k + 1);
  }
  free(seen); free(seenl);
}

static void cs_decl_add(Split *S, const char *name, int kind, int item, int k) {
  if (!name[0] || nm_find(&S->dmap, name, strlen(name)) >= 0) return;   /* the first declaration wins */
  if (S->opts->drop_decl && strcmp(name, S->opts->drop_decl) == 0) return;
  if (S->ndecl == S->dcap) { S->dcap = S->dcap ? S->dcap * 2 : 4096; S->decl = realloc(S->decl, sizeof *S->decl * (size_t)S->dcap); }
  CsDecl *d = &S->decl[S->ndecl];
  memset(d, 0, sizeof *d);
  d->name = strdup(name);
  d->kind = kind; d->item = item; d->k = k;
  nm_put(&S->dmap, d->name, S->ndecl++);
}

/* The declaration table: one entry per program-region name a unit may use
   without defining it. A function's prototype (from its first prototype, or
   its definition's head), a variable's extern per declarator (from the
   defining occurrence, so an anonymous struct's tag matches the
   definition's). Runtime names are declared by sp_rt.h, inline helpers by
   sp_program.h. */
static void cs_decl_table(Split *S) {
  const char *t = S->t;
  for (int k = 0; k < S->nit; k++) {
    Item *x = &S->it[k];
    if (!x->region) continue;
    if (x->kind == IT_FN && !x->keep_inline) cs_decl_add(S, x->name, DK_FN, k, 0);
    else if (x->kind == IT_PROTO && !is_inline_name(S, x->name)) cs_decl_add(S, x->name, DK_PROTO, k, 0);
    else if (x->kind == IT_VAR) {
      size_t end = x->en - 1, a, b;
      if (top_comma(t, x->st, end) >= end) {
        cs_decl_add(S, x->name, DK_VAR, nm_find(&S->vars, x->name, strlen(x->name)), 0);
        continue;
      }
      for (int j = 0; var_declarator(t, x->st, x->en, j, &a, &b); j++) {
        char nm[128];
        declarator_name(t, a, b, nm, sizeof nm);
        cs_decl_add(S, nm, DK_VAR, k, j);
      }
    }
  }
}

static void put_decl(FILE *f, const Split *S, const CsDecl *d) {
  const Item *x = &S->it[d->item];
  if (d->kind == DK_FN) { put_stripped(f, S->t, x->st, x->body, 1); fputs(";\n", f); }
  else if (d->kind == DK_PROTO) { put_stripped(f, S->t, x->st, x->en, 1); fputc('\n', f); }
  else put_var_decl(f, S->t, x->st, x->en, d->k, d->name);
}

static int cmpint(const void *a, const void *b) {
  int x = *(const int *)a, y = *(const int *)b;
  return (x > y) - (x < y);
}

/* The table entries named in [st, en) (outside literals) that this scan has
   not met yet, appended to *list; pinned entries are in sp_program.h
   already and are skipped. */
static void cs_refs(Split *S, size_t st, size_t en, int **list, int *nl, int *cap) {
  const char *t = S->t;
  for (size_t i = st; i < en; ) {
    char ch = t[i];
    if (ch == '"' || ch == '\'') { i = skip_lit(t, en, i); continue; }
    if (!is_idch(ch)) { i++; continue; }
    size_t s = i;
    while (i < en && is_idch(t[i])) i++;
    if (t[s] >= '0' && t[s] <= '9') continue;
    int d = nm_find(&S->dmap, t + s, i - s);
    if (d < 0 || S->decl[d].pinned || S->decl[d].stamp == S->stamp) continue;
    S->decl[d].stamp = S->stamp;
    if (*nl == *cap) { *cap = *cap ? *cap * 2 : 256; *list = realloc(*list, sizeof **list * (size_t)*cap); }
    (*list)[(*nl)++] = d;
  }
}

/* An item's share of a unit: c_split's measure (bytes without markers and
   blank lines). */
static size_t cs_weight(const Split *S, const Item *x) {
  return fn_weight(S->t, x->st, x->en);
}

/* Does the item have a definition in some unit, and in which stream?
   0 runtime, 1 program, 2 tagged; -1 when it lives in a header or the
   declaration table only. */
static int cs_stream(const Item *x) {
  int placed = (x->kind == IT_FN && !x->keep_inline) || (x->kind == IT_VAR && !x->drop) ||
               (x->kind == IT_HDR && x->region && x->unit >= 0);
  if (!placed) return -1;
  return !x->region ? 0 : x->unit < 0 ? 1 : 2;
}

/* The program bucket's key: related items share a bucket, so an edit that
   adds a literal or a symbol touches one small unit, not one per name. */
static const char *cs_program_family(const char *name) {
  static const char *const sym[] = { "sp_sym_names", "sp_sym_htab", "sp_dyn_syms0", "sp_dyn_syms", "sp_dyn_cap", "sp_ndyn" };
  if (strncmp(name, "_fzl_", 5) == 0) return "fzl";
  if (strncmp(name, "_sym_", 5) == 0 || strncmp(name, "sp_dsym_", 8) == 0) return "sym";
  for (size_t k = 0; k < sizeof sym / sizeof sym[0]; k++) if (strcmp(name, sym[k]) == 0) return "sym";
  if (strncmp(name, "sp_re_pat_", 10) == 0) return "re";
  if (strcmp(name, "sp_bigl") == 0 || strcmp(name, "sp_bigl_s") == 0) return "bigl";
  return name;
}

static size_t ceil_div(size_t a, size_t b) {
  return (a + b - 1) / b;
}

typedef struct { char label[CS_LABEL_MAX]; int slot; } XLabel;
static int cmpxlabel(const void *a, const void *b) {
  return strcmp(((const XLabel *)a)->label, ((const XLabel *)b)->label);
}

/* Give every placed item its unit, and lay the units out in link order:
   runtime chunks (rt00, ...), program buckets (p00, ...), common tagged
   buckets (g000, ...), over-cap groups by label (x_<group>_<s>). Each rule
   is a function of the text alone; empty buckets are no unit. */
static void cs_assign_units(Split *S) {
  size_t U = S->opts->unit_bytes ? S->opts->unit_bytes : 262144;
  int nit = S->nit;
  int *slot = malloc(sizeof(int) * (size_t)(nit + 1));
  size_t *group_w = calloc((size_t)S->ngroup + 1, sizeof *group_w);
  size_t acc = 0, w_prog = 0, w_common = 0;
  int nrt = 0;
  for (int k = 0; k < nit; k++) {
    Item *x = &S->it[k];
    int st = cs_stream(x);
    size_t w = st < 0 ? 0 : cs_weight(S, x);
    if (st == 0) {   /* runtime: sequential chunks */
      slot[k] = nrt;
      acc += w;
      if (acc >= U) { nrt++; acc = 0; }
    }
    else if (st == 1) w_prog += w;
    else if (st == 2) group_w[S->tag_group[x->unit]] += w;
  }
  if (acc) nrt++;
  for (int g = 0; g < S->ngroup; g++) if (group_w[g] <= U) w_common += group_w[g];
  int bp = w_prog ? (int)ceil_div(w_prog, U) : 1;
  int bg = w_common ? (int)ceil_div(w_common, U) : 1;
  /* over-cap groups: sub-buckets at xbase[g] .. xbase[g] + S_g */
  int *xbase = calloc((size_t)S->ngroup + 1, sizeof(int));
  int nx = 0;
  for (int g = 0; g < S->ngroup; g++) {
    xbase[g] = nx;
    if (group_w[g] > U) nx += (int)ceil_div(group_w[g], U);
  }
  const char **gname = calloc((size_t)S->ngroup + 1, sizeof *gname);
  const char **tname = calloc((size_t)S->ntag + 1, sizeof *tname);
  for (int p = 0; p < S->nup; p++)
    if (S->ptag[p] >= 0) { tname[S->ptag[p]] = S->up[p].tag; gname[S->tag_group[S->ptag[p]]] = S->up[p].group; }
  for (int k = 0; k < nit; k++) {
    Item *x = &S->it[k];
    int st = cs_stream(x);
    if (st == 1) slot[k] = jump_hash(fnv1a64(cs_program_family(x->name)), bp);
    else if (st == 2) {
      int g = S->tag_group[x->unit];
      if (group_w[g] <= U) slot[k] = jump_hash(fnv1a64(gname[g]), bg);
      else slot[k] = xbase[g] + jump_hash(fnv1a64(tname[x->unit]), (int)ceil_div(group_w[g], U));
    }
  }
  /* the unit of each (stream, slot), -1 while empty */
  int base[4] = { 0, nrt, nrt + bp, nrt + bp + bg }, nslot = nrt + bp + bg + nx;
  int *su = malloc(sizeof(int) * (size_t)(nslot + 1));
  for (int s = 0; s < nslot; s++) su[s] = -1;
  for (int k = 0; k < nit; k++) {
    Item *x = &S->it[k];
    int st = cs_stream(x);
    if (st < 0) continue;
    if (st == 2 && group_w[S->tag_group[x->unit]] > U) st = 3;
    slot[k] += base[st];
    su[slot[k]] = 0;
  }
  S->u = calloc((size_t)nslot + 1, sizeof *S->u);
  for (int s = 0; s < base[3]; s++) {
    if (su[s] < 0) continue;
    CsUnit *u = &S->u[S->nu];
    if (s < base[1]) { u->kind = UK_RT; snprintf(u->label, sizeof u->label, "rt%02d", s); }
    else if (s < base[2]) { u->kind = UK_PROGRAM; snprintf(u->label, sizeof u->label, "p%02d", s - base[1]); }
    else { u->kind = UK_TAGGED; snprintf(u->label, sizeof u->label, "g%03d", s - base[2]); }
    su[s] = S->nu++;
  }
  XLabel *xl = calloc((size_t)nx + 1, sizeof *xl);
  int nxl = 0;
  for (int g = 0; g < S->ngroup; g++) {
    if (group_w[g] <= U) continue;
    int sg = (int)ceil_div(group_w[g], U);
    for (int j = 0; j < sg; j++) {
      int s = base[3] + xbase[g] + j;
      if (su[s] < 0) continue;
      snprintf(xl[nxl].label, sizeof xl[nxl].label, "x_%s_%02d", gname[g], j);
      xl[nxl++].slot = s;
    }
  }
  qsort(xl, (size_t)nxl, sizeof *xl, cmpxlabel);
  for (int j = 0; j < nxl; j++) {
    CsUnit *u = &S->u[S->nu];
    u->kind = UK_TAGGED;
    memcpy(u->label, xl[j].label, sizeof u->label);
    su[xl[j].slot] = S->nu++;
  }
  /* homes, the item lists, and each tagged unit's groups in text order */
  for (int k = 0; k < nit; k++) {
    if (cs_stream(&S->it[k]) < 0) continue;
    S->it[k].home = su[slot[k]];
    S->u[S->it[k].home].nitems++;
  }
  for (int j = 0; j < S->nu; j++) { S->u[j].items = malloc(sizeof(int) * (size_t)(S->u[j].nitems + 1)); S->u[j].nitems = 0; }
  for (int k = 0; k < nit; k++) {
    Item *x = &S->it[k];
    if (x->home < 0) continue;
    CsUnit *u = &S->u[x->home];
    u->items[u->nitems++] = k;
  }
  /* a unit at a time, so that gseen[g] == j + 1 means listed in this unit:
     an over-cap group's items alternate between its sub-units */
  int *gseen = calloc((size_t)S->ngroup + 1, sizeof(int));
  for (int j = 0; j < S->nu; j++) {
    CsUnit *u = &S->u[j];
    size_t glen = 0;
    for (int i = 0; i < u->nitems; i++) {
      const Item *x = &S->it[u->items[i]];
      if (!(x->region && x->unit >= 0)) continue;
      int g = S->tag_group[x->unit];
      if (gseen[g] == j + 1) continue;
      gseen[g] = j + 1;
      size_t l = strlen(gname[g]);
      u->groups = realloc(u->groups, glen + l + 2);
      if (glen) u->groups[glen++] = ',';
      memcpy(u->groups + glen, gname[g], l + 1);
      glen += l;
    }
  }
  free(gseen); free(xl); free(su); free(gname); free(tname);
  free(xbase); free(slot); free(group_w);
}

/* The SHA-256 of a written file as the cache sees it: without its line
   markers unless they are kept (-g), since they cannot reach an object
   built without debug info. *bytes: the marker-free size. -1 when it
   cannot be read. */
static int cs_hash_file(const char *path, int keep_markers, Sha256 *h, size_t *bytes) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  char *buf = NULL; size_t n = 0, cap = 0, r;
  char chunk[65536];
  while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
    if (n + r > cap) { cap = (n + r) * 2; buf = realloc(buf, cap); }
    memcpy(buf + n, chunk, r); n += r;
  }
  int bad = ferror(f);
  fclose(f);
  if (bad) { free(buf); return -1; }
  for (size_t i = 0; i < n; ) {
    size_t e = i;
    while (e < n && buf[e] != '\n') e++;
    if (e < n) e++;
    int mark = e - i >= 3 && buf[i] == '#' && buf[i + 1] == ' ' && buf[i + 2] >= '0' && buf[i + 2] <= '9';
    if (!mark) *bytes += e - i;
    if (!mark || keep_markers) sha256_update(h, buf + i, e - i);
    i = e;
  }
  free(buf);
  return 0;
}

/* sp_rt.h and sp_program.h, each hashed with the side files it includes,
   and like a unit without the markers unless -g keeps them. The program's
   literals and symbols stand among the header items in the generated C, so
   one literal added moves the line of every type after it: sp_program.h's
   text is unchanged (the literal is a unit's), but a hash over its markers
   would change and miss every unit that includes it. */
static int cs_header_hash(const char *dir, const char *hdr, const char *side, int nside, int keep, char out[65]) {
  Sha256 h; unsigned char dg[32]; size_t bytes = 0;
  char path[4200];
  sha256_init(&h);
  snprintf(path, sizeof path, "%s/%s", dir, hdr);
  if (cs_hash_file(path, keep, &h, &bytes) != 0) return -1;
  for (int k = 0; k < nside; k++) {
    snprintf(path, sizeof path, "%s/sp_sys_%s%d.h", dir, side, k);
    if (cs_hash_file(path, keep, &h, &bytes) != 0) return -1;
  }
  sha256_final(&h, dg);
  for (int k = 0; k < 32; k++) snprintf(out + 2 * k, 3, "%02x", dg[k]);
  return 0;
}

/* Close a header and its open side file; -1 when any write failed */
static int cs_out_close(Out *o) {
  out_side_end(o);
  return (o->bad | ferror(o->main) | (fclose(o->main) != 0)) ? -1 : 0;
}

/* sp_rt.h: the runtime region as c_split's header has it (types, externs,
   inline bodies, the other functions' prototypes). sp_program.h: sp_rt.h,
   the program region's untagged types and inline helpers, and ahead of
   each helper the table entries it names that are not in the header yet
   (pinned: no unit imports them again). Placing them there is valid
   because the original unit compiled the helper at that place, so every
   name it uses was declared before it. */
static int cs_write_headers(Split *S, CSplitResult *res) {
  const char *t = S->t;
  snprintf(res->rt_header, sizeof res->rt_header, "%s/sp_rt.h", S->dir);
  snprintf(res->program_header, sizeof res->program_header, "%s/sp_program.h", S->dir);
  FILE *hr = fopen(res->rt_header, "wb");
  FILE *hp = hr ? fopen(res->program_header, "wb") : NULL;
  if (!hp) { if (hr) fclose(hr); return -1; }
  Out orr, opp;
  out_init(&orr, hr, S->dir, "rt");
  out_init(&opp, hp, S->dir, "prog");
  fputs("#include \"sp_rt.h\"\n", hp);
  int *pins = NULL, npin = 0, pcap = 0;
  for (int k = 0; k < S->nit; k++) {
    Item *x = &S->it[k];
    Out *o = x->region ? &opp : &orr;
    if (x->kind == IT_MARK || x->kind == IT_UNIT) continue;
    if (!x->region) {
      out_sync(o, t, x);
      if (x->kind == IT_HDR) fwrite(t + x->st, 1, x->en - x->st, o->f);
      else if (x->kind == IT_PROTO && is_inline_name(S, x->name)) fwrite(t + x->st, 1, x->en - x->st, o->f);
      else if (x->kind == IT_PROTO) put_stripped(o->f, t, x->st, x->en, 1);
      else if (x->kind == IT_FN && x->keep_inline) put_body(o->f, t, x->st, x->en);
      else if (x->kind == IT_FN) { put_stripped(o->f, t, x->st, x->body, 1); fputc(';', o->f); }
      else {   /* IT_VAR: every occurrence is declared where it stood */
        size_t end = x->en - 1, a, b;
        if (top_comma(t, x->st, end) >= end) put_var_decl(o->f, t, x->st, x->en, 0, x->name);
        else for (int j = 0; var_declarator(t, x->st, x->en, j, &a, &b); j++) put_var_decl(o->f, t, x->st, x->en, j, x->name);
        o->line = -1;
        continue;
      }
      fputc('\n', o->f);
      out_advance(o, t, x->st, x->kind == IT_FN && !x->keep_inline ? x->body : x->en, 1);
      continue;
    }
    int hdr = (x->kind == IT_HDR && x->unit < 0) || (x->kind == IT_PROTO && is_inline_name(S, x->name));
    if (x->kind == IT_FN && x->keep_inline) {
      npin = 0; S->stamp++;
      cs_refs(S, x->st, x->en, &pins, &npin, &pcap);
      qsort(pins, (size_t)npin, sizeof *pins, cmpint);
      if (npin) out_sync(o, t, x);
      for (int j = 0; j < npin; j++) { put_decl(o->f, S, &S->decl[pins[j]]); S->decl[pins[j]].pinned = 1; }
      if (npin) o->line = -1;
      out_sync(o, t, x);
      put_body(o->f, t, x->st, x->en);
    }
    else if (hdr) { out_sync(o, t, x); fwrite(t + x->st, 1, x->en - x->st, o->f); }
    else continue;
    fputc('\n', o->f);
    out_advance(o, t, x->st, x->en, 1);
  }
  free(pins);
  int nrt = orr.nside, npg = opp.nside;
  int bad = cs_out_close(&orr) | cs_out_close(&opp);
  if (bad) return -1;
  int keep = S->opts->keep_markers;
  if (cs_header_hash(S->dir, "sp_rt.h", "rt", nrt, keep, res->rt_hash) != 0 ||
      cs_header_hash(S->dir, "sp_program.h", "prog", npg, keep, res->program_hash) != 0)
    return -1;
  return 0;
}

/* One unit: the header it includes, then its items in text order with
   their line markers inline (a unit opens no side file; a system-header
   item keeps its 3/4 flags), each preceded by the table entries it is the
   first in the unit to name, in table order. Not all of them at the top: an
   entry may spell a type that a tagged item of this same unit defines
   (the dispatch tag's inspect tables are typed by its own typedefs), and
   just before the first item that names it is always after that typedef,
   since the original unit declared the name there and used it after.
   Written under a scratch name, then renamed to unit_<first 16 hex of its
   hash>.c. */
static int cs_write_unit(Split *S, int ui, CSplitUnit *cu, const CSplitUnit *done, int ndone) {
  const char *t = S->t;
  CsUnit *u = &S->u[ui];
  char tmp[4200];
  snprintf(tmp, sizeof tmp, "%s/unit.tmp", S->dir);
  FILE *f = fopen(tmp, "wb");
  if (!f) return -1;
  fprintf(f, "#include \"%s\"\n", u->kind == UK_RT ? "sp_rt.h" : "sp_program.h");
  Out o;
  out_init(&o, f, S->dir, "u");
  o.noside = 1;
  int *imp = NULL, nimp = 0, icap = 0;
  S->stamp++;
  for (int j = 0; j < u->nitems; j++) {
    Item *x = &S->it[u->items[j]];
    if (u->kind != UK_RT) {   /* the imports this item is the first to name */
      nimp = 0;
      cs_refs(S, x->st, x->en, &imp, &nimp, &icap);
      qsort(imp, (size_t)nimp, sizeof *imp, cmpint);
      for (int d = 0; d < nimp; d++) put_decl(f, S, &S->decl[imp[d]]);
      if (nimp) o.line = -1;
    }
    out_sync(&o, t, x);
    if (x->kind == IT_VAR) { put_var_def(f, t, x->st, x->en, x->name); o.line = -1; continue; }
    if (x->kind == IT_FN) {
      /* the storage words go from the head only: a body's static locals stay */
      put_stripped(f, t, x->st, x->body, 1);
      put_body(f, t, x->body, x->en);
    }
    else fwrite(t + x->st, 1, x->en - x->st, f);
    fputc('\n', f);
    out_advance(&o, t, x->st, x->en, 1);
  }
  free(imp);
  if (ferror(f) | (fclose(f) != 0)) { remove(tmp); return -1; }
  Sha256 h; unsigned char dg[32];
  sha256_init(&h);
  cu->bytes = 0;
  if (cs_hash_file(tmp, S->opts->keep_markers, &h, &cu->bytes) != 0) { remove(tmp); return -1; }
  sha256_final(&h, dg);
  for (int k = 0; k < 32; k++) snprintf(cu->hash + 2 * k, 3, "%02x", dg[k]);
  snprintf(cu->label, sizeof cu->label, "%s", u->label);
  snprintf(cu->path, sizeof cu->path, "%s/unit_%.16s.c", S->dir, cu->hash);
  cu->is_rt = u->kind == UK_RT;
  cu->groups = u->groups ? strdup(u->groups) : NULL;
  /* the same name twice (the same text, or a 64-bit prefix collision)
     would compile one object for two units */
  for (int k = 0; k < ndone; k++)
    if (strcmp(done[k].path, cu->path) == 0) {
      if (getenv("SPINEL_SPLIT_DEBUG")) fprintf(stderr, "c_split_units: declined: %s and %s are both %s\n", done[k].label, cu->label, cu->path);
      remove(tmp); return -1;
    }
  if (rename(tmp, cu->path) != 0) { remove(tmp); return -1; }
  return 0;
}

/* units.txt: the header hashes, then one unit per line in link order:
   label, file, hash, kind, marker-free bytes, groups (`-` for none). */
static int cs_write_manifest(const char *dir, const CSplitResult *res) {
  char path[4200];
  snprintf(path, sizeof path, "%s/units.txt", dir);
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  fprintf(f, "rt_hash %s\nprogram_hash %s\n", res->rt_hash, res->program_hash);
  for (int k = 0; k < res->n; k++) {
    const CSplitUnit *u = &res->units[k];
    const char *base = strrchr(u->path, '/');
    fprintf(f, "%s %s %s %s %zu %s\n", u->label, base ? base + 1 : u->path, u->hash,
            u->is_rt ? "rt" : u->groups ? "tagged" : "program", u->bytes, u->groups ? u->groups : "-");
  }
  return (ferror(f) | (fclose(f) != 0)) ? -1 : 0;
}

int c_split_units_manifest(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  char h[73];
  size_t n = fread(h, 1, sizeof h, f);
  fclose(f);
  if (n < sizeof h || memcmp(h, "rt_hash ", 8) != 0 || h[72] != '\n') return 0;
  for (int k = 8; k < 72; k++)
    if (!((h[k] >= '0' && h[k] <= '9') || (h[k] >= 'a' && h[k] <= 'f'))) return 0;
  return 1;
}

char *c_link_flags(const char *flags) {
  size_t n = strlen(flags);
  char *out = malloc(n + 1);
  if (!out) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  size_t w = 0;
  for (size_t i = 0; i < n; ) {
    if ((i == 0 || flags[i - 1] == ' ') && strncmp(flags + i, "-mllvm ", 7) == 0) {
      i += 7;
      while (i < n && flags[i] == ' ') i++;
      while (i < n && flags[i] != ' ') i++;
      while (i < n && flags[i] == ' ') i++;
      continue;
    }
    out[w++] = flags[i++];
  }
  out[w] = 0;
  return out;
}

void c_split_result_free(CSplitResult *res) {
  for (int k = 0; k < res->n; k++) free(res->units[k].groups);
  free(res->units);
  res->units = NULL; res->n = 0;
}

static void cs_split_free(Split *S) {
  for (int j = 0; j < S->nu; j++) { free(S->u[j].items); free(S->u[j].groups); }
  for (int d = 0; d < S->ndecl; d++) free(S->decl[d].name);
  free(S->u); free(S->decl); free(S->ptag); free(S->tag_group);
  nm_free(&S->tags); nm_free(&S->groups); nm_free(&S->inl); nm_free(&S->vars); nm_free(&S->dmap);
  free(S->it); free(S->up);
}

int c_split_units(const char *pre_path, const char *out_dir, const CSplitOpts *opts, CSplitResult *res) {
  memset(res, 0, sizeof *res);
  FILE *in = fopen(pre_path, "rb");
  if (!in) return -1;
  fseek(in, 0, SEEK_END);
  long fl = ftell(in);
  fseek(in, 0, SEEK_SET);
  if (fl <= 0) { fclose(in); return -1; }
  char *raw = malloc((size_t)fl + 1);
  size_t n = fread(raw, 1, (size_t)fl, in);
  fclose(in);
  raw[n] = 0;
  size_t tn = 0;
  char *t = cs_canon_markers(raw, n, &tn);
  free(raw);
  if (!t) return -1;

  Split S;
  memset(&S, 0, sizeof S);
  S.t = t; S.n = tn; S.opts = opts; S.dir = out_dir;
  int rc = cs_lex(t, tn, &S.it, &S.nit, &S.up, &S.nup);
  if (rc == 0 && cs_regions(S.it, S.nit, t) != 0) {
    if (getenv("SPINEL_SPLIT_DEBUG")) fprintf(stderr, "c_split_units: declined: no return from the runtime header\n");
    rc = -1;
  }
  if (rc == 0) {
    cs_tags(&S);
    cs_inline_names(&S);
    cs_vars(&S);
    cs_decl_table(&S);
    cs_assign_units(&S);
    rc = cs_write_headers(&S, res);
  }
  if (rc == 0) {
    res->units = calloc((size_t)S.nu + 1, sizeof *res->units);
    for (int j = 0; j < S.nu && rc == 0; j++) {
      rc = cs_write_unit(&S, j, &res->units[j], res->units, j);
      if (rc == 0) res->n++;
    }
  }
  if (rc == 0) rc = cs_write_manifest(out_dir, res);
  cs_split_free(&S);
  free(t);
  if (rc != 0) c_split_result_free(res);
  return rc;
}
