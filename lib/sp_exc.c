/* sp_exc.c -- cold sp_Exception ops (see sp_exc.h). 0 optcarrot uses. */
#include "sp_exc.h"
#include "sp_string.h"
#include <stdarg.h>
#include "sp_exc_ctx.h"
#include <errno.h>

/* Check if exception class name `raised` is the same as or a subclass of
   `target`, using both the built-in hierarchy and the user hierarchy callback. */
const char *const *(*sp_user_exc_modules_fn)(const char *) = 0;

/* Errno::EWOULDBLOCK and Errno::EAGAIN are the same class in CRuby, as are the
   IO::EWOULDBLOCKWait* and IO::EAGAINWait* pairs; a name-keyed hierarchy has to
   fold them together or `rescue Errno::EWOULDBLOCK` would miss an EAGAIN. */
const char *sp_exc_canonical_name(const char *cls) {
  if (!cls) return cls;
  if (!strcmp(cls, "Errno::EWOULDBLOCK")) return SPL("Errno::EAGAIN");
  if (!strcmp(cls, "IO::EWOULDBLOCKWaitReadable")) return SPL("IO::EAGAINWaitReadable");
  if (!strcmp(cls, "IO::EWOULDBLOCKWaitWritable")) return SPL("IO::EAGAINWaitWritable");
  return cls;
}

/* The builtin classes that include a module. Only the exception side needs
   this: class VALUES already walk the module-aware sp_class_ancestors. */
const char *const *sp_exc_modules_of_name(const char *cls) {
  if (!cls) return 0;
  static const char *const WAIT_R[] = { "IO::WaitReadable", 0 };
  static const char *const WAIT_W[] = { "IO::WaitWritable", 0 };
  if (!strcmp(cls, "IO::EAGAINWaitReadable") ||
      !strcmp(cls, "IO::EINPROGRESSWaitReadable")) return WAIT_R;
  if (!strcmp(cls, "IO::EAGAINWaitWritable") ||
      !strcmp(cls, "IO::EINPROGRESSWaitWritable")) return WAIT_W;
  return 0;
}

/* Does `cls` itself, through any module it includes, answer to `target`? */
static int sp_exc_level_matches(const char *cls, const char *target) {
  if (!strcmp(cls, target)) return 1;
  const char *const *mods = sp_exc_modules_of_name(cls);
  if (!mods && sp_user_exc_modules_fn) mods = sp_user_exc_modules_fn(cls);
  for (int i = 0; mods && mods[i]; i++)
    if (!strcmp(mods[i], target)) return 1;
  return 0;
}

/* Class names are rodata by contract -- every caller passes a bare literal and
   sp_exc_gc_scan never marks them -- so they take no string root here or in
   the raise path. Rooting one had the collector read its marker byte, which
   for a bare literal is the byte BEFORE the object: an out-of-bounds read on
   every raise, and a WRITE of 0xfc over whatever precedes it whenever that
   byte happened to read as an unmarked heap string. */
int sp_exc_cls_matches(const char *raised, const char *target) {
  if (!raised || !target) return 0;
#ifdef SP_CEXT
  if (!strcmp(raised, "fatal")) return 0; /* rb_fatal runs ensures, bypasses rescues */
#endif
  raised = sp_exc_canonical_name(raised);
  target = sp_exc_canonical_name(target);
  /* The builtin hierarchy lives in sp_exc_parent_of_name -- there used to be a
     second copy here, and the two drifted (Errno::* / SystemCallError reached
     only one of them, so `rescue SystemCallError` missed what #is_a? matched). */
  const char *cls = raised;
  for (int depth = 0; depth < 30 && cls; depth++) {
    if (sp_exc_level_matches(cls, target)) return 1;
    const char *parent = NULL;
    /* user hierarchy first */
    if (sp_user_exc_parent_fn) parent = sp_user_exc_parent_fn(cls);
    if (!parent) parent = sp_exc_parent_of_name(cls);
    if (!parent) {
      if (!strcmp(target, "Exception") || !strcmp(target, "Object") || !strcmp(target, "BasicObject")) return 1;
      break;
    }
    cls = parent;
  }
  if (!strcmp(target, "Object") || !strcmp(target, "BasicObject") || !strcmp(target, "Kernel")) return 1;
  return 0;
}
/* Which of n class names `raised` reaches first going up its ancestry (the
   class itself first, then its user and builtin parents): the index of that
   name, or -1 when none is an ancestor. Picks the most-derived reopening of
   a builtin exception class that defines a method, the way Ruby's lookup
   does, whatever order the reopenings were written in. */
int sp_exc_nearest_cls(const char *raised, const char *const *targets, int n) {
  if (!raised) return -1;
  const char *cls = sp_exc_canonical_name(raised);
  for (int depth = 0; depth < 30 && cls; depth++) {
    for (int i = 0; i < n; i++)
      if (sp_exc_level_matches(cls, sp_exc_canonical_name(targets[i]))) return i;
    const char *parent = NULL;
    if (sp_user_exc_parent_fn) parent = sp_user_exc_parent_fn(cls);
    if (!parent) parent = sp_exc_parent_of_name(cls);
    cls = parent;
  }
  for (int i = 0; i < n; i++)
    if (!strcmp(targets[i], "Exception")) return i;
  return -1;
}
/* Class-gated introspection accessors (#2753-#2756, #2770): each answers only
   on its CRuby-defining class (walking the name-carried hierarchy) and raises
   NoMethodError elsewhere, matching per-class method definitions. */
SP_COLD void sp_exc_acc_gate(sp_Exception *e, const char *cls, const char *acc) {SP_GC_ROOT(e);
  if (e && sp_exc_cls_matches(e->cls_name, cls)) return;
  sp_raise_cls("NoMethodError",
               sp_sprintf("undefined method '%s' for %s", acc,
                          e ? sp_sprintf("an instance of %s", e->cls_name) : "nil"));
}
/* Does `raised` descend from StandardError? Used by a bare `rescue` (no class),
   which catches StandardError and its subclasses only. CRuby's non-StandardError
   branch is a small fixed set of system exceptions; EVERY other exception -- all
   library and user classes -- descends from StandardError. So an unknown class
   (a C-raised package error like JSON::ParserError, or a user class with no
   registered parent) defaults to StandardError; only the listed roots and their
   subclasses answer false. */
int sp_exc_is_standard_error(const char *raised) {
  if (!raised) return 0;
  static const char *const NONSTD[] = {
    "Exception", "NoMemoryError", "ScriptError", "LoadError",
    "NotImplementedError", "SyntaxError", "SecurityError", "SignalException",
    "Interrupt", "SystemExit", "SystemStackError", "fatal", NULL
  };
  const char *cls = raised;
  for (int depth = 0; depth < 30 && cls; depth++) {
    if (!strcmp(cls, "StandardError")) return 1;
    for (int i = 0; NONSTD[i]; i++)
      if (!strcmp(cls, NONSTD[i])) return 0;
    /* walk user subclasses toward their declared parent; a builtin StandardError
       subclass has no user parent and terminates here, defaulting to true. */
    const char *parent = sp_user_exc_parent_fn ? sp_user_exc_parent_fn(cls) : NULL;
    if (!parent) return 1;
    cls = parent;
  }
  return 1;
}
/* the 0xff byte is the frozen-literal marker sp_exc_gc_scan reads at msg[-1];
   the string itself is the empty one that follows it */
static const char sp_exc_no_msg_storage[] = "\xff" "\0" "\xff";
const char *const sp_exc_no_msg = sp_exc_no_msg_storage + 1;
/* the frozen empty sentinel: the second "" of the same storage, behind a
   marker byte of its own (sp_exc_no_msg's NUL, then 0xff) */
const char *const sp_exc_no_msg_frozen = sp_exc_no_msg_storage + 3;

/* SystemCallError#errno reads what SystemCallError#initialize stored: the
   number of the Errno class the exception descends from. An exception built
   under a class of that family gets it here, at construction -- the runtime's
   own raises, a rescue binding rebuilt from a raised class and message, and
   `.new` with no initialize of the program's own in between. (A program's
   initialize that never calls super leaves it nil, as in CRuby: its
   constructor clears it, and the super call sets it.) */
void sp_exc_syserr_init(sp_Exception *e) {
  sp_int num = 0;
  if (e && sp_syserr_kind(e->cls_name, &num) == SP_SYSERR_NUM) e->xkey = sp_box_int(num);
}
/* Create an exception for a `rescue => e` binding: like sp_exc_new but
   also looks up the parent class via the user hierarchy callback. */
/* The exception's own copy of its message. The length is strlen's: what arrives
   is a bare C string as often as a String (see sp_msg_heapify). */
const char *sp_exc_msg_counted(const char *m, size_t n) {
  SP_GC_ROOT_STR(m);
  char *r = sp_str_alloc(SP_CMSG_HDR + n);
  memcpy(r, "\xff\xfe" "CM" "\xfd\x01", 6);
  uint32_t n32 = (uint32_t)n;
  memcpy(r + 6, &n32, sizeof n32);
  memcpy(r + SP_CMSG_HDR, m, n);
  return r;
}
const char *sp_exc_msg_counted_frozen(const char *m, size_t n) {
  char *r = (char *)sp_exc_msg_counted(m, n);
  r[5] = 0x02;
  return r;
}
/* How an exception's class prints. A Class.new class is keyed by the name the
   compiler gave it, which is not the name Ruby shows (#<Class:0x...> until a
   constant names it); a program that has one installs sp_class_display_fn, and
   every other program prints the key. */
extern const char *(*sp_class_display_fn)(sp_Class);
const char *sp_exc_cls_display(const char *cn) {
  if (!sp_class_display_fn || !cn) return cn;
  sp_Class c = {-1, cn};
  const char *d = sp_class_display_fn(c);
  return d ? d : cn;
}
/* An unnamed Class.new class prints in the address form, and CRuby adds no
   "(Class)" suffix for it. */
sp_bool sp_exc_cls_unnamed(const char *shown) {
  return shown && strncmp(shown, "#<Class:", 8) == 0;
}
static const char *sp_exc_msg_copy(const char *m) {
  if (sp_cmsg_p(m)) {   /* the counted message decodes to its payload, a NUL kept */
    size_t cn = sp_cmsg_len(m);
    char *r = sp_str_alloc(cn);
    memcpy(r, m + SP_CMSG_HDR, cn);
    /* a frozen String's message is frozen (0xfa: a frozen heap String that
       is collected) */
    if (sp_cmsg_frozen(m)) ((unsigned char *)r)[-1] = 0xfa;
    return r;
  }
  size_t n = strlen(m);
  char *r = sp_str_alloc(n);
  memcpy(r, m, n);
  return r;
}
/* A raise's frozen message, counted (sp_exc_msg_given) and with no NUL in
   it, as plain text that is frozen (0xfa): what sp_raise_cls carries in
   the handler stack, so every reader of the raw message (the uncaught
   report, a thread's or fiber's re-raise, the name recovery) reads its
   text, and the rescue's exception keeps the mark (sp_exc_new_for_catch).
   Allocated without a collection's turn, as sp_msg_heapify's copy is. */
const char *sp_exc_msg_plain(const char *m) {
  if (!sp_cmsg_p(m) || !sp_cmsg_frozen(m)) return m;
  size_t n = sp_cmsg_len(m);
  /* an empty message is the explicit-empty sentinel, as sp_exc_msg_given
     makes it for an unfrozen "" */
  if (n == 0) return sp_exc_no_msg_frozen;
  if (memchr(m + SP_CMSG_HDR, 0, n)) return m;
  char *r = sp_str_alloc_nogc(n);
  memcpy(r, m + SP_CMSG_HDR, n);
  ((unsigned char *)r)[-1] = 0xfa;
  return r;
}
sp_Exception *sp_exc_new_for_catch(const char *cls, const char *msg) {
  /* every caller hands a message with a marker byte (a raise's, from the
     handler stack, or sp_exc_msg_given's): a frozen one stays frozen */
  int fz = msg && msg != sp_exc_no_msg && !sp_cmsg_p(msg) &&
           (((const unsigned char *)msg)[-1] == 0xfa || ((const unsigned char *)msg)[-1] == 0xf8);
  /* the caller's message may be held by nothing else: rooted before the
     copy allocates (#8323) */
  const char *msg0 = msg; SP_GC_ROOT_STR(msg0);
  if (!sp_exc_msg_empty_given(msg)) msg = sp_msg_heapify(msg); SP_GC_ROOT_STR(msg);
  sp_Exception *e = sp_exc_new(cls, msg);
  if (fz && e->msg) ((unsigned char *)e->msg)[-1] = 0xfa;
  if (sp_user_exc_parent_fn) {
    const char *par = sp_user_exc_parent_fn(cls);
    if (par) {
      e->parent_cls_name = par;
      sp_exc_syserr_init(e);
      /* the runtime raises no class of the program, so nothing recorded a
         key or a receiver: KeyError#key raises for one, as in CRuby */
      e->has_key = 0; e->has_recv = 0;
    }
  }
  return e;
}

/* An ensure that an exception passes through makes it its in-flight cause.
   A raise of a String carries no object, only its message in the ensure's C
   local, which nothing roots: the object made here holds the message, and
   the local is pointed at the object's copy, so the re-raise after the
   ensure body (which may collect) reads a live String (#8323). */
void *sp_exc_ensure_obj(void **obj, const char **msg, const char *cls) {
  if (*obj) return *obj;
  sp_Exception *e = sp_exc_new_for_catch(cls, *msg);
  *obj = e;
  if (*msg && !sp_exc_msg_empty_given(*msg) && e->msg) *msg = e->msg;
  return e;
}
/* Allocate a zeroed exception-subclass struct of `sz` bytes with the base
   {cls_name, parent_cls_name, msg} prefix set, for the degenerate catch path
   where a user subclass with ivars was raised without a carried object
   (#1415). Its ivar fields stay zero (nil/0). msg is the only heap field, so
   the base scan suffices. */
void *sp_exc_new_sub_sized(size_t sz, const char *cls_name, const char *msg) {if (!sp_exc_msg_empty_given(msg)) msg = sp_msg_heapify(msg); SP_GC_ROOT_STR(msg);
  sp_Exception *e = (sp_Exception *)sp_gc_alloc(sz, NULL, sp_exc_gc_scan);
  memset(e, 0, sz);
  e->cls_name = cls_name ? cls_name : "RuntimeError";
  e->result = sp_box_nil();   /* memset left tag 0 (int 0); StopIteration#result wants nil */
  e->xname = sp_box_nil();
  e->xkey = sp_box_nil();
  e->xrecv = sp_box_nil();
  if (sp_user_exc_parent_fn) e->parent_cls_name = sp_user_exc_parent_fn(e->cls_name);
  if (e->parent_cls_name) sp_exc_syserr_init(e);
  /* heap-launder the message (see sp_exc_new); memset left msg NULL, so a GC
     during the copy scans a consistent struct */
  SP_GC_ROOT(e);
  /* an explicitly given message stays, even empty (#3713); none given leaves
     msg NULL, and #message answers the class name when it is asked */
  if ((msg && msg[0]) || sp_exc_msg_empty_given(msg)) {
    e->msg = sp_exc_msg_copy(msg && msg[0] ? msg : "");
    if (msg == sp_exc_no_msg_frozen) ((unsigned char *)e->msg)[-1] = 0xfa;   /* `raise C, ""` */
  }
  /* The copy can collect, and a collection promotes the rooted object it
     is filling: an old holder then receives a young string. Recorded after
     the store, since the allocation would clear a record made before it. */
  sp_gc_wb((void *)e);
  return e;
}
void sp_exc_gc_scan(void *p) {
  sp_Exception *e = (sp_Exception *)p;
  if (e->msg) sp_mark_string(e->msg);
  if (e->msg_h) sp_gc_mark(e->msg_h);
  if (e->cause) sp_gc_mark(e->cause);
  sp_mark_rbval(e->result);
  sp_mark_rbval(e->xname);
  sp_mark_rbval(e->xkey);
  sp_mark_rbval(e->xrecv);
  /* backtrace storage: the base sp_Exception's `backtrace` field is
   * mirrored by every user exception subclass struct (codegen emits
   * it in the struct definition for ivar-bearing classes; nivars==0
   * subclasses are typedef'd to sp_Exception). So `e->backtrace` is
   * valid for both shapes and the GC can mark it directly. */
  if (e->backtrace) sp_gc_mark(e->backtrace);
  /* cls_name/parent_cls_name point into rodata -- not GC-managed strings */
}

/* Exception#set_backtrace: replace the stored backtrace with `bt`.
 * The base sp_Exception's `backtrace` field is mirrored by every
 * user exception subclass struct (codegen emits it in the struct
 * definition for ivar-bearing classes; nivars==0 subclasses are
 * typedef'd to sp_Exception). So `e->backtrace` is valid for both
 * shapes, no offset arithmetic needed.
 *
 * The codegen gate at src/codegen_call.c stands down for any user
 * class that defines its own #set_backtrace, so this builtin only
 * runs for the base sp_Exception and for user subclasses that did
 * NOT define their own.
 *
 * CRuby returns the array; the AOT codegen reads the receiver from
 * this return value (the sp_RbVal of the stored array) to satisfy
 * `e.set_backtrace(bt)` in a chain. */
sp_RbVal sp_Exception_set_backtrace(sp_Exception *e, sp_StrArray *bt) {
  SP_GC_ROOT(e);
  SP_GC_ROOT(bt);
  sp_gc_wb((void *)e);   /* the receiver may be old; bt is not */
  e->backtrace = bt;
  return bt ? sp_box_obj(bt, SP_BUILTIN_STR_ARRAY) : sp_box_nil();
}
/* cls_name is rodata -- every caller passes a bare literal, and the scan below
   says so and never marks it. Rooting it as a STRING had the collector read
   its marker byte, which for a bare literal is the byte BEFORE the object: a
   global-buffer-overflow on every raise, and a WRITE of 0xfc over whatever
   precedes it whenever that byte happens to read as an unmarked heap string.
   ASAN reports it on the first collection inside any raise. */
/* The message is copied onto the string heap first, then rooted. The caller's
   pointer cannot be rooted as it stands: a bare C literal -- which every raise
   the runtime and the generated code issue passes -- has no marker byte, so
   sp_mark_string reads the byte BEFORE the object, and WRITES 0xfc over it
   whenever that byte reads as an unmarked heap string. The copy runs with no
   collection in between (sp_msg_heapify), so an unrooted heap message cannot
   be swept out from under it either. The no-message sentinel keeps its
   identity: it is what tells an empty message apart from none at all. */
sp_Exception *sp_exc_new(const char *cls_name, const char *msg) {if (!sp_exc_msg_empty_given(msg)) msg = sp_msg_heapify(msg); SP_GC_ROOT_STR(msg);
  sp_Exception *e = (sp_Exception *)sp_gc_alloc(sizeof(sp_Exception), NULL, sp_exc_gc_scan);
  e->cls_name = cls_name ? cls_name : "RuntimeError";
  e->parent_cls_name = NULL;
  e->msg = NULL;    /* set below; scan-safe if the copy triggers a GC */
  e->cause = NULL;  /* set all fields explicitly; sp_exc_gc_scan reads cause */
  e->result = sp_box_nil();
  e->xname = sp_box_nil();
  /* An Interrupt carries SIGINT as its #signo however it was constructed --
     including the bare `raise Interrupt` class form (#3039). */
  e->xkey = (e->cls_name && !strcmp(e->cls_name, "Interrupt"))
              ? sp_box_int((sp_int)SIGINT) : sp_box_nil();
  e->xrecv = sp_box_nil();
  /* an Errno class's number (#errno); a class of the program reaches its
     Errno ancestor through sp_exc_new_sub / sp_exc_new_for_catch instead */
  if (!strncmp(e->cls_name, "Errno::", 7) || !strncmp(e->cls_name, "IO::E", 5))
    sp_exc_syserr_init(e);
  e->has_recv = 1;   /* cleared by the explicit .new emits that record neither */
  e->has_key = 1;
  /* Launder the message into a GC-heap string: sp_exc_gc_scan marks it via
     the tag byte at msg[-1], which only heap strings carry -- keeping a
     raise site's rodata literal would under-read one byte before it. */
  SP_GC_ROOT(e);
  if ((msg && msg[0]) || sp_exc_msg_empty_given(msg)) {
    e->msg = sp_exc_msg_copy(msg && msg[0] ? msg : "");
    if (msg == sp_exc_no_msg_frozen) ((unsigned char *)e->msg)[-1] = 0xfa;   /* `raise C, ""` */
  }
  sp_gc_wb((void *)e);   /* same reason as sp_exc_new_sub_sized */
  return e;
}
/* Exception#==: same class and message (CRuby value equality); #equal?
   stays pointer identity at the emit site. */
sp_bool sp_exc_eq(sp_Exception *a, sp_Exception *b) {
  if (a == b) return 1;
  if (!a || !b) return 0;
  if (strcmp(a->cls_name ? a->cls_name : "", b->cls_name ? b->cls_name : "") != 0) return 0;
  /* Exception#== compares class, the STORED message and the backtrace.
     UncaughtThrowError alone leaves its stored message nil and renders
     "uncaught throw :tag" lazily, so Ruby sees two of them as equal whatever
     the tag. We keep the rendered text in ->msg, so skip it for that class
     (#3098). Backtraces are empty here by design, see docs/limitations.md. */
  if (a->cls_name && strcmp(a->cls_name, "UncaughtThrowError") == 0) return 1;
  { const char *am = sp_exc_msg_text(a), *bm = sp_exc_msg_text(b);
    /* a message never given is nil in CRuby, which equals only another nil:
       not an empty message, not the class name #message shows */
    if (!am || !bm) return !am && !bm;
    size_t al = sp_str_byte_len(am), bl = sp_str_byte_len(bm);
    return al == bl && memcmp(am, bm, al) == 0; }
}
sp_Exception *sp_exc_new_sub(const char *cls_name, const char *parent_cls, const char *msg) {if (!sp_exc_msg_empty_given(msg)) msg = sp_msg_heapify(msg); SP_GC_ROOT_STR(msg);
  sp_Exception *e = sp_exc_new(cls_name, msg);   /* empty msg already fell back to cls_name */
  e->parent_cls_name = parent_cls;
  sp_exc_syserr_init(e);
  /* `.new` records no key or receiver (KeyError#key raises for it) */
  e->has_key = 0; e->has_recv = 0;
  return e;
}
/* Exception#dup / #clone: a fresh allocation of the receiver's full
   (subclass-sized) payload -- the GC header carries the size, so subclass
   ivar fields copy along (as references, matching Object#dup). */
sp_Exception *sp_exc_dup(sp_Exception *e) {
  if (!e) return e;
  sp_gc_hdr *h = (sp_gc_hdr *)((char *)e - sizeof(sp_gc_hdr));
  size_t payload = h->size - sizeof(sp_gc_hdr);
  SP_GC_ROOT(e);
  sp_Exception *n = (sp_Exception *)sp_gc_alloc(payload, h->finalize, h->scan);
  memcpy(n, e, payload);
  return n;
}
/* Write the staged introspection values (receiver/key/value) into the carried
   exception, creating one when the raise had none (see sp_raise_cls). */
void *sp_exc_apply_staged(const char *cls, const char *msg, void *obj) {if (!sp_exc_msg_empty_given(msg)) msg = sp_msg_heapify(msg); SP_GC_ROOT_STR(msg);
  sp_Exception *e = (sp_Exception *)obj;
  if (!e) e = sp_exc_new(cls, msg);
  /* `obj` is a caller-supplied exception that may have been promoted long ago
     (`raise SomeError` on a constant instance, a re-raised one), and the three
     stores below put young values into it. */
  sp_gc_wb((void *)e);
  if (sp_pending_exc_flags & 1) { e->xrecv = sp_pending_exc_recv; e->has_recv = 1; }
  if (sp_pending_exc_flags & 2) { e->xkey = sp_pending_exc_key; e->has_key = 1; }
  if (sp_pending_exc_flags & 4) e->result = sp_pending_exc_val;
  return e;
}
/* SystemExit#status carried in the result slot; 0 when unset. */
int sp_exc_exit_status(void *obj) {
  sp_Exception *e = (sp_Exception *)obj;
  return (e && e->result.tag == SP_TAG_INT) ? (int)e->result.v.i : 0;
}
/* Exception#exception(msg): a copy of the receiver carrying the new message. */
sp_Exception *sp_exc_exception(sp_Exception *e, const char *msg) {SP_GC_ROOT(e);if (!sp_exc_msg_empty_given(msg)) msg = sp_msg_heapify(msg); SP_GC_ROOT_STR(msg);
  sp_Exception *n = sp_exc_dup(e);
  SP_GC_ROOT(n);
  /* an explicitly given empty message stays empty, frozen for a literal's,
     as sp_exc_new keeps it; none given (exception(nil)) clears the message */
  n->msg = NULL;
  if ((msg && msg[0]) || sp_exc_msg_empty_given(msg)) {
    n->msg = sp_exc_msg_copy(msg && msg[0] ? msg : "");
    if (msg == sp_exc_no_msg_frozen) ((unsigned char *)n->msg)[-1] = 0xfa;
  }
  n->msg_h = NULL;   /* the new message, not the receiver's handle */
  sp_gc_wb((void *)n);   /* same reason as sp_exc_new_sub_sized */
  return n;
}
/* Accept `volatile` pointers: LV slots holding sp_Exception * are
   declared volatile when they live across setjmp, so callers may
   pass volatile-qualified pointers in. The pointee itself isn't
   volatile (cls_name/msg are stable post-construction), so we
   strip volatile internally for one access. */
const char *sp_exc_class_name(volatile sp_Exception *ve) {
  sp_Exception *e = (sp_Exception *)ve;
  /* cls_name points into rodata (see sp_exc_gc_scan) and it comes from the
     raise site's bare literal, so it carries no marker byte. This name reaches
     Ruby as `e.class.to_s`, where the caller roots it and the collector reads
     that byte -- hand back a marked copy instead. One copy is made for a name
     and kept, as a literal is. A Class value carries its name in a plain
     struct that no root and no scan knows (`k = e.class` held in a local, a
     parameter or an instance variable), so a copy on the string heap was
     collected under it and `k.to_s` answered the String that took its place. */
  struct kept_name { const char *key, *copy; };
  static struct kept_name *kept = NULL;   /* guarded by the heap lock */
  static size_t n = 0, cap = 0;           /* cap is a power of two; open addressing */
  const char *name, *r = NULL;
  if (!e || !e->cls_name) return SPL("RuntimeError");
  name = e->cls_name;
  /* A name with no header is a literal, or a copy kept here: its address
     names it, and the table is keyed by address. A heap name (a class
     raised across threads) is found by its text. */
  int by_addr = !sp_str_has_hdr(name);
#define SP_KEPT_SLOT(p) ((size_t)(((uint64_t)(uintptr_t)(p) * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1))
  SP_HEAP_LOCK();
  if (by_addr && cap)
    for (size_t h = SP_KEPT_SLOT(name); kept[h].key && !r; h = (h + 1) & (cap - 1))
      if (kept[h].key == name) r = kept[h].copy;
  if (!r) {
    /* The first sight of this address. Room for it first, so that a copy
       made is a copy kept. */
    if ((n + 1) * 2 > cap) {
      size_t oc = cap, nc = cap ? cap * 2 : 16;
      struct kept_name *ok = kept, *nk = (struct kept_name *)calloc(nc, sizeof *nk);
      if (nk) {
        kept = nk; cap = nc;
        for (size_t i = 0; i < oc; i++) if (ok[i].key) {
          size_t h = SP_KEPT_SLOT(ok[i].key);
          while (kept[h].key) h = (h + 1) & (cap - 1);
          kept[h] = ok[i];
        }
        free(ok);
      }
    }
    if ((n + 1) * 2 <= cap) {
      /* the same name may be kept already, written at another address */
      const char *copy = NULL;
      for (size_t i = 0; i < cap && !copy; i++)
        if (kept[i].key && !strcmp(kept[i].copy, name)) copy = kept[i].copy;
      int made = 0;
      if (!copy) {
        size_t len = strlen(name);
        char *m = (char *)malloc(len + 2);
        if (m) { m[0] = (char)0xff; memcpy(m + 1, name, len + 1); copy = m + 1; made = 1; }
      }
      /* keyed by this address from now on; a heap name's address is no key,
         so a copy made for one is keyed by itself (`raise k` hands it back) */
      if (copy && (by_addr || made)) {
        size_t h = SP_KEPT_SLOT(by_addr ? name : copy);
        while (kept[h].key) h = (h + 1) & (cap - 1);
        kept[h].key = by_addr ? name : copy; kept[h].copy = copy; n++;
      }
      r = copy;
    }
  }
  SP_HEAP_UNLOCK();
#undef SP_KEPT_SLOT
  return r ? r : sp_str_dup_external(e->cls_name);   /* out of memory */
}
/* A copy of a message handle's String as it is now: #message answers a
   String a caller keeps, and the handle's buffer moves as it grows. Sized
   by the handle's length, so a NUL inside stays. */
static const char *sp_exc_msg_handle_copy(sp_String *h) {
  int64_t n = sp_String_length(h);
  char *r = sp_str_alloc((size_t)n);
  memcpy(r, sp_String_cstr(h), (size_t)n);
  return r;
}
const char *sp_exc_msg_text(volatile sp_Exception *ve) {
  sp_Exception *e = (sp_Exception *)ve;
  if (!e) return NULL;
  return e->msg_h ? sp_String_cstr((sp_String *)e->msg_h) : e->msg;
}
void *sp_exc_attach_msg(void *p, void *h) {
  sp_Exception *e = (sp_Exception *)p;
  if (e && h) { e->msg_h = h; sp_gc_wb((void *)e); }
  return p;
}
const char *sp_exc_message(volatile sp_Exception *ve) {
  sp_Exception *e = (sp_Exception *)ve;
  /* a message never set (super(nil), Exception.new) defaults to the class
     name, as CRuby's Exception#message does */
  if (!e) return sp_str_empty;
  if (e->msg_h) return sp_exc_msg_handle_copy((sp_String *)e->msg_h);
  if (e->msg) return e->msg;
  /* none given: the class name as it shows now, read when it is asked, so a
     class named after the exception was built answers its new name */
  return e->cls_name ? sp_str_dup_external(sp_exc_cls_display(e->cls_name)) : sp_str_empty;
}
/* Exception#cause: the exception active when this one was raised, or NULL. */
sp_Exception *sp_exc_cause(volatile sp_Exception *ve) {
  sp_Exception *e = (sp_Exception *)ve;
  return e ? e->cause : NULL;
}
/* StopIteration#result: the value the finished iteration returned (nil for a
   non-StopIteration exception or a past-the-end materialized enumerator). */
sp_RbVal sp_exc_result(volatile sp_Exception *ve) {
  sp_Exception *e = (sp_Exception *)ve;
  return e ? e->result : sp_box_nil();
}
/* The Errno:: family by number. One table both ways: a raise site picks the
   class for the errno it read, and SystemCallError#errno recovers the number
   from the class name the exception carries -- no field on the exception, as
   in CRuby the class determines the number (#4560). The names are the ones
   the runtime raises today plus the common POSIX rest; an errno with no row
   raises the parent SystemCallError, and a class with no row answers nil.
   Rows go in CRuby's order: where two names share a number, the first is the
   class an errno raises and the other is the same class (the compiler spells
   it the first way, errno_canonical_name), so ENOTSUP precedes EOPNOTSUPP. */
#define SP_ERRNO_ROWS(X) \
  X(EPERM) X(ENOENT) X(ESRCH) X(EINTR) X(EIO) X(ENXIO) X(E2BIG) X(ENOEXEC) \
  X(EBADF) X(ECHILD) X(EAGAIN) X(ENOMEM) X(EACCES) X(EFAULT) X(EBUSY) \
  X(EEXIST) X(EXDEV) X(ENODEV) X(ENOTDIR) X(EISDIR) X(EINVAL) X(ENFILE) \
  X(EMFILE) X(ENOTTY) X(EFBIG) X(ENOSPC) X(ESPIPE) X(EROFS) X(EMLINK) \
  X(EPIPE) X(EDOM) X(ERANGE) X(EDEADLK) X(ENAMETOOLONG) X(ENOLCK) X(ENOSYS) \
  X(ENOTEMPTY) X(ELOOP) X(ENOTSOCK) X(EMSGSIZE) X(EPROTOTYPE) \
  X(ENOPROTOOPT) X(EPROTONOSUPPORT) X(ENOTSUP) X(EOPNOTSUPP) X(EAFNOSUPPORT) \
  X(EADDRINUSE) X(EADDRNOTAVAIL) X(ENETDOWN) X(ENETUNREACH) X(ENETRESET) \
  X(ECONNABORTED) X(ECONNRESET) X(ENOBUFS) X(EISCONN) X(ENOTCONN) \
  X(ETIMEDOUT) X(ECONNREFUSED) X(EHOSTUNREACH) X(EALREADY) X(EINPROGRESS) \
  X(ESTALE) X(EDQUOT) X(ECANCELED) X(EOVERFLOW) X(EILSEQ)
static const struct { const char *name; int num; } SP_ERRNO_TAB[] = {
#define SP_ERRNO_ROW(n) { "Errno::" #n, n },
  SP_ERRNO_ROWS(SP_ERRNO_ROW)
#undef SP_ERRNO_ROW
  /* names a platform may give a number of their own */
#ifdef EWOULDBLOCK
  { "Errno::EWOULDBLOCK", EWOULDBLOCK },
#endif
#ifdef EDEADLOCK
  { "Errno::EDEADLOCK", EDEADLOCK },
#endif
  /* the rest of CRuby's Errno classes, each where the platform has it: a
     number with no row here would build a plain SystemCallError where CRuby
     builds its Errno class (SystemCallError.new(msg, n)) */
#ifdef ENOTBLK
  { "Errno::ENOTBLK", ENOTBLK },
#endif
#ifdef ETXTBSY
  { "Errno::ETXTBSY", ETXTBSY },
#endif
#ifdef EDESTADDRREQ
  { "Errno::EDESTADDRREQ", EDESTADDRREQ },
#endif
#ifdef ESOCKTNOSUPPORT
  { "Errno::ESOCKTNOSUPPORT", ESOCKTNOSUPPORT },
#endif
#ifdef EPFNOSUPPORT
  { "Errno::EPFNOSUPPORT", EPFNOSUPPORT },
#endif
#ifdef ESHUTDOWN
  { "Errno::ESHUTDOWN", ESHUTDOWN },
#endif
#ifdef ETOOMANYREFS
  { "Errno::ETOOMANYREFS", ETOOMANYREFS },
#endif
#ifdef EHOSTDOWN
  { "Errno::EHOSTDOWN", EHOSTDOWN },
#endif
#ifdef EPROCLIM
  { "Errno::EPROCLIM", EPROCLIM },
#endif
#ifdef EUSERS
  { "Errno::EUSERS", EUSERS },
#endif
#ifdef EREMOTE
  { "Errno::EREMOTE", EREMOTE },
#endif
#ifdef EBADRPC
  { "Errno::EBADRPC", EBADRPC },
#endif
#ifdef ERPCMISMATCH
  { "Errno::ERPCMISMATCH", ERPCMISMATCH },
#endif
#ifdef EPROGUNAVAIL
  { "Errno::EPROGUNAVAIL", EPROGUNAVAIL },
#endif
#ifdef EPROGMISMATCH
  { "Errno::EPROGMISMATCH", EPROGMISMATCH },
#endif
#ifdef EPROCUNAVAIL
  { "Errno::EPROCUNAVAIL", EPROCUNAVAIL },
#endif
#ifdef EFTYPE
  { "Errno::EFTYPE", EFTYPE },
#endif
#ifdef EAUTH
  { "Errno::EAUTH", EAUTH },
#endif
#ifdef ENEEDAUTH
  { "Errno::ENEEDAUTH", ENEEDAUTH },
#endif
#ifdef EPWROFF
  { "Errno::EPWROFF", EPWROFF },
#endif
#ifdef EDEVERR
  { "Errno::EDEVERR", EDEVERR },
#endif
#ifdef EBADEXEC
  { "Errno::EBADEXEC", EBADEXEC },
#endif
#ifdef EBADARCH
  { "Errno::EBADARCH", EBADARCH },
#endif
#ifdef ESHLIBVERS
  { "Errno::ESHLIBVERS", ESHLIBVERS },
#endif
#ifdef EBADMACHO
  { "Errno::EBADMACHO", EBADMACHO },
#endif
#ifdef EIDRM
  { "Errno::EIDRM", EIDRM },
#endif
#ifdef ENOMSG
  { "Errno::ENOMSG", ENOMSG },
#endif
#ifdef ENOATTR
  { "Errno::ENOATTR", ENOATTR },
#endif
#ifdef EBADMSG
  { "Errno::EBADMSG", EBADMSG },
#endif
#ifdef EMULTIHOP
  { "Errno::EMULTIHOP", EMULTIHOP },
#endif
#ifdef ENODATA
  { "Errno::ENODATA", ENODATA },
#endif
#ifdef ENOLINK
  { "Errno::ENOLINK", ENOLINK },
#endif
#ifdef ENOSR
  { "Errno::ENOSR", ENOSR },
#endif
#ifdef ENOSTR
  { "Errno::ENOSTR", ENOSTR },
#endif
#ifdef EPROTO
  { "Errno::EPROTO", EPROTO },
#endif
#ifdef ETIME
  { "Errno::ETIME", ETIME },
#endif
#ifdef ENOPOLICY
  { "Errno::ENOPOLICY", ENOPOLICY },
#endif
#ifdef ENOTRECOVERABLE
  { "Errno::ENOTRECOVERABLE", ENOTRECOVERABLE },
#endif
#ifdef EOWNERDEAD
  { "Errno::EOWNERDEAD", EOWNERDEAD },
#endif
#ifdef EQFULL
  { "Errno::EQFULL", EQFULL },
#endif
#ifdef ECHRNG
  { "Errno::ECHRNG", ECHRNG },
#endif
#ifdef EL2NSYNC
  { "Errno::EL2NSYNC", EL2NSYNC },
#endif
#ifdef EL3HLT
  { "Errno::EL3HLT", EL3HLT },
#endif
#ifdef EL3RST
  { "Errno::EL3RST", EL3RST },
#endif
#ifdef ELNRNG
  { "Errno::ELNRNG", ELNRNG },
#endif
#ifdef EUNATCH
  { "Errno::EUNATCH", EUNATCH },
#endif
#ifdef ENOCSI
  { "Errno::ENOCSI", ENOCSI },
#endif
#ifdef EL2HLT
  { "Errno::EL2HLT", EL2HLT },
#endif
#ifdef EBADE
  { "Errno::EBADE", EBADE },
#endif
#ifdef EBADR
  { "Errno::EBADR", EBADR },
#endif
#ifdef EXFULL
  { "Errno::EXFULL", EXFULL },
#endif
#ifdef ENOANO
  { "Errno::ENOANO", ENOANO },
#endif
#ifdef EBADRQC
  { "Errno::EBADRQC", EBADRQC },
#endif
#ifdef EBADSLT
  { "Errno::EBADSLT", EBADSLT },
#endif
#ifdef EBFONT
  { "Errno::EBFONT", EBFONT },
#endif
#ifdef ENONET
  { "Errno::ENONET", ENONET },
#endif
#ifdef ENOPKG
  { "Errno::ENOPKG", ENOPKG },
#endif
#ifdef EADV
  { "Errno::EADV", EADV },
#endif
#ifdef ESRMNT
  { "Errno::ESRMNT", ESRMNT },
#endif
#ifdef ECOMM
  { "Errno::ECOMM", ECOMM },
#endif
#ifdef EDOTDOT
  { "Errno::EDOTDOT", EDOTDOT },
#endif
#ifdef ENOTUNIQ
  { "Errno::ENOTUNIQ", ENOTUNIQ },
#endif
#ifdef EBADFD
  { "Errno::EBADFD", EBADFD },
#endif
#ifdef EREMCHG
  { "Errno::EREMCHG", EREMCHG },
#endif
#ifdef ELIBACC
  { "Errno::ELIBACC", ELIBACC },
#endif
#ifdef ELIBBAD
  { "Errno::ELIBBAD", ELIBBAD },
#endif
#ifdef ELIBSCN
  { "Errno::ELIBSCN", ELIBSCN },
#endif
#ifdef ELIBMAX
  { "Errno::ELIBMAX", ELIBMAX },
#endif
#ifdef ELIBEXEC
  { "Errno::ELIBEXEC", ELIBEXEC },
#endif
#ifdef ERESTART
  { "Errno::ERESTART", ERESTART },
#endif
#ifdef ESTRPIPE
  { "Errno::ESTRPIPE", ESTRPIPE },
#endif
#ifdef EUCLEAN
  { "Errno::EUCLEAN", EUCLEAN },
#endif
#ifdef ENOTNAM
  { "Errno::ENOTNAM", ENOTNAM },
#endif
#ifdef ENAVAIL
  { "Errno::ENAVAIL", ENAVAIL },
#endif
#ifdef EISNAM
  { "Errno::EISNAM", EISNAM },
#endif
#ifdef EREMOTEIO
  { "Errno::EREMOTEIO", EREMOTEIO },
#endif
#ifdef ENOMEDIUM
  { "Errno::ENOMEDIUM", ENOMEDIUM },
#endif
#ifdef EMEDIUMTYPE
  { "Errno::EMEDIUMTYPE", EMEDIUMTYPE },
#endif
#ifdef ENOKEY
  { "Errno::ENOKEY", ENOKEY },
#endif
#ifdef EKEYEXPIRED
  { "Errno::EKEYEXPIRED", EKEYEXPIRED },
#endif
#ifdef EKEYREVOKED
  { "Errno::EKEYREVOKED", EKEYREVOKED },
#endif
#ifdef EKEYREJECTED
  { "Errno::EKEYREJECTED", EKEYREJECTED },
#endif
#ifdef ERFKILL
  { "Errno::ERFKILL", ERFKILL },
#endif
#ifdef EHWPOISON
  { "Errno::EHWPOISON", EHWPOISON },
#endif
#ifdef EIPSEC
  { "Errno::EIPSEC", EIPSEC },
#endif
#ifdef EDOOFUS
  { "Errno::EDOOFUS", EDOOFUS },
#endif
#ifdef ECAPMODE
  { "Errno::ECAPMODE", ECAPMODE },
#endif
#ifdef ENOTCAPABLE
  { "Errno::ENOTCAPABLE", ENOTCAPABLE },
#endif
};
const char *sp_errno_class_name(int e) {
  for (size_t i = 0; i < sizeof SP_ERRNO_TAB / sizeof SP_ERRNO_TAB[0]; i++)
    if (SP_ERRNO_TAB[i].num == e) return SP_ERRNO_TAB[i].name;
  return "SystemCallError";
}
/* Errno::ENOENT::Errno: the number the class name stands for */
sp_int sp_errno_num(const char *cls) {
  for (size_t i = 0; i < sizeof SP_ERRNO_TAB / sizeof SP_ERRNO_TAB[0]; i++)
    if (!strcmp(SP_ERRNO_TAB[i].name, cls)) return SP_ERRNO_TAB[i].num;
  /* a name the platform has no number for is CRuby's Errno 0 class */
  return strncmp(cls, "Errno::", 7) ? -1 : 0;
}
/* Where class `cls` stands in the SystemCallError family, walking its user
   and builtin parents: SP_SYSERR_NUM when it is (or descends from) an Errno
   class, whose number goes to *num; SP_SYSERR_BASE for SystemCallError
   itself; SP_SYSERR_BARE for a class of the program directly under
   SystemCallError, which has no Errno constant of its own -- CRuby's
   SystemCallError#initialize reads the constant through the class, finds the
   Errno MODULE and raises TypeError converting it; SP_SYSERR_NONE off the
   family. */
int sp_syserr_kind(const char *cls, sp_int *num) {
  const char *cn = sp_exc_canonical_name(cls);
  for (int depth = 0; depth < 30 && cn; depth++) {
    /* a class of the program named into Errno (`class Errno::Mine <
       Errno::ENOENT`) reads its parent's number, as CRuby reads the
       inherited Errno constant */
    const char *uparent = sp_user_exc_parent_fn ? sp_user_exc_parent_fn(cn) : NULL;
    if (!strncmp(cn, "Errno::", 7) && !uparent) {
      /* a name this platform has no number for: CRuby still defines the
         class, with Errno 0 */
      sp_int n = sp_errno_num(cn);
      if (num) *num = n < 0 ? 0 : n;
      return SP_SYSERR_NUM;
    }
    if (!strcmp(cn, "SystemCallError")) return depth == 0 ? SP_SYSERR_BASE : SP_SYSERR_BARE;
    cn = sp_exc_canonical_name(uparent ? uparent : sp_exc_parent_of_name(cn));
  }
  return SP_SYSERR_NONE;
}
/* SystemCallError#message as CRuby's rb_syserr_initialize builds it: the
   C library's text for the number ("unknown error" without one), then
   " @ func" and " - msg" for the arguments given. strerror is what CRuby
   asks too, so the text is the platform's own. `msg` is NULL when none was
   given (nil) -- an empty one is given, and keeps its " - ". */
const char *sp_syserr_text(int has_num, sp_int num, const char *func, const char *msg) {
  /* one allocation: the caller holds func and msg */
  return sp_sprintf("%s%s%s%s%s", has_num ? strerror((int)num) : "unknown error",
                    func ? " @ " : "", func ? func : "", msg ? " - " : "", msg ? msg : "");
}
/* SystemCallError#errno: what SystemCallError#initialize stored -- the
   number of the Errno class the exception descends from (sp_exc_syserr_init),
   or what a plain SystemCallError.new(msg, n) was given -- nil when it never
   ran; NoMethodError off the family, as CRuby defines the reader on
   SystemCallError alone. */
sp_RbVal sp_exc_errno_acc(sp_Exception *e) {SP_GC_ROOT(e);
  sp_exc_acc_gate(e, "SystemCallError", "errno");
  return e->xkey;
}
/* The builtin exception hierarchy, as {class, direct superclass} pairs. Shared
   by Exception#is_a? and the by-name #superclass lookup (#3031). */
const char *sp_exc_parent_of_name(const char *cls) {
  if (!cls) return NULL;
  static const char *const HIER[][2] = {
    {"RuntimeError",          "StandardError"},
    {"ArgumentError",         "StandardError"},
    {"TypeError",             "StandardError"},
    {"NameError",             "StandardError"},
    {"NoMethodError",         "NameError"},
    {"IndexError",            "StandardError"},
    {"KeyError",              "IndexError"},
    {"RangeError",            "StandardError"},
    {"IOError",               "StandardError"},
    {"EOFError",              "IOError"},
    {"ZeroDivisionError",     "StandardError"},
    {"NotImplementedError",   "ScriptError"},
    {"StopIteration",         "IndexError"},
    {"FloatDomainError",      "RangeError"},
    {"Math::DomainError",      "StandardError"},
    {"FrozenError",           "RuntimeError"},
    {"EncodingError",         "StandardError"},
    {"Encoding::UndefinedConversionError", "EncodingError"},
    {"Encoding::InvalidByteSequenceError", "EncodingError"},
    {"Encoding::CompatibilityError",       "EncodingError"},
    /* a LoadError is a ScriptError: `rescue StandardError` and `rescue => e`
       let it through, as sp_exc_is_standard_error already said */
    {"LoadError",             "ScriptError"},
    {"RegexpError",           "StandardError"},
    {"StringScanner_Error",   "StandardError"},
    /* the json package raises these by name; NestingError is what a document
       too deep to serialize raises, and `rescue JSON::ParserError` catches it
       in CRuby because it is a ParserError */
    {"JSON::NestingError",    "JSON::ParserError"},
    /* the json package's errors descend from StandardError through JSONError, as
       CRuby's do: an explicit `rescue StandardError` and `is_a?(StandardError)` say so (#7797) */
    {"JSON::JSONError",       "StandardError"},
    {"JSON::ParserError",     "JSON::JSONError"},
    /* a Float JSON has no spelling for (Infinity, NaN) is refused with it */
    {"JSON::GeneratorError",  "JSON::JSONError"},
    /* IO::Buffer's errors (lib/sp_iobuffer.c raises them by name): all
       RuntimeError subclasses in CRuby, except MaskError < ArgumentError */
    {"IO::Buffer::AccessError",      "RuntimeError"},
    {"IO::Buffer::LockedError",      "RuntimeError"},
    {"IO::Buffer::AllocationError",  "RuntimeError"},
    {"IO::Buffer::InvalidatedError", "RuntimeError"},
    {"IO::Buffer::MaskError",        "ArgumentError"},
    {"FiberError",            "StandardError"},
    {"UncaughtThrowError",    "ArgumentError"},
    {"SyntaxError",           "ScriptError"},
    {"ScriptError",           "Exception"},
    {"StandardError",         "Exception"},
    {"SecurityError",         "Exception"},
    {"SignalException",       "Exception"},
    {"Interrupt",             "SignalException"},
    {"ThreadError",           "StandardError"},
    {"ClosedQueueError",      "StopIteration"},
    {"NoMatchingPatternError", "StandardError"},
    {"NoMatchingPatternKeyError", "NoMatchingPatternError"},
    {"LocalJumpError",        "StandardError"},   /* (#3025) */
    {"SystemExit",            "Exception"},
    {"SystemStackError",      "Exception"},
    {"NoMemoryError",         "Exception"},
    {"SystemCallError",       "StandardError"},
    /* The non-blocking readiness exceptions. Each is an Errno subclass that
       also includes IO::WaitReadable / IO::WaitWritable (see
       sp_exc_modules_of_name) -- the module is why a single-parent chain could
       not express them: WaitWritable is included by classes under two
       different Errno parents. */
    {"IO::EAGAINWaitReadable",      "Errno::EAGAIN"},
    {"IO::EAGAINWaitWritable",      "Errno::EAGAIN"},
    {"IO::EINPROGRESSWaitReadable", "Errno::EINPROGRESS"},
    {"IO::EINPROGRESSWaitWritable", "Errno::EINPROGRESS"},
    {NULL, NULL}
  };
  for (int i = 0; HIER[i][0]; i++)
    if (!strcmp(cls, HIER[i][0])) return HIER[i][1];
  /* Every Errno::* is a SystemCallError, as in CRuby. The names are open --
     sp_file_raise_errno picks one from errno at run time -- so this is a
     prefix rule rather than a table row, and `rescue SystemCallError` catches
     the whole family. */
  if (!strncmp(cls, "Errno::", 7)) return SPL("SystemCallError");
  return NULL;
}
/* Does the exception's class have the class-gated accessor `acc` at all?
   The same classes the accessors' own gates admit. A program that adds a
   method of that name to Object reaches it on every other exception, as
   CRuby's lookup does. */
sp_bool sp_exc_has_acc(sp_Exception *e, const char *acc) {
  if (!e || !acc) return 0;
  const char *c = e->cls_name;
  if (!strcmp(acc, "receiver"))
    return sp_exc_cls_matches(c, "NameError") || sp_exc_cls_matches(c, "KeyError") ||
           sp_exc_cls_matches(c, "FrozenError");
  static const char *const OWN[][2] = {
    {"key", "KeyError"}, {"args", "NoMethodError"}, {"private_call?", "NoMethodError"},
    {"reason", "LocalJumpError"}, {"exit_value", "LocalJumpError"},
    {"tag", "UncaughtThrowError"}, {"value", "UncaughtThrowError"},
    {"status", "SystemExit"}, {"success?", "SystemExit"},
    {"signo", "SignalException"}, {"signm", "SignalException"},
    {"name", "NameError"}, {"errno", "SystemCallError"}, {"result", "StopIteration"},
  };
  for (size_t i = 0; i < sizeof OWN / sizeof OWN[0]; i++)
    if (!strcmp(acc, OWN[i][0])) return sp_exc_cls_matches(c, OWN[i][1]);
  return 0;
}
/* NameError#name (NoMethodError inherits it): the carried missing name.
   Any other exception class raises CRuby's NoMethodError -- the receiver
   type is class-erased at compile time, so the check is a runtime one. */
sp_RbVal sp_exc_name_acc(sp_Exception *e) {SP_GC_ROOT(e);
  if (!e) return sp_box_nil();
  if (sp_exc_cls_matches(e->cls_name, "NameError")) return e->xname;
  sp_raise_cls("NoMethodError",
               sp_sprintf("undefined method 'name' for an instance of %s", e->cls_name));
}
/* Exception accessors on a POLY receiver (an exception rescued into a
   union-typed local): unbox and delegate; a non-exception value is CRuby's
   NoMethodError (#3120, #3122). */
sp_RbVal sp_exc_key_acc(sp_Exception *e) {SP_GC_ROOT(e);
  sp_exc_acc_gate(e, "KeyError", "key");
  /* KeyError.new("m") records no key, and CRuby raises rather than
     answering nil -- nil is a legal key (#3030) */
  if (e && !e->has_key) sp_raise_cls("ArgumentError", "no key is available");
  return e->xkey;
}
sp_RbVal sp_exc_receiver_acc(sp_Exception *e) {SP_GC_ROOT(e);
  if (!(e && (sp_exc_cls_matches(e->cls_name, "NameError") ||
              sp_exc_cls_matches(e->cls_name, "KeyError") ||
              sp_exc_cls_matches(e->cls_name, "FrozenError"))))
    sp_exc_acc_gate(e, "NameError", "receiver");
  /* an explicitly built NameError.new(msg, name) never recorded one, and
     CRuby raises rather than answering nil -- nil is a legal receiver (#3036) */
  if (e && !e->has_recv) sp_raise_cls("ArgumentError", "no receiver is available");
  return e->xrecv;
}
/* LoadError#path: no LoadError the program raises carries one (a require is
   resolved at compile time), so nil -- what one raised by hand answers */
sp_RbVal sp_exc_path_acc(sp_Exception *e) {SP_GC_ROOT(e);
  sp_exc_acc_gate(e, "LoadError", "path");
  return sp_box_nil();
}
sp_RbVal sp_exc_args_acc(sp_Exception *e) {SP_GC_ROOT(e);
  sp_exc_acc_gate(e, "NoMethodError", "args");
  return e->xkey;
}
sp_bool sp_exc_private_call_acc(sp_Exception *e) {SP_GC_ROOT(e);
  sp_exc_acc_gate(e, "NoMethodError", "private_call?");
  return e ? e->priv_call : 0;   /* set only by the explicit .new (#3042) */
}
sp_RbVal sp_exc_exit_value_acc(sp_Exception *e) {SP_GC_ROOT(e);
  sp_exc_acc_gate(e, "LocalJumpError", "exit_value");
  return e->result;
}
sp_RbVal sp_exc_throw_value_acc(sp_Exception *e) {SP_GC_ROOT(e);
  sp_exc_acc_gate(e, "UncaughtThrowError", "value");
  return e->result;
}
sp_int sp_exc_status_acc(sp_Exception *e) {SP_GC_ROOT(e);
  sp_exc_acc_gate(e, "SystemExit", "status");
  return (sp_int)sp_exc_exit_status(e);
}
sp_bool sp_exc_success_acc(sp_Exception *e) {SP_GC_ROOT(e);
  sp_exc_acc_gate(e, "SystemExit", "success?");
  return sp_exc_exit_status(e) == 0;
}
sp_int sp_exc_signo_acc(sp_Exception *e) {SP_GC_ROOT(e);
  sp_exc_acc_gate(e, "SignalException", "signo");
  return (e->xkey.tag == SP_TAG_INT) ? e->xkey.v.i : 0;
}
const char *sp_exc_signm_acc(sp_Exception *e) {SP_GC_ROOT(e);
  sp_exc_acc_gate(e, "SignalException", "signm");
  return sp_exc_message(e);
}

/* `p e` on an exception instance: the same string #inspect answers, for the
   dispatch a container read or a `p` of a user subclass goes through (#3813). */
/* The parts joined by their byte lengths, a NUL in one kept: the message of an exception
   can hold one (#7556), which a %s would cut. Each part is a Spinel String. The copy is
   made with no collection, so the parts need no root (as sp_msg_heapify). */
const char *sp_exc_cat(int n, ...) {
  const char *parts[8]; size_t lens[8]; size_t total = 0;
  va_list ap; va_start(ap, n);
  for (int i = 0; i < n && i < 8; i++) { parts[i] = va_arg(ap, const char *); lens[i] = sp_str_byte_len(parts[i]); total += lens[i]; }
  va_end(ap);
  char *r = sp_str_alloc_nogc(total), *w = r;
  for (int i = 0; i < n && i < 8; i++) { memcpy(w, parts[i], lens[i]); w += lens[i]; }
  return r;
}
/* "<Class>: <msg>" and "<msg> (<Class>)": the class name is read from the exception
   itself (no allocation), msg is the Spinel String the caller computed first, and the
   copy runs with no collection, so no argument needs a root. */
const char *sp_exc_full_text(volatile sp_Exception *ve, const char *msg) {
  sp_Exception *e = (sp_Exception *)ve;
  const char *cn = sp_exc_cls_display(e && e->cls_name ? e->cls_name : "RuntimeError");
  size_t lc = strlen(cn), lm = sp_str_byte_len(msg);
  char *r = sp_str_alloc_nogc(lc + 2 + lm);
  memcpy(r, cn, lc); memcpy(r + lc, ": ", 2); memcpy(r + lc + 2, msg, lm);
  return r;
}
const char *sp_exc_detailed_text(volatile sp_Exception *ve, const char *msg) {
  sp_Exception *e = (sp_Exception *)ve;
  const char *cn = sp_exc_cls_display(e && e->cls_name ? e->cls_name : "RuntimeError");
  size_t lc = strlen(cn), lm = sp_str_byte_len(msg);
  /* An empty message names the class, except RuntimeError's default text.
     The class annotation belongs on the first line; a lone final newline
     is replaced by it, while subsequent lines keep their bytes. */
  if (!lm) {
    const char *text = strcmp(cn, "RuntimeError") == 0 ? "unhandled exception" : cn;
    size_t len = strlen(text);
    char *r = sp_str_alloc_nogc(len);
    memcpy(r, text, len);
    return r;
  }
  /* no class annotation for an unnamed Class.new class */
  if (sp_exc_cls_unnamed(cn)) {
    char *r = sp_str_alloc_nogc(lm);
    memcpy(r, msg, lm);
    return r;
  }
  const char *nl = memchr(msg, '\n', lm);
  size_t first = nl ? (size_t)(nl - msg) : lm;
  size_t tail = lm - first;
  if (tail == 1) tail = 0;
  char *r = sp_str_alloc_nogc(first + 2 + lc + 1 + tail);
  memcpy(r, msg, first); memcpy(r + first, " (", 2); memcpy(r + first + 2, cn, lc); r[first + 2 + lc] = ')';
  if (tail) memcpy(r + first + 2 + lc + 1, msg + first, tail);
  return r;
}
const char *sp_exc_inspect(void *p) {
  sp_Exception *e = (sp_Exception *)p;
  if (!e) return SPL("nil");
  SP_GC_ROOT(e);
  const char *msg = sp_exc_to_s_text(e);
  SP_GC_ROOT(msg);
  const char *cn = sp_exc_class_name(e);
  SP_GC_ROOT(cn);
  cn = sp_exc_cls_display(cn);
  SP_GC_ROOT(cn);
  return (!msg || !sp_str_byte_len(msg)) ? cn : sp_exc_cat(5, SPL("#<"), cn, SPL(": "), msg, SPL(">"));
}

const char *(*sp_user_exc_to_s_fn)(sp_Exception *) = NULL;
const char *sp_exc_to_s_text(sp_Exception *e) {
  return sp_user_exc_to_s_fn ? sp_user_exc_to_s_fn(e) : sp_exc_message(e);
}

/* CRuby's arity ArgumentError (sp_exc.h): the message is built once here for
   every count only the run time knows, as arity_message builds it for the
   counts the compiler sees. */
SP_NORETURN void sp_raise_arity(sp_int given, sp_int min, sp_int max, const char *kw) {
  const char *msg;
  if (!kw) kw = "";
  if (min == max)
    msg = sp_sprintf("wrong number of arguments (given %lld, expected %lld%s)",
                     (long long)given, (long long)min, kw);
  else if (max < 0)
    msg = sp_sprintf("wrong number of arguments (given %lld, expected %lld+%s)",
                     (long long)given, (long long)min, kw);
  else
    msg = sp_sprintf("wrong number of arguments (given %lld, expected %lld..%lld%s)",
                     (long long)given, (long long)min, (long long)max, kw);
  /* the message lives on the collected heap and the raise allocates */
  SP_GC_ROOT_STR(msg);
  sp_raise_cls("ArgumentError", msg);
}
/* The keyword ArgumentErrors (sp_exc.h), as kw_error_message spells them
   for the lists the compiler sees. */
SP_NORETURN void sp_raise_kw_error(const char *kind, sp_int count, const char *names) {
  const char *msg = sp_sprintf("%s keyword%s: %s", kind, count > 1 ? "s" : "", names);
  SP_GC_ROOT_STR(msg);
  sp_raise_cls("ArgumentError", msg);
}

/* Exception#is_a?(ClassName): checks class name and known hierarchy. */
sp_int sp_exc_is_a(volatile sp_Exception *ve, const char *cn) {
  sp_Exception *e = (sp_Exception *)ve;
  if (!e || !cn) return 0;
  /* one authority for "does this level answer to cn", modules included: the
     matcher rescue arms use. Without it #is_a?(SomeModule) said false where
     `rescue SomeModule` said yes (#3366 follow-up). */
  cn = sp_exc_canonical_name(cn);
  if (sp_exc_cls_matches(e->cls_name, cn)) return 1;
  /* find the exception's class chain and check if cn appears in it */
  const char *cls = e->cls_name;
  int used_parent = 0;
  for (int depth = 0; depth < 20 && cls; depth++) {
    if (!strcmp(cls, cn)) return 1;
    const char *parent = sp_exc_parent_of_name(cls);
    if (!parent) {
      /* unknown (user) class: try user hierarchy first */
      if (sp_user_exc_parent_fn) { parent = sp_user_exc_parent_fn(cls); }
      if (!parent) {
        if (!used_parent && e->parent_cls_name) {
          cls = e->parent_cls_name;
          used_parent = 1;
          continue;
        }
        if (!strcmp(cn, "Exception")) return 1;
        if (!strcmp(cn, "Object") || !strcmp(cn, "BasicObject")) return 1;
        break;
      }
    }
    cls = parent;
  }
  if (!strcmp(cn, "Object") || !strcmp(cn, "BasicObject") || !strcmp(cn, "Kernel")) return 1;
  return 0;
}

/* Each of the fixed-depth handler stacks in spinel_rt.h fails the same way when
   a program nests deeper than its array holds; see the comment there. */
SP_NORETURN SP_COLD void sp_stack_too_deep(void) {
  fputs("stack level too deep (SystemStackError)\n", stderr);
  exit(1);
}

/* The per-fiber handler context (lib/sp_exc_ctx.h): the operations that touch
   only the context itself. */
void *sp_exc_ctx_new(void) {
#ifdef SP_CEXT
  sp_exc_ctx_t *ctx = calloc(1, sizeof(sp_exc_ctx_t));
  if (ctx) ctx->cext_errinfo = Qnil;
  return ctx;
#else
  return calloc(1, sizeof(sp_exc_ctx_t));
#endif
}
void sp_exc_ctx_free(void *p) {
  sp_exc_ctx_t *x = (sp_exc_ctx_t *)p;
  if (!x) return;
#ifdef SP_CEXT
  rb_gc_unregister_address(&x->cext_errinfo);
  sp_cext_arena_context_dispose(&x->cext_arena);
  free(x->cext_marks);
#endif
  free(x->es); free(x->em); free(x->ec); free(x->eo);
  free(x->cs); free(x->ct); free(x->ctk); free(x->cv); free(x->cet);
  free(x->bs); free(x->bv); free(x->bser); free(x->bet); free(x->shand);
  free(x->rrf); free(x->rrem); free(x->rrcm); free(x->rrbm);
  free(x->erm); free(x->ersm); free(x->crm); free(x);
}
void sp_exc_ctx_mark(void *p) {            /* GC: mark a suspended fiber's carried exc objects */
  sp_exc_ctx_t *x = (sp_exc_ctx_t *)p;
  if (!x) return;
  for (int i = 0; i < x->en; i++) if (x->eo[i]) sp_gc_mark(x->eo[i]);
  /* a suspended fiber's proc-return chain (nodes on its preserved C stack) may
     carry an in-flight return value; mark each so it survives a GC during yield. */
  for (sp_proc_home *h = x->prhead; h; h = h->prev) sp_mark_rbval(h->val);
  for (int i = 0; i < x->bn; i++) sp_mark_rbval(x->bv[i]);   /* carried break scopes */
  for (int i = 0; i < x->rn; i++) if (x->shand[i]) sp_gc_mark(x->shand[i]);  /* handled excs */
  if (x->icause) sp_gc_mark(x->icause);   /* the one a suspended ensure body runs for */
}
