/* sp_exc.c -- cold sp_Exception ops (see sp_exc.h). 0 optcarrot uses. */
#include "sp_exc.h"
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
static const char sp_exc_no_msg_storage[] = "\xff";
const char *const sp_exc_no_msg = sp_exc_no_msg_storage + 1;

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
sp_Exception *sp_exc_new_for_catch(const char *cls, const char *msg) {if (msg != sp_exc_no_msg) msg = sp_msg_heapify(msg); SP_GC_ROOT_STR(msg);
  sp_Exception *e = sp_exc_new(cls, msg);
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
/* Allocate a zeroed exception-subclass struct of `sz` bytes with the base
   {cls_name, parent_cls_name, msg} prefix set, for the degenerate catch path
   where a user subclass with ivars was raised without a carried object
   (#1415). Its ivar fields stay zero (nil/0). msg is the only heap field, so
   the base scan suffices. */
void *sp_exc_new_sub_sized(size_t sz, const char *cls_name, const char *msg) {if (msg != sp_exc_no_msg) msg = sp_msg_heapify(msg); SP_GC_ROOT_STR(msg);
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
  /* an explicitly given message stays, even empty (#3713) */
  e->msg = sp_sprintf("%s", (msg && msg[0]) ? msg
                            : (msg == sp_exc_no_msg ? "" : e->cls_name));
  /* The sprintf can collect, and a collection promotes the rooted object it
     is filling: an old holder then receives a young string. Recorded after
     the store, since the allocation would clear a record made before it. */
  sp_gc_wb((void *)e);
  return e;
}
void sp_exc_gc_scan(void *p) {
  sp_Exception *e = (sp_Exception *)p;
  if (e->msg) sp_mark_string(e->msg);
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
sp_Exception *sp_exc_new(const char *cls_name, const char *msg) {if (msg != sp_exc_no_msg) msg = sp_msg_heapify(msg); SP_GC_ROOT_STR(msg);
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
  e->msg = sp_sprintf("%s", (msg && msg[0]) ? msg
                            : (msg == sp_exc_no_msg ? ""
                                                    : (cls_name ? cls_name : "RuntimeError")));
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
  return strcmp(a->msg ? a->msg : "", b->msg ? b->msg : "") == 0;
}
sp_Exception *sp_exc_new_sub(const char *cls_name, const char *parent_cls, const char *msg) {if (msg != sp_exc_no_msg) msg = sp_msg_heapify(msg); SP_GC_ROOT_STR(msg);
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
void *sp_exc_apply_staged(const char *cls, const char *msg, void *obj) {if (msg != sp_exc_no_msg) msg = sp_msg_heapify(msg); SP_GC_ROOT_STR(msg);
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
sp_Exception *sp_exc_exception(sp_Exception *e, const char *msg) {SP_GC_ROOT(e);if (msg != sp_exc_no_msg) msg = sp_msg_heapify(msg); SP_GC_ROOT_STR(msg);
  sp_Exception *n = sp_exc_dup(e);
  SP_GC_ROOT(n);
  n->msg = sp_sprintf("%s", (msg && msg[0]) ? msg : (n->cls_name ? n->cls_name : "RuntimeError"));
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
     that byte -- hand back a string of our own instead. Every caller is a cold
     path (a render, a cross-thread re-raise), so the copy costs nothing that
     matters. */
  return e && e->cls_name ? sp_str_dup_external(e->cls_name) : SPL("RuntimeError");
}
const char *sp_exc_message(volatile sp_Exception *ve) {
  sp_Exception *e = (sp_Exception *)ve;
  /* a message never set (super(nil), Exception.new) defaults to the class
     name, as CRuby's Exception#message does */
  if (!e) return sp_str_empty;
  if (e->msg) return e->msg;
  return e->cls_name ? sp_str_dup_external(e->cls_name) : sp_str_empty;
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
  return strncmp(cls, "Errno::", 7) ? SP_INT_NIL : 0;
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
      if (num) *num = n == SP_INT_NIL ? 0 : n;
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
    /* a Float JSON has no spelling for (Infinity, NaN) is refused with it */
    {"JSON::GeneratorError",  "StandardError"},
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
const char *sp_exc_inspect(void *p) {
  sp_Exception *e = (sp_Exception *)p;
  if (!e) return SPL("nil");
  SP_GC_ROOT(e);
  const char *msg = sp_exc_to_s_text(e);
  SP_GC_ROOT(msg);
  const char *cn = sp_exc_class_name(e);
  SP_GC_ROOT(cn);
  return (!msg || !*msg) ? cn : sp_sprintf("#<%s: %s>", cn, msg);
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
