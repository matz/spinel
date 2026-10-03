/* codegen_call_io.c -- emit_call_body's arms of IO, File, Dir, ARGF and the other handle types.
   Moved from emit_call_body (codegen_call.c) unchanged: each function holds
   a run of its arms, in their order, and answers 1 when one emitted the call
   (codegen_call_arms.h). */

#include "codegen_internal.h"
#include "codegen_poly.h"
#include "builtin_ops.h"
#include "call_plan.h"
#include "codegen_call_arms.h"

/* the IO methods on a poly receiver that may hold a stream (write, read, gets, puts, print, ...) */
int emit_call_poly_io_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* IO instance methods on a poly-carried handle (an IO.pipe element): unbox
     and dispatch, unless a user class defines the name (then the general poly
     dispatch owns it). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_POLY &&
      (sp_streq(name, "write") || sp_streq(name, "read") || sp_streq(name, "gets") ||
       sp_streq(name, "readline") || sp_streq(name, "close") || sp_streq(name, "flush") ||
       sp_streq(name, "fileno") ||
       /* the output family and the cheap predicates: a stream read back out of
          a container (`[$stdout, $stderr][0].puts`) answered NoMethodError
          naming IO, which is what it was. Names with a non-IO meaning on a
          poly value -- `size`, `<<`, `each` -- stay off this list; they have
          their own arms. */
       sp_streq(name, "puts") || sp_streq(name, "print") || sp_streq(name, "putc") ||
       (sp_streq(name, "printf") && boxed_printf_list_ok(c, argv, argc)) ||
       sp_streq(name, "eof?") || sp_streq(name, "closed?") || sp_streq(name, "path") ||
       sp_streq(name, "to_path") ||
       sp_streq(name, "tty?") || sp_streq(name, "isatty") ||
       (sp_streq(name, "winsize") && sp_feature_enabled("io/console")) ||
       sp_streq(name, "readlines") || sp_streq(name, "rewind") ||
       sp_streq(name, "readpartial") ||
       /* a socket's addresses: a connection passed into a Thread arrives
          boxed, and a server reads REMOTE_ADDR from it */
       ((sp_streq(name, "addr") || sp_streq(name, "peeraddr")) && argc == 0 &&
        sp_feature_required("socket")) ||
       /* the descriptor controls, at CRuby's arities, unless a splat carries
          the arguments, the advice is not a Symbol, or a class method or an
          attribute writer of that name may be the receiver's */
       (boxed_desc_control_arity(name, argc) && !call_has_splat_arg(nt, argv, argc) &&
        !(sp_streq(name, "advise") && comp_ntype(c, argv[0]) != TY_SYMBOL) &&
        !class_method_named(c, name) && !attr_writer_named(c, -1, name)) ||
       /* the descriptor surface a boxed handle needs as much as a typed one:
          an fd table is a mixed Hash (0/1/2 an IO, the rest Files), so every
          one of these reached the unresolved-call gate (#4611) */
       /* File::Stat's mode and numeric fields: a stat read out of a
          container is the same boxed handle. Not where a class method may
          own the name, or an OpenStruct may carry it as a field. */
       (argc == 0 && boxed_stat_name(name) && !class_method_named(c, name) &&
        !sp_feature_required("ostruct")) ||
       sp_streq(name, "stat") || sp_streq(name, "seek") || sp_streq(name, "tell") ||
       sp_streq(name, "pos") || sp_streq(name, "pread") || sp_streq(name, "pwrite") ||
       sp_streq(name, "fsync") || sp_streq(name, "fdatasync") ||
       sp_streq(name, "sync") || sp_streq(name, "sync=") ||
       /* the non-blocking pair: a Socket destructured out of Socket.pair, or
          read back out of a container, is a poly value like any other, and
          without an arm here `w.read_nonblock(n, exception: false)` had no
          emitter at all (#4236/#4237) */
       sp_streq(name, "read_nonblock") || sp_streq(name, "write_nonblock") ||
       /* the readiness family: a lambda's parameter is boxed, so a handle
          passed through one reached `wait_readable` with no emitter, and in
          a condition it was refused as non-bool. IO#wait stays off the list,
          ConditionVariable#wait shares the name. */
       ((sp_streq(name, "wait_readable") || sp_streq(name, "wait_writable") ||
         sp_streq(name, "wait_priority")) && argc <= 1) ||
       /* File::Stat's predicates: a stat read out of a container is the
          same boxed handle. Not where a class method may own the name. */
       (argc == 0 && boxed_stat_pred(name) >= 0 && !class_method_named(c, name)))) {
    int iocand = 0;
    for (int k = 0; k < c->nclasses && !iocand; k++) {
      /* a native class's methods are its declared bindings, which is the
         rule the poly dispatch counts candidates by: a Ruby-side def on it
         (IO::Buffer#write over an IO, #4474) is not a candidate there, so
         it must not send the call there either, or the dispatch declines
         and `fds[1].write(s)` on a real IO raises NoMethodError again. The
         old blanket test disabled this whole arm whenever ANY native class
         existed -- IO::Buffer's implicit splice made that every program
         touching it. */
      if (c->classes[k].is_native_class) {
        if (comp_native_method_find(c, k, name, argc, 0) >= 0) iocand = 1;
        continue;
      }
      if (comp_method_in_chain(c, k, name, NULL) >= 0 ||
          comp_reader_in_chain(c, k, name, NULL))
        iocand = 1;
      /* an attr writer answers `x.sync = v` as a reader answers `x.sync` */
      size_t nl = strlen(name);
      if (nl > 1 && nl < 256 && name[nl - 1] == '=') {
        char base[256]; memcpy(base, name, nl - 1); base[nl - 1] = 0;
        if (comp_writer_in_chain(c, k, base, NULL)) iocand = 1;
      }
    }
    /* printf: the receiver and every argument, the format included, are
       evaluated and rooted before the handle is unboxed, so a receiver that
       is no IO raises NoMethodError after them, as in CRuby. The format
       then comes off the front of the list (a splat may supply it) and the
       rest are formatted and written as the typed arm does. */
    if (!iocand && sp_streq(name, "printf")) {
      int trv = ++g_tmp, tpa = ++g_tmp, tio3 = ++g_tmp, tfv = ++g_tmp, tfs = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", trv);
      emit_boxed(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ",
                 trv, tpa, tpa);
      emit_push_arg_list(c, argv, argc, tpa, b);
      buf_printf(b, "sp_File *_t%d = sp_poly_as_io(_t%d, \"printf\"); ", tio3, trv);
      buf_printf(b, "if (_t%d->len == 0) sp_raise_cls(\"ArgumentError\", \"too few arguments\"); ", tpa);
      buf_printf(b, "sp_RbVal _t%d = sp_PolyArray_shift(_t%d); SP_GC_ROOT_RBVAL(_t%d); ", tfv, tpa, tfv);
      buf_printf(b, "const char *_t%d = sp_poly_arg_str_chk(_t%d); SP_GC_ROOT_STR(_t%d); ", tfs, tfv, tfs);
      buf_printf(b, "sp_File_write_bin(_t%d, sp_str_format_polyarr(_t%d, _t%d)); sp_box_nil(); })",
                 tio3, tfs, tpa);
      return 1;
    }
    if (!iocand) {
      /* write with an argument list (boxed_write_takes_list): the receiver
         and then the arguments are evaluated (a splat contributes its
         elements), the receiver and the list rooted, then the handle is
         unboxed and checked open and each argument converted and written
         in turn, answering the total byte count. The branch below writes
         one argument. */
      if (sp_streq(name, "write") && boxed_write_takes_list(c, id, argv, argc)) {
        int trv = ++g_tmp, tpa = ++g_tmp, tio3 = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", trv);
        emit_boxed(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ",
                   trv, tpa, tpa);
        emit_push_arg_list(c, argv, argc, tpa, b);
        buf_printf(b, "sp_File *_t%d = sp_poly_as_io(_t%d, \"write\"); SP_IO_OPEN(_t%d); sp_int _t%d = 0; ",
                   tio3, trv, tio3, tn);
        buf_printf(b, "for (sp_int _i = 0; _i < _t%d->len; _i++) _t%d += sp_File_write_poly(_t%d, _t%d->data[_i]); _t%d; })",
                   tpa, tn, tio3, tpa, tn);
        return 1;
      }
      int tio2 = ++g_tmp;
      /* pos=, sysseek, flock, fcntl and advise, answering what the typed
         arms answer: the offset pos= set, sysseek's and fcntl's integers,
         flock's status, nil from advise. The receiver and then the
         arguments are evaluated before the handle is unboxed, so a receiver
         that is no IO raises NoMethodError after them, as in CRuby, and so
         does a File::Stat, which rides in the same boxed handle. An integer
         argument the compiler cannot type Integer or Float is held and
         converted once the handle is known, with the typed arms'
         sp_poly_arg_int_chk. */
      if (boxed_desc_control_arity(name, argc)) {
        int trv = ++g_tmp, first_int = sp_streq(name, "advise") ? 1 : 0, tadv = 0;
        int targ[3] = {0, 0, 0}, theld[3] = {0, 0, 0};
        buf_printf(b, "({ sp_RbVal _t%d = ", trv);
        emit_boxed(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", trv);
        if (first_int) {
          /* the advice is a Symbol (:normal, :sequential, ...); read its name */
          tadv = ++g_tmp;
          buf_printf(b, "const char *_t%d = sp_sym_to_s(", tadv);
          emit_expr(c, argv[0], b);
          buf_printf(b, "); SP_GC_ROOT_STR(_t%d); ", tadv);
        }
        for (int ai = first_int; ai < argc; ai++) {
          TyKind ak = comp_ntype(c, argv[ai]);
          targ[ai] = ++g_tmp;
          if (ak == TY_INT || ak == TY_FLOAT) {
            buf_printf(b, "sp_int _t%d = ", targ[ai]);
            emit_int_expr(c, argv[ai], b);
            buf_puts(b, "; ");
          }
          else {
            theld[ai] = ++g_tmp;
            buf_printf(b, "sp_RbVal _t%d = ", theld[ai]);
            emit_boxed(c, argv[ai], b);
            buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", theld[ai]);
          }
        }
        buf_printf(b, "sp_File *_t%d = sp_poly_as_io(_t%d, \"%s\"); ", tio2, trv, name);
        emit_stat_handle_nomethod(tio2, trv, name, b);
        /* flock is File's alone: a pipe, a standard stream or a socket
           raises CRuby's NoMethodError */
        if (sp_streq(name, "flock"))
          buf_printf(b, "if (strcmp(sp_io_kind_name(_t%d), \"File\") != 0) "
                        "sp_raise_poly_nomethod(\"flock\", _t%d); ", tio2, trv);
        for (int ai = first_int; ai < argc; ai++)
          if (theld[ai])
            buf_printf(b, "sp_int _t%d = sp_poly_arg_int_chk(_t%d); ", targ[ai], theld[ai]);
        if (sp_streq(name, "pos=")) {
          buf_printf(b, "sp_File_seek(_t%d, _t%d, 0); _t%d; })", tio2, targ[0], targ[0]);
        }
        else if (sp_streq(name, "flock")) {
          buf_printf(b, "sp_File_flock(_t%d, _t%d); })", tio2, targ[0]);
        }
        else if (sp_streq(name, "advise")) {
          buf_printf(b, "sp_File_advise(_t%d, _t%d, ", tio2, tadv);
          if (argc >= 2) buf_printf(b, "_t%d", targ[1]); else buf_puts(b, "0");
          buf_puts(b, ", ");
          if (argc >= 3) buf_printf(b, "_t%d", targ[2]); else buf_puts(b, "0");
          buf_puts(b, "); sp_box_nil(); })");
        }
        else {
          buf_printf(b, "sp_File_%s(_t%d, _t%d, ", name, tio2, targ[0]);
          if (argc >= 2) buf_printf(b, "_t%d", targ[1]); else buf_puts(b, "0");
          buf_puts(b, "); })");
        }
        return 1;
      }
      /* a Dir read out of a container reaches this arm by name: path,
         to_path, read, tell (and pos), fileno and close are its own methods
         too, so a Dir answers them through the typed Dir arm's helpers, and
         anything else goes on to the IO handle */
      const char *dirfn = NULL;
      if (argc == 0) {
        if (sp_streq(name, "path") || sp_streq(name, "to_path")) dirfn = "sp_Dir_path";
        else if (sp_streq(name, "read")) dirfn = "sp_Dir_read";
        else if (sp_streq(name, "tell") || sp_streq(name, "pos")) dirfn = "sp_Dir_tell";
        else if (sp_streq(name, "fileno")) dirfn = "sp_Dir_fileno";
        else if (sp_streq(name, "close")) dirfn = "sp_Dir_close";
      }
      /* and a boxed Queue closes and answers closed? itself */
      int qname = argc == 0 && (sp_streq(name, "close") || sp_streq(name, "closed?"));
      int tdr = 0;
      if (dirfn || qname) {
        tdr = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", tdr);
        emit_boxed(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", tdr);
        if (qname) {
          buf_printf(b, "_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_QUEUE ? ", tdr, tdr);
          if (sp_streq(name, "close")) buf_printf(b, "((void)sp_Queue_close((sp_queue *)_t%d.v.p), _t%d) : ", tdr, tdr);
          else buf_printf(b, "sp_Queue_closed((sp_queue *)_t%d.v.p) : ", tdr);
        }
        if (dirfn) {
          buf_printf(b, "_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_DIR ? ", tdr, tdr);
          if (sp_streq(name, "close")) buf_printf(b, "((void)sp_Dir_close((sp_Dir *)_t%d.v.p), sp_box_nil()) : ", tdr);
          else buf_printf(b, "%s((sp_Dir *)_t%d.v.p) : ", dirfn, tdr);
        }
      }
      buf_printf(b, "({ sp_File *_t%d = sp_poly_as_io(", tio2);
      if (tdr) buf_printf(b, "_t%d", tdr);
      else emit_boxed(c, recv, b);
      buf_printf(b, ", \"%s\"); ", name);
      if (sp_streq(name, "write") && argc >= 1) {
        /* Same String/non-String split as the TY_IO arm: a String knows its own
           byte count, a stringified value may be an unmarked static name. */
        int sk2 = comp_ntype(c, argv[0]) == TY_STRING;
        buf_printf(b, "%s(_t%d, ", sk2 ? "sp_File_write_bin" : "sp_File_write", tio2);
        emit_to_s_expr(c, argv[0], b);
        buf_puts(b, "); })");
      }
      else if (sp_streq(name, "puts") || sp_streq(name, "print")) {
        /* sp_File_puts appends a newline per argument unless the argument
           already ends in one, and flattens an array argument through
           sp_File_puts_val -- the same split the TY_IO arm makes. */
        int is_puts2 = sp_streq(name, "puts");
        for (int k2 = 0; k2 < argc; k2++) {
          TyKind akt2 = comp_ntype(c, argv[k2]);
          Buf ab2; memset(&ab2, 0, sizeof ab2);
          int arr2 = 0;
          if (is_puts2 && (ty_is_array(akt2) || akt2 == TY_POLY)) { arr2 = 1; emit_boxed(c, argv[k2], &ab2); }
          else if (akt2 == TY_STRING) emit_expr(c, argv[k2], &ab2);
          else if (akt2 == TY_POLY) { arr2 = 1; emit_boxed(c, argv[k2], &ab2); }
          else { buf_puts(&ab2, "sp_poly_to_s("); emit_boxed(c, argv[k2], &ab2); buf_puts(&ab2, ")"); }
          const char *at2 = ab2.p ? ab2.p : "\"\"";
          /* the same String/non-String split the typed arm makes (#4629) */
          int bin2 = (akt2 == TY_STRING);
          if (is_puts2 && arr2) buf_printf(b, "sp_File_puts_val(_t%d, %s); ", tio2, at2);
          else if (is_puts2) {
            if (bin2) buf_printf(b, "sp_File_puts_bin(_t%d, %s); ", tio2, at2);
            else {
              int ts2 = ++g_tmp;
              buf_printf(b, "sp_File_puts(_t%d, ({ const char *_t%d = %s; _t%d ? _t%d : \"\"; })); ",
                         tio2, ts2, at2, ts2, ts2);
            }
          }
          else buf_printf(b, "%s(_t%d, %s); ",
                          arr2 ? "sp_File_write_poly" : bin2 ? "sp_File_write_bin" : "sp_File_write", tio2, at2);
          free(ab2.p);
        }
        if (is_puts2 && argc == 0) buf_printf(b, "sp_File_write(_t%d, \"\\n\"); ", tio2);
        buf_puts(b, "((sp_int)0); })");
      }
      else if (sp_streq(name, "putc") && argc == 1) {
        buf_printf(b, "sp_File_putc(_t%d, ", tio2); emit_boxed(c, argv[0], b); buf_puts(b, "); })");
      }
      else if (sp_streq(name, "eof?")) buf_printf(b, "sp_File_eof_p(_t%d); })", tio2);
      else if (sp_streq(name, "closed?")) buf_printf(b, "sp_File_closed_p(_t%d); })", tio2);
      else if (sp_streq(name, "tty?") || sp_streq(name, "isatty"))
        buf_printf(b, "sp_File_tty_p(_t%d); })", tio2);
      else if (sp_streq(name, "winsize")) buf_printf(b, "sp_File_winsize(_t%d); })", tio2);
      else if (sp_streq(name, "addr") || sp_streq(name, "peeraddr"))
        buf_printf(b, "sp_sock_addr(_t%d, %d); })", tio2, sp_streq(name, "peeraddr") ? 1 : 0);
      /* a stat's mode and fields, answered as the TY_IO arms answer them,
         for a stat's handle only */
      else if (sp_streq(name, "mode")) {
        emit_stat_handle_only(tio2, name, b);
        buf_printf(b, "sp_stat_mode(_t%d); })", tio2);
      }
      else if (boxed_stat_name(name)) {
        int k = 0;
        while (boxed_stat_sfield[k] && !sp_streq(name, boxed_stat_sfield[k])) k++;
        emit_stat_handle_only(tio2, name, b);
        buf_printf(b, "sp_stat_field(_t%d, %d); })", tio2, k);
      }
      else if (sp_streq(name, "path") || sp_streq(name, "to_path"))
        buf_printf(b, "sp_File_path(_t%d); })", tio2);
      else if (sp_streq(name, "readlines")) buf_printf(b, "sp_File_readlines(_t%d); })", tio2);
      else if (sp_streq(name, "rewind")) buf_printf(b, "sp_File_rewind(_t%d); })", tio2);
      /* a stat's predicates, answered as the TY_IO arms answer them, for a
         stat's handle only */
      else if (argc == 0 && boxed_stat_pred(name) >= 100) {
        emit_stat_handle_only(tio2, name, b);
        buf_printf(b, "sp_stat_type_pred(_t%d, %d); })", tio2, boxed_stat_pred(name) - 100);
      }
      else if (argc == 0 && boxed_stat_pred(name) >= 0) {
        emit_stat_handle_only(tio2, name, b);
        buf_printf(b, "sp_stat_pred(_t%d, %d); })", tio2, boxed_stat_pred(name));
      }
      /* the same answers the typed-receiver arms give (#2792 semantics):
         sync reads the handle kind, sync= flushes on a truthy value and
         answers it (#4229) */
      else if (sp_streq(name, "sync") && argc == 0)
        buf_printf(b, "sp_File_sync_p(_t%d); })", tio2);
      else if (sp_streq(name, "sync=") && argc >= 1) {
        /* answers its argument; only its truth sets the mode */
        int ts3 = ++g_tmp;
        buf_printf(b, "sp_RbVal _t%d = ", ts3);
        emit_boxed(c, argv[0], b);
        buf_printf(b, "; sp_File_set_sync(_t%d, sp_poly_truthy(_t%d)); ", tio2, ts3);
        emit_unbox_or_keep(c, comp_ntype(c, id), ts3, b);
        buf_puts(b, "; })");
      }
      /* read_nonblock / write_nonblock, the same answers the typed-receiver
         arms give: `exception: false` answers the wait symbol (read) or nil
         (write) instead of raising, so those shapes are poly (#4236/#4237). */
      else if ((sp_streq(name, "read_nonblock") || sp_streq(name, "write_nonblock")) &&
               argc >= 1) {
        const char *lty9 = nt_type(nt, argv[argc - 1]);
        int kwh9 = (lty9 && sp_streq(lty9, "KeywordHashNode")) ? argv[argc - 1] : -1;
        int exc9 = kwh9 >= 0 ? kwh_lookup(nt, kwh9, "exception") : -1;
        int no_exc9 = exc9 >= 0 && nt_type(nt, exc9) &&
                      sp_streq(nt_type(nt, exc9), "FalseNode");
        if (sp_streq(name, "read_nonblock")) {
          if (no_exc9) {
            int tw9 = ++g_tmp, te9 = ++g_tmp;
            buf_printf(b, "sp_bool _e%d; const char *_t%d = sp_sock_read_nb(_t%d, ", te9, tw9, tio2);
            emit_int_expr(c, argv[0], b);
            buf_printf(b, ", 0, 0, &_e%d); _t%d ? sp_box_str(_t%d)"
                          " : (_e%d ? sp_box_nil() : sp_box_sym(sp_sym_intern(\"wait_readable\"))); })",
                         te9, tw9, tw9, te9);
          }
          else {
            buf_printf(b, "sp_sock_read_nb(_t%d, ", tio2);
            emit_int_expr(c, argv[0], b);
            buf_puts(b, ", 1, 0, NULL); })");
          }
        }
        else {
          /* a String operand keeps its byte length (an embedded NUL is a byte
             of the message); anything else goes through to_s */
          int skw9 = comp_ntype(c, argv[0]) == TY_STRING;
          const char *wfn9 = skw9 ? "sp_sock_write_nb_bin" : "sp_sock_write_nb";
          int tw9 = ++g_tmp;
          buf_printf(b, "sp_int _t%d = %s(_t%d, ", tw9, wfn9, tio2);
          emit_to_s_expr(c, argv[0], b);
          if (no_exc9)
            buf_printf(b, ", 0); _t%d < 0 ? sp_box_nil() : sp_box_int(_t%d); })", tw9, tw9);
          else
            buf_printf(b, ", 1); _t%d; })", tw9);
        }
      }
      /* read(n) reads UP TO n bytes; dropping the count read to EOF, which on
         a socket with a live peer never comes -- `r.read(5)` hung forever. */
      else if (sp_streq(name, "read") && argc >= 1) {
        buf_printf(b, "sp_File_read_n(_t%d, ", tio2);
        emit_int_expr(c, argv[0], b);
        buf_puts(b, "); })");
      }
      else if (sp_streq(name, "read")) buf_printf(b, "sp_File_read(_t%d); })", tio2);
      /* the same answers the typed arms give for these names */
      else if (sp_streq(name, "stat") && argc == 0)
        buf_printf(b, "sp_io_stat_handle(_t%d); })", tio2);
      else if (sp_streq(name, "seek") && argc >= 1) {
        buf_printf(b, "sp_File_seek(_t%d, ", tio2);
        emit_int_expr(c, argv[0], b); buf_puts(b, ", ");
        if (argc >= 2) emit_int_expr(c, argv[1], b); else buf_puts(b, "0");
        buf_puts(b, "); })");
      }
      else if (sp_streq(name, "tell") || sp_streq(name, "pos"))
        buf_printf(b, "sp_File_tell(_t%d); })", tio2);
      else if (sp_streq(name, "pread") && argc >= 1) {
        buf_printf(b, "sp_File_pread(_t%d, ", tio2);
        emit_int_expr(c, argv[0], b); buf_puts(b, ", ");
        if (argc >= 2) emit_int_expr(c, argv[1], b); else buf_puts(b, "0");
        buf_puts(b, "); })");
      }
      else if (sp_streq(name, "pwrite") && argc >= 1) {
        buf_printf(b, "%s(_t%d, ", comp_ntype(c, argv[0]) == TY_STRING
                                   ? "sp_File_pwrite_bin" : "sp_File_pwrite", tio2);
        emit_to_s_expr(c, argv[0], b); buf_puts(b, ", ");
        if (argc >= 2) emit_int_expr(c, argv[1], b); else buf_puts(b, "0");
        buf_puts(b, "); })");
      }
      else if (sp_streq(name, "fsync") || sp_streq(name, "fdatasync"))
        buf_printf(b, "sp_File_fsync(_t%d); })", tio2);
      else if (sp_streq(name, "gets") && argc == 0) buf_printf(b, "sp_File_gets(_t%d); })", tio2);
      /* the separator, limit and `chomp:` a typed handle takes: dropping
         them read `f.gets("o")` and `f.gets(3)` as a whole line. The handle
         is rooted across the arguments, which are evaluated after it. */
      else if (sp_streq(name, "gets") || sp_streq(name, "readline")) {
        buf_printf(b, "SP_GC_ROOT(_t%d); sp_File_%s(_t%d, ", tio2,
                   sp_streq(name, "readline") ? "readline_sep" : "gets_sep", tio2);
        emit_gets_sep_args(c, argv, argc, b);
        buf_puts(b, "); })");
      }
      /* readpartial takes a count and an optional buffer, nothing else:
         count-less, the arm below was skipped and the fallback answered
         fileno; over-long, the extras were read and dropped. CRuby refuses
         both with ArgumentError before touching the stream. */
      else if (sp_streq(name, "readpartial") && (argc == 0 || argc > 2)) {
        buf_printf(b, "sp_raise_cls(\"ArgumentError\", \"wrong number of arguments "
                      "(given %d, expected 1..2)\"); (const char *)0; })", argc);
      }
      /* the same (len, outbuf) rebind the typed arm makes (#3336) */
      else if (sp_streq(name, "readpartial") && argc >= 1) {
        const char *sbp = NULL;
        if (argc >= 2 && nt_type(nt, argv[1]) &&
            sp_streq(nt_type(nt, argv[1]), "LocalVariableReadNode"))
          sbp = nt_str(nt, argv[1], "name");
        int tsp = ++g_tmp;
        if (argc >= 2) {
          buf_puts(b, "sp_str_check_mutable("); emit_expr(c, argv[1], b); buf_puts(b, "); ");
          buf_printf(b, "const char *_t%d = ", tsp);
        }
        buf_printf(b, "sp_File_readpartial(_t%d, ", tio2);
        emit_int_expr(c, argv[0], b);
        buf_puts(b, ")");
        if (argc >= 2) {
          if (sbp) buf_printf(b, "; lv_%s = _t%d", rename_local(sbp), tsp);
          buf_printf(b, "; _t%d", tsp);
        }
        buf_puts(b, "; })");
      }
      else if (sp_streq(name, "wait_readable") || sp_streq(name, "wait_writable") ||
               sp_streq(name, "wait_priority")) {
        char tr[32]; snprintf(tr, sizeof tr, "_t%d", tio2);
        emit_io_wait(c, name, argc, argv, tr, b);
        buf_puts(b, "; })");
      }
      else if (sp_streq(name, "close")) buf_printf(b, "(void)sp_File_close(_t%d); sp_box_nil(); })", tio2);
      else if (sp_streq(name, "flush")) buf_printf(b, "sp_File_flush(_t%d); })", tio2);
      else buf_printf(b, "sp_File_fileno(_t%d); })", tio2);
      if (tdr) buf_puts(b, "; })");
      return 1;
    }
  }
  return 0;
}

/* the instance methods of an IO / File handle (TY_IO) */
int emit_call_io_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  if (recv >= 0 && comp_ntype(c, recv) == TY_IO) {
    const char *r = NULL;
    Buf rb = {0};
    emit_expr(c, recv, &rb);
    r = rb.p ? rb.p : "NULL";
    /* metadata via the handle's path (#2790) */
    /* f.chown(uid, gid) on the handle's path; a nil id leaves it unchanged and
       the instance form evaluates to 0, not the path count (#3104) */
    if (sp_streq(name, "chown") && argc == 2) {
      buf_printf(b, "((void)sp_file_chown(sp_File_path(%s), ", r);
      for (int ci = 0; ci < 2; ci++) {
        if (ci) buf_puts(b, ", ");
        if (nt_type(nt, argv[ci]) && sp_streq(nt_type(nt, argv[ci]), "NilNode")) buf_puts(b, "-1LL");
        else emit_int_expr(c, argv[ci], b);
      }
      buf_puts(b, "), (sp_int)0)");
      free(rb.p); return 1;
    }
    /* builtin-op rows (builtin_ops.c), over the receiver rendered above */
    if (emit_builtin_op_text(c, id, recv, TY_IO, name, r, b)) { free(rb.p); return 1; }
    /* A handle's class is a runtime property (a socket kind, a path-backed
       File, a bare stream), so the ancestor walk runs on the kind rather than
       on a static class id. */
    if (argc == 1 && is_kind_query(name)) {
      char icq[192];
      const char *icn = isa_match_name(nt, argv[0], icq, sizeof icq);
      if (icn) {
        buf_printf(b, "%s(%s, \"%s\")",
                   sp_streq(name, "instance_of?") ? "sp_io_instance_of" : "sp_io_is_a", r, icn);
        free(rb.p); return 1;
      }
    }
    if ((sp_streq(name, "wait_readable") || sp_streq(name, "wait_writable") ||
         sp_streq(name, "wait_priority") || sp_streq(name, "wait")) && argc <= 2) {
      emit_io_wait(c, name, argc, argv, r, b);
      free(rb.p); return 1;
    }
    /* socket methods on the IO handle (#2922). The handle kind decides at run
       time whether the receiver actually owns them: a plain File answers
       NoMethodError, as CRuby does. */
    if (sp_feature_required("socket") && argc == 0 && sp_streq(name, "accept")) {
      buf_printf(b, "sp_sock_accept(%s)", r);
      free(rb.p); return 1;
    }
    if (sp_feature_required("socket") && argc == 0 &&
        (sp_streq(name, "addr") || sp_streq(name, "peeraddr"))) {
      buf_printf(b, "sp_sock_addr(%s, %d)", r, sp_streq(name, "peeraddr") ? 1 : 0);
      free(rb.p); return 1;
    }
    /* The non-blocking family. `exception: false` swaps the IO::*Wait*
       exception for the :wait_readable / :wait_writable marker, which changes
       the result type, so the two forms take different arms. */
    if (sp_feature_required("socket") &&
        (sp_streq(name, "accept_nonblock") || sp_streq(name, "connect_nonblock") ||
         sp_streq(name, "recv_nonblock"))) {
      const char *lty9 = argc > 0 ? nt_type(nt, argv[argc - 1]) : NULL;
      int kwh9 = (lty9 && sp_streq(lty9, "KeywordHashNode")) ? argv[argc - 1] : -1;
      int exc9 = kwh9 >= 0 ? kwh_lookup(nt, kwh9, "exception") : -1;
      int no_exc = exc9 >= 0 && nt_type(nt, exc9) && sp_streq(nt_type(nt, exc9), "FalseNode");
      int pos9 = kwh9 >= 0 ? argc - 1 : argc;
      if (sp_streq(name, "accept_nonblock") && pos9 == 0) {
        if (no_exc) {
          int tw = ++g_tmp;
          buf_printf(b, "({ sp_File *_t%d = sp_sock_accept_nb(%s, 0);"
                        " _t%d ? sp_box_obj(_t%d, SP_BUILTIN_IO)"
                        " : sp_box_sym(sp_sym_intern(\"wait_readable\")); })", tw, r, tw, tw);
        }
        else buf_printf(b, "sp_sock_accept_nb(%s, 1)", r);
        free(rb.p); return 1;
      }
      if (sp_streq(name, "recv_nonblock") && pos9 == 1) {
        if (no_exc) {
          /* No EOF arm here: recv_nonblock answers "" at EOF, which the
             runtime returns as an ordinary string, so NULL means would-block
             and nothing else. (The statement-expression's opening `({` is
             what the rest of this line closes.) */
          int tw = ++g_tmp;
          buf_printf(b, "({ const char *_t%d = sp_sock_read_nb(%s, ", tw, r);
          emit_int_expr(c, argv[0], b);
          buf_printf(b, ", 0, 1, NULL); _t%d ? sp_box_str(_t%d)"
                        " : sp_box_sym(sp_sym_intern(\"wait_readable\")); })", tw, tw);
        }
        else {
          buf_printf(b, "sp_sock_read_nb(%s, ", r); emit_int_expr(c, argv[0], b);
          buf_puts(b, ", 1, 1, NULL)");
        }
        free(rb.p); return 1;
      }
      if (sp_streq(name, "connect_nonblock") && pos9 == 1) {
        /* 1-arg form: a packed sockaddr String. Mirror the 2-arg
           shape: `exception: false` swaps the IO::WaitWritable raise
           for the :wait_writable symbol so polling loops can stay
           non-raising. */
        int ts = ++g_tmp;
        if (no_exc) {
          int tn = ++g_tmp;
          buf_printf(b, "({ const char *_t%d = ", ts);
          emit_str_expr(c, argv[0], b);
          buf_printf(b, "; sp_int _n%d = sp_sock_connect_nb_sa(%s, _t%d,", tn, r, ts);
          buf_printf(b, " (sp_int)sp_str_byte_len(_t%d), 0);", ts);
          buf_printf(b, " _n%d == SP_INT_NIL", tn);
          buf_printf(b, " ? sp_box_sym(sp_sym_intern(\"wait_writable\"))");
          buf_printf(b, " : sp_box_int(_n%d); })", tn);
        }
        else {
          buf_printf(b, "({ const char *_t%d = ", ts);
          emit_str_expr(c, argv[0], b);
          buf_printf(b, "; sp_sock_connect_nb_sa(%s, _t%d,"
                        " (sp_int)sp_str_byte_len(_t%d), 1); })",
                        r, ts, ts);
        }
        free(rb.p); return 1;
      }
      if (sp_streq(name, "connect_nonblock") && pos9 == 2) {
        if (no_exc) {
          int tw = ++g_tmp;
          buf_printf(b, "({ sp_int _t%d = sp_sock_connect_nb(%s, ", tw, r);
          emit_str_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
          buf_printf(b, ", 0); _t%d == SP_INT_NIL"
                        " ? sp_box_sym(sp_sym_intern(\"wait_writable\")) : sp_box_int(_t%d); })", tw, tw);
        }
        else {
          buf_printf(b, "sp_sock_connect_nb(%s, ", r); emit_str_expr(c, argv[0], b);
          buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ", 1)");
        }
        free(rb.p); return 1;
      }
    }
    if (sp_feature_required("socket") && argc == 0 &&
        (sp_streq(name, "local_address") || sp_streq(name, "remote_address"))) {
      buf_printf(b, "sp_sock_address(%s, %d)", r, sp_streq(name, "remote_address") ? 1 : 0);
      free(rb.p); return 1;
    }
    if (sp_feature_required("socket")) {
      /* the address-taking pair; UDPSocket#send's 3-arg form carries the
         destination, the 2-arg form goes to the connected peer */
      if ((sp_streq(name, "bind") || sp_streq(name, "connect")) && argc == 2) {
        buf_printf(b, "sp_sock_%s(%s, ", name, r);
        emit_str_expr(c, argv[0], b); buf_puts(b, ", ");
        emit_int_expr(c, argv[1], b); buf_puts(b, ")");
        free(rb.p); return 1;
      }
      if (sp_streq(name, "send") && (argc == 2 || argc == 4)) {
        int tsd = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = ", tsd); emit_str_expr(c, argv[0], b);
        buf_printf(b, "; sp_sock_send(%s, _t%d, (sp_int)sp_str_byte_len(_t%d), ", r, tsd, tsd);
        if (argc == 4) { emit_str_expr(c, argv[2], b); buf_puts(b, ", "); emit_int_expr(c, argv[3], b); }
        else buf_puts(b, "NULL, 0");
        buf_puts(b, "); })");
        free(rb.p); return 1;
      }
      if (sp_streq(name, "recv") && argc == 1) {
        buf_printf(b, "sp_sock_recv(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
        free(rb.p); return 1;
      }
      /* recvfrom -> [payload, ["AF_INET", port, ip, ip]] */
      if (sp_streq(name, "recvfrom") && argc == 1) {
        int tp = ++g_tmp, ti = ++g_tmp, to = ++g_tmp, ta = ++g_tmp, tw = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = NULL; sp_int _t%d = 0;"
                      " const char *_t%d = sp_sock_recvfrom(%s, ", ti, tp, to, r);
        emit_int_expr(c, argv[0], b);
        buf_printf(b, ", &_t%d, &_t%d);"
                      " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_str(strchr(_t%d, ':') ? SPL(\"AF_INET6\") : SPL(\"AF_INET\")));"
                      " sp_PolyArray_push(_t%d, sp_box_int(_t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_str(_t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_str(_t%d));"
                      " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_str(_t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d)); _t%d; })",
                   ti, tp, ta, ta, ta, ti, ta, tp, ta, ti, ta, ti, tw, tw, tw, to, tw, ta, tw);
        free(rb.p); return 1;
      }
      if (sp_streq(name, "shutdown") && argc <= 1) {
        buf_printf(b, "sp_sock_shutdown(%s, ", r);
        if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "2");
        buf_puts(b, ")");
        free(rb.p); return 1;
      }
      if (sp_streq(name, "listen") && argc == 1) {
        buf_printf(b, "sp_sock_listen(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
        free(rb.p); return 1;
      }
      if (sp_streq(name, "setsockopt") && argc == 3) {
        buf_printf(b, "sp_sock_setsockopt(%s, ", r);
        emit_int_expr(c, argv[0], b); buf_puts(b, ", ");
        emit_int_expr(c, argv[1], b); buf_puts(b, ", ");
        emit_int_expr(c, argv[2], b); buf_puts(b, ")");
        free(rb.p); return 1;
      }
      if (sp_streq(name, "getsockopt") && argc == 2) {
        buf_printf(b, "sp_sock_getsockopt(%s, ", r);
        emit_int_expr(c, argv[0], b); buf_puts(b, ", ");
        emit_int_expr(c, argv[1], b); buf_puts(b, ")");
        free(rb.p); return 1;
      }
    }
    if (sp_streq(name, "read")) {
      if (argc >= 2 && nt_type(nt, argv[1]) &&
               sp_streq(nt_type(nt, argv[1]), "LocalVariableReadNode")) {
        /* read(len, buffer): rebind the buffer local to the bytes read (#2811) */
        const char *bnm = nt_str(nt, argv[1], "name");
        int trd = ++g_tmp;
        /* CRuby raises FrozenError on the output buffer BEFORE reading (#3335) */
        buf_puts(b, "({ sp_str_check_mutable("); emit_expr(c, argv[1], b); buf_puts(b, "); ");
        buf_printf(b, "const char *_t%d = sp_File_read_n(%s, ", trd, r);
        emit_int_expr(c, argv[0], b);
        buf_printf(b, "); lv_%s = _t%d; _t%d; })", bnm ? rename_local(bnm) : "?", trd, trd);
      }
      else {
        /* read(nil) is read with no length: the rest of the stream */
        TyKind lt0 = comp_ntype(c, argv[0]);
        if (lt0 == TY_NIL) {
          buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
          buf_printf(b, "); sp_File_read(%s); })", r);
        }
        else if (lt0 == TY_POLY) {
          int tl = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", tl); emit_boxed(c, argv[0], b);
          buf_printf(b, "; _t%d.tag == SP_TAG_NIL ? sp_File_read(%s) : sp_File_read_n(%s, sp_poly_arg_int_chk(_t%d)); })",
                     tl, r, r, tl);
        }
        else { buf_puts(b, "sp_File_read_n("); buf_puts(b, r); buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      }
      free(rb.p); return 1;
    }
    if (sp_streq(name, "gets") || sp_streq(name, "readline")) {
      /* readline raises EOFError at end of file (#2817) */
      int is_rdl = sp_streq(name, "readline");
      buf_printf(b, "sp_File_%s(%s, ", is_rdl ? "readline_sep" : "gets_sep", r);
      emit_gets_sep_args(c, argv, argc, b);
      buf_puts(b, ")");
      free(rb.p); return 1;
    }
    if ((sp_streq(name, "close_on_exec=") || sp_streq(name, "autoclose=")) && argc == 1) {
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_bool _t%d = ", tv); emit_cond(c, argv[0], b);
      buf_printf(b, "; ");
      /* the GC-owned handle closes at collection either way; the flag only
         has to drive #autoclose? (#3131) */
      if (sp_streq(name, "close_on_exec="))
        buf_printf(b, "sp_File_set_close_on_exec(%s, _t%d); ", r, tv);
      else buf_printf(b, "sp_File_set_autoclose(%s, _t%d); ", r, tv);
      buf_printf(b, "_t%d; })", tv);
      free(rb.p); return 1;
    }
    if (sp_streq(name, "pread") && argc >= 1) {
      /* pread(len, off, buf): CRuby fills the buffer argument; when it is a
         plain local, rebind it to the read result (#3131) */
      const char *bufn = NULL;
      if (argc >= 3 && nt_type(nt, argv[2]) &&
          sp_streq(nt_type(nt, argv[2]), "LocalVariableReadNode"))
        bufn = nt_str(nt, argv[2], "name");
      int tpr = ++g_tmp;
      if (bufn) {
        /* the output buffer must be mutable, checked before the read (#3335) */
        buf_puts(b, "({ sp_str_check_mutable("); emit_expr(c, argv[2], b); buf_puts(b, "); ");
        buf_printf(b, "const char *_t%d = ", tpr);
      }
      buf_printf(b, "sp_File_pread(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ", ");
      if (argc >= 2) emit_int_expr(c, argv[1], b); else buf_puts(b, "0");
      buf_puts(b, ")");
      if (bufn) buf_printf(b, "; lv_%s = _t%d; _t%d; })", rename_local(bufn), tpr, tpr);
      free(rb.p); return 1;
    }
    if (sp_streq(name, "pwrite") && argc >= 1) {
      /* the same String/non-String split the write arm makes: a String knows
         its own byte count, so an embedded NUL reaches the descriptor (#4623) */
      buf_printf(b, "%s(%s, ", comp_ntype(c, argv[0]) == TY_STRING
                               ? "sp_File_pwrite_bin" : "sp_File_pwrite", r);
      emit_to_s_expr(c, argv[0], b); buf_puts(b, ", ");
      if (argc >= 2) emit_int_expr(c, argv[1], b); else buf_puts(b, "0");
      buf_puts(b, ")"); free(rb.p); return 1;
    }
    if (sp_streq(name, "reopen") && argc >= 1) {
      buf_printf(b, "sp_File_reopen(%s, ", r); emit_path_expr(c, argv[0], b); buf_puts(b, ", ");
      if (argc >= 2) emit_str_expr(c, argv[1], b); else buf_puts(b, "\"r\"");
      buf_puts(b, ")"); free(rb.p); return 1;
    }
    /* read_nonblock / write_nonblock: a real non-blocking try. `exception:
       false` answers nil instead of raising IO::*Wait* / EOFError. */
    if ((sp_streq(name, "read_nonblock") || sp_streq(name, "write_nonblock")) && argc >= 1) {
      const char *lty8 = nt_type(nt, argv[argc - 1]);
      int kwh8 = (lty8 && sp_streq(lty8, "KeywordHashNode")) ? argv[argc - 1] : -1;
      int exc8 = kwh8 >= 0 ? kwh_lookup(nt, kwh8, "exception") : -1;
      int no_exc8 = exc8 >= 0 && nt_type(nt, exc8) && sp_streq(nt_type(nt, exc8), "FalseNode");
      if (sp_streq(name, "read_nonblock")) {
        if (no_exc8) {
          int tw = ++g_tmp, te = ++g_tmp;
          buf_printf(b, "({ sp_bool _e%d; const char *_t%d = sp_sock_read_nb(%s, ", te, tw, r);
          emit_int_expr(c, argv[0], b);
          buf_printf(b, ", 0, 0, &_e%d); _t%d ? sp_box_str(_t%d)"
                        " : (_e%d ? sp_box_nil() : sp_box_sym(sp_sym_intern(\"wait_readable\"))); })",
                     te, tw, tw, te);
        }
        else {
          buf_printf(b, "sp_sock_read_nb(%s, ", r); emit_int_expr(c, argv[0], b);
          buf_puts(b, ", 1, 0, NULL)");
        }
      }
      else {
        /* String operand -> the _bin entry (header length, so an embedded NUL
           is written); anything else reaches sp_poly_to_s, whose static
           class/symbol names carry no marker byte. See sp_File_write above. */
        int skw = comp_ntype(c, argv[0]) == TY_STRING;
        const char *wfn = skw ? "sp_sock_write_nb_bin" : "sp_sock_write_nb";
        if (no_exc8) {
          int tw = ++g_tmp;
          buf_printf(b, "({ sp_int _t%d = %s(%s, ", tw, wfn, r);
          emit_to_s_expr(c, argv[0], b);
          buf_printf(b, ", 0); _t%d == SP_INT_NIL"
                        " ? sp_box_sym(sp_sym_intern(\"wait_writable\")) : sp_box_int(_t%d); })", tw, tw);
        }
        else {
          buf_printf(b, "%s(%s, ", wfn, r); emit_to_s_expr(c, argv[0], b);
          buf_puts(b, ", 1)");
        }
      }
      free(rb.p); return 1;
    }
    if ((sp_streq(name, "readpartial") || sp_streq(name, "sysread")) && argc >= 1) {
      /* (len, outbuf): CRuby fills the buffer and RETURNS it; when the buffer
         is a plain local, rebind it to the bytes read so the caller sees them
         and `r.equal?(b)` holds (#3336). A frozen buffer raises first. */
      const char *sbn = NULL;
      if (argc >= 2 && nt_type(nt, argv[1]) &&
          sp_streq(nt_type(nt, argv[1]), "LocalVariableReadNode"))
        sbn = nt_str(nt, argv[1], "name");
      int tsr = ++g_tmp;
      if (argc >= 2) {
        buf_puts(b, "({ sp_str_check_mutable("); emit_expr(c, argv[1], b); buf_puts(b, "); ");
        buf_printf(b, "const char *_t%d = ", tsr);
      }
      buf_printf(b, "sp_File_readpartial(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      if (argc >= 2) {
        if (sbn) buf_printf(b, "; lv_%s = _t%d", rename_local(sbn), tsr);
        buf_printf(b, "; _t%d; })", tsr);
      }
      free(rb.p); return 1;
    }
    if (sp_streq(name, "printf") && argc >= 1) {
      /* format into a string, then write it (#2796) */
      int tfp = ++g_tmp, tfa = ++g_tmp;
      /* the format goes in a String slot, and is emitted before its
         declaration is opened, as in Kernel#format */
      Buf ffb; memset(&ffb, 0, sizeof ffb);
      emit_str_expr(c, argv[0], &ffb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "const char *_t%d = %s; SP_GC_ROOT_STR(_t%d);\n", tfp, ffb.p ? ffb.p : "\"\"", tfp);
      free(ffb.p);
      emit_pre_format_args(c, argv, argc, tfa);
      /* the formatted value is a spinel String, so it sizes by its header */
      buf_printf(b, "({ sp_File_write_bin(%s, sp_str_format_polyarr(_t%d, _t%d)); sp_box_nil(); })", r, tfp, tfa);
      free(rb.p); return 1;
    }
    /* each_char / each_byte (#2794) / each_codepoint (#3038) */
    if ((sp_streq(name, "each_char") || sp_streq(name, "each_byte") ||
         sp_streq(name, "each_codepoint")) &&
        nt_ref(nt, id, "block") >= 0) {
      int blk2 = nt_ref(nt, id, "block");
      const char *bp2 = block_param_name(c, blk2, 0);
      const char *bpn2 = bp2 ? rename_local(bp2) : NULL;
      int bdy2 = nt_ref(nt, blk2, "body");
      int bbn2 = 0; const int *bbb2 = bdy2 >= 0 ? nt_arr(nt, bdy2, "body", &bbn2) : NULL;
      int is_byte = sp_streq(name, "each_byte");
      int is_cp = sp_streq(name, "each_codepoint");
      int rf2 = ++g_tmp, lt2 = ++g_tmp;
      buf_puts(b, "({ ");
      buf_printf(b, "sp_File *_t%d = %s; SP_GC_ROOT(_t%d); ", rf2, r, rf2);
      if (is_byte)
        buf_printf(b, "sp_int _t%d; while ((_t%d = sp_File_getbyte(_t%d)) != SP_INT_NIL) {", lt2, lt2, rf2);
      else if (is_cp)
        /* each_codepoint yields the ordinal of each character, so read a
           whole UTF-8 character and decode it (#3038) */
        buf_printf(b, "const char *_t%dc; sp_int _t%d; while ((_t%dc = sp_File_getc(_t%d)) != NULL"
                      " && (_t%d = sp_str_ord(_t%dc), 1)) {", lt2, lt2, lt2, rf2, lt2, lt2);
      else
        /* the character is a fresh string, rooted -- slot and block parameter
           -- like the line in each_line below. each_byte and each_codepoint
           hand the block an sp_int instead; each_codepoint's own string is
           read by the loop condition and never again, so it needs no root */
        buf_printf(b, "const char *_t%d = NULL; SP_GC_ROOT_STR(_t%d);"
                      " while ((_t%d = sp_File_getc(_t%d)) != NULL) {", lt2, lt2, lt2, rf2);
      if (bpn2 && file_block_param_poly(c, id, bpn2))
        buf_printf(b, " sp_RbVal lv_%s = %s(_t%d); SP_GC_ROOT_RBVAL(lv_%s);", bpn2,
                   (is_byte || is_cp) ? "sp_box_int" : "sp_box_str", lt2, bpn2);
      else if (bpn2) {
        buf_printf(b, " %s lv_%s = _t%d;", (is_byte || is_cp) ? "sp_int" : "const char *", bpn2, lt2);
        if (!is_byte && !is_cp) buf_printf(b, " SP_GC_ROOT_STR(lv_%s);", bpn2);
      }
      for (int k = 0; k < bbn2; k++) emit_stmt(c, bbb2[k], b, 0);
      buf_printf(b, " } (sp_File *)_t%d; })", rf2);
      free(rb.p); return 1;
    }
    if (sp_streq(name, "readlines")) {
      int rsep = -1;
      Buf rchomp; memset(&rchomp, 0, sizeof rchomp);
      for (int k = 0; k < argc; k++) {
        const char *kty = nt_type(nt, argv[k]);
        if (kty && sp_streq(kty, "KeywordHashNode")) {
          int cv = struct_kwarg_value(c, argv[k], "chomp");
          emit_kw_flag(c, cv, &rchomp);
        }
        else rsep = argv[k];
      }
      if (rsep < 0 && (!rchomp.p || sp_streq(rchomp.p, "0"))) buf_printf(b, "sp_File_readlines(%s)", r);
      else {
        buf_printf(b, "sp_File_readlines_sep(%s, ", r);
        if (rsep >= 0) emit_expr(c, rsep, b); else buf_puts(b, "\"\\n\"");
        buf_printf(b, ", %s)", rchomp.p ? rchomp.p : "0");
      }
      free(rb.p); return 1;
    }
    if (sp_streq(name, "write") || sp_streq(name, "syswrite")) {
      /* every argument writes in order; the return is the total byte count (#2814) */
      /* A String operand goes to the _bin entry, which sizes it with the
         header length so an embedded NUL is written rather than truncating
         the write. The sp_poly_to_s arm keeps the plain entry: it can answer
         a static class/symbol name with no marker byte, which _bin would read
         out of bounds at s[-1]. syswrite is unbuffered, so it routes through
         sp_File_syswrite (which writes straight to the descriptor) rather
         than through the stdio-backed write entries. */
      int is_sw = sp_streq(name, "syswrite");
      if (argc == 1) {
        /* An operand whose class is only known at run time picks the entry by
           its TAG, not by its static type: chosen statically it took the plain
           entry and an embedded NUL truncated the write. syswrite sizes the
           operand itself (sp_str_byte_len for a String source, strlen for a
           converted value) and hands both pointer and count to
           sp_File_syswrite, so a poly operand takes that path here rather
           than being routed through sp_File_write_poly. */
        if (is_sw && comp_ntype(c, argv[0]) == TY_POLY) {
          int sl = ++g_tmp;
          /* The operand's class is only known at run time, so the length is
             chosen by its TAG rather than off the pointer: a marked String
             keeps its header length (binary-safe, embedded NULs survive),
             anything else is a NUL-terminated C string from sp_poly_to_s
             and is strlen'd. emit_boxed keeps the tag alongside the
             pointer, so the check is exact -- a marker-byte probe cannot
             tell a managed String from an unmarked conversion result. */
          buf_printf(b, "({ sp_RbVal _t%d = ", sl);
          emit_boxed(c, argv[0], b);
          buf_printf(b, "; const char *_s%d = (_t%d.tag == SP_TAG_STR) ? _t%d.v.s : sp_poly_to_s(_t%d); "
                     "sp_int _l%d = (_t%d.tag == SP_TAG_STR) ? "
                     "sp_str_byte_len(_s%d) : strlen(_s%d); "
                     "sp_int _r%d = sp_File_syswrite(%s, _s%d, _l%d); _r%d; })",
                     sl, sl, sl, sl, sl, sl, sl, sl, sl, r, sl, sl, sl);
        }
        else if (comp_ntype(c, argv[0]) == TY_POLY) {
          buf_printf(b, "sp_File_write_poly(%s, ", r);
          emit_boxed(c, argv[0], b);
          buf_puts(b, ")");
        }
        else {
          int sk = comp_ntype(c, argv[0]) == TY_STRING;
          const char *wfn = sk ? "sp_File_write_bin" : "sp_File_write";
          if (is_sw) {
            /* syswrite needs the byte length alongside the pointer:
               sp_str_byte_len for a String source (embedded NUL survives),
               strlen for a converted value -- sp_poly_to_s always returns
               a NUL-terminated C string, so its length is the C-string
               length and the GC reclaims the conversion storage. */
            int sl = ++g_tmp;
            buf_printf(b, "({ const char *_s%d = ", sl);
            emit_to_s_expr(c, argv[0], b);
            buf_printf(b, "; sp_int _l%d = ", sl);
            if (sk) buf_printf(b, "sp_str_byte_len(_s%d)", sl);
            else    buf_printf(b, "strlen(_s%d)", sl);
            buf_printf(b, "; sp_int _r%d = sp_File_syswrite(%s, _s%d, _l%d); _r%d; })",
                       sl, r, sl, sl, sl);
          }
          else {
            buf_printf(b, "%s(%s, ", wfn, r);
            emit_to_s_expr(c, argv[0], b);
            buf_puts(b, ")");
          }
        }
      }
      else if (argc >= 2) {
        int tw = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = 0;", tw);
        /* each operand converts right before its own write, as CRuby
           interleaves them, so each write is its own held unit rather than
           the call's */
        for (int k = 0; k < argc; k++) {
          if (is_sw && comp_ntype(c, argv[k]) == TY_POLY) {
            /* syswrite sizes the operand itself and routes through
               sp_File_syswrite, so a poly operand takes that path here
               rather than being routed through sp_File_write_poly. */
            buf_printf(b, " _t%d += ({ const char *_s = ", tw);
            emit_to_s_expr(c, argv[k], b);
            buf_printf(b, "; sp_int _l = strlen(_s); sp_int _r = "
                       "sp_File_syswrite(%s, _s, _l); _r; })", r);
            continue;
          }
          if (comp_ntype(c, argv[k]) == TY_POLY) {   /* see the one-argument form */
            buf_printf(b, " _t%d += sp_File_write_poly(%s, ", tw, r);
            emit_boxed(c, argv[k], b);
            buf_puts(b, ");");
            continue;
          }
          int sk = comp_ntype(c, argv[k]) == TY_STRING;
          ConvHold hk; memset(&hk, 0, sizeof hk);
          ConvHold *outer = g_conv_hold; g_conv_hold = &hk;
          Buf conv; memset(&conv, 0, sizeof conv);
          emit_to_s_expr(c, argv[k], &conv);
          g_conv_hold = outer;
          buf_printf(b, " _t%d += ", tw);
          if (hk.n > 0) { buf_puts(b, "({ "); buf_puts(b, hk.b.p); }
          if (is_sw) {
            /* syswrite needs the byte length: sp_str_byte_len for a String
               source (embedded NUL survives), strlen for a converted value. */
            buf_printf(b, "({ const char *_s = %s; sp_int _l = %s; "
                       "_t%d += sp_File_syswrite(%s, _s, _l); })",
                       conv.p ? conv.p : "",
                       sk ? "sp_str_byte_len(_s)" : "strlen(_s)",
                       tw, r);
          }
          else
            buf_printf(b, "%s(%s, %s)", sk ? "sp_File_write_bin" : "sp_File_write", r, conv.p ? conv.p : "");
          if (hk.n > 0) buf_puts(b, "; })");
          buf_puts(b, ";");
          free(conv.p); free(hk.b.p); free(hk.tmp);
        }
        buf_printf(b, " _t%d; })", tw);
      }
      else buf_puts(b, "0");
      free(rb.p); return 1;
    }
    if (sp_streq(name, "<<") && argc == 1) {
      /* IO#<< writes the (stringified) operand and returns self, so it chains
         (`io << a << b`). Hold the handle in a temp, write, yield the handle. */
      int t = ++g_tmp;
      int sk = comp_ntype(c, argv[0]) == TY_STRING;
      int pk = comp_ntype(c, argv[0]) == TY_POLY;
      buf_printf(b, "({ sp_File *_t%d = %s; %s(_t%d, ", t, r,
                 pk ? "sp_File_write_poly" : sk ? "sp_File_write_bin" : "sp_File_write", t);
      if (pk) emit_boxed(c, argv[0], b);
      else emit_to_s_expr(c, argv[0], b);
      buf_printf(b, "); _t%d; })", t);
      free(rb.p); return 1;
    }
    if (sp_streq(name, "winsize") && sp_feature_enabled("io/console")) {
      buf_printf(b, "sp_File_winsize(%s)", r); free(rb.p); return 1;
    }
    if (sp_streq(name, "print") || sp_streq(name, "puts")) {
      /* emit as a statement-like expression: print each arg, return nil.
         Non-string args are stringified via sp_poly_to_s (sp_File_write wants
         a char *), matching Kernel#puts/#print coercion.

         Render each arg into a local buffer first: a compound arg
         (format/sprintf/join, array/hash literal) pushes its temp decls to
         g_pre, which must land as whole statements before this block, not
         inside the half-built sp_File_write(...) call. Same class of bug as
         the format/sprintf codegen below (#1498 / #1508). */
      Buf *abs = (Buf *)calloc(argc > 0 ? (size_t)argc : 1, sizeof(Buf));
      int *aarr = (int *)calloc(argc > 0 ? (size_t)argc : 1, sizeof(int));
      /* a String operand carries its own byte count, so the write can be the
         binary-safe entry and an embedded NUL survives (#4629) */
      int *abin = (int *)calloc(argc > 0 ? (size_t)argc : 1, sizeof(int));
      for (int k = 0; k < argc; k++) {
        TyKind akt = comp_ntype(c, argv[k]);
        abin[k] = (akt == TY_STRING);
        if (sp_streq(name, "puts") && (ty_is_array(akt) || akt == TY_POLY_ARRAY || akt == TY_POLY)) {
          /* an Array argument prints one element per line (#2813) */
          aarr[k] = 1;
          emit_boxed(c, argv[k], &abs[k]);
        }
        else if (akt == TY_STRING) emit_expr(c, argv[k], &abs[k]);
        /* a boxed operand is written by its runtime tag: a String keeps its
           byte count, as the write arm does */
        else if (akt == TY_POLY) { aarr[k] = 1; emit_boxed(c, argv[k], &abs[k]); }
        else { buf_puts(&abs[k], "sp_poly_to_s("); emit_boxed(c, argv[k], &abs[k]); buf_puts(&abs[k], ")"); }
      }
      /* puts uses sp_File_puts, which appends a newline per argument (and only
         when the arg isn't already newline-terminated), matching CRuby's
         `puts a, b` -> "a\nb\n" and `puts "x\n"` -> "x\n". A nullable string
         arg can be NULL (nil); sp_File_puts is a no-op on NULL, so coalesce it
         to "" first so `puts nil` still prints the blank line. print writes the
         raw arg (a NULL write is correctly a no-op). Array flattening
         (puts [1,2] -> one line each) is not yet modelled here -- a non-string
         arg is stringified via sp_poly_to_s above. */
      int is_puts = sp_streq(name, "puts");
      /* An empty `print` (argc == 0, not puts) would emit an empty `({ })`,
         which is not a valid statement-expression; skip the block entirely. */
      if (argc > 0 || is_puts) {
        emit_indent(g_pre, g_indent);
        buf_puts(g_pre, "({ ");
        for (int k = 0; k < argc; k++) {
          const char *at = abs[k].p ? abs[k].p : "\"\"";
          if (is_puts && aarr[k])
            buf_printf(g_pre, "sp_File_puts_val(%s, %s); ", r, at);
          else if (is_puts) {
            /* a nil String is a NULL, which the binary entry answers with the
               bare newline itself; the "" stand-in has no header to read */
            if (abin[k]) buf_printf(g_pre, "sp_File_puts_bin(%s, %s); ", r, at);
            else {
              int ts = ++g_tmp;
              buf_printf(g_pre, "sp_File_puts(%s, ({ const char *_t%d = %s; _t%d ? _t%d : \"\"; })); ",
                         r, ts, at, ts, ts);
            }
          }
          else buf_printf(g_pre, "%s(%s, %s); ",
                          aarr[k] ? "sp_File_write_poly" : abin[k] ? "sp_File_write_bin" : "sp_File_write", r, at);
          free(abs[k].p);
        }
        if (is_puts && argc == 0) buf_printf(g_pre, "sp_File_write(%s, \"\\n\"); ", r);
        buf_puts(g_pre, "});\n");
      }
      else for (int k = 0; k < argc; k++) free(abs[k].p);
      free(abs);
      free(aarr);
      free(abin);
      buf_puts(b, "((sp_int)0)");
      free(rb.p); return 1;
    }
    if (sp_streq(name, "sync=") && argc >= 1) {
      /* answers its argument; only its truth sets the mode */
      int ts2 = ++g_tmp;
      buf_puts(b, "({ sp_RbVal ");
      buf_printf(b, "_t%d = ", ts2); emit_boxed(c, argv[0], b);
      buf_printf(b, "; sp_File_set_sync(%s, sp_poly_truthy(_t%d)); ", r, ts2);
      emit_unbox_or_keep(c, comp_ntype(c, id), ts2, b);
      buf_puts(b, "; })");
      free(rb.p); return 1;
    }
    if ((sp_streq(name, "each_line") || sp_streq(name, "each")) &&
        nt_ref(nt, id, "block") >= 0) {
      int blk = nt_ref(nt, id, "block");
      const char *bp = block_param_name(c, blk, 0);
      const char *bpn = bp ? rename_local(bp) : NULL;
      int bdy = nt_ref(nt, blk, "body");
      int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
      int lt = ++g_tmp, rf = ++g_tmp;
      buf_puts(b, "({ ");
      /* rooted as the File.open block form roots its handle (below): a
         temporary receiver -- File.open(p).each_line -- is otherwise swept by
         a GC inside the body and the loop ends early */
      buf_printf(b, "sp_File *_t%d = %s; SP_GC_ROOT(_t%d); ", rf, r, rf);
      free(rb.p); r = NULL;
      /* Each iteration yields a FRESH heap line string, matching CRuby --
         a stored reference must keep its own line, not a mutated shared
         buffer (#2803). That string is held in this C temporary and nowhere
         else, so the slot is rooted for the block: a block that allocates
         otherwise collects the line it was just handed and reads it back
         empty. NULL first, because the mark walker reads the slot as it
         stands. The block's own parameter is rooted for the same reason
         emit_hash_filter_loop roots its key and value (codegen_iter.c): a
         block that REBINDS it -- line = line.upcase -- holds the new string
         in that slot alone. */
      /* the separator, the limit and `chomp:` (#2810) are evaluated once,
         ahead of the loop, as CRuby evaluates them */
      int ls = ++g_tmp, ll = ++g_tmp, lc = ++g_tmp;
      if (io_line_args_spread(nt, argv, argc)) {
        /* a splat or a `**` carries them: the positional ones are gathered
           into a list and read as CRuby reads them (sp_io_line_args), and
           `chomp:` is looked up in the call's keywords merged in order */
        int tpa = ++g_tmp, kwh = -1;
        buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", tpa, tpa);
        if (argc > 0 && nt_kind(nt, argv[argc - 1]) == NK_KeywordHashNode) kwh = argv[argc - 1];
        emit_push_arg_list(c, argv, kwh >= 0 ? argc - 1 : argc, tpa, b);
        /* every argument is evaluated before any of them is checked */
        int th = ++g_tmp, any = kwh >= 0 ? poly_kw_any_key(c, kwh) : 0;
        if (kwh >= 0) emit_poly_kw_all(c, kwh, th, any, 1, b);
        buf_printf(b, "const char *_t%d = NULL; SP_GC_ROOT_STR(_t%d); sp_int _t%d = 0; "
                      "sp_io_line_args(_t%d, &_t%d, &_t%d); sp_bool _t%d = 0; ",
                   ls, ls, ll, tpa, ls, ll, lc);
        if (kwh >= 0) {
          int chs = comp_sym_intern(c, "chomp");
          if (any)
            buf_printf(b, "if (sp_PolyPolyHash_has_key(_t%d, sp_box_sym((sp_sym)%d))) "
                          "_t%d = sp_poly_truthy(sp_PolyPolyHash_get(_t%d, sp_box_sym((sp_sym)%d))); ",
                       th, chs, lc, th, chs);
          else
            buf_printf(b, "if (sp_SymPolyHash_has_key(_t%d, (sp_sym)%d)) "
                          "_t%d = sp_poly_truthy(sp_SymPolyHash_get(_t%d, (sp_sym)%d)); ",
                       th, chs, lc, th, chs);
        }
      }
      else {
        Buf esep, elim, echomp; memset(&esep, 0, sizeof esep); memset(&elim, 0, sizeof elim); memset(&echomp, 0, sizeof echomp);
        gets_sep_arg_texts(c, argv, argc, 1, &esep, &elim, &echomp);
        buf_printf(b, "const char *_t%d = %s; SP_GC_ROOT_STR(_t%d); sp_int _t%d = %s; sp_bool _t%d = %s;",
                   ls, esep.p, ls, ll, elim.p, lc, echomp.p);
        free(esep.p); free(elim.p); free(echomp.p);
        /* an Integer argument is the limit (gets_sep_arg_texts), and 0 is
           CRuby's ArgumentError, where sp_File_gets_sep reads it as none */
        for (int k = 0; k < argc; k++)
          if (comp_ntype(c, argv[k]) == TY_INT) {
            buf_printf(b, " if (_t%d == 0) sp_raise_cls(\"ArgumentError\", \"invalid limit: 0 for each_line\");", ll);
            break;
          }
      }
      buf_printf(b, "const char *_t%d = NULL; SP_GC_ROOT_STR(_t%d);"
                    " while ((_t%d = sp_File_gets_sep(_t%d, _t%d, _t%d, _t%d)) != NULL) {",
                 lt, lt, lt, rf, ls, ll, lc);
      if (bpn && file_block_param_poly(c, id, bpn))
        buf_printf(b, " sp_RbVal lv_%s = sp_box_str(_t%d); SP_GC_ROOT_RBVAL(lv_%s);", bpn, lt, bpn);
      else if (bpn) buf_printf(b, " const char *lv_%s = _t%d; SP_GC_ROOT_STR(lv_%s);", bpn, lt, bpn);
      for (int k = 0; k < bbn; k++) emit_stmt(c, bbb[k], b, 0);
      buf_printf(b, " } (sp_File *)_t%d; })", rf);
      return 1;
    }
    free(rb.p);
  }
  return 0;
}

/* ARGF's pseudo-IO methods, the Socket::Option and Addrinfo readers, and a Dir handle's methods */
int emit_call_handle_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* ARGF pseudo-IO methods: read the ARGV files (or stdin) in sequence. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_ARGF) {
    if (sp_streq(name, "read")) { buf_puts(b, "sp_argf_read()"); return 1; }
    /* `chomp:` read as IO#gets reads it (emit_gets_sep_args): the flag
       first, then the line, whose ending "\n" or "\r\n" goes when the flag
       holds (a last line without "\n" keeps its "\r") */
    int chomp_kw = argc == 1 && nt_kind(nt, argv[0]) == NK_KeywordHashNode
                   ? struct_kwarg_value(c, argv[0], "chomp") : -1;
    if ((sp_streq(name, "gets") || sp_streq(name, "readline")) && chomp_kw >= 0) {
      int tf = ++g_tmp, tl = ++g_tmp;
      buf_printf(b, "({ int _t%d = ", tf);
      emit_kw_flag(c, chomp_kw, b);
      buf_printf(b, "; const char *_t%d = sp_argf_gets(); SP_GC_ROOT_STR(_t%d);"
                    " _t%d && _t%d && sp_str_byte_len(_t%d) > 0 && _t%d[sp_str_byte_len(_t%d) - 1] == '\\n'"
                    " ? sp_str_chomp(_t%d) : _t%d; })", tl, tl, tl, tf, tl, tl, tl, tl, tl);
      return 1;
    }
    if (sp_streq(name, "gets") || sp_streq(name, "readline")) { buf_puts(b, "sp_argf_gets()"); return 1; }
    if (sp_streq(name, "readlines") || sp_streq(name, "to_a")) { buf_puts(b, "sp_argf_readlines()"); return 1; }
    if (sp_streq(name, "filename") || sp_streq(name, "path")) { buf_puts(b, "sp_argf_filename()"); return 1; }
    if (sp_streq(name, "eof?") || sp_streq(name, "eof")) { buf_puts(b, "sp_argf_eof()"); return 1; }
    if (sp_streq(name, "to_s")) { buf_puts(b, "SPL(\"ARGF\")"); return 1; }
    if ((sp_streq(name, "each_line") || sp_streq(name, "each_string") || sp_streq(name, "each")) &&
        nt_ref(nt, id, "block") >= 0) {
      int blk = nt_ref(nt, id, "block");
      const char *bp = block_param_name(c, blk, 0);
      const char *bpn = bp ? rename_local(bp) : NULL;
      int bdy = nt_ref(nt, blk, "body");
      int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
      int lt = ++g_tmp;
      buf_puts(b, "({ ");
      /* the line ARGF yields is a fresh string held in this C temporary and
         nowhere else: root the slot, and the block's parameter with it, the
         way IO#each_line below does */
      buf_printf(b, "const char *_t%d = NULL; SP_GC_ROOT_STR(_t%d);"
                    " while ((_t%d = sp_argf_gets()) != NULL) {", lt, lt, lt);
      if (bpn) buf_printf(b, " const char *lv_%s = _t%d; SP_GC_ROOT_STR(lv_%s);", bpn, lt, bpn);
      for (int k = 0; k < bbn; k++) emit_stmt(c, bbb[k], b, 0);
      buf_puts(b, " } (&sp_argf_obj); })");
      return 1;
    }
  }

  /* Socket::Option and Addrinfo readers: builtin-op rows (builtin_ops.c) */
  if (recv >= 0 && (comp_ntype(c, recv) == TY_SOCKOPT || comp_ntype(c, recv) == TY_ADDRINFO) &&
      emit_builtin_op(c, id, recv, comp_ntype(c, recv), name, b)) return 1;
  /* TY_IO (File/IO handle) instance methods */
  /* Dir handle instance methods (#2821) */
  if (recv >= 0 && comp_ntype(c, recv) == TY_DIR) {
    Buf drb = {0};
    emit_expr(c, recv, &drb);
    const char *dr = drb.p ? drb.p : "NULL";
    if (sp_streq(name, "read") && argc == 0) { buf_printf(b, "sp_Dir_read(%s)", dr); free(drb.p); return 1; }
    if ((sp_streq(name, "path") || sp_streq(name, "to_path")) && argc == 0) {
      buf_printf(b, "sp_Dir_path(%s)", dr); free(drb.p); return 1;
    }
    /* Dir#inspect renders as #<Dir:PATH> (#3250). */
    if (sp_streq(name, "inspect") && argc == 0) {
      buf_printf(b, "({ const char *_dp = sp_Dir_path(%s);"
                    " sp_sprintf(\"#<Dir:%%s>\", _dp ? _dp : \"\"); })", dr);
      free(drb.p); return 1;
    }
    if (sp_streq(name, "close") && argc == 0) { buf_printf(b, "sp_Dir_close(%s)", dr); free(drb.p); return 1; }
    if (sp_streq(name, "rewind") && argc == 0) { buf_printf(b, "sp_Dir_rewind(%s)", dr); free(drb.p); return 1; }
    if ((sp_streq(name, "tell") || sp_streq(name, "pos")) && argc == 0) {
      buf_printf(b, "sp_Dir_tell(%s)", dr); free(drb.p); return 1;
    }
    if (sp_streq(name, "seek") && argc == 1) {  /* Dir#seek(pos) -> self (#2967) */
      buf_printf(b, "sp_Dir_seek(%s, ", dr); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      free(drb.p); return 1;
    }
    if (sp_streq(name, "fileno") && argc == 0) { buf_printf(b, "sp_Dir_fileno(%s)", dr); free(drb.p); return 1; }
    if (sp_streq(name, "pos=") && argc == 1) {  /* Dir#pos= -> the assigned value (#2968) */
      int tp = ++g_tmp;
      buf_printf(b, "({ sp_int _t%d = ", tp); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; sp_Dir_seek(%s, _t%d); _t%d; })", dr, tp, tp);
      free(drb.p); return 1;
    }
    if ((sp_streq(name, "children") || sp_streq(name, "entries")) && argc == 0) {
      buf_printf(b, "sp_Dir_entries_h(%s, %d)", dr, sp_streq(name, "children") ? 1 : 0);
      free(drb.p); return 1;
    }
    /* each_entry is Enumerable's, and on a Dir it walks exactly what #each
       walks -- same entries, dots included, receiver as the value (#3395). */
    if ((sp_streq(name, "each") || sp_streq(name, "each_child") ||
         sp_streq(name, "each_entry")) &&
        nt_ref(nt, id, "block") >= 0) {
      int dblk2 = nt_ref(nt, id, "block");
      const char *dbp = block_param_name(c, dblk2, 0);
      const char *dbpn = dbp ? rename_local(dbp) : NULL;
      int dbdy = nt_ref(nt, dblk2, "body");
      int dbbn = 0; const int *dbbb = dbdy >= 0 ? nt_arr(nt, dbdy, "body", &dbbn) : NULL;
      int tdh = ++g_tmp, tdn = ++g_tmp;
      int skip_dots = sp_streq(name, "each_child");
      buf_puts(b, "({ ");
      /* rooted for the block's duration: a temporary receiver (Dir.open(d).each)
         has no other reference, and a body that allocates would let the
         collector close it mid-loop (the File loops below root theirs too).
         The entry string the loop yields is a fresh allocation held in a C
         temporary just the same, so it and the block's parameter take roots
         of their own. */
      buf_printf(b, "sp_Dir *_t%d = %s; SP_GC_ROOT(_t%d); const char *_t%d = NULL; SP_GC_ROOT_STR(_t%d); ",
                 tdh, dr, tdh, tdn, tdn);
      buf_printf(b, "while ((_t%d = sp_Dir_read(_t%d)) != NULL) {", tdn, tdh);
      if (skip_dots)
        buf_printf(b, " if (sp_str_eq(_t%d, (&(\"\\xff\" \".\")[1])) ||"
                      " sp_str_eq(_t%d, (&(\"\\xff\" \"..\")[1]))) continue;", tdn, tdn);
      if (dbpn) buf_printf(b, " const char *lv_%s = _t%d; SP_GC_ROOT_STR(lv_%s);", dbpn, tdn, dbpn);
      for (int k = 0; k < dbbn; k++) emit_stmt(c, dbbb[k], b, 0);
      buf_printf(b, " } (sp_Dir *)_t%d; })", tdh);
      return 1;
    }
    free(drb.p);
  }
  return 0;
}
