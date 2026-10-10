/* codegen_call_module.c -- emit_call_body's arms of the builtin modules and their class methods (GC, Process, Math, Marshal, File, Dir, Time, ...).
   Moved from emit_call_body (codegen_call.c) unchanged: each function holds
   a run of its arms, in their order, and answers 1 when one emitted the call
   (codegen_call_arms.h). */

#include "codegen_internal.h"
#include "repr.h"
#include "codegen_poly.h"
#include "builtin_ops.h"
#include "call_plan.h"
#include "repr.h"
#include "codegen_call_arms.h"

/* File.join's scalar and flattened routes keep their operands alive
   across every later argument, including argument setup emitted in g_pre. */
static void emit_file_join_args(Compiler *c, const int *argv, int argc, int boxed, Buf *b) {
  int literals = !boxed;
  for (int i = 0; i < argc; i++)
    if (nt_kind(c->nt, argv[i]) != NK_StringNode) literals = 0;
  if (literals) {
    buf_puts(b, "sp_file_join((const char*[]){");
    for (int i = 0; i < argc; i++) { if (i) buf_puts(b, ", "); emit_path_expr(c, argv[i], b); }
    if (!argc) buf_puts(b, "(const char*)0");
    buf_printf(b, "}, %d)", argc);
    return;
  }
  buf_puts(b, "({ ");
  int first = emit_rooted_arg_list(c, argv, argc,
                                 boxed ? "sp_RbVal" : "const char *",
                                 boxed ? "SP_GC_ROOT_RBVAL" : "SP_GC_ROOT_STR",
                                 boxed ? emit_boxed : emit_path_expr, NULL, b);
  buf_printf(b, "%s((%s[]){", boxed ? "sp_file_join_vals" : "sp_file_join",
             boxed ? "sp_RbVal" : "const char *");
  for (int i = 0; i < argc; i++) buf_printf(b, "%s_t%d", i ? ", " : "", first + i);
  if (!argc) buf_puts(b, "(const char *)0");
  buf_printf(b, "}, %d); })", argc);
}

/* realdirpath takes (path, base), while join consumes (base, path).
   Hold the values in Ruby's order before reversing the slots. */
static void emit_file_realdirpath2(Compiler *c, const int *argv, Buf *b) {
  buf_puts(b, "({ ");
  int first = emit_rooted_arg_list(c, argv, 2, "const char *", "SP_GC_ROOT_STR", emit_path_expr, NULL, b);
  buf_printf(b, "sp_file_realdirpath(sp_file_join((const char *[]){_t%d, _t%d}, 2)); })",
             first + 1, first);
}

/* the class methods of File / FileTest, Dir and Time */
int emit_call_file_dir_time_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* File class methods -> runtime helpers (the runtime has long carried
     these; only the dispatch was missing). FileTest shares File's predicate
     surface (#2819). */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") &&
      (sp_streq(nt_str(nt, recv, "name"), "File") ||
       sp_streq(nt_str(nt, recv, "name"), "FileTest"))) {
    if ((sp_streq(name, "basename") || sp_streq(name, "dirname") || sp_streq(name, "extname")) && argc == 1) {
      /* the path argument must reach sp_file_* as a const char*; emit_path_expr
         coerces a poly / nullable-string path so it does not pass an sp_RbVal
         into the char* slot (a C type error) (#3262), and asks a user object
         for its #to_path */
      buf_printf(b, "sp_file_%s(", name); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "basename") && argc == 2) {
      buf_puts(b, "sp_file_basename2("); emit_path_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_str_expr(c, argv[1], b); buf_puts(b, ")"); return 1;
    }
    if ((sp_streq(name, "read") || sp_streq(name, "binread")) && argc == 1) {
      /* binread answers BYTES (CRuby names them ASCII-8BIT); read answers text */
      int bin_r = sp_streq(name, "binread");
      if (bin_r) buf_puts(b, "sp_str_as_binary(");
      buf_puts(b, "sp_file_read("); emit_path_expr(c, argv[0], b); buf_puts(b, ")");
      if (bin_r) buf_puts(b, ")");
      return 1;
    }
    /* File.read(path, length) -> the first length bytes (#2776); a nil
       length is "the whole file", as CRuby */
    if ((sp_streq(name, "read") || sp_streq(name, "binread")) && argc == 2) {
      if (comp_ntype(c, argv[1]) == TY_NIL) {
        buf_puts(b, "({ (void)("); emit_expr(c, argv[1], b);
        buf_puts(b, "); sp_file_read("); emit_path_expr(c, argv[0], b); buf_puts(b, "); })");
        return 1;
      }
      { int bin_r2 = sp_streq(name, "binread");
        if (bin_r2) buf_puts(b, "sp_str_as_binary(");
        buf_puts(b, "sp_file_read_len("); emit_path_expr(c, argv[0], b); buf_puts(b, ", ");
        emit_int_expr(c, argv[1], b); buf_puts(b, ")");
        if (bin_r2) buf_puts(b, ")"); }
      return 1;
    }
    /* the stat/predicate family (#2775) */
    if (sp_streq(name, "ftype") && argc == 1) {
      buf_puts(b, "sp_file_ftype("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "writable?") && argc == 1) {
      buf_puts(b, "sp_file_writable("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "executable?") && argc == 1) {
      buf_puts(b, "sp_file_executable("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "size?") && argc == 1) {
      buf_puts(b, "sp_file_size_q("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "pipe?") && argc == 1) {
      buf_puts(b, "sp_file_pipe("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "identical?") && argc == 2) {
      buf_puts(b, "sp_file_identical("); emit_path_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_path_expr(c, argv[1], b); buf_puts(b, ")"); return 1;
    }
    if ((sp_streq(name, "atime") || sp_streq(name, "ctime") || sp_streq(name, "birthtime")) && argc == 1) {
      buf_printf(b, "sp_file_%s(", name); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "realpath") && argc == 1) {
      buf_puts(b, "sp_file_realpath("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "realdirpath") && (argc == 1 || argc == 2)) {
      if (argc == 2) emit_file_realdirpath2(c, argv, b);
      else {
        buf_puts(b, "sp_file_realdirpath("); emit_path_expr(c, argv[0], b); buf_puts(b, ")");
      }
      return 1;
    }

    if (sp_streq(name, "stat") && argc == 1) {
      buf_puts(b, "sp_file_stat_handle("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "lstat") && argc == 1) {
      buf_puts(b, "sp_file_lstat_handle("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    /* the path-manipulation family (#2774, #2787) */
    if (sp_streq(name, "split") && argc == 1) {
      buf_puts(b, "sp_file_split("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "path") && argc == 1) {
      /* the identity is only for path-like values: File.path(nil) raises */
      emit_path_expr(c, argv[0], b); return 1;
    }
    if ((sp_streq(name, "absolute_path") || sp_streq(name, "expand_path")) && (argc == 1 || argc == 2)) {
      buf_puts(b, "sp_file_expand_path("); emit_path_expr(c, argv[0], b); buf_puts(b, ", ");
      if (argc == 2) emit_path_expr(c, argv[1], b); else buf_puts(b, "(const char *)0");
      buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "absolute_path?") && argc == 1) {  /* (#2988) */
      buf_puts(b, "sp_file_absolute_path_p("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "chown") && argc == 3) {  /* File.chown(uid, gid, path); nil id -> -1 (#2987) */
      buf_puts(b, "sp_file_chown("); emit_path_expr(c, argv[2], b); buf_puts(b, ", ");
      for (int ci = 0; ci < 2; ci++) {
        if (ci) buf_puts(b, ", ");
        if (nt_type(nt, argv[ci]) && sp_streq(nt_type(nt, argv[ci]), "NilNode")) buf_puts(b, "-1LL");
        else emit_int_expr(c, argv[ci], b);
      }
      buf_puts(b, ")"); return 1;
    }
    if ((sp_streq(name, "fnmatch") || sp_streq(name, "fnmatch?")) && argc >= 2) {
      buf_puts(b, "sp_file_fnmatch("); emit_str_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_path_expr(c, argv[1], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "dirname") && argc == 2) {
      /* File.dirname(path, level): apply dirname `level` times (#2787) */
      int td = ++g_tmp, ti2 = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", td); emit_path_expr(c, argv[0], b);
      buf_printf(b, "; sp_int _tl%d = ", td); emit_int_expr(c, argv[1], b);
      buf_printf(b, "; for (sp_int _t%d = 0; _t%d < _tl%d; _t%d++) _t%d = sp_file_dirname(_t%d); _t%d; })",
                 ti2, ti2, td, ti2, td, td, td);
      return 1;
    }
    /* chmod / truncate (#2778) */
    if (sp_streq(name, "chmod") && argc == 2) {
      buf_puts(b, "sp_file_chmod("); emit_int_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_path_expr(c, argv[1], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "truncate") && argc == 2) {
      buf_puts(b, "sp_file_truncate("); emit_path_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_int_expr(c, argv[1], b); buf_puts(b, ")"); return 1;
    }
    /* File.write(path, str, offset) / File.write(path, str, mode: "a") (#2782) */
    if ((sp_streq(name, "write") || sp_streq(name, "binwrite")) && argc == 3) {
      const char *k3 = nt_type(nt, argv[2]);
      if (k3 && sp_streq(k3, "KeywordHashNode")) {
        int mv = struct_kwarg_value(c, argv[2], "mode");
        if (mv >= 0) {
          buf_puts(b, "sp_file_write_mode("); emit_path_expr(c, argv[0], b); buf_puts(b, ", ");
          emit_to_s_expr(c, argv[1], b); buf_puts(b, ", ");
          emit_str_expr(c, mv, b); buf_puts(b, ")");
          return 1;
        }
      }
      else if (comp_ntype(c, argv[2]) == TY_NIL) {
        /* a nil OFFSET is "no offset" -- a plain truncating write, not a
           write at position 0 (which would keep the file's tail) */
        buf_puts(b, "({ (void)("); emit_expr(c, argv[2], b);
        buf_puts(b, "); sp_file_write("); emit_path_expr(c, argv[0], b);
        buf_puts(b, ", "); emit_to_s_expr(c, argv[1], b); buf_puts(b, "); })");
        return 1;
      }
      else {
        buf_puts(b, "sp_file_write_at("); emit_path_expr(c, argv[0], b); buf_puts(b, ", ");
        emit_to_s_expr(c, argv[1], b); buf_puts(b, ", ");
        emit_int_expr(c, argv[2], b); buf_puts(b, ")");
        return 1;
      }
    }
    if ((sp_streq(name, "write") || sp_streq(name, "binwrite")) && argc == 2) {
      /* the runtime answers the byte count it wrote */
      buf_puts(b, "sp_file_write("); emit_path_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_to_s_expr(c, argv[1], b); buf_puts(b, ")");
      return 1;
    }
    /* File.exists? was removed in Ruby 4.0: NoMethodError at the call (#2780) */
    if (sp_streq(name, "exists?") && argc == 1) {
      buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
      buf_puts(b, "); sp_raise_cls(\"NoMethodError\", \"undefined method 'exists?' for class File\"); (sp_bool)0; })");
      return 1;
    }
    if (sp_streq(name, "exist?") && argc == 1) {
      buf_puts(b, "sp_file_exist("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "readable?") && argc == 1) {
      buf_puts(b, "sp_file_readable("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if ((sp_streq(name, "readable_real?") || sp_streq(name, "writable_real?") ||
         sp_streq(name, "executable_real?")) && argc == 1) {
      buf_printf(b, "sp_file_%.*s_real(", (int)(strlen(name) - 6), name);
      emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "directory?") && argc == 1) {
      buf_puts(b, "sp_file_directory("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    /* zero-length non-directory, not the directory test it aliased to (#2783) */
    if ((sp_streq(name, "zero?") || sp_streq(name, "empty?")) && argc == 1) {
      buf_puts(b, "sp_file_zero("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "symlink?") && argc == 1) {
      buf_puts(b, "sp_file_symlink("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    /* POSIX ownership / type predicates and helpers (#3005) */
    {
      static const struct { const char *m; const char *fn; } fpred[] = {
        {"owned?", "sp_file_owned"}, {"grpowned?", "sp_file_grpowned"},
        {"setuid?", "sp_file_setuid"}, {"setgid?", "sp_file_setgid"},
        {"sticky?", "sp_file_sticky"}, {"socket?", "sp_file_socket"},
        {"blockdev?", "sp_file_blockdev"}, {"chardev?", "sp_file_chardev"},
        {"world_readable?", "sp_file_world_readable"},
        {"world_writable?", "sp_file_world_writable"},
      };
      for (size_t fi = 0; fi < sizeof(fpred)/sizeof(fpred[0]); fi++) {
        if (sp_streq(name, fpred[fi].m) && argc == 1) {
          buf_printf(b, "%s(", fpred[fi].fn); emit_path_expr(c, argv[0], b); buf_puts(b, ")");
          return 1;
        }
      }
    }
    if ((sp_streq(name, "symlink") || sp_streq(name, "link")) && argc == 2) {
      buf_printf(b, "sp_file_do_%s(", name); emit_path_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_path_expr(c, argv[1], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "readlink") && argc == 1) {
      buf_puts(b, "sp_file_readlink("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "mkfifo") && (argc == 1 || argc == 2)) {
      buf_puts(b, "sp_file_mkfifo("); emit_path_expr(c, argv[0], b); buf_puts(b, ", ");
      if (argc == 2) emit_int_expr(c, argv[1], b); else buf_puts(b, "0666");
      buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "umask") && (argc == 0 || argc == 1)) {
      buf_puts(b, "sp_file_umask(");
      if (argc == 1) { emit_int_expr(c, argv[0], b); buf_puts(b, ", 1)"); }
      else buf_puts(b, "0, 0)");
      return 1;
    }
    if ((sp_streq(name, "utime") || sp_streq(name, "lutime")) && argc >= 3) {
      /* File.utime(atime, mtime, *paths): set the times on every path, return
         the count. Time args carry .tv_sec; numeric args are seconds. */
      int ua = ++g_tmp, um = ++g_tmp;
      char uas[24], uan[24], ums[24], umn[24];
      snprintf(uas, sizeof uas, "_t%ds", ua); snprintf(uan, sizeof uan, "_t%dn", ua);
      snprintf(ums, sizeof ums, "_t%ds", um); snprintf(umn, sizeof umn, "_t%dn", um);
      buf_printf(b, "({ int64_t %s = 0; int32_t %s = 0; int64_t %s = 0; int32_t %s = 0; ",
                 uas, uan, ums, umn);
      emit_utime_arg_ns(c, argv[0], uas, uan, b);
      emit_utime_arg_ns(c, argv[1], ums, umn, b);
      for (int k = 2; k < argc; k++) {
        /* lutime is the same call on the LINK itself (#4616) */
        buf_printf(b, "sp_file_%sutime_ns(%s, %s, %s, %s, ", sp_streq(name, "lutime") ? "l" : "",
                   uas, uan, ums, umn);
        emit_path_expr(c, argv[k], b); buf_puts(b, "); ");
      }
      buf_printf(b, "(sp_int)%d; })", argc - 2); return 1;
    }
    if (sp_streq(name, "file?") && argc == 1) {
      /* the path is named once: a #to_path behind it runs once */
      int tfp = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", tfp); emit_path_expr(c, argv[0], b);
      buf_printf(b, "; !sp_file_directory(_t%d) && sp_file_exist(_t%d); })", tfp, tfp); return 1;
    }
    if ((sp_streq(name, "delete") || sp_streq(name, "unlink")) &&
        (argc >= 1 || sp_streq(nt_str(nt, recv, "name"), "File"))) {
      /* a lone splat: every path of the Array converted and checked before
         the first unlink, as the several-path form below does, and the count
         answered */
      if (argc == 1 && nt_kind(nt, argv[0]) == NK_SplatNode) {
        int tl = ++g_tmp, tq = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_poly_to_poly_array(", tl); emit_boxed(c, argv[0], b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " for (sp_int _i = 0; _i < _t%d->len; _i++)"
                      " sp_PolyArray_push(_t%d, sp_box_str(sp_poly_arg_path(_t%d->data[_i])));"
                      " for (sp_int _i = 0; _i < _t%d->len; _i++) sp_file_path_check(_t%d->data[_i].v.s);"
                      " for (sp_int _i = 0; _i < _t%d->len; _i++) sp_file_delete(_t%d->data[_i].v.s);"
                      " _t%d->len; })",
                   tl, tq, tq, tl, tq, tl, tq, tq, tq, tq, tq);
        return 1;
      }
      if (argc == 1) {
        buf_puts(b, "({ sp_file_delete("); emit_path_expr(c, argv[0], b);
        buf_puts(b, "); (sp_int)1; })"); return 1;
      }
      /* several paths: every one is evaluated, rooted and checked for nil
         before the first unlink, as CRuby converts them all first -- now
         that a failing path raises, it must not skip a later argument's
         effects, and a nil among them must not cost the earlier files.
         `File.delete` with no path deletes nothing and answers 0; the
         guard keeps FileTest out of that form. */
      int td = ++g_tmp;
      buf_puts(b, "({ ");
      for (int k = 0; k < argc; k++) {
        buf_printf(b, "const char *_del_%d_%d = ", td, k); emit_path_expr(c, argv[k], b);
        buf_printf(b, "; SP_GC_ROOT_STR(_del_%d_%d); ", td, k);
      }
      for (int k = 0; k < argc; k++) buf_printf(b, "sp_file_path_check(_del_%d_%d); ", td, k);
      for (int k = 0; k < argc; k++) buf_printf(b, "sp_file_delete(_del_%d_%d); ", td, k);
      buf_printf(b, "(sp_int)%d; })", argc); return 1;
    }
    if (sp_streq(name, "rename") && argc == 2) {
      buf_puts(b, "({ sp_file_rename("); emit_path_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_path_expr(c, argv[1], b); buf_puts(b, "); (sp_int)0; })"); return 1;
    }
    if (sp_streq(name, "mtime") && argc == 1) {
      buf_puts(b, "sp_file_mtime("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "size") && argc == 1) {
      buf_puts(b, "sp_file_size("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "join")) {
      int has_dyn = 0;
      for (int k = 0; k < argc; k++) {
        Repr jr = repr_of(c, argv[k]);
        TyKind jt = jr.as_ty;
        if (ty_is_array(jt) || jt == TY_POLY_ARRAY || jr.kind == RK_BOXED) has_dyn = 1;
      }
      emit_file_join_args(c, argv, argc, has_dyn, b);
      return 1;
    }

    if (sp_streq(name, "readlines") && argc >= 1) {
      /* File.readlines(path[, sep][, chomp: true]) (#2820) */
      int csep = -1;
      Buf chomp; memset(&chomp, 0, sizeof chomp);
      for (int ki = 1; ki < argc; ki++) {
        const char *kty = nt_type(nt, argv[ki]);
        if (kty && sp_streq(kty, "KeywordHashNode")) {
          int cv = struct_kwarg_value(c, argv[ki], "chomp");
          emit_kw_flag(c, cv, &chomp);
        }
        else csep = argv[ki];
      }
      /* a flag decided at run time takes the separator form, which carries it */
      if (csep >= 0 || (chomp.p && !sp_streq(chomp.p, "0") && !sp_streq(chomp.p, "1"))) {
        buf_puts(b, "sp_file_readlines_sep(");
        emit_path_expr(c, argv[0], b); buf_puts(b, ", ");
        if (csep >= 0) emit_str_expr(c, csep, b); else buf_puts(b, "\"\\n\"");
        buf_printf(b, ", %s)", chomp.p ? chomp.p : "0");
        free(chomp.p); return 1;
      }
      if (chomp.p && sp_streq(chomp.p, "1")) buf_puts(b, "sp_file_readlines_chomp(");
      else buf_puts(b, "sp_file_readlines(");
      free(chomp.p);
      emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    /* File.open(path, mode) / File.new(path, mode) without block -> TY_IO
       handle. The mode may be a string, an integer flag word (#2788), or a
       trailing `mode:` keyword (#2789). */
    if (is_open_constructor(name)) {
      int block = nt_ref(nt, id, "block");
      int kw_mode = -1;
      if (argc >= 2 && nt_type(nt, argv[argc - 1]) &&
          sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode"))
        kw_mode = struct_kwarg_value(c, argv[argc - 1], "mode");
      /* the mode is the `mode:` keyword's value or the second positional;
         an Integer flag word, or an object answering #to_int (CRuby asks it
         before #to_str), takes the flags entry; a third positional is the
         permission bits, which reach open(2) for either mode form */
      int mnode = kw_mode >= 0 ? kw_mode : argc >= 2 ? argv[1] : -1;
      int int_mode = mnode >= 0 &&
                     (comp_ntype(c, mnode) == TY_INT ||
                      obj_conv_method(c, comp_ntype(c, mnode), "to_int", TY_INT, NULL) >= 0);
      int perm = -1;
      if (argc >= 1 && nt_type(nt, argv[argc - 1]) &&
          sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode"))
        perm = struct_kwarg_value(c, argv[argc - 1], "perm");
      if (perm < 0 && kw_mode < 0 && argc >= 3 &&
          !(nt_type(nt, argv[2]) && sp_streq(nt_type(nt, argv[2]), "KeywordHashNode")))
        perm = argv[2];
      /* a nil perm is CRuby's default; the runtime reads a nil sp_oint as 0666 */
      if (perm >= 0 && comp_ntype(c, perm) == TY_NIL) perm = -1;
      #define emit_perm_expr(c, n, b) do { \
        if (yield_site_type(c, n) == TY_POLY) { buf_puts(b, "sp_poly_arg_perm("); emit_expr(c, n, b); buf_puts(b, ")"); } \
        else emit_oint_expr(c, n, TY_INT, b); \
      } while (0)
      /* A mode the analysis cannot classify (a boxed read, a computed flags
         word in a poly slot) is decided at run time rather than assumed to be
         a mode string -- assumed, `File.open(path, h[:mode])` reached
         sp_poly_arg_str_chk and raised "no implicit conversion of Integer
         into String" for the flag word CRuby accepts (#4596). */
      int poly_mode = mnode >= 0 && !int_mode && repr_of(c, mnode).kind == RK_BOXED;
      #define EMIT_FILE_OPEN() do { \
        if (poly_mode) { \
          buf_puts(b, "sp_File_open_val("); \
          emit_path_expr(c, argv[0], b); buf_puts(b, ", "); \
          emit_boxed(c, mnode, b); buf_puts(b, ", "); \
          if (perm >= 0) emit_perm_expr(c, perm, b); else buf_puts(b, "sp_oint_nil()"); \
          buf_puts(b, ")"); \
        } \
        else if (int_mode) { \
          buf_puts(b, perm >= 0 ? "sp_File_open_flags_perm(" : "sp_File_open_flags("); \
          emit_path_expr(c, argv[0], b); buf_puts(b, ", "); \
          emit_int_expr(c, mnode, b); \
          if (perm >= 0) { buf_puts(b, ", "); emit_perm_expr(c, perm, b); } \
          buf_puts(b, ")"); \
        } \
        else { \
          /* the path and mode are const char * slots: a String reached
             through a poly binding -- a String ivar the fixpoint widened, an
             untyped accessor -- would otherwise go in as a raw sp_RbVal
             (#3385). Identity for a real String. */ \
          buf_puts(b, perm >= 0 ? "sp_File_open_perm(" : "sp_File_open("); \
          emit_path_expr(c, argv[0], b); buf_puts(b, ", "); \
          if (kw_mode >= 0) emit_str_expr(c, kw_mode, b); \
          else if (argc >= 2 && !(nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "KeywordHashNode"))) emit_str_expr(c, argv[1], b); \
          else buf_puts(b, "\"r\""); \
          if (perm >= 0) { buf_puts(b, ", "); emit_perm_expr(c, perm, b); } \
          buf_puts(b, ")"); \
        } \
      } while (0)
      if (block < 0) {
        EMIT_FILE_OPEN();
        return 1;
      }
      /* File.open(path, mode) { |f| body } -> open, run body, close, return body value */
      const char *fp = block_param_name(c, block, 0);
      const char *frn = fp ? rename_local(fp) : NULL;
      int bbody = nt_ref(nt, block, "body");
      int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
      TyKind res = repr_of(c, id).as_ty;
      int rv = ++g_tmp, tf = ++g_tmp;
      int scalar = is_scalar_ret(res) && res != TY_VOID && res != TY_NIL && res != TY_UNKNOWN;
      buf_puts(b, "({ ");
      buf_printf(b, "sp_File *_t%d = ", tf); EMIT_FILE_OPEN(); buf_puts(b, "; ");
      #undef EMIT_FILE_OPEN
      #undef emit_perm_expr
      /* Root the handle for the block's duration: the body may allocate and
         trigger a GC, and an unrooted sp_File would be swept (its finalizer
         fcloses mid-iteration, silently truncating each_line loops). */
      buf_printf(b, "SP_GC_ROOT(_t%d); ", tf);
      if (frn) {
        /* Declare the file param as a local: look it up in the enclosing scope.
           Since it's the block param, just use the sp_File * type directly. */
        buf_printf(b, "sp_File *lv_%s = _t%d; ", frn, tf);
      }
      for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], b, 0);
      if (bn > 0) {
        TyKind lty = repr_of(c, bb[bn-1]).as_ty;
        /* Emit last stmt as expression when it has a usable non-void value.
           For void/nil/unknown side-effecting calls (e.g. f.print), emit_stmt
           handles g_pre correctly; then synthesize a return value. */
        int can_expr = (lty != TY_VOID && lty != TY_UNKNOWN &&
                        (lty != TY_NIL ||
                         (nt_type(nt, bb[bn-1]) && sp_streq(nt_type(nt, bb[bn-1]), "NilNode"))));
        if (scalar && can_expr) {
          /* Catch the tail expression's statement-shaped setup in a local
             buffer: g_pre here is the buffer for the whole `File.open(...) {
             ... }` line, so anything routed there runs BEFORE the file is
             opened. `h[k] += f.read.length` put its read-modify-write out
             there and read the file before it existed (same shape as #3387). */
          Buf fpre; memset(&fpre, 0, sizeof fpre);
          Buf fval; memset(&fval, 0, sizeof fval);
          Buf *sv_fp = g_pre; int sv_fi = g_indent;
          g_pre = &fpre; g_indent = 0;
          if (res == TY_POLY && lty != TY_POLY) emit_boxed(c, bb[bn-1], &fval);
          else emit_expr(c, bb[bn-1], &fval);
          g_pre = sv_fp; g_indent = sv_fi;
          if (fpre.p) buf_puts(b, fpre.p);
          emit_ctype(c, res, b); buf_printf(b, " _t%d = ", rv);
          buf_puts(b, fval.p ? fval.p : "0");
          buf_puts(b, "; ");
          free(fpre.p); free(fval.p);
        }
        else {
          emit_stmt(c, bb[bn-1], b, 0);
          if (scalar) {
            emit_ctype(c, res, b); buf_printf(b, " _t%d = ", rv);
            if (res == TY_POLY) buf_puts(b, "sp_box_nil()");
            else buf_puts(b, default_value_from_compiler(c, res));
            buf_puts(b, "; ");
          }
        }
      }
      buf_printf(b, "sp_File_close(_t%d); ", tf);
      /* The value of the whole statement-expression has to fit the slot the
         call was typed into. A bare `0` fits an int slot only, so an EMPTY
         block (`File.open(p, "a") { |f| }`) in a poly-returning method emitted
         an int where an sp_RbVal was expected (#3385). */
      if (scalar && bn > 0) buf_printf(b, "_t%d; })", rv);
      else if (scalar && res == TY_POLY) buf_puts(b, "sp_box_nil(); })");
      else if (scalar) { buf_puts(b, default_value_from_compiler(c, res)); buf_puts(b, "; })"); }
      else buf_puts(b, "0; })");
      return 1;
    }
  }
  /* Path arguments throughout the File / Dir / IO surface go through
     emit_path_expr rather than emit_expr: they land in `const char *` slots,
     and a String reached through a poly binding -- a widened String ivar, an
     untyped accessor, an element read -- would otherwise be handed over as a
     raw sp_RbVal (#3256, #3330, #3385 are three sightings of the same shape),
     and a user object is asked for its #to_path. The emitter is the identity
     for a value already typed String, so it costs nothing where inference
     succeeded. */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir")) {
    if (sp_streq(name, "for_fd") && argc == 1) {
      buf_puts(b, "sp_Dir_for_fd("); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "fchdir") && argc == 1) {
      buf_puts(b, "sp_Dir_fchdir("); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* Dir.new / Dir.open -> a directory handle; the block form closes on
       exit and returns the block's value (#2821) */
    if ((is_open_constructor(name)) && argc >= 1) {
      int dblk = nt_ref(nt, id, "block");
      if (dblk < 0) {
        buf_puts(b, "sp_Dir_new("); emit_path_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      const char *dp0 = block_param_name(c, dblk, 0);
      const char *dpn = dp0 ? rename_local(dp0) : NULL;
      int dbody = nt_ref(nt, dblk, "body");
      int dbn = 0; const int *dbb = dbody >= 0 ? nt_arr(nt, dbody, "body", &dbn) : NULL;
      TyKind dres = repr_of(c, id).as_ty;
      int dscalar = is_scalar_ret(dres) && dres != TY_VOID && dres != TY_NIL && dres != TY_UNKNOWN;
      int td = ++g_tmp, tdv = ++g_tmp;
      buf_puts(b, "({ ");
      buf_printf(b, "sp_Dir *_t%d = sp_Dir_new(", td); emit_path_expr(c, argv[0], b); buf_puts(b, "); ");
      buf_printf(b, "SP_GC_ROOT(_t%d); ", td);
      if (dpn) buf_printf(b, "sp_Dir *lv_%s = _t%d; ", dpn, td);
      for (int k = 0; k < dbn - 1; k++) emit_stmt(c, dbb[k], b, 0);
      if (dbn > 0 && dscalar) {
        emit_ctype(c, dres, b); buf_printf(b, " _t%d = ", tdv);
        if (dres == TY_POLY && repr_of(c, dbb[dbn - 1]).kind != RK_BOXED) emit_boxed(c, dbb[dbn - 1], b);
        else emit_expr(c, dbb[dbn - 1], b);
        buf_puts(b, "; ");
      }
      else if (dbn > 0) emit_stmt(c, dbb[dbn - 1], b, 0);
      buf_printf(b, "sp_Dir_close(_t%d); ", td);
      if (dscalar && dbn > 0) buf_printf(b, "_t%d; })", tdv);
      else buf_puts(b, "0; })");
      return 1;
    }
    if (sp_streq(name, "pwd") && argc == 0) { buf_puts(b, "sp_dir_pwd()"); return 1; }
    if (sp_streq(name, "home") && argc == 0) { buf_puts(b, "sp_dir_home()"); return 1; }
    if (sp_streq(name, "empty?") && argc == 1) {
      buf_puts(b, "sp_dir_empty("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "home") && argc == 1) {
      buf_puts(b, "sp_dir_home_user("); emit_str_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "glob") && argc == 2 && nt_type(nt, argv[1]) &&
        sp_streq(nt_type(nt, argv[1]), "ConstantPathNode") &&
        nt_str(nt, argv[1], "name") && sp_streq(nt_str(nt, argv[1], "name"), "FNM_DOTMATCH")) {
      buf_puts(b, "sp_dir_glob_dot("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    /* Dir.glob(pattern, base: dir): a lone base: keyword; nil is the current directory */
    if (sp_streq(name, "glob") && argc == 2 && nt_kind(nt, argv[1]) == NK_KeywordHashNode &&
        !ty_is_array(comp_ntype(c, argv[0]))) {
      int en = 0; (void)nt_arr(nt, argv[1], "elements", &en);
      int bv = kwh_lookup(nt, argv[1], "base");
      if (en == 1 && bv >= 0) {
        buf_puts(b, "sp_dir_glob_base("); emit_path_expr(c, argv[0], b); buf_puts(b, ", ");
        if (comp_ntype(c, bv) == TY_NIL) buf_puts(b, "NULL"); else emit_path_expr(c, bv, b);
        buf_puts(b, ")"); return 1;
      }
    }
    if (sp_streq(name, "glob") && argc == 1 && ty_is_array(comp_ntype(c, argv[0]))) {
      buf_puts(b, "sp_dir_glob_multi("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "glob") && argc == 1) {
      buf_puts(b, "sp_dir_glob("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if ((is_directory_entries(name)) && argc == 1) {
      buf_printf(b, "sp_dir_%s(", name); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    /* Dir.entries / Dir.children(path, encoding: e), and the foreach /
       each_child forms desugared onto them: Find.find lists a directory so.
       The keyword was counted as a second argument, and the call fell through
       to the run-time NoMethodError. Only a lone encoding: is taken; another
       keyword keeps the old path. */
    if (is_directory_entries(name) && argc == 2 && nt_kind(nt, argv[1]) == NK_KeywordHashNode) {
      int en = 0; (void)nt_arr(nt, argv[1], "elements", &en);
      int ev = kwh_lookup(nt, argv[1], "encoding");
      if (en == 1 && ev >= 0) {
        buf_puts(b, "sp_dir_entries_enc("); emit_path_expr(c, argv[0], b);
        buf_printf(b, ", %d, ", sp_streq(name, "children") ? 1 : 0);
        emit_boxed(c, ev, b); buf_puts(b, ")");
        return 1;
      }
    }
    if ((sp_streq(name, "mkdir") || sp_streq(name, "rmdir") || sp_streq(name, "chdir")) && argc >= 1) {
      if (sp_streq(name, "mkdir") && argc == 2) {
        /* the permission mode is unused on this backend but still validated:
           Dir.mkdir(path, nil) is CRuby's TypeError, not a created directory */
        int tp = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = ", tp); emit_path_expr(c, argv[0], b);
        buf_puts(b, "; (void)("); emit_int_expr_conv(c, argv[1], b);
        buf_printf(b, "); sp_dir_mkdir(_t%d); })", tp);
        return 1;
      }
      /* the block form's switch and restore, spliced in by desugar_dir_surface,
         carry CRuby's label for that form (dir_chdir0, not chdir_path) */
      if (sp_streq(name, "chdir") && nt_str(c->nt, id, "chdir_label"))
        buf_puts(b, "sp_dir_chdir0(");
      else buf_printf(b, "sp_dir_%s(", name);
      emit_path_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
  }

  /* Time class constructors */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Time")) {
    if ((sp_streq(name, "now") || sp_streq(name, "new")) && argc == 0) { buf_puts(b, "sp_time_now()"); return 1; }
    /* Time.now(in: zone): the current instant in the zone the keyword names,
       read as Time.at's `in:` is. With no arm here the call fell through to
       the run-time NoMethodError. */
    if (sp_streq(name, "now") && argc == 1 &&
        nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode") &&
        struct_kwarg_value(c, argv[0], "in") >= 0) {
      int ts = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = sp_time_now();", ts);
      emit_time_in_zone(c, ts, struct_kwarg_value(c, argv[0], "in"), b);
      return 1;
    }
    /* Time.at(x, *rest) and any other spread: every argument into one list,
       which the runtime reads as Time.at's arguments, as for a lone splat */
    if (sp_streq(name, "at") && argc >= 2) {
      int any_splat = 0;
      for (int k = 0; k < argc; k++) if (nt_kind(nt, argv[k]) == NK_SplatNode) any_splat = 1;
      if (any_splat) {
        buf_puts(b, "({ ");
        int tf = emit_bm_flat_args(c, argv, argc, b);
        buf_printf(b, " sp_time_at_args(sp_box_poly_array(_t%d)); })", tf);
        return 1;
      }
    }
    /* Time.at(*args): the runtime reads the list as Time.at's arguments */
    if (sp_streq(name, "at") && argc == 1 && nt_kind(nt, argv[0]) == NK_SplatNode &&
        nt_ref(nt, argv[0], "expression") >= 0) {
      buf_puts(b, "sp_time_at_args(");
      emit_boxed(c, nt_ref(nt, argv[0], "expression"), b);
      buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "at") && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_TIME) { emit_expr(c, argv[0], b); return 1; }  /* value copy */
      if (at == TY_RATIONAL) {
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_Rational _t%d = ", tr);
        emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_time_at_div(_t%d.num, _t%d.den); })", tr, tr);
        return 1;
      }
      /* a non-numeric argument raises CRuby's TypeError; the expression
         still needs the arm's sp_Time type for downstream emitters */
      const char *atc = at == TY_STRING ? "String" : at == TY_SYMBOL ? "Symbol" :
                        at == TY_NIL ? "NilClass" : NULL;
      if (atc) {
        buf_printf(b, "({ sp_raise_cls(\"TypeError\", "
                      "\"can't convert %s into an exact number\"); (sp_Time){0, 0, 0}; })", atc);
        return 1;
      }
      /* a poly argument (`Time.at(x)` where x is a heterogeneous/nullable
         numeric) reaches sp_time_at_int's sp_int slot: coerce it. A boxed
         float is understood by sp_poly_to_f, so route poly through the float
         ctor (which also accepts an integral value). */
      if (at == TY_POLY) {
        buf_puts(b, "sp_time_at_float(sp_poly_to_f("); emit_expr(c, argv[0], b);
        buf_puts(b, "))");
        return 1;
      }
      if (at != TY_INT && at != TY_FLOAT && at != TY_BIGINT && at != TY_UNKNOWN) {
        /* an object answering #to_int is its Integer; any other class is
           CRuby's "can't convert X into an exact number" */
        if (ty_is_object(at) && obj_conv_method(c, at, "to_int", TY_INT, NULL) >= 0) {
          buf_puts(b, "sp_time_at_int("); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
          return 1;
        }
        const char *ocn = conv_cls_name_of(c, at);
        if (ocn) {
          buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
          buf_printf(b, "); sp_raise_cls(\"TypeError\", \"can't convert %s into an exact number\"); (sp_Time){0, 0, 0}; })", ocn);
          return 1;
        }
      }
      buf_printf(b, "sp_time_at_%s(", at == TY_FLOAT ? "float" : "int");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* Time.at(sec, in: "+HH:MM"): the epoch instant carried with a fixed UTC
       offset (is_utc == 2), so #utc_offset returns it. (#2681) */
    /* Time.at(..., in: zone): build the time from the positional arguments,
       then re-read it in the zone the keyword names -- a name, an Integer
       offset or an offset string, whatever their types (#3696, #3698). */
    if (sp_streq(name, "at") && argc >= 2 &&
        nt_type(nt, argv[argc - 1]) && sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode") &&
        struct_kwarg_value(c, argv[argc - 1], "in") >= 0) {
      int inv = struct_kwarg_value(c, argv[argc - 1], "in");
      int ts = ++g_tmp;
      TyKind st = comp_ntype(c, argv[0]);
      buf_printf(b, "({ sp_Time _t%d = ", ts);
      if (st == TY_TIME) emit_expr(c, argv[0], b);
      else if (st == TY_FLOAT) { buf_puts(b, "sp_time_at_float("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (st == TY_RATIONAL) {
        int trq = ++g_tmp;
        buf_printf(b, "({ sp_Rational _t%d = ", trq); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_time_at_div(_t%d.num, _t%d.den); })", trq, trq);
      }
      else if (st == TY_POLY || st == TY_UNKNOWN) {
        buf_puts(b, "sp_time_at_float(sp_poly_to_f("); emit_boxed(c, argv[0], b); buf_puts(b, "))");
      }
      else { buf_puts(b, "sp_time_at_int("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_puts(b, ";");
      /* a second positional argument is microseconds */
      if (argc >= 3) {
        buf_printf(b, " _t%d = sp_time_add_nsec(_t%d, (int64_t)((", ts, ts);
        emit_int_expr(c, argv[1], b);
        buf_puts(b, ")) * 1000);");
      }
      emit_time_in_zone(c, ts, inv, b);
      return 1;
    }
    /* Time.at(sec, num, :unit): the third argument names the second one's
       unit -- :millisecond / :usec / :microsecond / :nanosecond (and their
       plurals/aliases), scaled to tv_nsec. Only a literal symbol resolves the
       scale at compile time; anything else falls through. (#2714) */
    if (sp_streq(name, "at") && argc == 3 &&
        nt_type(nt, argv[2]) && sp_streq(nt_type(nt, argv[2]), "SymbolNode")) {
      const char *un = nt_str(nt, argv[2], "value");
      long mult = 0;
      /* CRuby's exact unit set -- no plurals; an unknown symbol is its
         runtime ArgumentError */
      if (un && sp_streq(un, "millisecond")) mult = 1000000;
      else if (un && (sp_streq(un, "usec") || sp_streq(un, "microsecond"))) mult = 1000;
      else if (un && (sp_streq(un, "nsec") || sp_streq(un, "nanosecond"))) mult = 1;
      else if (un) {
        buf_printf(b, "({ sp_raise_cls(\"ArgumentError\", \"unexpected unit: %s\"); (sp_Time){0}; })", un);
        return 1;
      }
      if (mult > 0) {
        TyKind st = comp_ntype(c, argv[0]);
        int ts = ++g_tmp;
        buf_printf(b, "({ sp_Time _t%d = ", ts);
        if (st == TY_FLOAT) { buf_puts(b, "sp_time_at_float("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else { buf_puts(b, "sp_time_at_int("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
        buf_printf(b, "; _t%d = sp_time_add_nsec(_t%d, (int64_t)(", ts, ts);
        TyKind ut = comp_ntype(c, argv[1]);
        if (ut == TY_FLOAT) { buf_puts(b, "("); emit_expr(c, argv[1], b); buf_printf(b, ") * %ld.0", mult); }
        else if (ut == TY_RATIONAL) { buf_puts(b, "sp_rational_to_f("); emit_expr(c, argv[1], b); buf_printf(b, ") * %ld.0", mult); }
        else { buf_puts(b, "((int64_t)("); emit_int_expr(c, argv[1], b); buf_printf(b, ")) * %ld", mult); }
        /* a whole unit or more carries into the seconds field (#3704) */
        buf_printf(b, ")); _t%d; })", ts);
        return 1;
      }
    }
    /* Time.at(sec, usec): the second positional argument is microseconds
       (no unit keyword). tv_nsec = usec * 1000. (#2646) */
    if (sp_streq(name, "at") && argc == 2 &&
        !(nt_type(nt, argv[1]) && (sp_streq(nt_type(nt, argv[1]), "KeywordHashNode") ||
                                   sp_streq(nt_type(nt, argv[1]), "HashNode")))) {
      TyKind st = comp_ntype(c, argv[0]);
      int ts = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = ", ts);
      if (st == TY_FLOAT) { buf_puts(b, "sp_time_at_float("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (st == TY_POLY || st == TY_UNKNOWN) { buf_puts(b, "sp_time_at_float(sp_poly_to_f("); emit_boxed(c, argv[0], b); buf_puts(b, "))"); }
      else { buf_puts(b, "sp_time_at_int("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_printf(b, "; _t%d = sp_time_add_nsec(_t%d, (int64_t)(", ts, ts);
      TyKind ut = comp_ntype(c, argv[1]);
      if (ut == TY_FLOAT) { emit_expr(c, argv[1], b); buf_puts(b, " * 1000.0"); }
      else if (ut == TY_RATIONAL) { buf_puts(b, "sp_rational_to_f("); emit_expr(c, argv[1], b); buf_puts(b, ") * 1000.0"); }
      else { buf_puts(b, "((int64_t)("); emit_int_expr(c, argv[1], b); buf_puts(b, ")) * 1000"); }
      /* a whole second or more carries into the seconds field (#3704) */
      buf_printf(b, ")); _t%d; })", ts);
      return 1;
    }
    if ((sp_streq(name, "local") || sp_streq(name, "mktime") ||
         sp_streq(name, "utc") || sp_streq(name, "gm") || sp_streq(name, "new")) && argc >= 1) {
      int is_utc = (sp_streq(name, "utc") || sp_streq(name, "gm"));
      if (emit_time_civil_ctor(c, id, is_utc, sp_streq(name, "new"), b)) return 1;
      unsupported(c, id, "Time constructor argument form");
      return 1;
    }
  }
  return 0;
}

/* the module functions of GC, Fiber, Process, Marshal and Math, Integer.sqrt, to_json and Dir.exist? */
int emit_call_module_fn_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* StringIO is a native-bound package class; .open is Ruby in the package. */

  /* Encoding.find(name) with a name known only at run time (a literal one is
     the constant it names, desugar_encoding_queries) */
  if (recv >= 0 && nt_kind(nt, recv) == NK_ConstantReadNode && argc == 1 &&
      sp_streq(name, "find") && nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Encoding")) {
    buf_puts(b, "sp_encoding_find("); emit_boxed(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }

  /* GC module methods */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "GC")) {
    if (sp_streq(name, "start") && argc == 0) { buf_puts(b, "(sp_gc_collect_request(), (sp_int)0)"); return 1; }
    /* GC.start(full_mark:, immediate_mark:, immediate_sweep:): one collector,
       one kind of collection -- the options are hints, their values
       evaluated for what they do */
    if (sp_streq(name, "start") && argc == 1 && nt_kind(nt, argv[0]) == NK_KeywordHashNode) {
      int en = 0; const int *els = nt_arr(nt, argv[0], "elements", &en);
      int ok = 1;
      for (int e = 0; e < en && ok; e++) {
        int key = nt_kind(nt, els[e]) == NK_AssocNode ? nt_ref(nt, els[e], "key") : -1;
        const char *kn = key >= 0 && nt_kind(nt, key) == NK_SymbolNode ? nt_str(nt, key, "value") : NULL;
        ok = kn && (sp_streq(kn, "full_mark") || sp_streq(kn, "immediate_mark") ||
                    sp_streq(kn, "immediate_sweep"));
      }
      if (ok) {
        buf_puts(b, "(");
        for (int e = 0; e < en; e++) {
          buf_puts(b, "(void)("); emit_expr(c, nt_ref(nt, els[e], "value"), b); buf_puts(b, "), ");
        }
        buf_puts(b, "sp_gc_collect_request(), (sp_int)0)");
        return 1;
      }
    }
    if (sp_streq(name, "compact") && argc == 0) { buf_puts(b, "(sp_gc_collect_request(), (sp_int)0)"); return 1; }
    if (sp_streq(name, "stat") && argc == 0) { buf_puts(b, "sp_gc_stat()"); return 1; }
    /* the time every collection so far took (sp_gc_stat_seconds), in
       nanoseconds as CRuby answers it */
    if (sp_streq(name, "total_time") && argc == 0) { buf_puts(b, "((sp_int)(sp_gc_stat_seconds * 1e9))"); return 1; }
    /* GC.stat(key): the one statistic, ArgumentError for a key not kept */
    if (sp_streq(name, "stat") && argc == 1 && nt_kind(nt, argv[0]) != NK_KeywordHashNode) {
      buf_puts(b, "sp_gc_stat_key("); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
  }

  /* Fiber class methods: Fiber.yield(val) and Fiber.current */
  if (recv_is_const(nt, recv, "Fiber")) {
    if (sp_streq(name, "yield")) {
      /* Fiber.yield(a, b) hands the resumer [a, b] */
      emit_fiber_pass_call(c, "sp_Fiber_yield", NULL, argc, argv, b);
      return 1;
    }
    if (sp_streq(name, "current") && argc == 0) {
      buf_puts(b, "sp_fiber_current");
      return 1;
    }
    /* Fiber.blocking? is 1 in a blocking fiber, false otherwise (CRuby) */
    if (sp_streq(name, "blocking?") && argc == 0) {
      buf_puts(b, "(sp_Fiber_blocking_p(sp_fiber_current) ? sp_box_int(1) : sp_box_bool(0))");
      return 1;
    }
    if (sp_streq(name, "blocking") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
      int cblk = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
      int bt = cblk >= 0 ? hoist_block_proc(c, cblk) : -1;
      if (bt < 0) unsupported(c, id, "Fiber.blocking block form");
      buf_printf(b, "sp_Fiber_blocking_proc(_t%d)", bt);
      return 1;
    }
    /* there is no fiber scheduler to set, so there is never one to answer */
    if ((sp_streq(name, "scheduler") || sp_streq(name, "current_scheduler")) && argc == 0) {
      buf_puts(b, "sp_box_nil()");
      return 1;
    }
  }

  /* Process module methods */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Process")) {
    if (sp_streq(name, "pid") && argc == 0) { buf_puts(b, "((sp_int)getpid())"); return 1; }
    if (sp_streq(name, "times") && argc == 0) { buf_puts(b, "sp_process_times()"); return 1; }
    if (sp_streq(name, "ppid") && argc == 0) { buf_puts(b, "sp_process_ppid()"); return 1; }
    /* real/effective user & group ids (#3043) */
    if (sp_streq(name, "uid") && argc == 0) { buf_puts(b, "((sp_int)getuid())"); return 1; }
    if (sp_streq(name, "gid") && argc == 0) { buf_puts(b, "((sp_int)getgid())"); return 1; }
    if (sp_streq(name, "euid") && argc == 0) { buf_puts(b, "((sp_int)geteuid())"); return 1; }
    if (sp_streq(name, "egid") && argc == 0) { buf_puts(b, "((sp_int)getegid())"); return 1; }
    if (sp_streq(name, "getpgrp") && argc == 0) { buf_puts(b, "((sp_int)getpgrp())"); return 1; }
    if (sp_streq(name, "getsid") && argc <= 1) {
      buf_puts(b, "((sp_int)getsid(");
      if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "0");
      buf_puts(b, "))"); return 1;
    }
    if ((sp_streq(name, "clock_gettime") || sp_streq(name, "clock_getres")) && argc >= 1) {
      /* honor the clock id, and the unit (default :float_second). An integer
         unit yields an Integer; the float units and the default yield a Float.
         clock_getres (#3045) differs only in the runtime call. */
      const char *unit = NULL;
      if (argc >= 2 && nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "SymbolNode"))
        unit = nt_str(nt, argv[1], "value");
      /* an unknown literal unit is CRuby's ArgumentError, not a silent
         float_second (#2727) */
      if (unit && !sp_streq(unit, "nanosecond") && !sp_streq(unit, "microsecond") &&
          !sp_streq(unit, "millisecond") && !sp_streq(unit, "second") &&
          !sp_streq(unit, "float_microsecond") && !sp_streq(unit, "float_millisecond") &&
          !sp_streq(unit, "float_second")) {
        buf_printf(b, "({ sp_raise_cls(\"ArgumentError\", (&(\"\\xff\" \"unexpected unit: %s\")[1])); 0.0; })", unit);
        return 1;
      }
      buf_puts(b, sp_streq(name, "clock_getres") ? "(sp_process_clock_res_ns(" : "(sp_process_clock_ns(");
      if (!emit_clock_id(c, argv[0], b)) emit_int_expr(c, argv[0], b);
      buf_puts(b, ")");
      if (unit && sp_streq(unit, "nanosecond")) buf_puts(b, ")");
      else if (unit && sp_streq(unit, "microsecond")) buf_puts(b, " / 1000)");
      else if (unit && sp_streq(unit, "millisecond")) buf_puts(b, " / 1000000)");
      else if (unit && sp_streq(unit, "second")) buf_puts(b, " / 1000000000)");
      else if (unit && sp_streq(unit, "float_microsecond")) buf_puts(b, " / 1e3)");
      else if (unit && sp_streq(unit, "float_millisecond")) buf_puts(b, " / 1e6)");
      else buf_puts(b, " / 1e9)");  /* float_second (default) */
      return 1;
    }
    if (sp_streq(name, "getpriority") && argc == 2) {  /* (#3046) */
      buf_puts(b, "sp_process_getpriority("); emit_int_expr(c, argv[0], b);
      buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "groups") && argc == 0) {  /* (#3046) */
      buf_puts(b, "sp_process_groups()"); return 1;
    }
  }

  /* Integer.sqrt(n) -> integer square root (exact, Newton's method) */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Integer") &&
      sp_streq(name, "sqrt") && argc == 1) {
    if (repr_of(c, argv[0]).big) {
      buf_puts(b, "sp_bigint_isqrt("); emit_expr(c, argv[0], b); buf_puts(b, ")");  /* (#2420) */
      return 1;
    }
    buf_puts(b, "sp_int_sqrt("); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }

  /* Marshal (Phase 1): dump a value to a binary String, load one back as poly */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Marshal")) {
    if (sp_streq(name, "dump") && argc == 1) {
      buf_puts(b, "sp_marshal_dump("); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* Marshal.dump(obj, io): the bytes go to the stream and the stream comes
       back. Written binary -- a dump is full of NULs, so the length has to
       come from the header rather than from strlen. */
    if (sp_streq(name, "dump") && argc == 2 && comp_ntype(c, argv[1]) == TY_IO) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_File *_t%d = ", t); emit_expr(c, argv[1], b);
      buf_printf(b, "; sp_File_write_bin(_t%d, sp_marshal_dump(", t);
      emit_boxed(c, argv[0], b);
      buf_printf(b, ")); _t%d; })", t);
      return 1;
    }
    if (sp_streq(name, "load") && argc == 1) {
      int t = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", t);
      /* Marshal.load takes either the bytes or an IO to read them from. The
         stream form used to reach emit_str_expr, which handed the sp_File*
         over as though the handle itself were the bytes (#4112). */
      if (comp_ntype(c, argv[0]) == TY_IO) {
        buf_puts(b, "sp_File_read("); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
else {
        emit_str_expr(c, argv[0], b);
      }
      buf_printf(b, "; sp_marshal_load(_t%d, (sp_int)sp_str_byte_len(_t%d)); })", t, t);
      return 1;
    }
  }

  /* Warning[category] / Warning[category] = flag / Warning.warn(msg): the
     category flags live in the runtime (sp_warning_*, lib/sp_cold.c), so a
     program can silence or re-enable a category at run time and Kernel#warn's
     `category:` gate reads the same flags. */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Warning")) {
    /* the category argument as a C string: a literal symbol becomes a string
       literal; a symbol-typed expression resolves through the symbol table */
    int cat_ok = argc >= 1 && nt_type(nt, argv[0]) &&
                 (sp_streq(nt_type(nt, argv[0]), "SymbolNode") ||
                  comp_ntype(c, argv[0]) == TY_SYMBOL);
    if (sp_streq(name, "[]") && argc == 1 && cat_ok) {
      buf_puts(b, "sp_warning_aref(");
      if (sp_streq(nt_type(nt, argv[0]), "SymbolNode"))
        emit_str_literal(b, nt_str(nt, argv[0], "value"));   /* escapes " and \ */
      else { buf_puts(b, "sp_sym_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "[]=") && argc == 2 && cat_ok) {
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tv);
      emit_boxed(c, argv[1], b);
      buf_puts(b, "; sp_warning_aset(");
      if (sp_streq(nt_type(nt, argv[0]), "SymbolNode"))
        emit_str_literal(b, nt_str(nt, argv[0], "value"));
      else { buf_puts(b, "sp_sym_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_printf(b, ", sp_poly_truthy(_t%d)); _t%d; })", tv, tv);
      return 1;
    }
    if (sp_streq(name, "warn") && argc >= 1) {
      buf_puts(b, "sp_warning_warn(");
      emit_str_expr(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
  }

  /* Math module functions -> C math.h equivalents */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Math")) {
    /* 1-arg functions */
    /* Domain-restricted functions route through sp_math_* wrappers that
       raise Math::DomainError on out-of-domain input (CRuby parity); the
       rest call libc directly (all reals are in domain). */
    const char *cfn = NULL;
    if      (sp_streq(name, "sin"))   cfn = "sin";
    else if (sp_streq(name, "cos"))   cfn = "cos";
    else if (sp_streq(name, "tan"))   cfn = "tan";
    else if (sp_streq(name, "asin"))  cfn = "sp_math_asin";
    else if (sp_streq(name, "acos"))  cfn = "sp_math_acos";
    else if (sp_streq(name, "atan"))  cfn = "atan";
    else if (sp_streq(name, "sinh"))  cfn = "sinh";
    else if (sp_streq(name, "cosh"))  cfn = "cosh";
    else if (sp_streq(name, "tanh"))  cfn = "tanh";
    else if (sp_streq(name, "asinh")) cfn = "asinh";
    else if (sp_streq(name, "acosh")) cfn = "sp_math_acosh";
    else if (sp_streq(name, "atanh")) cfn = "sp_math_atanh";
    else if (sp_streq(name, "exp"))   cfn = "exp";
    else if (sp_streq(name, "sqrt"))  cfn = "sp_math_sqrt";
    else if (sp_streq(name, "cbrt"))  cfn = "cbrt";
    /* expm1/log1p keep their precision near zero, where exp(x)-1 and log(1+x)
       cancel away most of the significant digits */
    else if (sp_streq(name, "expm1")) cfn = "expm1";
    else if (sp_streq(name, "log1p")) cfn = "log1p";
    else if (sp_streq(name, "erf"))   cfn = "erf";
    else if (sp_streq(name, "erfc"))  cfn = "erfc";
    else if (sp_streq(name, "gamma")) cfn = "sp_math_gamma";
    if (cfn && argc == 1) {
      /* emit_math_arg casts a plain int and coerces a poly value alike -- a
         bare `if (a0t==TY_INT) "(double)"` cast left a poly-typed arg (e.g.
         `Math.sqrt(dx*dx + dy*dy)` over locals that unify to Integer|Float)
         passed straight through as an unconvertible sp_RbVal -- and raises
         TypeError on a nil / String / non-numeric operand. */
      buf_printf(b, "%s(", cfn);
      emit_math_arg(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "lgamma") && argc == 1) {
      /* Math.lgamma(x) -> [log(|gamma(x)|), sign] as a poly array */
      buf_puts(b, "sp_math_lgamma("); emit_math_arg(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* Math.frexp(x) -> [fraction, exponent] as a poly array */
    if (sp_streq(name, "frexp") && argc == 1) {
      int te = ++g_tmp, tf = ++g_tmp, o = ++g_tmp;
      buf_printf(b, "({ int _t%d; sp_float _t%d = frexp(", te, tf);
      emit_math_arg(c, argv[0], b);
      buf_printf(b, ", &_t%d); sp_PolyArray *_t%d = sp_PolyArray_new();"
                    " sp_PolyArray_push(_t%d, sp_box_float(_t%d));"
                    " sp_PolyArray_push(_t%d, sp_box_int(_t%d)); _t%d; })",
                 te, o, o, tf, o, te, o);
      return 1;
    }
    /* Math.log(x) or Math.log(x, base) */
    if (sp_streq(name, "log") && (argc == 1 || argc == 2)) {
      if (argc == 1) {
        buf_puts(b, "sp_math_log(");
        emit_math_arg(c, argv[0], b);
        buf_puts(b, ")");
      }
      else {
        int t0 = ++g_tmp, t1 = ++g_tmp;
        /* the argument value is captured into a scratch buffer so a
           sub-expression that hoists (an array-literal builder for `[..].max`)
           writes to g_pre AHEAD of this declaration rather than inline into
           the middle of it (#2453). */
        Buf a0; memset(&a0, 0, sizeof a0); emit_math_arg(c, argv[0], &a0);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "double _t%d = %s;\n", t0, a0.p ? a0.p : "0"); free(a0.p);
        Buf a1; memset(&a1, 0, sizeof a1); emit_math_arg(c, argv[1], &a1);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "double _t%d = %s;\n", t1, a1.p ? a1.p : "0"); free(a1.p);
        /* log base 1 is NaN in CRuby (the division log(x)/log(1) is x/0 = Inf) */
        buf_printf(b, "(_t%d == 1.0 ? (0.0/0.0) : sp_math_log(_t%d) / sp_math_log(_t%d))", t1, t0, t1);
      }
      return 1;
    }
    /* Math.log2(x), Math.log10(x) */
    if (sp_streq(name, "log2") && argc == 1) {
      buf_puts(b, "sp_math_log2(");
      emit_math_arg(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "log10") && argc == 1) {
      buf_puts(b, "sp_math_log10(");
      emit_math_arg(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* Math.atan2(y, x), Math.hypot(x, y), Math.ldexp(x, e) */
    if ((sp_streq(name, "atan2") || sp_streq(name, "hypot")) && argc == 2) {
      buf_printf(b, "%s(", name);
      emit_math_arg(c, argv[0], b); buf_puts(b, ", ");
      emit_math_arg(c, argv[1], b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "ldexp") && argc == 2) {
      /* a Bignum exponent overflows a C long -> RangeError (CRuby), not a
         pointer-to-int cast that silently truncates to Infinity (#2616) */
      if (repr_of(c, argv[1]).big) {
        buf_puts(b, "((void)("); emit_math_arg(c, argv[0], b); buf_puts(b, "), (void)(");
        emit_expr(c, argv[1], b);
        buf_puts(b, "), (sp_raise_cls(\"RangeError\", \"bignum too big to convert into `long'\"), 0.0))");
        return 1;
      }
      buf_puts(b, "ldexp(");
      emit_math_arg(c, argv[0], b); buf_puts(b, ", (int)");
      /* the exponent may be a poly array element (`Math.ldexp(f[0], f[1])`):
         emit_int_expr coerces it to sp_int instead of casting a struct (#2592) */
      emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      return 1;
    }
  }

  /* JSON.generate(x) / JSON.dump(x) -> serialize a boxed value */
  /* JSON.generate/dump have NO special-case here: they flow through the native
     binding (packages/json -> sp_json_val). A Struct arg serializes via the
     generic sp_obj_to_hash reflection hook (codegen.c), reached from
     sp_json_val, which then serializes the resulting hash. */

  /* `x.to_json` -- CRuby's json defines it on every core class, so the idiom
     `{...}.to_json` inside a user #to_json is ordinary. A user class that
     defines its own to_json keeps the dispatch (this arm declines then). */
  if (recv >= 0 && sp_streq(name, "to_json") && nt_ref(nt, id, "block") < 0 &&
      sp_feature_required("json") && json_to_json_is_builtin(c, recv)) {
    /* every builtin's #to_json takes an optional state and nothing more
       (a boxed one, whose class the arity guard cannot name, included) */
    if (argc > 1) {
      buf_puts(b, "({ (void)("); emit_boxed(c, recv, b); buf_puts(b, "); ");
      for (int a = 0; a < argc; a++) { buf_puts(b, "(void)("); emit_boxed(c, argv[a], b); buf_puts(b, "); "); }
      buf_printf(b, "sp_raise_cls(\"ArgumentError\", \"wrong number of arguments (given %d, expected 0..1)\");"
                    " (const char *)0; })", argc);
      return 1;
    }
    for (int a = 0; a < argc; a++) { buf_puts(b, "((void)("); emit_boxed(c, argv[a], b); buf_puts(b, "), "); }
    buf_puts(b, "sp_json_val("); emit_boxed(c, recv, b); buf_puts(b, ")");
    for (int a = 0; a < argc; a++) buf_puts(b, ")");
    return 1;
  }

  /* Dir.exist? -> directory test; Dir.exists? was removed in Ruby 4.0 (#2780) */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir") &&
      (is_exist_alias(name)) && argc == 1) {
    if (sp_streq(name, "exists?")) {
      buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
      buf_puts(b, "); sp_raise_cls(\"NoMethodError\", \"undefined method 'exists?' for class Dir\"); (sp_bool)0; })");
      return 1;
    }
    buf_puts(b, "sp_file_directory("); emit_path_expr(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  return 0;
}

/* the class methods of the builtin classes named by a constant (Thread, an exception class, ...), ahead of the Class.new dispatch */
/* A family (or socktype) argument of Socket.getaddrinfo: nil or absent is 0
   (unrestricted), anything else goes to sp_sock_addrinfo_hint, which reads an
   Integer, or a String or Symbol name, as CRuby does. */
static void emit_getaddrinfo_hint(Compiler *c, int arg, int is_family, Buf *b) {
  if (arg < 0 || nt_kind(c->nt, arg) == NK_NilNode) {
    buf_puts(b, "0");
    return;
  }
  buf_puts(b, "sp_sock_addrinfo_hint(");
  emit_boxed(c, arg, b);
  buf_printf(b, ", %d)", is_family);
}

int emit_call_builtin_cmethod_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* Thread class methods: Thread.current / Thread.pass (recv is the Thread
     constant). Handled before the Class.new dispatch since they are not `new`. */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")) {
    const char *tcn = nt_str(nt, recv, "name");
    /* IO.popen is not implemented: refused at compile time, as an
       unsupported API is, rather than a NoMethodError the first time the
       line runs (#7199). A program's own IO.popen is its own. */
    /* Time.parse / Time.strptime, refused here under --defer-refusals (the
       pass ahead of emission refuses them otherwise; see reject_time_parse) */
    { char tpm[320];
      if (defer_refusals() && time_parse_refusal(c, id, tpm, sizeof tpm)) {
        unsupported_feature(c, id, tpm);
        buf_puts(b, "sp_box_nil()");
        return 1;
      } }
    if (tcn && (sp_streq(tcn, "IO") || sp_streq(tcn, "File")) && sp_streq(name, "popen")) {
      int ioc = comp_class_index(c, tcn);
      if (ioc < 0 || comp_cmethod_in_chain(c, ioc, name, NULL) < 0) {
        unsupported_feature(c, id, "IO.popen is not supported (Open3.capture2 / capture3 or Process.spawn cover its uses)");
        buf_puts(b, "sp_box_nil()");
        return 1;
      }
    }
    /* Exception class-level: Cls.exception(msg) is Cls.new (#2740);
       Exception.to_tty? reports whether stderr is a terminal (#2757). */
    if (tcn && is_exc_name(tcn)) {
      if (sp_streq(name, "exception")) {
        if (emit_syserr_family_new(c, id, tcn, argc, argv, b)) return 1;
        buf_printf(b, "sp_exc_new(\"%s\", ", tcn);
        emit_exc_msg_arg(c, argc >= 1 ? argv[0] : -1, b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "to_tty?") && argc == 0) {
        buf_puts(b, "(isatty(2) != 0)"); return 1;
      }
    }
    /* Addrinfo.tcp(host, port) / .udp(host, port) / .ip(host) / .unix(path) */
    if (tcn && sp_streq(tcn, "Addrinfo") && sp_feature_required("socket")) {
      int is_unix = sp_streq(name, "unix");
      if ((sp_streq(name, "tcp") || sp_streq(name, "udp")) && argc == 2) {
        buf_puts(b, "sp_addrinfo_new("); emit_str_expr(c, argv[0], b); buf_puts(b, ", ");
        emit_int_expr(c, argv[1], b);
        buf_printf(b, ", sp_sock_const(\"%s\"), 0)", sp_streq(name, "udp") ? "SOCK_DGRAM" : "SOCK_STREAM");
        return 1;
      }
      if (sp_streq(name, "ip") && argc == 1) {
        buf_puts(b, "sp_addrinfo_new("); emit_str_expr(c, argv[0], b); buf_puts(b, ", 0, 0, 0)");
        return 1;
      }
      if (is_unix && argc == 1) {
        buf_puts(b, "sp_addrinfo_new("); emit_str_expr(c, argv[0], b);
        buf_puts(b, ", 0, sp_sock_const(\"SOCK_STREAM\"), 1)");
        return 1;
      }
    }
    /* BasicSocket.do_not_reverse_lookup and its writer, on any socket class:
       one setting they share, which each new socket takes. The writer
       answers its argument; only its truth sets the default. */
    if (tcn && sp_feature_required("socket") && io_family_name(tcn) && !is_io_class_name(tcn) &&
        ((sp_streq(name, "do_not_reverse_lookup") && argc == 0) ||
         (sp_streq(name, "do_not_reverse_lookup=") && argc == 1))) {
      int skc = comp_class_index(c, tcn);
      if (skc < 0 || comp_cmethod_in_chain(c, skc, name, NULL) < 0) {
        if (argc == 0) { buf_puts(b, "sp_sock_dnrl_default"); return 1; }
        int tv = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", tv);
        emit_boxed(c, argv[0], b);
        buf_printf(b, "; sp_sock_dnrl_default = sp_poly_truthy(_t%d); ", tv);
        emit_unbox_or_keep(c, repr_of(c, id).as_ty, tv, b);
        buf_puts(b, "; })");
        return 1;
      }
    }
    /* Socket class methods (#2922) */
    if (tcn && sp_streq(tcn, "Socket") && sp_feature_required("socket")) {
      if (sp_streq(name, "gethostname") && argc == 0) {
        buf_puts(b, "sp_sock_gethostname()"); return 1;
      }
      /* Socket.pair / .socketpair -> [end0, end1]; the runtime creates both on
         the first call and hands back the second on the next. */
      if ((is_socket_pair_alias(name)) && argc >= 2) {
        int tp0 = ++g_tmp, tp1 = ++g_tmp, tpa = ++g_tmp;
        buf_printf(b, "({ sp_File *_t%d = sp_sock_pair_end(", tp0);
        emit_int_expr(c, argv[0], b); buf_puts(b, ", ");
        emit_int_expr(c, argv[1], b); buf_puts(b, ", ");
        if (argc >= 3) emit_int_expr(c, argv[2], b); else buf_puts(b, "0");
        buf_printf(b, ", 0); SP_GC_ROOT(_t%d); sp_File *_t%d = sp_sock_pair_end(0, 0, 0, 1);"
                      " SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_obj(_t%d, SP_BUILTIN_IO));"
                      " sp_PolyArray_push(_t%d, sp_box_obj(_t%d, SP_BUILTIN_IO)); _t%d; })",
                   tp0, tp1, tp1, tpa, tpa, tpa, tp0, tpa, tp1, tpa);
        return 1;
      }
      /* Socket.getaddrinfo(host, port) ->
         [[family, port, host, ip, pfamily, socktype, protocol], ...].
         The rows are built in the runtime: the family constants are
         platform-dependent and the walk is a plain loop, neither of which
         belongs in emitted C. */
      /* Socket.sockaddr_in(port, host) and its documented alias
         pack_sockaddr_in -> the packed sockaddr String. The layouts and the
         AF_* values are platform-dependent, so the packing is a runtime call
         rather than emitted C (#4137). */
      if ((sp_streq(name, "sockaddr_in") || sp_streq(name, "pack_sockaddr_in")) && argc == 2) {
        buf_puts(b, "sp_sock_pack_sockaddr_in(");
        emit_int_expr(c, argv[0], b);
        buf_puts(b, ", ");
        emit_str_expr(c, argv[1], b);
        buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "sockaddr_un") || sp_streq(name, "pack_sockaddr_un")) && argc == 1) {
        buf_puts(b, "sp_sock_pack_sockaddr_un(");
        emit_str_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "unpack_sockaddr_in") && argc == 1) {
        buf_puts(b, "sp_sock_unpack_sockaddr_in(");
        emit_str_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "unpack_sockaddr_un") && argc == 1) {
        buf_puts(b, "sp_sock_unpack_sockaddr_un(");
        emit_str_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "getaddrinfo") && argc >= 2) {
        buf_puts(b, "sp_sock_getaddrinfo(");
        emit_str_expr(c, argv[0], b);
        buf_puts(b, ", ");
        if (nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "NilNode")) buf_puts(b, "0");
        else emit_int_expr(c, argv[1], b);
        /* the family and socktype restrict the answer, as in CRuby */
        buf_puts(b, ", ");
        emit_getaddrinfo_hint(c, argc > 2 ? argv[2] : -1, 1, b);
        buf_puts(b, ", ");
        emit_getaddrinfo_hint(c, argc > 3 ? argv[3] : -1, 0, b);
        buf_puts(b, ")");
        return 1;
      }
    }
    /* IO.pipe / IO.copy_stream / IO.sysopen (#2815) */
    if (tcn && sp_streq(tcn, "IO")) {
      if (sp_streq(name, "pipe") && argc == 0) { buf_puts(b, "sp_io_pipe()"); return 1; }
      /* IO.for_fd(fd [, mode] [, autoclose: bool]). IO.new takes the same
         descriptor form -- it is how CRuby spells "wrap this fd", and the way
         to reach a File::Stat from one. */
      if ((sp_streq(name, "for_fd") || sp_streq(name, "new")) && argc >= 1) {
        const char *lty = nt_type(nt, argv[argc - 1]);
        int kwh = (lty && sp_streq(lty, "KeywordHashNode")) ? argv[argc - 1] : -1;
        int ac = kwh >= 0 ? kwh_lookup(nt, kwh, "autoclose") : -1;
        buf_puts(b, "sp_io_for_fd(");
        emit_int_expr(c, argv[0], b);
        buf_puts(b, ", ");
        /* the positional mode rides through whatever its inferred type: a
           STRING as-is, a widened (poly) one unboxed -- the TY_STRING-only
           guard dropped a mode variable that widened in a larger program
           (PR #4209's observation). argv[1] may also BE the keyword hash. */
        if (argc >= 2 && argv[1] != kwh && comp_ntype(c, argv[1]) == TY_STRING)
          emit_expr(c, argv[1], b);
        else if (argc >= 2 && argv[1] != kwh && repr_of(c, argv[1]).kind == RK_BOXED) {
          buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[1], b); buf_puts(b, ")");
        }
        /* no mode: the runtime derives it from the fd's own access mode
           (a fixed "r" made fdopen fail on a write-only fd, #4208) */
        else buf_puts(b, "\"\"");
        buf_puts(b, ", ");
        if (ac >= 0) { buf_puts(b, "("); emit_expr(c, ac, b); buf_puts(b, ")"); }
        else buf_puts(b, "1");
        buf_puts(b, ")");
        return 1;
      }
      /* IO.select(read, write, error, timeout): a nil array watches nothing */
      if (sp_streq(name, "select") && argc >= 1 && argc <= 4) {
        buf_puts(b, "sp_io_select(");
        for (int k = 0; k < 3; k++) {
          if (k) buf_puts(b, ", ");
          if (k >= argc || (nt_type(nt, argv[k]) && sp_streq(nt_type(nt, argv[k]), "NilNode")))
            buf_puts(b, "NULL");
          else emit_expr(c, argv[k], b);
        }
        buf_puts(b, ", ");
        if (argc >= 4 && !(nt_type(nt, argv[3]) && sp_streq(nt_type(nt, argv[3]), "NilNode")))
          emit_float_expr(c, argv[3], b);
        else buf_puts(b, "-1.0");
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "copy_stream") && argc == 2) {
        /* IO.copy_stream(src, dst): read the whole source, write it to the dest,
           returning the byte count. sp_io_copy_stream takes two path strings, so
           a StringIO (#3216) or an IO/socket (#3217, an sp_File*) endpoint passed
           straight in was an incompatible-pointer error. When both endpoints are
           stream objects (StringIO or IO), read/write them by kind; two path
           strings keep the filename copy. */
        int a0_sio = node_is_stringio(c, argv[0]), a0_io = comp_ntype(c, argv[0]) == TY_IO;
        int a1_sio = node_is_stringio(c, argv[1]), a1_io = comp_ntype(c, argv[1]) == TY_IO;
        int a0_poly = repr_of(c, argv[0]).kind == RK_BOXED;
        int a1_poly = repr_of(c, argv[1]).kind == RK_BOXED;
        /* A boxed endpoint may hold a path, including a shared String handle,
           or a stream. Check its kind before reading the payload as sp_File. */
        if (a0_poly || a1_poly) {
          int sio_cid = comp_class_index(c, "StringIO");
          int td = ++g_tmp;
          buf_printf(b, "({ const char *_t%d = ", td);
          if (a0_poly) {
            int ts = ++g_tmp;
            buf_puts(b, "({ "); ts = hold_operand(c, argv[0], TY_POLY, 0, ts, 1, " ", b);
            buf_printf(b, "(_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d))"
                          " ? sp_file_read(sp_poly_unbox_s(_t%d)) : ", ts, ts, ts);
            if (sio_cid >= 0)
              buf_printf(b, "_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == %d"
                            " ? sp_StringIO_read((sp_StringIO *)_t%d.v.p) : ", ts, ts, sio_cid, ts);
            buf_printf(b, "_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_IO"
                          " ? sp_File_read((sp_File *)_t%d.v.p)"
                          " : (sp_raise_nomethod(sp_nomethod_msg(\"read\", _t%d)), (const char *)0); })", ts, ts, ts, ts);
          }
          else if (a0_sio) { buf_puts(b, "sp_StringIO_read("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
          else if (a0_io) { buf_puts(b, "sp_File_read("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
          else { buf_puts(b, "sp_file_read("); emit_path_expr(c, argv[0], b); buf_puts(b, ")"); }
          buf_printf(b, "; SP_GC_ROOT(_t%d); ", td);
          if (a1_poly) {
            int tdd = ++g_tmp;
            tdd = hold_operand(c, argv[1], TY_POLY, 0, tdd, 1, " ", b);
            buf_printf(b, "(_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d))"
                          " ? sp_file_write(sp_poly_unbox_s(_t%d), _t%d) : ", tdd, tdd, tdd, td);
            if (sio_cid >= 0)
              buf_printf(b, "_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == %d"
                            " ? sp_StringIO_write((sp_StringIO *)_t%d.v.p, _t%d) : ", tdd, tdd, sio_cid, tdd, td);
            buf_printf(b, "_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_IO"
                          " ? sp_File_write_bin((sp_File *)_t%d.v.p, _t%d)"
                          " : (sp_raise_nomethod(sp_nomethod_msg(\"write\", _t%d)), (sp_int)0); })", tdd, tdd, tdd, td, tdd);
          }
          else if (a1_sio) { buf_puts(b, "sp_StringIO_write("); emit_expr(c, argv[1], b); buf_printf(b, ", _t%d); })", td); }
          else if (a1_io) { buf_puts(b, "sp_File_write_bin("); emit_expr(c, argv[1], b); buf_printf(b, ", _t%d); })", td); }
          else { buf_puts(b, "sp_file_write("); emit_path_expr(c, argv[1], b); buf_printf(b, ", _t%d); })", td); }
          return 1;
        }
        if ((a0_sio || a0_io) && (a1_sio || a1_io)) {
          buf_puts(b, a1_sio ? "sp_StringIO_write(" : "sp_File_write_bin(");
          emit_expr(c, argv[1], b);
          buf_puts(b, a0_sio ? ", sp_StringIO_read(" : ", sp_File_read(");
          emit_expr(c, argv[0], b);
          buf_puts(b, "))"); return 1;
        }
        buf_puts(b, "sp_io_copy_stream("); emit_path_expr(c, argv[0], b); buf_puts(b, ", ");
        emit_path_expr(c, argv[1], b); buf_puts(b, ")"); return 1;
      }
      if (sp_streq(name, "sysopen") && argc >= 1) {
        buf_puts(b, "sp_io_sysopen("); emit_path_expr(c, argv[0], b);
        buf_puts(b, ", ");
        if (argc >= 2) emit_int_expr(c, argv[1], b); else buf_puts(b, "0");
        buf_puts(b, ", ");
        if (argc >= 3) emit_int_expr(c, argv[2], b); else buf_puts(b, "0");
        buf_puts(b, ")"); return 1;
      }
    }
    /* Signal module queries (#2735) */
    if (tcn && sp_streq(tcn, "Signal") && sp_streq(name, "list") && argc == 0) {
      buf_puts(b, "sp_signal_list()"); return 1;
    }
    if (tcn && sp_streq(tcn, "Signal") && sp_streq(name, "signame") && argc == 1) {
      /* signame takes an Integer; a statically non-Integer argument is a
         TypeError, not a bogus name or a compile abort (#3075, #3076) */
      TyKind sa0 = comp_ntype(c, argv[0]);
      if (sa0 != TY_INT && sa0 != TY_BIGINT && sa0 != TY_FLOAT &&
          sa0 != TY_POLY && sa0 != TY_UNKNOWN) {
        buf_puts(b, "((void)("); emit_boxed(c, argv[0], b);
        buf_puts(b, "), (sp_raise_cls(\"TypeError\", \"no implicit conversion to integer\"), (const char *)0))");
        return 1;
      }
      buf_puts(b, "sp_signal_signame(");
      /* a Float argument truncates toward zero, as CRuby's to_int does (#3105) */
      if (sa0 == TY_FLOAT) { buf_puts(b, "sp_float_arg_i("); emit_float_expr(c, argv[0], b); buf_puts(b, ")"); }
      else emit_int_expr(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    /* Process.kill(sig, *pids): per-pid sends, counting successes (#2750) */
    if (tcn && sp_streq(tcn, "Process") && sp_streq(name, "kill") && argc >= 2) {
      g_uses_symbols = 1;   /* :USR1 designators resolve through the sym table */
      int tk5 = ++g_tmp;
      buf_printf(b, "({ sp_int _t%d = 0;", tk5);
      for (int k = 1; k < argc; k++) {
        buf_printf(b, " _t%d += sp_process_kill1(", tk5);
        emit_boxed(c, argv[0], b);
        buf_puts(b, ", ");
        emit_int_expr(c, argv[k], b);
        buf_puts(b, ");");
      }
      buf_printf(b, " _t%d; })", tk5);
      return 1;
    }
    /* Process.spawn(cmd, *args, opts) -- CRuby-compatible: cmd is the
       first arg; remaining args are flattened into a positional array;
       the LAST arg is treated as opts only if it is a Hash literal.
       The opts hash is unpacked at the call site: for each known key
       (:in, :out, :err, :pgroup, :rlimit_cpu, :rlimit_as, :chdir) we
       emit a sp_poly_hash_get_pair_val call, resolve the value to a
       primitive (Integer fd, true, Integer, String), and push it into
       a flat PolyArray. The runtime (sp_process_spawn) takes the
       8-element flat array positionally -- this keeps the runtime
       TU out of spinel_rt.h's static-inline family entirely. The
       [:child, :out|:err|Integer] redirect is recognized inline. */
    if (tcn && sp_streq(tcn, "Process") && (sp_streq(name, "spawn") || sp_streq(name, "exec")) && argc >= 1) {
      int is_exec = sp_streq(name, "exec");
      g_uses_symbols = 1;
      int tcmd = ++g_tmp;
      int targs = ++g_tmp;
      int topts = ++g_tmp;
      int extra = argc - 1;
      /* Detect if the last positional arg is a Hash (opts). A splat is the
         argument list, not the options. */
      int last_is_opts = 0;
      if (extra >= 1 && nt_kind(nt, argv[argc - 1]) != NK_SplatNode) {
        TyKind ltk = comp_ntype(c, argv[argc - 1]);
        if (ltk == TY_SYM_POLY_HASH || ltk == TY_STR_POLY_HASH ||
            ltk == TY_POLY_POLY_HASH || ltk == TY_UNKNOWN ||
            ltk == TY_POLY) {
          last_is_opts = 1;
        }
      }
      /* exec's options (chdir:, redirections) are not taken: refused, not
         dropped */
      if (is_exec && last_is_opts) {
        unsupported_feature(c, id, "exec with an options Hash (Process.spawn takes chdir: and the redirections)");
        buf_puts(b, "0");
        return 1;
      }
      int n_args = extra - (last_is_opts ? 1 : 0);
      int has_splat = 0;
      for (int k = 0; k <= n_args; k++) has_splat |= nt_kind(nt, argv[k]) == NK_SplatNode;
      if (has_splat) {
        /* `spawn(*args)`: the command and its arguments are spread at run
           time, then split, as the literal list is (#7192) */
        int tall = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tall, tall);
        for (int k = 0; k <= n_args; k++) {
          if (nt_kind(nt, argv[k]) == NK_SplatNode) {
            int se = nt_ref(nt, argv[k], "expression");
            int ts = ++g_tmp;
            buf_printf(b, " { sp_PolyArray *_t%d = sp_poly_to_poly_array(", ts);
            if (se >= 0) emit_boxed(c, se, b); else buf_puts(b, "sp_box_nil()");
            buf_printf(b, "); for (sp_int _i = 0; _i < _t%d->len; _i++) sp_PolyArray_push(_t%d, _t%d->data[_i]); }",
                       ts, tall, ts);
          }
          else {
            buf_printf(b, " sp_PolyArray_push(_t%d, ", tall);
            emit_boxed(c, argv[k], b);
            buf_puts(b, ");");
          }
        }
        buf_printf(b, " if (_t%d->len == 0) sp_raise_cls(\"ArgumentError\", \"wrong number of arguments (given 0, expected 1+)\");", tall);
        buf_printf(b, " sp_RbVal _t%d = _t%d->data[0];", tcmd, tall);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", targs, targs);
        buf_printf(b, " for (sp_int _i = 1; _i < _t%d->len; _i++) sp_PolyArray_push(_t%d, _t%d->data[_i]);",
                   tall, targs, tall);
      }
      else {
        /* cmd = argv[0] (boxed, so it can be String or Array) */
        buf_printf(b, "({ sp_RbVal _t%d = ", tcmd);
        emit_boxed(c, argv[0], b);
        buf_puts(b, ";");
        /* args: collect argv[1..argc-2] (or empty if argc==1) into a
           PolyArray. Skip the last arg if it's a Hash (the opts). */
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", targs, targs);
        for (int k = 1; k <= n_args; k++) {
          buf_printf(b, " sp_PolyArray_push(_t%d, ", targs);
          emit_boxed(c, argv[k], b);
          buf_puts(b, ");");
        }
      }
      /* a leading Hash is the child's environment and the command follows
         it; through a splat or a boxed value it is told apart only at run
         time, so the test is made there */
      int tenv = 0;
      TyKind tfirst = nt_kind(nt, argv[0]) == NK_SplatNode ? TY_POLY : comp_ntype(c, argv[0]);
      if (!is_exec && (ty_is_hash(tfirst) || tfirst == TY_POLY || tfirst == TY_UNKNOWN)) {
        tenv = ++g_tmp;
        buf_printf(b, " sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);", tenv, tenv);
        buf_printf(b, " if (sp_json_kind(_t%d) == 2) { if (_t%d->len == 0) sp_raise_cls(\"ArgumentError\", \"wrong number of arguments (given 0, expected 1+)\");"
                      " _t%d = _t%d; _t%d = sp_PolyArray_shift(_t%d); }",
                   tcmd, targs, tenv, tcmd, tcmd, targs);
      }
      /* a [program, argv0] pair of Strings arrives as a String array: the
         runtime reads it as a general one */
      buf_printf(b, " if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id != SP_BUILTIN_POLY_ARRAY && sp_rbval_is_array(_t%d))"
                    " _t%d = sp_box_poly_array(sp_poly_to_poly_array(_t%d));", tcmd, tcmd, tcmd, tcmd, tcmd);
      /* opts: build an 8-element flat PolyArray
         [in_fd, out_fd, err_fd, pgroup, rlimit_cpu, rlimit_as, chdir, owned].
         If last_is_opts, each entry is a hash lookup result resolved
         to a primitive. If not, all entries are nil/0. */
      buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", topts, topts);
      int town = ++g_tmp;
      buf_printf(b, " int _t%d[3] = { -1, -1, -1 };", town);
      if (last_is_opts) {
        int tth = ++g_tmp;
        int tkv = ++g_tmp;
        int tfd = ++g_tmp;
        buf_printf(b, " sp_RbVal _t%d = ", tth);
        emit_boxed(c, argv[argc - 1], b);
        buf_puts(b, ";");
        /* For the three fd slots (in/out/err): look up the value and
           resolve it. The C block is emitted three times with the
           key name changed -- a top-level static helper would be
           cleaner but cannot be declared inside a statement
           expression. A filename is opened by the runtime, read-only
           for :in and write-truncate for :out and :err, and the fds
           it opened are recorded in _town so the runtime can close
           the parent's copies once the child has its own; a caller's
           IO is never closed. */
        const char *fd_keys[] = { "in", "out", "err" };
        for (int i = 0; i < 3; i++) {
          buf_printf(b,
            "{ sp_RbVal _t%d; sp_bool _t%d; "
            "_t%d = sp_poly_hash_get_pair_val(_t%d, "
            "sp_box_sym(sp_sym_intern(\"%s\")), &_t%d); "
            "if (_t%d) { "
            "  sp_RbVal _v = _t%d; "
            "  if (_v.tag == SP_TAG_NIL) { "
            "    sp_PolyArray_push(_t%d, sp_box_int(-1)); "
            "  }\nelse if (_v.tag == SP_TAG_BOOL && !_v.v.i) { "
            "    sp_PolyArray_push(_t%d, sp_box_int(-1)); "
            "  }\nelse if (_v.tag == SP_TAG_INT) { "
            "    sp_PolyArray_push(_t%d, _v); "
            "  }\nelse if (_v.tag == SP_TAG_OBJ && _v.cls_id == SP_BUILTIN_IO && _v.v.p) { "
            "    sp_PolyArray_push(_t%d, sp_box_int(sp_File_fileno((sp_File*)_v.v.p))); "
            "  }\nelse if (_v.tag == SP_TAG_STR) { "
            "    sp_PolyArray_push(_t%d, sp_box_int(sp_process_open_redirect(_v.v.s, %d, _t%d))); "
            "  }\nelse if (_v.tag == SP_TAG_OBJ && _v.cls_id == SP_BUILTIN_POLY_ARRAY) { "
            "    sp_PolyArray *_a = (sp_PolyArray*)_v.v.p; "
            "    if (_a->len >= 2 && _a->data[0].tag == SP_TAG_SYM "
            "        && _a->data[0].v.i == sp_sym_intern(\"child\")) { "
            "      sp_RbVal _fdv = _a->data[1]; "
            "      if (_fdv.tag == SP_TAG_SYM) { "
            "        if (_fdv.v.i == sp_sym_intern(\"out\")) { "
            "          sp_PolyArray_push(_t%d, sp_box_int(1)); "
            "        }\nelse if (_fdv.v.i == sp_sym_intern(\"err\")) { "
            "          sp_PolyArray_push(_t%d, sp_box_int(2)); "
            "        }\nelse { "
            "          sp_process_spawn_fail(_t%d, \"ArgumentError\", \"bad redirect value\"); "
            "        } "
            "      }\nelse if (_fdv.tag == SP_TAG_INT) { "
            "        sp_PolyArray_push(_t%d, sp_box_int((int)_fdv.v.i)); "
            "      }\nelse { "
            "        sp_process_spawn_fail(_t%d, \"ArgumentError\", \"bad redirect value\"); "
            "      } "
            "    }\nelse { "
            "      sp_process_spawn_fail(_t%d, \"ArgumentError\", \"bad child-array shape\"); "
            "    } "
            "  }\nelse { "
            "    sp_process_spawn_fail(_t%d, \"ArgumentError\", \"bad redirect type\"); "
            "  } "
            "}\nelse { sp_PolyArray_push(_t%d, sp_box_int(-1)); } }",
            tkv, tfd, tkv, tth, fd_keys[i], tfd,
            tfd, tkv,
            topts,
            topts,
            topts,
            topts,
            topts, i, town,
            topts,
            topts,
            town,
            topts,
            town,
            town,
            town,
            topts);
        }
        /* pgroup: look up, accept true / 0 / Integer, or 0. */
        buf_printf(b,
          " { sp_RbVal _t%d; sp_bool _t%d; "
          "_t%d = sp_poly_hash_get_pair_val(_t%d, "
          "sp_box_sym(sp_sym_intern(\"pgroup\")), &_t%d); "
          "if (_t%d && _t%d.tag == SP_TAG_BOOL && _t%d.v.i) "
          "{ sp_PolyArray_push(_t%d, sp_box_int(1)); } "
          "\nelse if (_t%d && _t%d.tag == SP_TAG_INT) "
          "{ sp_PolyArray_push(_t%d, _t%d); } "
          "\nelse { sp_PolyArray_push(_t%d, sp_box_nil()); } }",
          tkv, tfd, tkv, tth, tfd,
          tfd, tkv, tkv, topts,
          tfd, tkv, topts, tkv,
          topts);
        /* rlimit_cpu, rlimit_as: look up, must be Integer, or nil. */
        const char *rlim_keys[] = { "rlimit_cpu", "rlimit_as" };
        for (int i = 0; i < 2; i++) {
          buf_printf(b,
            " { sp_RbVal _t%d; sp_bool _t%d; "
            "_t%d = sp_poly_hash_get_pair_val(_t%d, "
            "sp_box_sym(sp_sym_intern(\"%s\")), &_t%d); "
            "if (_t%d && _t%d.tag == SP_TAG_INT) "
            "{ sp_PolyArray_push(_t%d, _t%d); } "
            "\nelse { sp_PolyArray_push(_t%d, sp_box_nil()); } }",
            tkv, tfd, tkv, tth, rlim_keys[i], tfd,
            tfd, tkv, topts, tkv,
            topts);
        }
        /* chdir: look up, must be String, or nil. */
        buf_printf(b,
          " { sp_RbVal _t%d; sp_bool _t%d; "
          "_t%d = sp_poly_hash_get_pair_val(_t%d, "
          "sp_box_sym(sp_sym_intern(\"chdir\")), &_t%d); "
          "if (_t%d && _t%d.tag == SP_TAG_STR) "
          "{ sp_PolyArray_push(_t%d, _t%d); } "
          "\nelse { sp_PolyArray_push(_t%d, sp_box_nil()); } }",
          tkv, tfd, tkv, tth, tfd,
          tfd, tkv, topts, tkv,
          topts);
      }
      else {
        /* No opts: push defaults (nils / 0). */
        for (int i = 0; i < 3; i++)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(-1));", topts);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_nil());", topts);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_nil());", topts);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_nil());", topts);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_nil());", topts);
      }
      /* Slot 7: which of in/out/err the runtime opened itself, a bit per
         slot, so it closes the parent's copies and never a caller's IO. */
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int((_t%d[0] >= 0) | ((_t%d[1] >= 0) << 1) | ((_t%d[2] >= 0) << 2)));",
                 topts, town, town, town);
      /* Slot 8, when the call can have one: the environment Hash, or nil */
      if (tenv) buf_printf(b, " sp_PolyArray_push(_t%d, _t%d);", topts, tenv);
      if (is_exec)   /* Kernel#exec: the process becomes the command, or raises its Errno */
        buf_printf(b, " sp_process_exec(_t%d, sp_box_poly_array(_t%d)); sp_int _r = 0; (void)_t%d;", tcmd, targs, topts);
      else
        buf_printf(b, " sp_int _r = sp_process_spawn(_t%d, sp_box_poly_array(_t%d), sp_box_poly_array(_t%d));", tcmd, targs, topts);
      buf_printf(b, " _r; })\n");
      return 1;
    }
    /* Process.last_status is $? (#7196) */
    if (tcn && sp_streq(tcn, "Process") && sp_streq(name, "last_status") && argc == 0) {
      buf_puts(b, "sp_last_process_status()");
      return 1;
    }
    /* Process.waitpid2(pid) -> [pid, raw_status]: the runtime hands back a
       2-element PolyArray, unboxed. The call's own type comes from
       infer_call, like every other arm here -- g_ret_type is the enclosing
       FUNCTION's return type, and writing it at a call site leaked into
       every later `return` in that function. */
    /* Process.wait2 is the same call; with no pid, either waits for any
       child (-1) */
    if (tcn && sp_streq(tcn, "Process") &&
        (sp_streq(name, "waitpid2") || sp_streq(name, "wait2")) && argc <= 1) {
      buf_puts(b, "sp_process_waitpid2(");
      if (argc == 1) emit_int_expr(c, argv[0], b);
      else buf_puts(b, "-1");
      buf_puts(b, ")");
      return 1;
    }
    /* Process.wait / waitpid: the pid reaped, its status left in $? */
    if (tcn && sp_streq(tcn, "Process") &&
        (sp_streq(name, "wait") || sp_streq(name, "waitpid")) && argc <= 1) {
      buf_puts(b, "sp_process_waitpid(");
      if (argc == 1) emit_int_expr(c, argv[0], b);
      else buf_puts(b, "-1");
      buf_puts(b, ")");
      return 1;
    }
    /* Process::Status.new(status_int) is handled earlier in
       emit_call_body (the ConstantPathNode arm above); the
       ConstantReadNode-scoped block here does not see it. */
    if (tcn && sp_streq(tcn, "Thread") && sp_streq(name, "current") && argc == 0) {
      buf_puts(b, "sp_Thread_current()"); return 1;
    }
    if (tcn && sp_streq(tcn, "Thread") && sp_streq(name, "main") && argc == 0) {
      buf_puts(b, "sp_Thread_main()"); return 1;
    }
    if (tcn && sp_streq(tcn, "Thread") && sp_streq(name, "list") && argc == 0) {
      /* build a poly array of the live threads (the TU owns sp_PolyArray) */
      int ta = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
      buf_printf(b, " sp_int _t%d = sp_Thread_list_count();", tn);
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)"
                    " sp_PolyArray_push(_t%d, sp_box_obj((void *)sp_Thread_list_at(_t%d), SP_BUILTIN_THREAD));",
                 ti, ti, tn, ti, ta, ti);
      buf_printf(b, " _t%d; })", ta);
      return 1;
    }
    if (tcn && sp_streq(tcn, "Thread") && sp_streq(name, "pass") && argc == 0) {
      /* Thread.pass yields the scheduler and evaluates to nil. */
      buf_puts(b, "(sp_Thread_pass(), sp_box_nil())"); return 1;
    }
    if (tcn && sp_streq(tcn, "Thread") && sp_streq(name, "stop") && argc == 0) {
      /* Thread.stop sleeps until #wakeup and evaluates to nil. */
      buf_puts(b, "(sp_Thread_stop(), sp_box_nil())"); return 1;
    }
    if (tcn && sp_streq(tcn, "Thread") && sp_streq(name, "report_on_exception=") && argc == 1) {
      buf_puts(b, "sp_Thread_set_report_default(");
      emit_coerce(c, argv[0], TY_BOOL, CO_CONVERT, "Thread.report_on_exception=", b);
      buf_puts(b, ")"); return 1;
    }
    if (tcn && sp_streq(tcn, "Thread") && sp_streq(name, "report_on_exception") && argc == 0) {
      buf_puts(b, "sp_Thread_get_report_default()"); return 1;
    }
  }
  return 0;
}

/* Enumerator and Random: an Enumerator's methods, Random's class methods, Enumerator.product, and a Random instance's methods */
int emit_call_enum_random_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* Enumerator instance methods: #next / #peek (raise StopIteration past the
     end), #rewind (reset, returns self), #size. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_ENUMERATOR) {
    /* the readers that render the receiver and the arguments: builtin-op rows */
    if (emit_builtin_op(c, id, recv, TY_ENUMERATOR, name, b)) return 1;
    /* find_index(v) walks it only as far as the hit, so an endless one
       answers too */
    if (sp_streq(name, "find_index") && argc == 1 && nt_ref(nt, id, "block") < 0) {
      int t = ++g_tmp;
      int as_int = repr_of(c, id).as_ty == TY_INT;
      if (as_int && !node_is_oint(c, id)) buf_puts(b, "sp_oint_arg(");
      buf_printf(b, "({ sp_oint _t%d = sp_enum_find_index_val(sp_box_obj(", t);
      emit_expr(c, recv, b);
      buf_puts(b, ", SP_BUILTIN_ENUMERATOR), ");
      emit_boxed(c, argv[0], b);
      if (as_int) buf_printf(b, "); _t%d; })%s", t, node_is_oint(c, id) ? "" : ")");
      else buf_printf(b, "); sp_box_oint(_t%d); })", t);
      return 1;
    }
    /* Enumerator#+ chains two enumerators (#2481): the concatenation of their
       element sequences, materialized. */
    if (sp_streq(name, "+") && argc == 1 && comp_ntype(c, argv[0]) == TY_ENUMERATOR) {
      /* Both sides materialize before the concat, and materializing the second
         allocates: the first array has to be rooted across it. */
      { int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_Enumerator_to_a(", t);
        emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d);", t);
        buf_printf(b, " sp_Enumerator_new_from(sp_box_poly_array(sp_PolyArray_concat(_t%d, sp_Enumerator_to_a(", t);
        emit_expr(c, argv[0], b); buf_puts(b, ")))); })"); }
      return 1;
    }
    if ((is_to_array_alias(name)) && argc == 0) {
      /* the hop a block of map and its kin reads (desugar_enum_block_yield_view) */
      const char *view = nt_str(nt, id, "enum_yield_view");
      if (view) {
        buf_puts(b, "sp_Enumerator_to_a_yielded("); emit_expr(c, recv, b);
        buf_printf(b, ", %d)", sp_streq(view, "args")); return 1;
      }
      buf_puts(b, "sp_Enumerator_to_a("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
  }

  /* Random class methods: Random.rand(n) / Random.rand / Random.bytes(n)
     share a lazily-seeded default instance. */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Random")) {
    if (sp_streq(name, "rand")) {
      if (argc >= 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        buf_puts(b, "sp_Random_rand_float_bound(sp_random_default_get(), ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (argc >= 1 && repr_of(c, argv[0]).big) {
        /* Random.rand(Bignum bound): a uniform Bigint in [0, bound) (#3058) */
        buf_puts(b, "sp_bigint_rand(sp_random_default_get(), ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      /* a Range of either kind, or a boxed argument, draws as Random#rand
         does (sp_rand_poly), answered in the call's own type */
      else if (argc >= 1 && (comp_ntype(c, argv[0]) == TY_RANGE || comp_ntype(c, argv[0]) == TY_FLOAT_RANGE ||
                             repr_of(c, argv[0]).kind == RK_BOXED)) {
        Buf rv; memset(&rv, 0, sizeof rv);
        buf_puts(&rv, "sp_rand_poly(sp_random_default_get(), "); emit_boxed(c, argv[0], &rv); buf_puts(&rv, ", 0)");
        TyKind rk = repr_of(c, id).as_ty;
        if (rk == TY_POLY || rk == TY_UNKNOWN) buf_puts(b, rv.p);
        else emit_unbox_text(c, rk, rv.p, b);
        free(rv.p);
      }
      else if (argc >= 1 && comp_ntype(c, argv[0]) == TY_NIL) {
        /* rand(nil) is CRuby's ArgumentError, not the int slot's TypeError */
        buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
        buf_puts(b, "); sp_raise_cls(\"ArgumentError\", \"invalid argument - \"); (sp_int)0; })");
      }
      else if (argc >= 1) {
        buf_puts(b, "sp_Random_rand_int(sp_random_default_get(), ");
        emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else buf_puts(b, "sp_Random_rand_float(sp_random_default_get())");
      return 1;
    }
    if (sp_streq(name, "bytes") && argc == 1) {
      buf_puts(b, "sp_Random_bytes(sp_random_default_get(), ");
      emit_to_int_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "new_seed") && argc == 0) {   /* #2523 */
      buf_puts(b, "sp_Random_new_seed()");
      return 1;
    }
    if (sp_streq(name, "urandom") && argc == 1) {     /* #2543 */
      buf_puts(b, "sp_Random_urandom("); emit_int_expr_conv(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "srand")) {                    /* #2525 (returns previous seed) */
      if (argc == 0) { buf_puts(b, "sp_kernel_srand((sp_int)time(NULL))"); return 1; }
      buf_puts(b, "sp_kernel_srand("); emit_to_int_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
  }

  /* Enumerator.product(*enums) (and Enumerator::Product.new, desugared to
     it) -> an Enumerator::Product over the cartesian product of any number
     of factors (sp_enum_product_new). The factors are boxed into one rooted
     array in order, a splat contributing its elements. Product takes no
     keywords: the keyword hash, the last argument, is checked at run time
     (sp_enum_product_kw_check), so a `**h` and a String key are named as
     CRuby names them. */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Enumerator") &&
      sp_streq(name, "product")) {
    int tf = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tf, tf);
    for (int i = 0; i < argc; i++) {
      if (nt_kind(nt, argv[i]) == NK_KeywordHashNode) {
        buf_puts(b, " sp_enum_product_kw_check("); emit_boxed(c, argv[i], b); buf_puts(b, ");");
      }
      else if (nt_kind(nt, argv[i]) == NK_SplatNode) {
        int sx = nt_ref(nt, argv[i], "expression");
        int tsp = ++g_tmp, tsi = ++g_tmp;
        buf_printf(b, " { sp_PolyArray *_t%d = sp_enum_items_from(", tsp);
        if (sx >= 0) emit_boxed(c, sx, b); else buf_puts(b, "sp_box_nil()");
        buf_printf(b, "); SP_GC_ROOT(_t%d);"
                      " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                      " sp_PolyArray_push(_t%d, _t%d->data[_t%d]); }",
                   tsp, tsi, tsi, tsp, tsi, tf, tsp, tsi);
      }
      else {
        buf_printf(b, " sp_PolyArray_push(_t%d, ", tf);
        emit_boxed(c, argv[i], b);
        buf_puts(b, ");");
      }
    }
    buf_printf(b, " sp_enum_product_new(_t%d); })", tf);
    return 1;
  }

  /* Random instance methods */
  if (recv >= 0 && comp_ntype(c, recv) == TY_RANDOM) {
    if (emit_builtin_op(c, id, recv, TY_RANDOM, name, b)) return 1;
    if (sp_streq(name, "rand")) {
      if (argc >= 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        buf_puts(b, "sp_Random_rand_float_bound("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (argc >= 1 && repr_of(c, argv[0]).range == TY_FLOAT) {
        /* Random#rand(1.0..2.0) -> a Float in [first, last), exact endpoints. */
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, argv[0], b);
        buf_puts(b, "; sp_Random_rand_float_range("); emit_expr(c, recv, b);
        buf_printf(b, ", _t%d.first, _t%d.last); })", tr, tr);
      }
      else if (argc >= 1 && repr_of(c, argv[0]).range == TY_INT) {
        /* a Float-endpoint range yields a Float (#2521); an int range an Int. */
        const char *atype = nt_type(nt, argv[0]);
        int islit = atype && sp_streq(atype, "RangeNode");
        int lo = islit ? nt_ref(nt, argv[0], "left") : -1;
        int hi = islit ? nt_ref(nt, argv[0], "right") : -1;
        /* an endless/beginless range has no finite span -> Errno::EDOM (#2550) */
        if (islit && (lo < 0 || hi < 0)) {
          buf_puts(b, "(sp_raise_cls(\"Errno::EDOM\", \"Domain error - rand\"), (sp_int)0)");
          return 1;
        }
        /* either bound a Float draws a Float, up to the end as written */
        int is_float = (lo >= 0 && comp_ntype(c, lo) == TY_FLOAT) || (hi >= 0 && comp_ntype(c, hi) == TY_FLOAT);
        if (is_float) {
          int tr = ++g_tmp;
          buf_printf(b, "({ sp_Range _t%d = ", tr); emit_expr(c, argv[0], b);
          buf_puts(b, "; sp_Random_rand_float_range(");
          emit_expr(c, recv, b);
          buf_printf(b, ", (sp_float)_t%d.first, sp_range_end_num(_t%d)); })", tr, tr);
        }
        else {
          buf_puts(b, "sp_Random_rand_range("); emit_expr(c, recv, b); buf_puts(b, ", ");
          emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
      }
      else if (argc >= 1 && repr_of(c, argv[0]).kind == RK_BOXED) {
        /* a boxed argument draws by its run-time kind (sp_rand_poly): a Range
           in a mixed slot was converted to an Integer bound and raised */
        buf_puts(b, "sp_rand_poly("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_boxed(c, argv[0], b); buf_puts(b, ", 0)");
      }
      else if (argc >= 1 && repr_of(c, argv[0]).big) {
        /* rand(Bignum bound): a uniform Bigint in [0, bound) (#3058) */
        buf_puts(b, "sp_bigint_rand("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (argc >= 1 && comp_ntype(c, argv[0]) == TY_NIL) {
        buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)("); emit_expr(c, argv[0], b);
        buf_puts(b, "); sp_raise_cls(\"ArgumentError\", \"invalid argument - \"); (sp_int)0; })");
      }
      else if (argc >= 1) {
        buf_puts(b, "sp_Random_rand_int("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else {
        buf_puts(b, "sp_Random_rand_float("); emit_expr(c, recv, b); buf_puts(b, ")");
      }
      return 1;
    }
    if ((is_text_conversion(name)) && argc == 0) {
      buf_puts(b, "sp_Random_inspect("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    /* a Random instance is an opaque object: #class, and identity #==/#equal? (#2524) */
    if (sp_streq(name, "class") && argc == 0) {
      if (!node_may_be_null_nil(c, recv)) {
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), ((sp_Class){0, SPL(\"Random\")}))");
        return 1;
      }
      /* a NULL slot is nil, whose class is NilClass */
      int tr = ++g_tmp;
      buf_printf(b, "({ sp_Random *_t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; _t%d ? ((sp_Class){0, SPL(\"Random\")}) : ((sp_Class){(sp_int)-1, SPL(\"NilClass\")}); })", tr);
      return 1;
    }
    /* equal? and eql? are identity; == compares by internal PRNG state
       (#2524). Random defines no eql? of its own, so Object's applies:
       Random.new(1).eql?(Random.new(1)) is false where == is true. */
    if ((is_eql_or_equal(name)) && argc == 1) {
      buf_puts(b, "((void *)("); emit_expr(c, recv, b); buf_puts(b, ") == (void *)(");
      if (comp_ntype(c, argv[0]) == TY_RANDOM) emit_expr(c, argv[0], b);
      else buf_puts(b, "0");
      buf_puts(b, "))");
      return 1;
    }
    /* dup / clone copy the generator: the same state, a distinct object */
    if ((is_copy_alias(name)) && argc == 0) {
      if (sp_streq(name, "clone")) {
        /* clone keeps a frozen receiver's frozen bit; dup does not */
        int ro = ++g_tmp, rd = ++g_tmp;
        buf_printf(b, "({ sp_Random *_t%d = ", ro); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Random *_t%d = sp_Random_dup(_t%d);"
                      " if (_t%d && sp_gc_is_frozen(_t%d)) sp_gc_freeze(_t%d); _t%d; })",
                   rd, ro, ro, ro, rd, rd);
      }
      else { buf_puts(b, "sp_Random_dup("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      return 1;
    }
    if (sp_streq(name, "==") && argc == 1) {
      if (comp_ntype(c, argv[0]) == TY_RANDOM) {
        buf_puts(b, "sp_Random_eq("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (comp_ntype(c, argv[0]) == TY_NIL) {
        /* `r == nil`: a NULL slot is nil. The receiver is evaluated first,
           then the argument, as Ruby orders them. */
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_Random *_t%d = ", tr); emit_expr(c, recv, b);
        buf_puts(b, "; (void)("); emit_expr(c, argv[0], b);
        buf_printf(b, "); _t%d == NULL; })", tr);
      }
      else {
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), FALSE)");
      }
      return 1;
    }
  }
  return 0;
}
