/* The recorder's literal declaration contract becomes ordinary Ruby AST
 * before scope registration. Method lookup, reachability and call plans then
 * see the same definitions as Ruby-written methods. */
#include "analyze_internal.h"
#include "codegen_internal.h"
#include "cext_bridge.h"
#include <ctype.h>
#include "builtin_ops.h"

static const char *text(Compiler *c, int site, int n) {
  if (nt_kind(c->nt, n) != NK_StringNode)
    unsupported_feature(c, site, "C extension declarations require literal strings");
  return nt_str(c->nt, n, "content");
}
static long long integer(Compiler *c, int site, int n) {
  if (nt_kind(c->nt, n) == NK_IntegerNode) return nt_int(c->nt, n, "value", 0);
  if (nt_kind(c->nt, n) == NK_CallNode && nt_str(c->nt, n, "name") &&
      sp_streq(nt_str(c->nt, n, "name"), "-@"))
    return -integer(c, site, nt_ref(c->nt, n, "receiver"));
  unsupported_feature(c, site, "C extension declarations require literal integers");
}
static int boolean(Compiler *c, int site, int n) {
  if (nt_kind(c->nt, n) == NK_TrueNode) return 1;
  if (nt_kind(c->nt, n) == NK_FalseNode) return 0;
  unsupported_feature(c, site, "C extension declarations require literal booleans");
}
static void need(Compiler *c, int site, int actual, int expected) {
  if (actual != expected) unsupported_feature(c, site, "invalid C extension declaration argument count");
}
static void path(Compiler *c, int site, const char *s) {
  const char *p = s;
  for (;;) {
    if (!p || !isupper((unsigned char)*p))
      unsupported_feature(c, site, "invalid C extension constant path");
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    if (!*p) return;
    if (p[0] != ':' || p[1] != ':')
      unsupported_feature(c, site, "invalid C extension constant path");
    p += 2;
  }
}
static void symbol(Buf *b, const char *s) {
  buf_puts(b, ":'");
  for (const char *p = s; *p; p++) {
    if (*p == '\'' || *p == '\\') buf_puts(b, "\\");
    buf_putn(b, p, 1);
  }
  buf_puts(b, "'");
}
static void graft(Compiler *c, int site, Buf *b, int ext, int slot, int arity) {
  NodeTable *nt = (NodeTable *)c->nt;
  NodeTable *tmp = sp_parse_snippet(b->p ? b->p : "nil");
  if (!tmp) unsupported_feature(c, site, "invalid recorder declaration");
  int st = nt_ref(tmp, tmp->root_id, "statements");
  int ns = 0; const int *nodes = nt_arr(tmp, st, "body", &ns);
  if (ns != 1) unsupported_feature(c, site, "recorder declaration must produce one statement");
  int root = nt_import_subtree(nt, tmp, nodes[0]);
  nt_free(tmp);
  if (root < 0) unsupported_feature(c, site, "cannot import recorder declaration");
  int line = nt_int(nt, site, "node_line", 1), file = nt_int(nt, site, "node_file", -1);
  for (int i = root; i < nt->count; i++) {
    nt_node_set_int(nt, i, "node_line", line);
    if (file >= 0) nt_node_set_int(nt, i, "node_file", file);
    if (nt_kind(nt, i) == NK_CallNode) {
      const char *name = nt_str(nt, i, "name");
      if (name && !strncmp(name, "__cext_", 7)) nt_node_set_int(nt, i, "cext_synthetic", 1);
    }
    if (slot >= 0 && nt_kind(nt, i) == NK_ParametersNode) nt_node_set_int(nt, i, "cext_arity", arity);
    if (slot >= 0 && nt_kind(nt, i) == NK_ArrayNode) nt_node_set_int(nt, i, "cext_args", 1);
    if (slot >= 0 && nt_kind(nt, i) == NK_DefNode) {
      nt_node_set_int(nt, i, "cext_extension", ext + 1);
      nt_node_set_int(nt, i, "cext_function", slot);
      nt_node_set_int(nt, i, "cext_arity", arity);
    }
  }
  elab_swap_nodes(c, site, root);
  nt_node_reset(nt, root, "NilNode");
  free(b->p);
  memset(b, 0, sizeof(*b));
}
static int owner_module(const Compiler *c, const char *name) {
  for (int i = c->n_cext_classes - 1; i >= 0; i--)
    if (sp_streq(c->cext_classes[i].path, name)) return c->cext_classes[i].module;
  return is_builtin_module_const_name(name);
}
static void owner_open(Compiler *c, int site, Buf *b, const char *name) {
  path(c, site, name);
  buf_printf(b, "%s %s\n", owner_module(c, name) ? "module" : "class", name);
}
void desugar_cext_decls(Compiler *c) {
  if (!c->nt->has_cext_decls) return;
  NodeTable *nt = (NodeTable *)c->nt;
  int n0 = nt->count, ext = -1;
  for (int site = 0; site < n0; site++) {
    if (nt_kind(nt, site) != NK_CallNode || nt_ref(nt, site, "receiver") >= 0) continue;
    const char *name0 = nt_str(nt, site, "name");
    if (!name0 || strncmp(name0, "__cext_", 7)) continue;
    char *name = strdup(name0);
    int an = nt_ref(nt, site, "arguments"), ac = 0;
    const int *av0 = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
    int *av = malloc(sizeof(int) * (size_t)(ac + 1));
    if (ac) memcpy(av, av0, sizeof(int) * (size_t)ac);
    Buf b = {0}; int slot = -1, arity = 0;
    if (sp_streq(name, "__cext_extension")) {
      need(c, site, ac, 3);
      const char *en = text(c, site, av[0]), *init = text(c, site, av[1]);
      if (integer(c, site, av[2]) != 1) unsupported_feature(c, site, "unsupported C extension manifest version");
      if (strncmp(init, "Init_", 5)) unsupported_feature(c, site, "invalid C extension initializer");
      for (const char *p = init; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_') unsupported_feature(c, site, "invalid C extension initializer");
      for (int q = 0; q < c->n_cext_extensions; q++)
        if (sp_streq(c->cext_extensions[q].name, en)) unsupported_feature(c, site, "duplicate C extension manifest");
      ext = c->n_cext_extensions++;
      c->cext_extensions = realloc(c->cext_extensions, sizeof(CextExtension) * (size_t)c->n_cext_extensions);
      c->cext_extensions[ext] = (CextExtension){strdup(en), strdup(init), 0, 0};
      buf_printf(&b, "__cext_init(%d)", ext);
    } else {
      if (ext < 0) unsupported_feature(c, site, "C extension declaration precedes its manifest header");
      if (sp_streq(name, "__cext_reference")) {
        need(c, site, ac, 1);
        const char *cn=text(c,site,av[0]);path(c,site,cn);
        int q=c->n_cext_classes++;
        c->cext_classes=realloc(c->cext_classes,sizeof(CextClass)*(size_t)c->n_cext_classes);
        c->cext_classes[q]=(CextClass){strdup(cn),0,-1,-1};
        buf_printf(&b,"%s\n",cn);
      } else if (sp_streq(name, "__cext_class") || sp_streq(name, "__cext_module")) {
        int mod = sp_streq(name, "__cext_module"); need(c, site, ac, mod ? 1 : 2);
        const char *cn = text(c, site, av[0]); path(c, site, cn);
        const char *parent = mod ? NULL : text(c, site, av[1]); if (parent) path(c, site, parent);
        int q = c->n_cext_classes++;
        c->cext_classes = realloc(c->cext_classes, sizeof(CextClass) * (size_t)c->n_cext_classes);
        c->cext_classes[q] = (CextClass){strdup(cn), mod, ext, -1};
        buf_printf(&b, "%s %s", mod ? "module" : "class", cn);
        if (parent) buf_printf(&b, " < %s", parent);
        buf_puts(&b, "\nend\n");
      } else if (sp_streq(name, "__cext_method")) {
        need(c, site, ac, 6);
        const char *cn = text(c, site, av[0]), *mn = text(c, site, av[1]);
        const char *scope = text(c, site, av[2]), *vis = text(c, site, av[3]);
        arity = (int)integer(c, site, av[4]); slot = (int)integer(c, site, av[5]);
        if (arity < -2 || arity > 15 || slot != c->cext_extensions[ext].slots++)
          unsupported_feature(c, site, "unsupported C extension arity or registration order");
        if (!sp_streq(scope, "instance") && !sp_streq(scope, "singleton")) unsupported_feature(c, site, "invalid C extension method scope");
        if (!sp_streq(vis, "private") && !sp_streq(vis, "public")) unsupported_feature(c, site, "invalid C extension visibility");
        if (sp_streq(vis, "private") && sp_streq(scope, "singleton"))
          unsupported_feature(c, site, "private C extension singleton methods are unsupported");
        owner_open(c, site, &b, cn);
        if (sp_streq(vis, "private") && sp_streq(scope, "instance")) buf_puts(&b, "private ");
        buf_printf(&b, "def %s__cext_method(", sp_streq(scope, "singleton") ? "self." : "");
        /* Fixed arguments must use ordinary arity validation; the double-underscore
         * convention exempts compiler forwarding slots from those checks. */
        if (arity < 0) buf_puts(&b, "*__cext_args");
        else for (int j = 0; j < arity; j++) buf_printf(&b, "%s_cext_a%d", j ? ", " : "", j);
        buf_puts(&b, arity == 0 ? "&__cext_block)\n" : ", &__cext_block)\n");
        buf_printf(&b, "__cext_invoke(%d, %d, self, ", ext, slot);
        if (arity < 0) buf_puts(&b, "__cext_args");
        else {
          buf_puts(&b, "[");
          for (int j = 0; j < arity; j++) buf_printf(&b, "%s_cext_a%d", j ? ", " : "", j);
          buf_puts(&b, "]");
        }
        buf_puts(&b, ", __cext_block)\nend\nend\n");
      } else if (sp_streq(name, "__cext_allocator")) {
        need(c, site, ac, 2); const char *cn = text(c, site, av[0]);
        int alloc = (int)integer(c, site, av[1]);
        if (alloc != c->cext_extensions[ext].slots++) unsupported_feature(c, site, "invalid C extension allocator registration order");
        int found = 0;
        for (int q = 0; q < c->n_cext_classes; q++) if (sp_streq(c->cext_classes[q].path, cn)) {
          c->cext_classes[q].allocator = alloc; found = 1;
        }
        if (!found) unsupported_feature(c, site, "C extension allocator needs a manifest class");
        buf_puts(&b, "nil");
      } else if (sp_streq(name, "__cext_undef_allocator")) {
        /* No allocator: instances come only from TypedData_Make_Struct /
         * TypedData_Wrap_Struct in C, and Ruby's new and allocate raise as
         * CRuby's do for a class whose allocator is undefined. */
        need(c, site, ac, 1); const char *cn = text(c, site, av[0]);
        int found = 0;
        for (int q = 0; q < c->n_cext_classes; q++) if (sp_streq(c->cext_classes[q].path, cn) && !c->cext_classes[q].module) {
          c->cext_classes[q].allocator = -2; c->cext_classes[q].extension = ext; found = 1;
        }
        if (!found) unsupported_feature(c, site, "C extension allocator needs a manifest class");
        owner_open(c, site, &b, cn);
        buf_printf(&b, "def self.allocate\nraise TypeError, \"allocator undefined for %s\"\nend\n"
                       "def self.new(*)\nraise TypeError, \"allocator undefined for %s\"\nend\n"
                       "def dup\nraise TypeError, \"allocator undefined for %s\"\nend\n"
                       "def clone(freeze: nil)\nraise TypeError, \"allocator undefined for %s\"\nend\nend\n", cn, cn, cn, cn);
      } else if (sp_streq(name, "__cext_undef_method")) {
        /* Only initialize / initialize_copy of a class without an allocator:
         * new, allocate, dup and clone raise before either could run, so
         * their absence is not observable. Any other undef is refused, as
         * Module#undef_method is. */
        need(c, site, ac, 2);
        const char *cn = text(c, site, av[0]), *mn = text(c, site, av[1]);
        int undefined_allocator = 0;
        for (int q = 0; q < c->n_cext_classes; q++)
          if (sp_streq(c->cext_classes[q].path, cn) && c->cext_classes[q].allocator == -2) undefined_allocator = 1;
        if (!undefined_allocator || (!sp_streq(mn, "initialize") && !sp_streq(mn, "initialize_copy")))
          unsupported_feature(c, site, "rb_undef_method is supported only for initialize and initialize_copy of a class whose allocator is undefined");
        buf_puts(&b, "nil");
      } else if (sp_streq(name, "__cext_alias")) {
        need(c, site, ac, 3); owner_open(c, site, &b, text(c, site, av[0]));
        buf_puts(&b, "alias_method "); symbol(&b, text(c, site, av[1])); buf_puts(&b, ", "); symbol(&b, text(c, site, av[2])); buf_puts(&b, "\nend\n");
      } else if (sp_streq(name, "__cext_attribute")) {
        need(c, site, ac, 4); owner_open(c, site, &b, text(c, site, av[0]));
        int r = boolean(c, site, av[2]), w = boolean(c, site, av[3]);
        if (r || w) { buf_printf(&b, "attr_%s ", r && w ? "accessor" : r ? "reader" : "writer"); symbol(&b, text(c, site, av[1])); }
        buf_puts(&b, "\nend\n");
      } else if (sp_streq(name, "__cext_include") || sp_streq(name, "__cext_extend")) {
        need(c, site, ac, 2); owner_open(c, site, &b, text(c, site, av[0]));
        const char *mod = text(c, site, av[1]); path(c, site, mod);
        buf_printf(&b, "%s %s\nend\n", name + 7, mod);
      } else if (sp_streq(name, "__cext_ivar") || sp_streq(name, "__cext_eval") || sp_streq(name, "__cext_constant_reads")) {
        need(c, site, ac, sp_streq(name, "__cext_constant_reads") ? 0 : 1);
        buf_puts(&b, "nil");
      } else if (sp_streq(name, "__cext_link")) {
        /* A bundled package's extension, built by make (packages/<pkg>/
           build/cext): the archive to link, relative to the declaration
           file, and the libraries it needs (`;`-separated). The driver takes
           the `_mt` variant of the archive for a threaded program. */
        need(c, site, ac, 2);
        const char *archive = text(c, site, av[0]), *libs = text(c, site, av[1]);
        const char *file = nt_file_path(nt, (int)nt_int(nt, site, "node_file", -1));
        /* root-relative, as native_obj's paths are (the driver resolves them
           beside lib/, RUN_ONE_TEST from the tree's root): from the file's
           last "packages/" on, with the archive's `..` folded */
        const char *pk = NULL;
        for (const char *q = file; q && (q = strstr(q, "/packages/")); q++) pk = q + 1;
        char path[2048];
        if (archive[0] == '/' || !pk) snprintf(path, sizeof path, "%s", archive);
        else {
          const char *slash = strrchr(pk, '/');
          snprintf(path, sizeof path, "%.*s/%s", (int)(slash - pk), pk, archive);
          /* fold each "seg/../" */
          for (char *dd; (dd = strstr(path, "/../")); ) {
            char *seg = dd;
            while (seg > path && seg[-1] != '/') seg--;
            memmove(seg, dd + 4, strlen(dd + 4) + 1);
          }
        }
        int seen = 0;
        for (int k = 0; k < c->n_native_objs; k++) if (sp_streq(c->native_objs[k].path, path)) seen = 1;
        if (!seen) {
          c->native_objs = ffi_grow(c->native_objs, c->n_native_objs, &c->c_native_objs, 8, sizeof(NativeObj));
          int oi = c->n_native_objs++;
          c->native_objs[oi].mod = strdup("cext");
          c->native_objs[oi].path = strdup(path);
          c->native_objs[oi].feat = strdup("");
        }
        if (libs[0]) {
          int li = -1;
          for (int k = 0; k < c->n_ffi_libs; k++) if (sp_streq(c->ffi_libs[k].mod, "cext")) li = k;
          if (li < 0) {
            c->ffi_libs = ffi_grow(c->ffi_libs, c->n_ffi_libs, &c->c_ffi_libs, 8, sizeof(FfiLib));
            c->ffi_libs[c->n_ffi_libs].mod = strdup("cext");
            c->ffi_libs[c->n_ffi_libs].names = strdup(libs);
            c->n_ffi_libs++;
          }
          else ffi_semi_append(&c->ffi_libs[li].names, libs);
        }
        buf_puts(&b, "nil");
      } else if (sp_streq(name, "__cext_stub")) {
        need(c, site, ac, 3);
        int kind = (int)integer(c, site, av[0]), argc = (int)integer(c, site, av[2]);
        const char *key = text(c, site, av[1]);
        if (kind < 0 || kind > 7 || argc < 0 || (kind >= 6 && argc != 1))
          unsupported_feature(c, site, "invalid C extension dispatch stub");
        int idx = c->n_cext_stubs++;
        c->cext_stubs = realloc(c->cext_stubs, sizeof(CextStub) * (size_t)c->n_cext_stubs);
        c->cext_stubs[idx] = (CextStub){strdup(key), kind, argc, site};
        buf_printf(&b, "def __cext_dispatch_%d(r", idx);
        if (kind == 5 || kind == 7) buf_puts(&b, ", name");
        for (int j = 0; j < argc; j++) buf_printf(&b, ", a%d", j);
        buf_puts(&b, ", &block)\n__cext_box(block)\n__cext_box(");
        if (kind == 5) {
          /* An ID the extension computed from a Symbol: sent as Ruby sends a
           * computed name, over the methods the program defines. */
          buf_puts(&b, "r.__send__(name");
          for (int j = 0; j < argc; j++) buf_printf(&b, ", a%d", j);
          buf_puts(&b, ", &block)");
        } else if (kind == 0) {
          buf_puts(&b, "r.__cext_target(");
          for (int j = 0; j < argc; j++) buf_printf(&b, "%sa%d", j ? ", " : "", j);
          buf_puts(&b, argc ? ", &block)" : "&block)");
        } else if (kind == 6) {
          /* a spread send: the arguments as one Array (rb_apply, and a
           * rb_funcallv whose count the extension computes) */
          buf_puts(&b, "r.__cext_target(*a0, &block)");
        } else if (kind == 7) {
          /* ...of a name the extension computed as well */
          buf_puts(&b, "r.__send__(name, *a0, &block)");
        } else if (kind == 1 || kind == 2) {
          buf_printf(&b, "r.instance_variable_%s(", kind == 1 ? "get" : "set"); symbol(&b, key);
          buf_puts(&b, kind == 1 ? ")" : ", a0)");
        } else if (kind == 3) {
          path(c, site, key); buf_printf(&b, "r.const_get("); symbol(&b, key); buf_puts(&b, ")");
        } else buf_printf(&b, "begin\n%s\nend", key);
        buf_puts(&b, ")\nend\n");
      } else if (sp_streq(name, "__cext_literal_id")) {
        need(c, site, ac, 1); symbol(&b, text(c, site, av[0]));
      } else if (sp_streq(name, "__cext_funcall_arity")) {
        need(c, site, ac, 1); (void)integer(c, site, av[0]); buf_puts(&b, "nil");
      } else if (sp_streq(name, "__cext_constant")) {
        need(c, site, ac, 6);
        const char *cn = text(c, site, av[0]), *key = text(c, site, av[1]);
        path(c, site, cn); path(c, site, key);
        if (strstr(key, "::")) unsupported_feature(c, site, "C extension constant key must be a single name");
        int idx = c->cext_extensions[ext].constants++;
        if (!sp_streq(cn, "Object")) owner_open(c, site, &b, cn);
        buf_printf(&b, "%s = __cext_constant_value(%d, %d)\n", key, ext, idx);
        if (!sp_streq(cn, "Object")) buf_puts(&b, "end\n");
      } else unsupported_feature(c, site, "unknown C extension declaration");
    }
    int imported = nt->count;
    graft(c, site, &b, ext, slot, arity);
    if (sp_streq(name, "__cext_stub")) {
      nt_node_set_int(nt, site, "cext_extension", ext + 1);
      nt_node_set_int(nt, site, "cext_stub", c->n_cext_stubs);
      CextStub *stub = &c->cext_stubs[c->n_cext_stubs - 1];
      for (int j = imported; j < nt->count; j++) if (nt_kind(nt, j) == NK_CallNode &&
          nt_str(nt, j, "name") && sp_streq(nt_str(nt, j, "name"), "__cext_target"))
        nt_node_set_str(nt, j, "name", stub->key);
      for (int j = imported; j < nt->count; j++) if (nt_kind(nt, j) == NK_DefNode) {
        nt_node_set_int(nt, j, "cext_extension", ext + 1);
        nt_node_set_int(nt, j, "cext_stub", c->n_cext_stubs);
      }
    }
    if (slot >= 0) {
      const char *mn = text(c, site, av[1]);
      for (int j = imported; j < nt->count; j++)
        if (nt_kind(nt, j) == NK_DefNode) nt_node_set_str(nt, j, "name", mn);
    }
    free(av); free(name);
  }
  if (c->n_cext_extensions) comp_grow_node_arrays(c);
}
int cext_scope(const Compiler *c, int mi) {
  return c->n_cext_extensions && mi >= 0 && mi < c->nscopes && c->scopes[mi].def_node >= 0 &&
    nt_int(c->nt, c->scopes[mi].def_node, "cext_extension", 0) != 0;
}
static int constant_owned_in_chain(Compiler *c, int owner, const char *key) {
  if (owner == CG_NONE) return 0;
  if (const_owned_by_class(c, owner, key)) return 1;
  for (int cid = owner, n = 0; cid >= 0 && n < c->nclasses; n++, cid = c->classes[cid].parent)
    if (const_owned_by_class(c, cid, key)) return 1;
  return const_owned_by_class(c, comp_core_class(c, CORE_OBJECT), key);
}
static void builtin_constant_read(Compiler *c, int call, int mi) {
  NodeTable *nt = (NodeTable *)c->nt;
  int an = nt_ref(nt, call, "arguments"), ac = 0;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  if (ac != 1 || nt_kind(nt, av[0]) != NK_SymbolNode) return;
  const char *key = nt_str(nt, av[0], "value");
  int recv = nt_ref(nt, call, "receiver");
  int owner = const_get_recv_class(c, call, recv);
  /* the receiver's spelling for the builtin test below: a class's name, or a
     constant receiver's own (Object not reopened, a builtin module) */
  const char *oname = owner >= 0 ? c->classes[owner].name : owner != CG_NONE ? nt_str(nt, recv, "name") : NULL;
  int value = -1;
  int cls;
  const char *source = const_get_resolve(c, owner, key, &cls);
  if (source && !constant_owned_in_chain(c, owner, key)) {
    nt_node_set_int(nt, call, "cext_const_missing", 1); return;
  }
  if (source) {
    value = nt_new_node(nt, "ConstantReadNode"); nt_node_set_str(nt, value, "name", source);
  } else if (
      (builtin_class_id(key) || is_builtin_module_const_name(key) || is_builtin_exception_name(key))) {
    value = nt_new_node(nt, "ConstantReadNode"); nt_node_set_str(nt, value, "name", key);
  } else if (oname && (builtin_class_id(oname) || is_builtin_module_const_name(oname))) {
    value = nt_new_node(nt, "ConstantPathNode"); nt_node_set_str(nt, value, "name", key);
    nt_node_set_ref(nt, value, "parent", recv);
    comp_grow_node_arrays(c); c->nscope[value] = mi;
    if (!infer_cext_builtin_constant(c, value)) value = -1;
  }
  if (value >= 0) {
    comp_grow_node_arrays(c); c->nscope[value] = mi;
    nt_node_set_int(nt, call, "cext_const_node", value + 1);
  }
}
void cext_resolve_constant(Compiler *c, int call) {
  if (nt_int(c->nt, call, "cext_const_node", 0)) return;
  int an = nt_ref(c->nt, call, "arguments"), ac = 0;
  const int *av = an >= 0 ? nt_arr(c->nt, an, "arguments", &ac) : NULL;
  if (ac != 1 || nt_kind(c->nt, av[0]) != NK_SymbolNode) return;
  const char *key = nt_str(c->nt, av[0], "value");
  int recv = nt_ref(c->nt, call, "receiver");
  int cls;
  if (!const_get_resolve(c, const_get_recv_class(c, call, recv), key, &cls)) return;
  Scope *scope = comp_scope_of(c, call);
  if (scope) builtin_constant_read(c, call, (int)(scope - c->scopes));
}
static void lower_constant_stub(Compiler *c, int mi) {
  Scope *sc = &c->scopes[mi];
  int index = nt_int(c->nt, sc->def_node, "cext_stub", 0) - 1;
  if (index < 0 || c->cext_stubs[index].kind != 3 ||
      nt_int(c->nt, sc->def_node, "cext_const_lowered", 0)) return;
  const char *key = c->cext_stubs[index].key;
  Buf body = {0};
  buf_puts(&body, "def __cext_constant_stub(r)\n");
  int opened = 0;
  for (int k = 0; k < c->nclasses; k++) {
    const char *cn = class_ruby_name(c, k);
    if (!cn) continue;
    buf_printf(&body, "%s r == __cext_class_key(\"%s\")\n__cext_box(%s.const_get(", opened++ ? "elsif" : "if", cn, cn);
    symbol(&body, key); buf_puts(&body, "))\n");
  }
  static const char *const core[] = {"Object", "BasicObject", "Class", "Module", "String", "Array", "Hash", "Integer", "Float", "IO", NULL};
  for (int i = 0; core[i]; i++) {
    buf_printf(&body, "%s r == %s\n__cext_box(%s.const_get(", opened++ ? "elsif" : "if", core[i], core[i]);
    symbol(&body, key); buf_puts(&body, "))\n");
  }
  for (int i = 0; i < c->n_cext_stubs; i++) {
    const char *cn = c->cext_stubs[i].key;
    if (c->cext_stubs[i].kind != 3 || (!builtin_class_id(cn) && !is_builtin_module_const_name(cn))) continue;
    buf_printf(&body, "elsif r == %s\n__cext_box(%s.const_get(", cn, cn);
    symbol(&body, key); buf_puts(&body, "))\n");
  }
  if (opened) buf_puts(&body, "else\n");
  buf_puts(&body, "__cext_box(Object.const_get("); symbol(&body, key); buf_puts(&body, "))\n");
  if (opened) buf_puts(&body, "end");
  buf_puts(&body, "\nend");
  NodeTable *tmp = sp_parse_snippet(body.p);
  if (!tmp) unsupported_feature(c, sc->def_node, "invalid C extension constant stub");
  int st = nt_ref(tmp, tmp->root_id, "statements"), n = 0;
  const int *roots = nt_arr(tmp, st, "body", &n);
  int start = c->nt->count;
  int tmp_body = n == 1 ? nt_ref(tmp, roots[0], "body") : -1;
  int root = tmp_body >= 0 ? nt_import_subtree((NodeTable *)c->nt, tmp, tmp_body) : -1;
  nt_free(tmp); free(body.p);
  if (root < 0) unsupported_feature(c, sc->def_node, "invalid C extension constant stub");
  nt_node_set_ref((NodeTable *)c->nt, sc->def_node, "body", root); { sc->body = root; comp_touch_scope(sc->idx); }
  nt_node_set_int((NodeTable *)c->nt, sc->def_node, "cext_const_lowered", 1);
  comp_grow_node_arrays(c);
  for (int id = start; id < c->nt->count; id++) {
    c->nscope[id] = mi;
    if (nt_kind(c->nt, id) == NK_CallNode && nt_str(c->nt, id, "name") &&
        sp_streq(nt_str(c->nt, id, "name"), "const_get")) {
      nt_node_set_int((NodeTable *)c->nt, id, "cext_const_read", 1);
      builtin_constant_read(c, id, mi);
    }
    if (nt_kind(c->nt, id) == NK_CallNode && nt_str(c->nt, id, "name") &&
        (sp_streq(nt_str(c->nt, id, "name"), "__cext_box") || sp_streq(nt_str(c->nt, id, "name"), "__cext_class_key")))
      nt_node_set_int((NodeTable *)c->nt, id, "cext_synthetic", 1);
  }
}
void register_cext_layouts(Compiler *c) {
  if (!c->n_cext_extensions) return;
  for (int mi = 0; mi < c->nscopes; mi++) if (cext_scope(c, mi)) {
    Scope *s = &c->scopes[mi];
    if (nt_int(c->nt, s->def_node, "cext_stub", 0)) { s->is_ext_entry = 1; lower_constant_stub(c, mi); }
    for (int p = 0; p < s->nparams; p++) {
      LocalVar *lv = scope_local(s, s->pnames[p]);
      if (lv) lv_set_type(lv, p == s->rest_idx ? TY_POLY_ARRAY : TY_POLY);
    }
  }
  /* A Ruby method C may call publishes an opaque VALUE. Pin its return
   * before inference, so ordinary callers and call plans see that same box;
   * only methods named by the literal send table cross this boundary. */
  for (int q = 0; q < c->n_cext_stubs; q++) {
    CextStub *stub = &c->cext_stubs[q];
    if (stub->kind != 0) continue;
    for (int mi = 1; mi < c->nscopes; mi++) {
      Scope *s = &c->scopes[mi];
      if (s->class_id < 0 || !s->name || !sp_streq(s->name, stub->key)) continue;
      scope_set_ret(s, TY_POLY); s->ret_rbs_seeded = 1;
    }
  }
  for (int q = 0; q < c->n_cext_classes; q++) {
    CextClass *cc = &c->cext_classes[q];
    int cid = -1;
    for (int k = 0; k < c->nclasses; k++) {
      const char *ruby_name = class_ruby_name(c, k);
      if (ruby_name && sp_streq(ruby_name, cc->path)) { cid = k; break; }
    }
    if (cid < 0) unsupported_feature(c, c->nt->root_id, "C extension class was not registered");
    /* -2: the allocator is undefined, so instances come only from
     * TypedData_Make_Struct / TypedData_Wrap_Struct; the layout is the same */
    if (cc->allocator >= 0 || cc->allocator == -2) {
      c->classes[cid].cext_layout = 1;
      c->classes[cid].instantiated = 1; comp_instantiated_gen++; comp_touch_class(cid);
      c->classes[cid].cext_extension = cc->extension + 1;
      c->classes[cid].cext_allocator = cc->allocator >= 0 ? cc->allocator : -1;
    }
  }
}

void inherit_cext_layouts(Compiler *c) {
  for (int round = 0; round < c->nclasses; round++) {
    int changed = 0;
    for (int k = 0; k < c->nclasses; k++) {
      ClassInfo *ci = &c->classes[k];
      if (ci->cext_extension || ci->parent < 0) continue;
      ClassInfo *parent = &c->classes[ci->parent];
      if (!parent->cext_extension) continue;
      ci->cext_extension = parent->cext_extension;
      ci->cext_allocator = parent->cext_allocator;
      changed = 1;
    }
    if (!changed) break;
  }
  for (int k = 0; k < c->nclasses; k++) if (c->classes[k].cext_extension) {
    if (c->classes[k].ary_root > 0 || class_is_exc_subclass(c, k))
      unsupported_feature(c, c->classes[k].def_node, "TypedData cannot use an embedded builtin layout");
    int top = k;
    while (c->classes[top].parent >= 0) top = c->classes[top].parent;
    for (int q = 0; q < c->nclasses; q++)
      for (int a = q; a >= 0; a = c->classes[a].parent)
        if (a == top) { c->classes[q].cext_layout = 1; break; }
  }
}

static void known_name(char ***names, int *count, const char *name) {
  if (!name) return;
  for (int i = 0; i < *count; i++) if (sp_streq((*names)[i], name)) return;
  *names = realloc(*names, sizeof(char *) * (size_t)(*count + 1));
  (*names)[(*count)++] = strdup(name);
}
static void emit_missing_cext_send(Compiler *c, Buf *b) {
  char **names = NULL; int count = 0;
  for (int i = 0; i < c->nscopes; i++) if (c->scopes[i].class_id >= 0)
    known_name(&names, &count, c->scopes[i].name);
  for (int i = 0; i < bop_row_count(); i++) known_name(&names, &count, bop_row(i)->name);
  for (int k = 0; k < c->nclasses; k++) {
    ClassInfo *ci = &c->classes[k];
    for (int j = 0; j < ci->naliases; j++) known_name(&names, &count, ci->alias_new[j]);
    for (int j = 0; j < ci->nreaders; j++) known_name(&names, &count, ci->readers[j]);
    for (int j = 0; j < ci->nwriters; j++) {
      Buf name = {0}; buf_printf(&name, "%s=", ci->writers[j]);
      known_name(&names, &count, name.p); free(name.p);
    }
  }
  buf_puts(b, "if (kind == 0) { static const char *const known[] = {\n");
  for (int i = 0; i < count; i++) { emit_str_literal(b, names[i]); buf_puts(b, ",\n"); free(names[i]); }
  free(names);
  buf_puts(b, "NULL}; for (int i = 0; known[i]; i++) if (!strcmp(key, known[i]))\n"
              "rb_bug(\"C extension called known method %s with uncompiled arity %d\", key, argc); }\n");
}
/* Stubs have ordinary call plans and representations; this table only adapts
 * the boxed external ABI to their permanently boxed parameters. */
void emit_cext_dispatch(Compiler *c, Buf *b) {
  if (!c->n_cext_extensions) return;
  buf_puts(b, "static sp_RbVal _sp_cext_dispatch(int kind, const char *key, sp_RbVal r, int argc, const sp_RbVal *argv, sp_Proc *block) {\n");
  /* Literal names first; a computed name the literal table does not hold
   * then goes to the send of its arity. */
  for (int computed = 0; computed < 2; computed++)
  for (int mi = 1; mi < c->nscopes; mi++) {
    Scope *sc = &c->scopes[mi];
    int idx = sc->def_node >= 0 ? nt_int(c->nt, sc->def_node, "cext_stub", 0) - 1 : -1;
    if (idx < 0) continue;
    CextStub *stub = &c->cext_stubs[idx];
    if (stub->kind == 7 || (stub->kind == 5) != computed) continue;
    if (computed) buf_printf(b, "if (kind == 0 && argc == %d) return ", stub->argc);
    else {
      buf_printf(b, "if (kind == %d && argc == %d && !strcmp(key, ", stub->kind, stub->argc);
      emit_str_literal(b, stub->key); buf_puts(b, ")) return ");
    }
    buf_puts(b, mc_method(c, sc)); buf_puts(b, computed ? "(r, sp_box_sym(sp_sym_intern(key))" : "(r");
    for (int j = 0; j < stub->argc; j++) buf_printf(b, ", argv[%d]", j);
    buf_puts(b, ", block);\n");
  }
  /* a literal name's send with a count no stub has: through its spread
   * stub, the arguments gathered into an Array; then a computed name's,
   * sent or applied, through the computed spread stub */
  for (int pass = 0; pass < 2; pass++)
  for (int mi = 1; mi < c->nscopes; mi++) {
    Scope *sc = &c->scopes[mi];
    int idx = sc->def_node >= 0 ? nt_int(c->nt, sc->def_node, "cext_stub", 0) - 1 : -1;
    if (idx < 0 || c->cext_stubs[idx].kind != (pass ? 7 : 6)) continue;
    if (pass) {
      buf_puts(b, "if (kind == 6 && argc == 1) return ");
      buf_puts(b, mc_method(c, sc)); buf_puts(b, "(r, sp_box_sym(sp_sym_intern(key)), argv[0], block);\n");
      buf_puts(b, "if (kind == 0) { sp_PolyArray *spread = sp_PolyArray_new(); SP_GC_ROOT(spread);\n"
                  "for (int i = 0; i < argc; i++) sp_PolyArray_push(spread, argv[i]);\nreturn ");
      buf_puts(b, mc_method(c, sc)); buf_puts(b, "(r, sp_box_sym(sp_sym_intern(key)), sp_box_poly_array(spread), block); }\n");
      continue;
    }
    buf_puts(b, "if (kind == 0 && !strcmp(key, "); emit_str_literal(b, c->cext_stubs[idx].key);
    buf_puts(b, ")) { sp_PolyArray *spread = sp_PolyArray_new(); SP_GC_ROOT(spread);\n"
                "for (int i = 0; i < argc; i++) sp_PolyArray_push(spread, argv[i]);\nreturn ");
    buf_puts(b, mc_method(c, sc)); buf_puts(b, "(r, sp_box_poly_array(spread), block); }\n");
  }
  emit_missing_cext_send(c, b);
  buf_puts(b, "sp_raise_cls(\"NoMethodError\", \"C extension dispatch has no compiled stub\");\n}\n");
}
