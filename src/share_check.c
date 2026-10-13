/* share_check.c -- --share-check: do the share facts and the emitted C agree?

   Under --share-strings a String the rule shares has to reach every
   consumer that keeps it, changes it or asks for its identity as the one
   sp_String * handle. The facts (share.h) say which Strings those are; the
   emitters decide on their own, in about two hundred places that write a
   copy helper as literal text, whether they hand the handle or a copy of
   its bytes. Nothing compared the two: a route the seal let through that
   an emitter still copied was a wrong answer found by hand.

   With the flag on, each value choke point (emit_expr, emit_boxed,
   emit_strbuf_value) writes an in-band mark before the text of a String
   value whose plan (share_plan.c) needs the handle. A mark is the control
   byte SK_OPEN, "sk", the node, the face, an optional "f" or "i", an optional
   "t" (the plan lists a consumer that tests the value), the plan's form
   as a digit (SPF_*, plus SK_LIVE when the memo could not answer the need),
   and the control byte SK_CLOSE. Every emitter that
   writes program text into the C escapes a control byte, so the text of a
   program cannot spell a mark; one that does not parse as a mark whole is
   none. The face is what the emitter asked for: bytes (b), handle (h), box
   (B) or a poly (p); an `f` says the value is a new String, which may be
   copied unless a box keeps it, and an `i` that the value needs no handle
   and is marked only for the identity its consumer asks, so no conversion
   of it is a copy.

   When a function's text is final -- before the passes that read it as
   text (gc_frame_build, pd_hoist's dispatch helpers) -- share_check_harvest
   reads each mark against the conversion around it, reports a copy, and
   strips the mark, so the C is the one the compile writes without the
   flag. Only text that reaches the function counts: a trial emission that
   was thrown away took its marks with it. (A dispatch helper's region is
   read apart from the function around it, so a conversion that lies
   outside the region is not seen.)

   A mark is a copy when the value's bytes reach:
     WRAP    a new handle (sp_String_new_shared, SP_AS_STRING_HANDLE,
             sp_poly_as_strbuf ...), directly or after an unwrap; a new
             String boxed and lifted into the handle is none;
     BOX     sp_box_str: the box has the bytes, not the handle, even for a
             new String;
     MUTCOPY an in-place change, through bytes and with no write-back of
             the changed copy around it;
     RET     a `return` of bytes an unwrap made that publishes no handle;
     PICKUP  a deep-return pickup over a temp that ran before the pickup's
             reset of the side channel: it always wraps a copy.
   The state lives in Compiler.share_agree and goes with the compiler; the
   only global is the flag (g_share_check).

   The marks follow the String plan (share_plan.c), asked live where the
   emitter is: a value is marked where its own need (a taker's) is a
   handle, or where a multiple write stores it into a target that needs
   one (a store's need), or where an identity query asks a String's
   identity (the plan's form for it is the handle or the bytes' pointer),
   and the mark carries the plan's form there for the role of the
   value's consumer (share_plan_role). The harvest gives each mark a
   consumer: a value whose bytes the C only tests for nil (a call that
   answers its receiver, whose handle is then the answer) is its test's,
   and any other is its consumer's in the program. Per node and consumer it
   notes the most copying conversion the marks show -- the bytes, read by
   a call or written back into the handle, with no copy the check sees
   keeping them; the handle (or a box holding it, or bytes returned with
   the handle published); the bytes wrapped into a new handle; the bytes
   kept (boxed), changed or answered. A read is the least: where another
   mark of the same consumer shows the handle, the bytes were read on the
   way (a nil test, a dropped value) and the consumer took the handle. The
   report compares the conversion with the plan:
     agree     the plan's handle and the handle, the plan's wrap and a wrap,
               the plan's refuse and a copy, or the plan's bytes (a
               reader's, a test's, an identity's: the bytes' pointer) and
               no new handle;
     read      a taker given the bytes, kept by nothing the check sees: a
               consumer that only reads (one the share facts have no row
               for, so the plan asks the object), or a handle passed on
               where the check cannot follow it (an identity reads the
               plan's form, emit_str_form: one the plan gives the handle
               read as bytes disagrees);
     disagree  anything else, named per node with its consumer.
   Where the plan's memo can answer the need (the node read as itself), its
   answer is compared with the live one, and a contradiction is counted
   stale. A site where it cannot (a view, an arm, a splice, a node codegen
   rewrote) is counted live, and named so when it disagrees. */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "codegen_internal.h"
#include "repr.h"
#include "share.h"

int g_share_check = 0;

struct ShareAgree {
  unsigned char *seen;          /* per node: reported already */
  int nseen;
  unsigned long marks, declared, copies;
  char **line;                  /* the reports, written sorted at the end */
  int nline, cline;
  /* the classifier's answers beside its return value */
  char via[64], helper[64], consumer[64];
  /* the marks of the emit_expr calls still open: the node, the buffer its
     mark went to and where (share_check_declare_value) */
  struct { int node, depth; Buf *b; size_t off; } *open;
  int nopen, copen;
  int by_call;                  /* it ended at a call that took the bytes, with
                                   no write-back of what it answers */
  /* the plan's side: per node and consumer role (node * SK_ROLES + role),
     1 + the most copying conversion (SKE_*) a mark of it showed, and the
     plan's form at that mark; the memo answers the live plan contradicted */
  unsigned char *site_e, *site_form;
  int published;                /* the bytes reached a return that publishes
                                   their handle (sp_strbuf_read_pub) */
  int at_return;                /* the value is a `return` statement's */
  unsigned long stale;
};

enum { SKC_NONE, SKC_WRAP, SKC_BOX, SKC_MUT, SKC_RET };
/* the conversion a mark shows, the most copying last */
enum { SKE_READ, SKE_HANDLE, SKE_WRAP, SKE_BYTES, SKE__COUNT };
/* the consumer roles a site is kept per (SPR_*) */
enum { SK_ROLES = SPR_IDENTITY + 1 };
/* the control bytes that open and close a mark */
enum { SK_OPEN = 1, SK_CLOSE = 2 };
/* beside a mark's form: the memo could not answer the need, asked live */
enum { SK_LIVE = 4 };

static void sk_oom(void) {
  fprintf(stderr, "spinel: out of memory\n");
  exit(1);
}

static struct ShareAgree *sk_state(Compiler *c) {
  if (!c->share_agree) {
    c->share_agree = calloc(1, sizeof *c->share_agree);
    if (!c->share_agree) sk_oom();
  }
  struct ShareAgree *A = c->share_agree;
  if (A->nseen < c->nt->count) {
    size_t n = (size_t)c->nt->count + 1, grow = n - (size_t)A->nseen;
    A->seen = realloc(A->seen, n);
    A->site_e = realloc(A->site_e, n * SK_ROLES);
    A->site_form = realloc(A->site_form, n * SK_ROLES);
    if (!A->seen || !A->site_e || !A->site_form) sk_oom();
    memset(A->seen + A->nseen, 0, grow);
    memset(A->site_e + (size_t)A->nseen * SK_ROLES, 0, grow * SK_ROLES);
    memset(A->site_form + (size_t)A->nseen * SK_ROLES, 0, grow * SK_ROLES);
    A->nseen = c->nt->count;
  }
  return A;
}

void share_check_free(Compiler *c) {
  struct ShareAgree *A = c->share_agree;
  if (!A) return;
  for (int i = 0; i < A->nline; i++) free(A->line[i]);
  free(A->line);
  free(A->open);
  free(A->seen);
  free(A->site_e);
  free(A->site_form);
  free(A);
  c->share_agree = NULL;
}

/* Mark the value of node v, which the emitter is about to write into b,
   when its own need, asked live, is a handle, a multiple write stores
   it into a target that needs one, or it is a String whose identity its
   consumer asks; the mark carries the plan's form for its consumer's
   role. face: 'x' the emitter's own face (what the node is
   as emitted), 'B' a box, 'H' a handle for a shared slot. Where the memo
   can answer the need, a contradiction of the live need is counted. */
int share_check_mark(Compiler *c, int v, char face, Buf *b) {
  if (!b) return 0;
  /* the mark follows the need asked live, so a stale memo cannot hide one;
     the memo's need is measured against it */
  int mn = share_plan_memo_need(c, v), memo = mn >= 0;
  int need = share_plan_need(c, v);
  if (memo && mn != need) sk_state(c)->stale++;
  int role = share_plan_role(c, v);
  SharePlan plan = share_plan_with_need(c, v, role, need);
  /* (a store's need is never below the value's own) */
  if (role == SPR_STORE) need = plan.need;
  /* (a String whose identity is asked is marked whatever its need) */
  int idonly = need == SPN_BYTES;
  TyKind vt = comp_ntype(c, v);
  if (idonly && (role != SPR_IDENTITY || (vt != TY_STRING && vt != TY_STRBUF))) return 0;
  int tests = role == SPR_TEST || share_plan_second_role(c, v) == SPR_TEST;
  int pushed = face == 'x';   /* (only emit_expr's marks stay open) */
  if (face == 'x') {
    TyKind t = repr_of(c, v).as_ty;
    face = t == TY_STRBUF ? 'h' : t == TY_POLY ? 'p' : 'b';
  }
  size_t off = b->len;
  buf_printf(b, "%csk%d%c%s%s%c%c", SK_OPEN, v, face, need == SPN_NEW ? "f" : idonly ? "i" : "", tests ? "t" : "",
             '0' + (plan.form | (memo ? 0 : SK_LIVE)), SK_CLOSE);
  if (!pushed) return 0;
  struct ShareAgree *A = sk_state(c);
  /* an emit_expr that a refusal unwound never ended: its entries, and its
     buffers, are gone */
  while (A->nopen > 0 && A->open[A->nopen - 1].depth >= g_expr_depth) A->nopen--;
  if (A->nopen >= A->copen) {
    A->copen = A->copen ? A->copen * 2 : 64;
    A->open = realloc(A->open, sizeof *A->open * (size_t)A->copen);
    if (!A->open) sk_oom();
  }
  A->open[A->nopen].node = v;
  A->open[A->nopen].depth = g_expr_depth;
  A->open[A->nopen].b = b;
  A->open[A->nopen++].off = off;
  return 1;
}

/* emit_expr is done with the node whose mark share_check_mark kept open. */
void share_check_mark_end(Compiler *c) {
  struct ShareAgree *A = c->share_agree;
  if (A && A->nopen > 0 && A->open[A->nopen - 1].depth == g_expr_depth) A->nopen--;
}

/* The mark that starts at p: its end, with the node, the face, whether the
   value is new (1) or marked for its identity only (2), and the plan's
   form; NULL when p starts no mark (whole: a lone SK_OPEN, or one with no
   end, is program text). */
static const char *sk_mark_at(const char *p, long *node, char *face, int *fresh, int *tested, int *form) {
  if (p[0] != SK_OPEN || p[1] != 's' || p[2] != 'k') return NULL;
  const char *q = p + 3;
  long v = 0;
  if (!isdigit((unsigned char)*q)) return NULL;
  for (; isdigit((unsigned char)*q); q++) {
    v = v * 10 + (*q - '0');
    if (v > 1000000000L) return NULL;
  }
  char f = *q;
  if (f != 'b' && f != 'h' && f != 'p' && f != 'B' && f != 'H' && f != 'e') return NULL;
  int fr = *++q == 'f' ? 1 : *q == 'i' ? 2 : 0;
  if (fr) q++;
  int ts = *q == 't';
  if (ts) q++;
  int fm = *q - '0';
  if (fm < 0 || (fm & ~SK_LIVE) > SPF_REFUSE) return NULL;
  q++;
  if (*q != SK_CLOSE) return NULL;
  if (node) *node = v;
  if (face) *face = f;
  if (fresh) *fresh = fr;
  if (tested) *tested = ts;
  if (form) *form = fm;
  return q + 1;
}

/* The first mark at or after p, or NULL. */
static const char *sk_next(const char *p) {
  for (p = strchr(p, SK_OPEN); p; p = strchr(p + 1, SK_OPEN))
    if (sk_mark_at(p, NULL, NULL, NULL, NULL, NULL)) return p;
  return NULL;
}

/* ---- declarations ----
   An emitter that knows a value it wrote is no copy of a String the facts
   call shared says so, and the mark is made exempt: face 'e'. The declared
   cases are that the arm is taken only for what is no String (a runtime
   guard's else arm), that the value is never produced (an arm that raises),
   and that the bytes are a shadow the emitter writes back into the handle
   (the shared-handle shim), the answer being dropped or stored over the
   slot the bytes came from. A mark is exempt, not removed, so the text the
   emitters read is the same. */

/* Make the mark that ends at e exempt, if it is not already. */
static void sk_exempt_mark(const char *e) {
  char *face = (char *)e - 3;   /* before the form and SK_CLOSE; an `f` or
                                   an `i`, and a `t`, may sit between */
  if (*face == 't') face--;
  if (*face == 'f' || *face == 'i') face--;
  if (*face != 'e') *face = 'e';
}

/* Declare the value node's emit_expr is still writing no copy: the mark it
   began with, kept open on the stack. */
void share_check_declare_value(Compiler *c, int node) {
  struct ShareAgree *A = c->share_agree;
  for (int i = A ? A->nopen - 1 : -1; i >= 0; i--) {
    if (A->open[i].node != node || A->open[i].depth != g_expr_depth - 1) continue;
    Buf *mb = A->open[i].b;
    size_t off = A->open[i].off;
    long n;
    const char *e = mb->p && off < mb->len ? sk_mark_at(mb->p + off, &n, NULL, NULL, NULL, NULL) : NULL;
    if (e && n == node) sk_exempt_mark(e);   /* (else the text moved: the report stays) */
    return;
  }
}

/* Declare every mark of node v in b[from..] exempt. */
void share_check_declare_reads(Buf *b, size_t from, int node) {
  if (!b || !b->p || from >= b->len) return;
  for (const char *p = sk_next(b->p + from); p; p = sk_next(p + 1)) {
    long n;
    const char *e = sk_mark_at(p, &n, NULL, NULL, NULL, NULL);
    if (n == node) sk_exempt_mark(e);
  }
}

/* Declare the marks in b[from..] that are reads of the local `name` exempt
   (a block parameter the emitter stores the block's answer back over). */
void share_check_declare_local_reads(Compiler *c, Buf *b, size_t from, const char *name) {
  if (!b || !b->p || from >= b->len || !name) return;
  for (const char *p = sk_next(b->p + from); p; p = sk_next(p + 1)) {
    long node;
    const char *e = sk_mark_at(p, &node, NULL, NULL, NULL, NULL);
    if (node >= c->nt->count || nt_kind(c->nt, (int)node) != NK_LocalVariableReadNode) continue;
    const char *nm = nt_str(c->nt, (int)node, "name");
    if (nm && sp_streq(nm, name)) sk_exempt_mark(e);
  }
}

/* ---- reading the text around a mark ---- */

static int sk_in(const char *name, const char *const *set) {
  for (int i = 0; set[i]; i++) if (sp_streq(name, set[i])) return 1;
  return 0;
}

/* The unwraps: the calls that read a handle's or a box's bytes, and how
   (a copy that publishes the handle (2), a view of its live buffer (3), a
   copy that publishes none (4)). The longer name of a prefix pair first. */
typedef struct { const char *name; int how; } SkUnwrap;
static const SkUnwrap *sk_unwraps(void) {
  static const SkUnwrap t[] = { { "sp_strbuf_read_pub", 2 }, { "sp_strbuf_read_frozen", 4 },
    { "sp_strbuf_read", 4 }, { "sp_String_cstr", 3 }, { "sp_poly_unbox_s", 3 }, { "sp_poly_to_s", 3 },
    { NULL, 0 } };
  return t;
}
static int sk_unwrap_how(const char *name) {
  const SkUnwrap *u = sk_unwraps();
  for (int i = 0; u[i].name; i++)
    if (sp_streq(name, u[i].name)) return u[i].how;
  return 0;
}

/* The calls around position m of t (from lo), innermost first, up to the
   statement boundary; *bound is set to the boundary, *outer to the start of
   the outermost enclosing call. */
static int sk_chain(const char *t, const char *lo, size_t m, char names[][64], int cap, size_t *bound,
                    size_t *outer) {
  int depth = 0, n = 0;
  size_t i = m, floor = (size_t)(lo - t);
  *outer = m;
  while (i > floor) {
    char ch = t[--i];
    if (ch == ')' || ch == ']') depth++;
    else if (ch == '(' || ch == '[') {
      if (depth > 0) { depth--; continue; }
      size_t e = i, s = i;
      while (s > floor && (isalnum((unsigned char)t[s - 1]) || t[s - 1] == '_')) s--;
      if (n < cap) {
        size_t l = e - s < 63 ? e - s : 63;
        memcpy(names[n], t + s, l);
        names[n][l] = 0;
        n++;
      }
      *outer = s;
    }
    else if (depth == 0 && (ch == ';' || ch == '{' || ch == '}')) break;
  }
  *bound = i;
  return n;
}

/* Skip the marks (and spaces) at q. */
static const char *sk_skip_marks(const char *q) {
  for (;;) {
    while (*q == ' ') q++;
    const char *e = sk_mark_at(q, NULL, NULL, NULL, NULL, NULL);
    if (!e) return q;
    q = e;
  }
}

/* Does the text at q start with an unwrap call? Its how, else 0; *name is
   the call. */
static int sk_starts_unwrap(const char *q, const char **name) {
  const SkUnwrap *u = sk_unwraps();
  for (q = sk_skip_marks(q); *q == ' ' || *q == '('; q = sk_skip_marks(q + 1)) { }
  for (int i = 0; u[i].name; i++) {
    size_t l = strlen(u[i].name);
    if (!strncmp(q, u[i].name, l) && q[l] == '(') { if (name) *name = u[i].name; return u[i].how; }
  }
  return 0;
}

/* Does the text after the mark (ending at e) read bytes out of a handle or
   a box at its top: an unwrap call, or a statement expression whose last
   statement is one? Its how, else 0; *name is the call. */
static int sk_text_bytes(const char *t, size_t e, const char **name) {
  const char *q = sk_skip_marks(t + e);
  if (sk_starts_unwrap(q, name)) return sk_starts_unwrap(q, name);
  while (q[0] == '(' && sk_skip_marks(q + 1)[0] == '(') q = sk_skip_marks(q + 1);
  if (q[0] != '(' || q[1] != '{') return 0;
  int depth = 0;
  const char *last = NULL;
  for (const char *r = q; *r; r++) {
    if (*r == '(' || *r == '{' || *r == '[') depth++;
    else if (*r == ')' || *r == '}' || *r == ']') { depth--; if (depth == 0) break; }
    else if (*r == ';' && depth == 2) {
      const char *x = r + 1;
      while (*x == ' ') x++;
      if (!(x[0] == '}' && x[1] == ')')) last = x;
    }
  }
  return last ? sk_starts_unwrap(last, name) : 0;
}

/* Classify the mark at position m: which copy it is, if any. bytes: the
   value is bytes at the mark (0 no, else its unwrap's how). tmp: the temp
   the value is assigned to whole, if it is one. */
static int sk_classify_at(struct ShareAgree *A, const char *t, const char *lo, size_t m, int bytes, char *tmp,
                          size_t tcap) {
  static const char *const wraps[] = { "sp_String_new_shared", "sp_String_new_fresh", "sp_String_new",
    "sp_String_new_len", "SP_AS_STRING_HANDLE", "sp_poly_as_strbuf", "sp_poly_strbuf_lift", "sp_box_str_new", NULL };
  static const char *const transparent[] = { "", "sp_str_uplus", "SP_GC_ROOT_VAL", NULL };
  char names[24][64];
  size_t bound, outer;
  /* (no temp unless the value is found assigned to one below: an early
     answer must not leave the caller the last mark's) */
  if (tmp) tmp[0] = 0;
  int n = sk_chain(t, lo, m, names, 24, &bound, &outer);
  A->by_call = 0;
  A->via[0] = A->helper[0] = A->consumer[0] = 0;
  if (bytes) snprintf(A->via, sizeof A->via, "bytes face");
  /* how the bytes were read: unknown (1), publishing the handle (2), a view
     of the live buffer (3), or through a copy that publishes none (4) */
  int how = bytes;
  for (int i = 0; i < n; i++) {
    int uh = sk_unwrap_how(names[i]);
    if (uh) {
      how = uh;
      bytes = 1;
      snprintf(A->via, sizeof A->via, "%s", names[i]);
      continue;
    }
    if (sk_in(names[i], wraps)) {
      if (bytes) { snprintf(A->helper, sizeof A->helper, "%s", names[i]); return SKC_WRAP; }
      continue;
    }
    if (sp_streq(names[i], "sp_box_str")) {
      if (!bytes) continue;
      /* a box lifted into the handle (sp_poly_strbuf_lift) is a wrap: for a
         new String no copy at all */
      if (i + 1 < n && sk_in(names[i + 1], wraps)) {
        snprintf(A->helper, sizeof A->helper, "%s", names[i + 1]);
        return SKC_WRAP;
      }
      snprintf(A->helper, sizeof A->helper, "sp_box_str");
      return SKC_BOX;
    }
    if (sk_in(names[i], transparent)) continue;
    /* bytes a call consumes, unless the call or one around it writes what
       it answers back into the handle (sp_String_set_bin(h, f(bytes of h)),
       or into a box that holds it, sp_poly_str_become) */
    A->by_call = 1;
    snprintf(A->consumer, sizeof A->consumer, "%s", names[i]);
    for (int j = i; j < n && A->by_call; j++)
      if (!strncmp(names[j], "sp_String_set", 13) || !strncmp(names[j], "sp_poly_str_become", 18)) A->by_call = 0;
    return SKC_NONE;
  }
  /* a method's tail answered as bytes that publish no handle: the caller's
     pickup wraps them into a new one */
  {
    size_t h = bound + 1;
    for (;;) {
      while (h < outer && isspace((unsigned char)t[h])) h++;
      if (h >= outer || t[h] != '#') break;
      while (h < outer && t[h] != '\n') h++;   /* a #line directive */
    }
    if ((how == 3 || how == 4) && outer - h >= 6 && !strncmp(t + h, "return", 6)) {
      snprintf(A->helper, sizeof A->helper, "return");
      return SKC_RET;
    }
    /* bytes returned with their handle published: the caller's pickup
       takes the handle */
    if (outer - h >= 6 && !strncmp(t + h, "return", 6)) {
      A->at_return = 1;
      if (how == 2) A->published = 1;
    }
  }
  /* a temp the value is assigned to whole: its uses decide */
  if (tmp) {
    size_t j = outer;
    while (j > bound && t[j - 1] == ' ') j--;
    if (j > bound && t[j - 1] == '=' && (j < 2 || t[j - 2] != '=')) {
      j--;
      while (j > bound && t[j - 1] == ' ') j--;
      size_t e = j;
      while (j > bound && (isalnum((unsigned char)t[j - 1]) || t[j - 1] == '_')) j--;
      if (e > j && e - j < tcap && t[j] == '_' && t[j + 1] == 't') { memcpy(tmp, t + j, e - j); tmp[e - j] = 0; }
    }
  }
  return SKC_NONE;
}

/* Record one report line (`tag`: copy, or the plan's disagree) about node
   v; the lines are written sorted, so the output does not depend on the
   order functions were emitted in. */
static void sk_line(Compiler *c, struct ShareAgree *A, const char *tag, int v, const char *what) {
  int fid = (int)nt_int(c->nt, v, "node_file", -1);
  const char *fp = nt_file_path(c->nt, fid);
  const char *nm = nt_str(c->nt, v, "name");
  Buf l;
  memset(&l, 0, sizeof l);
  buf_printf(&l, "share-check: %s: %s:%d node %d %s (%s): %s", tag, fp ? fp : "?",
             (int)nt_int(c->nt, v, "node_line", 0), v, nt_type(c->nt, v), nm ? nm : "-", what);
  if (A->nline >= A->cline) {
    A->cline = A->cline ? A->cline * 2 : 32;
    A->line = realloc(A->line, sizeof(char *) * (size_t)A->cline);
    if (!A->line) sk_oom();
  }
  A->line[A->nline++] = l.p;
}
static void sk_report(Compiler *c, struct ShareAgree *A, int v, const char *what) {
  sk_line(c, A, "copy", v, what);
  A->copies++;
}

/* Note the conversion e (SKE_*) a mark of node v shows for a consumer in
   `role`, under the plan's form at that mark: the site keeps its most
   copying one. */
static void sk_plan_note(struct ShareAgree *A, long v, int role, int e, int form) {
  size_t i = (size_t)v * SK_ROLES + (size_t)role;
  if (A->site_e[i] > e) return;
  A->site_e[i] = (unsigned char)(e + 1);
  A->site_form[i] = (unsigned char)form;
}

/* Does the statement expression at q end in bytes read with their handle
   published (its last statement, also after a block of its own)? */
static int sk_stmts_publish(const char *q) {
  q = sk_skip_marks(q);
  if (q[0] != '(' || q[1] != '{') return 0;
  int depth = 0;
  const char *last = NULL;
  for (const char *r = q; *r; r++) {
    if (*r == '(' || *r == '{' || *r == '[') {
      depth++;
      continue;
    }
    if (*r == ')' || *r == '}' || *r == ']') {
      if (--depth == 0) break;
      if (*r != '}' || depth != 2) continue;
    }
    else if (*r != ';' || depth != 2) continue;
    const char *x = r + 1;
    while (*x == ' ' || *x == '\n') x++;
    if (!(x[0] == '}' && x[1] == ')')) last = x;
  }
  const char *nm = NULL;
  return last && sk_starts_unwrap(last, &nm) == 2;
}

/* Is temp `tmp`, which the bytes at p were assigned to, only tested for nil
   in the rest of the function: each of its uses starts an expression and is
   the condition of a `?:`? */
static int sk_temp_tested(const char *p, const char *tmp) {
  size_t tl = strlen(tmp);
  const char *fn_end = strstr(p, "\n}\n");
  int uses = 0;
  for (const char *u = strstr(p + 1, tmp); u && (!fn_end || u < fn_end); u = strstr(u + tl, tmp)) {
    if (isalnum((unsigned char)u[tl]) || u[tl] == '_' || isalnum((unsigned char)u[-1]) || u[-1] == '_') continue;
    /* (rooting the temp is no use of its value) */
    if (u - p >= 11 && !strncmp(u - 11, "SP_GC_ROOT(", 11)) continue;
    const char *a = u + tl, *z = u;
    while (*a == ' ') a++;
    while (z > p && z[-1] == ' ') z--;
    if (*a != '?' || (z[-1] != ';' && z[-1] != '{' && z[-1] != '(')) return 0;
    uses++;
  }
  return uses > 0;
}

/* Read the mark at p (b->p + off) against its surroundings. */
static void sk_read_mark(Compiler *c, struct ShareAgree *A, Buf *b, const char *lo, size_t off) {
  const char *t = b->p, *p = t + off;
  long v;
  char face;
  int fresh, tested, form;
  const char *end = sk_mark_at(p, &v, &face, &fresh, &tested, &form);
  if (!end || v >= c->nt->count) return;
  A->marks++;
  if (face == 'e') { A->declared++; return; }
  char tmp[32], what[256];
  /* a deep-return pickup (`_sp_ret_strbuf = NULL; const char *_vN = X;
     _sp_ret_strbuf ? that handle : a new one`) over X that is no call (an
     argument that ran first, held in a temp): nothing can publish the
     handle between the reset and the read, so it always wraps a copy */
  {
    const char *x = sk_skip_marks(end);
    const char *lb = p;
    while (lb > lo && lb[-1] != '{' && lb[-1] != '\n') lb--;
    int bare = x[0] == '_' && (x[1] == 't' || x[1] == 'v') && isdigit((unsigned char)x[2]);
    if (bare) { const char *y = x + 2; while (isdigit((unsigned char)*y)) y++; bare = *y == ';'; }
    const char *reset = strstr(lb, "_sp_ret_strbuf = NULL;");
    if (bare && face == 'b' && (size_t)(p - lb) < 200 && reset && reset < p && !fresh) {
      sk_plan_note(A, v, share_plan_role(c, (int)v), SKE_WRAP, form);
      if (!A->seen[v]) {
        A->seen[v] = 1;
        sk_report(c, A, (int)v, "picked up after the call ran (always a new handle)");
      }
      return;
    }
  }
  const char *unwrap = NULL;
  A->published = A->at_return = 0;
  int tf = sk_text_bytes(t, (size_t)(end - t), &unwrap);
  if (tf) face = 'b';
  int bytes = face == 'b' ? (tf ? tf : 1) : 0;
  int kind = sk_classify_at(A, t, lo, (size_t)(p - t), bytes, tmp, sizeof tmp);
  /* the call that made the bytes, when the text after the mark is one */
  if (tf && unwrap && !strcmp(A->via, "bytes face")) snprintf(A->via, sizeof A->via, "%s", unwrap);
  if (kind == SKC_NONE && tmp[0]) {
    /* follow the temp to its uses in the same function */
    size_t tl = strlen(tmp);
    char *fn_end = strstr((char *)p, "\n}\n");
    for (char *u = strstr((char *)p + 1, tmp); u && (!fn_end || u < fn_end) && kind == SKC_NONE;
         u = strstr(u + tl, tmp)) {
      if (isalnum((unsigned char)u[tl]) || u[tl] == '_' || isalnum((unsigned char)u[-1]) || u[-1] == '_') continue;
      kind = sk_classify_at(A, t, lo, (size_t)(u - t), bytes, NULL, 0);
    }
  }
  if (kind == SKC_NONE && bytes && A->by_call && share_node_mutated(c, (int)v)) {
    kind = SKC_MUT;
    snprintf(A->helper, sizeof A->helper, "%s", A->consumer);
  }
  /* bytes returned as a statement expression that publishes them */
  if (kind == SKC_NONE && bytes && A->at_return && !A->published && sk_stmts_publish(end)) A->published = 1;
  /* the consumer: a nil test of bytes a temp holds, where the plan lists a
     consumer that tests the value, or the value's own */
  int role = share_plan_role(c, (int)v);
  if (tested && kind == SKC_NONE && bytes && tmp[0] && sk_temp_tested(p, tmp)) {
    role = SPR_TEST;
    form = (form & SK_LIVE) | SPF_BYTES;
  }
  sk_plan_note(A, v, role,
               kind == SKC_WRAP ? SKE_WRAP : kind != SKC_NONE ? SKE_BYTES
               : bytes && !A->published ? SKE_READ : SKE_HANDLE,
               form);
  if (fresh == 2 || (fresh && kind != SKC_BOX)) kind = SKC_NONE;
  if (kind == SKC_NONE || A->seen[v]) return;
  A->seen[v] = 1;
  static const char *const kn[] = { "", "bytes wrapped into a new handle", "boxed as bytes",
                                    "changed in place as bytes", "answered as bytes publishing no handle" };
  snprintf(what, sizeof what, "%s by %s (%s)", kn[kind], A->helper[0] ? A->helper : "?",
           A->via[0] ? A->via : face == 'B' ? "box" : face == 'h' || face == 'H' ? "handle" : "?");
  sk_report(c, A, (int)v, what);
}

/* The text without the marks that lead it. The emitters that take an
   operand's text for a temp (`_t12`) or compare it with one must not be
   told apart by a mark in front of it. */
const char *share_check_unmarked(const char *t) {
  for (const char *e; t && (e = sk_mark_at(t, NULL, NULL, NULL, NULL, NULL)); ) t = e;
  return t;
}

/* Strip every mark out of the len bytes at s, in place (s[len] is NUL);
   the new length. */
size_t share_check_strip(char *s, size_t len) {
  size_t w = 0;
  for (size_t r = 0; r < len; ) {
    const char *e = s[r] == SK_OPEN ? sk_mark_at(s + r, NULL, NULL, NULL, NULL, NULL) : NULL;
    if (e) r = (size_t)(e - s);
    else s[w++] = s[r++];
  }
  s[w] = 0;
  return w;
}

/* Read every mark in b[from..] and strip them: the text is the one the
   compile writes without the flag. */
void share_check_harvest(Compiler *c, Buf *b, size_t from) {
  if (!b->p || from >= b->len) return;
  const char *first = sk_next(b->p + from);
  if (!first) return;
  struct ShareAgree *A = sk_state(c);
  for (const char *p = first; p; p = sk_next(p + 1))
    sk_read_mark(c, A, b, b->p + from, (size_t)(p - b->p));
  size_t at = (size_t)(first - b->p);
  b->len = at + share_check_strip(b->p + at, b->len - at);
}

static int sk_cmp_line(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/* What consumes node v's value, by the node it sits in (through
   parentheses, a statement list's tail and an argument list; a statement
   before the tail drops it), for the plan's report. */
static const char *sk_consumer(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  const int *par = codegen_node_parents(c, v);
  int ch = v, p = par ? par[v] : -1;
  while (p >= 0 && (nt_kind(nt, p) == NK_ParenthesesNode || nt_kind(nt, p) == NK_StatementsNode ||
                    sp_streq(nt_type(nt, p), "ArgumentsNode"))) {
    if (nt_kind(nt, p) == NK_StatementsNode) {
      int n = 0;
      const int *body = nt_arr(nt, p, "body", &n);
      if (n > 0 && body[n - 1] != ch) return "statement";
    }
    ch = p;
    p = par[p];
  }
  if (p < 0) return "top";
  switch (nt_kind(nt, p)) {
  case NK_CallNode: return nt_ref(nt, p, "receiver") == ch ? "receiver" : "argument";
  case NK_SuperNode: case NK_ForwardingSuperNode: return "argument";
  case NK_YieldNode: return "yield";
  case NK_ReturnNode: case NK_NextNode: case NK_BreakNode: return "jump";
  case NK_DefNode: return "method tail";
  case NK_BlockNode: case NK_LambdaNode: return "block tail";
  case NK_ArrayNode: case NK_HashNode: case NK_AssocNode: case NK_KeywordHashNode: case NK_SplatNode:
    return "element";
  case NK_IfNode: case NK_UnlessNode: case NK_ElseNode: case NK_AndNode: case NK_OrNode: case NK_CaseNode:
  case NK_BeginNode: case NK_RescueNode: case NK_RescueModifierNode:
    return "arm";
  case NK_EmbeddedStatementsNode: return "interpolation";
  default: {
    const char *ty = nt_type(nt, p);
    size_t l = ty ? strlen(ty) : 0;
    if (l >= 9 && !strcmp(ty + l - 9, "WriteNode")) return "write";
    return l && (!strcmp(ty, "WhenNode") || !strcmp(ty, "EnsureNode")) ? "arm" : "other";
  }
  }
}

/* Does the conversion e (SKE_*) a consumer was given agree with the plan's
   form for it? */
static int sk_plan_agrees(int form, int e) {
  switch (form) {
  case SPF_HANDLE: return e == SKE_HANDLE;
  case SPF_WRAP: return e == SKE_WRAP;
  case SPF_REFUSE: return e != SKE_HANDLE;   /* no handle reaches it either way */
  default: return e != SKE_WRAP;             /* bytes: no new handle needed */
  }
}

/* The plan against the conversions the marks showed: a disagreement is
   named per node and consumer, and the summary counts each form's sites by
   the conversion the C has, and the sites by their consumer's role. */
static void sk_plan_report(Compiler *c, struct ShareAgree *A) {
  static const char *const en[] = { "read", "handle", "wrap", "bytes" };
  unsigned long tally[SPF_REFUSE + 1][SKE__COUNT], roles[SK_ROLES], sites = 0, agree = 0, read = 0, nlive = 0;
  memset(tally, 0, sizeof tally);
  memset(roles, 0, sizeof roles);
  for (int v = 0; v < A->nseen; v++)
    for (int role = 0; role < SK_ROLES; role++) {
      size_t i = (size_t)v * SK_ROLES + (size_t)role;
      if (!A->site_e[i]) continue;
      int e = A->site_e[i] - 1, form = A->site_form[i] & ~SK_LIVE, live = (A->site_form[i] & SK_LIVE) != 0;
      sites++;
      nlive += live;
      roles[role]++;
      tally[form][e]++;
      /* a taker given the bytes, kept by nothing the check sees: the C
         may hand them to a consumer that only reads (one the facts have no
         row for), or pass a handle on where the check cannot follow it. An
         identity reads the plan's form (emit_str_form): one the plan gives
         the handle read as bytes is a renderer's gap */
      if (e == SKE_READ && role != SPR_READ && role != SPR_TEST && role != SPR_IDENTITY) {
        read++;
        continue;
      }
      if (sk_plan_agrees(form, e)) { agree++; continue; }
      char what[160];
      snprintf(what, sizeof what, "plan %s%s, C %s (%s, %s)", share_plan_form_name(form), live ? " (live)" : "",
               en[e], share_plan_role_name(role), sk_consumer(c, v));
      sk_line(c, A, "plan: disagree", v, what);
    }
  qsort(A->line, (size_t)A->nline, sizeof(char *), sk_cmp_line);
  for (int i = 0; i < A->nline; i++) fprintf(stderr, "%s\n", A->line[i]);
  fprintf(stderr, "share-check: %lu marks, %lu declared, %lu copies\n", A->marks, A->declared, A->copies);
  fprintf(stderr, "share-check: plan: %lu sites, %lu agree, %lu read, %lu disagree", sites, agree, read,
          sites - agree - read);
  static const int order[] = { SPF_HANDLE, SPF_WRAP, SPF_REFUSE, SPF_BYTES };
  for (int k = 0; k < 4; k++) {
    int f = order[k];
    fprintf(stderr, "%s %s %lu/%lu/%lu/%lu", k == 0 ? ";" : ",", share_plan_form_name(f), tally[f][SKE_HANDLE],
            tally[f][SKE_READ], tally[f][SKE_WRAP], tally[f][SKE_BYTES]);
  }
  fprintf(stderr, " (C handle/read/wrap/bytes); by consumer");
  for (int r = 0; r < SK_ROLES; r++) fprintf(stderr, "%s %s %lu", r ? "," : "", share_plan_role_name(r), roles[r]);
  fprintf(stderr, "; %lu live, %lu stale\n", nlive, A->stale);
}

/* The unit is written (a refused one never gets here): the reports sorted,
   and a summary. */
void share_check_report(Compiler *c) {
  if (!c->share_strings) return;
  sk_plan_report(c, sk_state(c));
}
