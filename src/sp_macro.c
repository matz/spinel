/* Class-body macro expansion (included into spinel_parse.c).
 *
 * Library code often builds its class bodies with "macro" methods: a module
 * the class extends defines methods that the class body calls with literal
 * arguments, and those methods compute names and generate code at load time:
 *
 *   module Sodium
 *     def sodium_type(type = nil)
 *       return @type if type.nil?
 *       @type = type
 *     end
 *     def sodium_constant(constant, name: constant)
 *       fn = ["crypto", sodium_type, constant.to_s.downcase].compact.join("_")
 *       attach_function fn, [], :size_t
 *       const_set(name, public_send(fn))
 *     end
 *     def sodium_function(name, function, arguments)
 *       module_eval <<-RUBY
 *       attach_function #{function.inspect}, #{arguments.inspect}, :int
 *       def self.#{name}(*args) = #{function}(*args) == 0
 *       RUBY
 *     end
 *   end
 *   class Box
 *     extend Sodium
 *     sodium_type :secretbox
 *     sodium_constant :KEYBYTES
 *     sodium_function :box, :crypto_box, %i[pointer pointer]
 *   end
 *
 * Every name here is known when the program is compiled; only the evaluation
 * is dynamic. This pass partially evaluates such calls over the Prism tree of
 * the whole program: the arguments are literals, the module-level ivars the
 * macros keep (@type) are tracked per class body, and everything computable
 * is computed. What remains -- the attach_function, the constant, the
 * module_eval'd source -- is written back into the class body as ordinary
 * Ruby in place of the call:
 *
 *   attach_function "crypto_secretbox_keybytes", [], :size_t; KEYBYTES = crypto_secretbox_keybytes()
 *
 * Only a call whose macro (transitively) does something the compiler cannot
 * do at run time -- string module_eval/class_eval, const_set,
 * define_method, public_send/send, attach_function with a computed name --
 * is replaced; the other macro calls (sodium_type) are evaluated only to
 * track their state, and stay in the program as they are. Anything the
 * evaluator does not understand leaves the call untouched. The replacement
 * keeps the call's line count. */

typedef enum { MV_UNDEF = 0, MV_NIL, MV_TRUE, MV_FALSE, MV_INT, MV_STR, MV_SYM, MV_ARR } MvKind;
typedef struct Mv { MvKind k; long i; char *s; struct Mv *a; int n; } Mv;

typedef struct { char *name; Mv v; } MxVar;

typedef struct {
  char *module;           /* the defining module's (last) name */
  pm_def_node_t *def;
  int dynamic;            /* -1 unknown, 0 no, 1 yes */
  int expanded;           /* call sites replaced by their expansion */
  const pm_node_t *sites[512]; /* (debug) the expanded calls */
} MxMacro;

typedef struct MxBuf { char *p; size_t len, cap; } MxBuf;

static MxMacro *g_mx_macros; static int g_mx_nmacros, g_mx_cmacros;

static void mxb_putn(MxBuf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2 + 64;
    b->p = realloc(b->p, b->cap);
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = 0;
}
static void mxb_puts(MxBuf *b, const char *s) { mxb_putn(b, s, strlen(s)); }

typedef struct {
  MxVar loc[64]; int nloc;
  MxVar *iv; int *niv;    /* the class body's macro state */
  MxBuf *out;             /* residual code, or NULL: evaluate only */
  int returned; Mv ret;
  int stopped;            /* an unconditional raise was emitted */
  int in_ffi_rescue;      /* inside the rescue of a begin that attaches a function */
  int depth;
  const char *modname;
  int *unsure;            /* the class body may have state the evaluator did not see */
  char **ext; int next;   /* the class's extends so far, searched last first */
  int *ext_unsure;        /* set when what runs may change those */
} MxCtx;

static Mv mv_nil(void) { Mv v; memset(&v, 0, sizeof v); v.k = MV_NIL; return v; }
static Mv mv_str(const char *s, size_t n, MvKind k) {
  Mv v; memset(&v, 0, sizeof v); v.k = k;
  v.s = malloc(n + 1); memcpy(v.s, s, n); v.s[n] = 0;
  return v;
}
static int mv_truthy(Mv v) { return v.k != MV_NIL && v.k != MV_FALSE && v.k != MV_UNDEF; }
/* the same known value (an unknown one is never the same) */
static int mv_same(Mv a, Mv b) {
  if (a.k != b.k || a.k == MV_UNDEF) return 0;
  switch (a.k) {
  case MV_INT: return a.i == b.i;
  case MV_STR: case MV_SYM: return strcmp(a.s, b.s) == 0;
  case MV_ARR:
    if (a.n != b.n) return 0;
    for (int i = 0; i < a.n; i++) if (!mv_same(a.a[i], b.a[i])) return 0;
    return 1;
  default: return 1;
  }
}
static Mv mv_undef(void) { Mv v; memset(&v, 0, sizeof v); v.k = MV_UNDEF; return v; }

static char *mx_name(pm_constant_id_t id) { return cstr(id); }

static void mv_to_s(Mv v, MxBuf *b) {
  char tmp[32];
  switch (v.k) {
  case MV_NIL: break;
  case MV_TRUE: mxb_puts(b, "true"); break;
  case MV_FALSE: mxb_puts(b, "false"); break;
  case MV_INT: snprintf(tmp, sizeof tmp, "%ld", v.i); mxb_puts(b, tmp); break;
  case MV_STR: case MV_SYM: mxb_puts(b, v.s); break;
  default: break;
  }
}

static int mx_sym_plain(const char *s) {
  if (!*s) return 0;
  if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
  for (const char *p = s; *p; p++) {
    if (isalnum((unsigned char)*p) || *p == '_') continue;
    if ((*p == '?' || *p == '!' || *p == '=') && p[1] == 0) continue;
    return 0;
  }
  return 1;
}

static void mx_quote(const char *s, MxBuf *b) {
  mxb_puts(b, "\"");
  for (const char *p = s; *p; p++) {
    char e[8];
    unsigned char ch = (unsigned char)*p;
    if (ch == '"' || ch == '\\' || ch == '#') { e[0] = '\\'; e[1] = (char)ch; e[2] = 0; }
    else if (ch == '\n') strcpy(e, "\\n");
    else if (ch == '\t') strcpy(e, "\\t");
    else if (ch < 0x20) snprintf(e, sizeof e, "\\x%02x", ch);
    else { e[0] = (char)ch; e[1] = 0; }
    mxb_puts(b, e);
  }
  mxb_puts(b, "\"");
}

static void mv_inspect(Mv v, MxBuf *b) {
  switch (v.k) {
  case MV_NIL: mxb_puts(b, "nil"); break;
  case MV_STR: mx_quote(v.s, b); break;
  case MV_SYM:
    mxb_puts(b, ":");
    if (mx_sym_plain(v.s)) mxb_puts(b, v.s); else mx_quote(v.s, b);
    break;
  case MV_ARR:
    mxb_puts(b, "[");
    for (int i = 0; i < v.n; i++) { if (i) mxb_puts(b, ", "); mv_inspect(v.a[i], b); }
    mxb_puts(b, "]");
    break;
  default: mv_to_s(v, b); break;
  }
}

static int mv_eq(Mv a, Mv b) {
  if (a.k != b.k) return 0;
  switch (a.k) {
  case MV_INT: return a.i == b.i;
  case MV_STR: case MV_SYM: return strcmp(a.s, b.s) == 0;
  case MV_ARR:
    if (a.n != b.n) return 0;
    for (int i = 0; i < a.n; i++) if (!mv_eq(a.a[i], b.a[i])) return 0;
    return 1;
  default: return 1;
  }
}

static Mv *mx_lookup(MxVar *vars, int n, const char *name) {
  for (int i = n - 1; i >= 0; i--) if (strcmp(vars[i].name, name) == 0) return &vars[i].v;
  return NULL;
}
static int g_mx_full;   /* a table was full: the evaluation cannot be trusted */
static void mx_set(MxVar *vars, int *n, int cap, const char *name, Mv v) {
  Mv *p = mx_lookup(vars, *n, name);
  if (p) { *p = v; return; }
  if (*n >= cap) { g_mx_full = 1; return; }
  vars[*n].name = strdup(name); vars[*n].v = v; (*n)++;
}

/* A list of names (strdup'd). */
typedef struct { char **v; int n; } MxNames;
static MxNames g_mx_sdefs;    /* class methods the program defines, anywhere */
/* ... and their bodies, by name */
typedef struct { char *name; pm_node_t *body; } MxSdef;
static MxSdef *g_mx_sdef_bodies; static int g_mx_nsdef_bodies;
static void mx_names_add(MxNames *l, const char *name) {
  for (int i = 0; i < l->n; i++) if (strcmp(l->v[i], name) == 0) return;
  l->v = realloc(l->v, sizeof(char *) * (size_t)(l->n + 1));
  l->v[l->n++] = strdup(name);
}
static int mx_names_has(const MxNames *l, const char *name) {
  for (int i = 0; i < l->n; i++) if (strcmp(l->v[i], name) == 0) return 1;
  return 0;
}
static void mx_names_free(MxNames *l) {
  for (int i = 0; i < l->n; i++) free(l->v[i]);
  free(l->v); l->v = NULL; l->n = 0;
}

static const pm_parser_t *g_mx_main;   /* the program's parser: its names */

static MxMacro *mx_find_macro(const char *mod, const char *name) {
  /* the macros are the program's, even while code an expansion adds is
     being looked at under its own parser */
  const pm_parser_t *sv = g_parser;
  if (g_mx_main) g_parser = g_mx_main;
  MxMacro *found = NULL;
  for (int i = 0; i < g_mx_nmacros && !found; i++) {
    if (mod && strcmp(g_mx_macros[i].module, mod) != 0) continue;
    char *dn = mx_name(g_mx_macros[i].def->name);
    if (strcmp(dn, name) == 0) found = &g_mx_macros[i];
    free(dn);
  }
  g_parser = sv;
  return found;
}

/* A call on self in a macro body, looked up as Ruby does from the class: its
   own class methods first (the program's are not followed: *refuse), then the
   modules it extended, the last first (outside a class body, the calling
   macro's own module). NULL with *refuse set: the call is not evaluated. */
static MxMacro *mx_resolve_self(const char *modname, char **ext, int next, const char *name, int *refuse) {
  *refuse = 0;
  if (mx_names_has(&g_mx_sdefs, name)) { *refuse = 1; return NULL; }
  if (ext) {
    for (int e = next - 1; e >= 0; e--) {
      MxMacro *m = mx_find_macro(ext[e], name);
      if (m) return m;
    }
  }
  else {
    MxMacro *m = mx_find_macro(modname, name);
    if (m) return m;
  }
  if (mx_find_macro(NULL, name)) *refuse = 1;   /* reached another way, or not at all */
  return NULL;
}

static int mx_eval(MxCtx *c, pm_node_t *n, Mv *out);
static int mx_undef_writes(pm_node_t *s, MxVar *iv, int *niv, int *unsure);
static void mx_forget(MxVar *iv, int niv, int *unsure);
static int mx_exec_stmts(MxCtx *c, pm_statements_node_t *s);
static int mx_residual(MxCtx *c, pm_node_t *n, MxBuf *b);
static int mx_call_macro(MxCtx *c, MxMacro *m, pm_arguments_node_t *args, Mv *ret);

static int mx_eval_list(MxCtx *c, pm_node_list_t *l, Mv *out) {
  Mv v; memset(&v, 0, sizeof v); v.k = MV_ARR;
  v.a = calloc(l->size + 1, sizeof(Mv)); v.n = (int)l->size;
  for (size_t i = 0; i < l->size; i++) {
    if (PM_NODE_TYPE(l->nodes[i]) == PM_SPLAT_NODE) return 0;
    if (!mx_eval(c, l->nodes[i], &v.a[i])) return 0;
  }
  *out = v;
  return 1;
}

/* the value of a string-ish node's parts */
static int mx_eval_parts(MxCtx *c, pm_node_list_t *parts, MxBuf *b) {
  for (size_t i = 0; i < parts->size; i++) {
    pm_node_t *p = parts->nodes[i];
    if (PM_NODE_TYPE(p) == PM_STRING_NODE) {
      pm_string_node_t *s = (pm_string_node_t *)p;
      mxb_putn(b, (const char *)pm_string_source(&s->unescaped), pm_string_length(&s->unescaped));
    }
    else if (PM_NODE_TYPE(p) == PM_EMBEDDED_STATEMENTS_NODE) {
      pm_embedded_statements_node_t *e = (pm_embedded_statements_node_t *)p;
      if (!e->statements || e->statements->body.size != 1) return 0;
      Mv v;
      if (!mx_eval(c, e->statements->body.nodes[0], &v)) return 0;
      if (v.k == MV_ARR) mv_inspect(v, b); else mv_to_s(v, b);
    }
    else if (PM_NODE_TYPE(p) == PM_INTERPOLATED_STRING_NODE) {
      if (!mx_eval_parts(c, &((pm_interpolated_string_node_t *)p)->parts, b)) return 0;
    }
    else return 0;
  }
  return 1;
}

/* A call on a class that changes which module answers its macros. */
static int mx_outer_ext_name(const char *n) {
  return strcmp(n, "extend") == 0 || strcmp(n, "class_eval") == 0 || strcmp(n, "module_eval") == 0 ||
         strcmp(n, "instance_eval") == 0 || strcmp(n, "class_exec") == 0 ||
         strcmp(n, "module_exec") == 0 || strcmp(n, "instance_exec") == 0;
}

static int mx_ascii(const char *s) {
  for (; *s; s++) if ((unsigned char)*s >= 0x80) return 0;
  return 1;
}

/* A name a send may reach that writes what the evaluator does not follow. */
static int mx_sdef_writes(const char *name, int depth);
static int mx_writer_name(const char *name) {
  return strcmp(name, "instance_variable_set") == 0 || mx_find_macro(NULL, name) ||
         mx_sdef_writes(name, 0);
}

/* Does a class method of the program's by this name write an ivar, or call
   something that does (a macro, instance_variable_set, another such class
   method)? Its self is the class, so its ivars are the macros' state. */
typedef struct { int depth; int hit; } MxSdefScan;
static bool mx_sdef_visit(const pm_node_t *n, void *data) {
  MxSdefScan *sc = data;
  if (sc->hit) return false;
  switch (PM_NODE_TYPE(n)) {
  case PM_DEF_NODE: case PM_CLASS_NODE: case PM_MODULE_NODE: case PM_SINGLETON_CLASS_NODE:
    return false;
  case PM_INSTANCE_VARIABLE_WRITE_NODE: case PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE:
  case PM_INSTANCE_VARIABLE_OR_WRITE_NODE: case PM_INSTANCE_VARIABLE_AND_WRITE_NODE:
  case PM_INSTANCE_VARIABLE_TARGET_NODE:
    sc->hit = 1; return false;
  case PM_CALL_NODE: {
    const pm_call_node_t *cn = (const pm_call_node_t *)n;
    if (!cn->receiver || PM_NODE_TYPE(cn->receiver) == PM_SELF_NODE) {
      char *cname = mx_name(cn->name);
      int w = strcmp(cname, "instance_variable_set") == 0 || strcmp(cname, "send") == 0 ||
              strcmp(cname, "public_send") == 0 || strcmp(cname, "__send__") == 0 ||
              mx_find_macro(NULL, cname) || mx_sdef_writes(cname, sc->depth + 1);
      free(cname);
      if (w) { sc->hit = 1; return false; }
    }
    return true;
  }
  default: return true;
  }
}
static int mx_sdef_writes(const char *name, int depth) {
  if (depth > 4) return 1;   /* deep enough to be unsure */
  /* the bodies are the program's, even while code an expansion adds is
     being looked at under its own parser */
  const pm_parser_t *sv = g_parser;
  g_parser = g_mx_main;
  int hit = 0;
  for (int i = 0; i < g_mx_nsdef_bodies; i++) {
    if (strcmp(g_mx_sdef_bodies[i].name, name) != 0) continue;
    if (!g_mx_sdef_bodies[i].body) continue;
    MxSdefScan sc = { depth, 0 };
    pm_visit_node(g_mx_sdef_bodies[i].body, mx_sdef_visit, &sc);
    if (sc.hit) { hit = 1; break; }
  }
  g_parser = sv;
  return hit;
}

/* Does code an expansion adds write the macros' state: a statement it runs
   in the class body now, or a class method it defines (`def self.x`, a def in
   `class << self`) that writes when called? Parsed on its own, with its own
   names. */
static bool mx_code_defs_visit(const pm_node_t *n, void *data) {
  MxSdefScan *sc = data;
  if (sc->hit) return false;
  if (PM_NODE_TYPE(n) == PM_DEF_NODE) {
    const pm_def_node_t *d = (const pm_def_node_t *)n;
    if (d->receiver && PM_NODE_TYPE(d->receiver) == PM_SELF_NODE && d->body)
      pm_visit_node(d->body, mx_sdef_visit, sc);
    return false;
  }
  if (PM_NODE_TYPE(n) == PM_SINGLETON_CLASS_NODE) {
    const pm_node_t *b = ((const pm_singleton_class_node_t *)n)->body;
    if (b && PM_NODE_TYPE(b) == PM_STATEMENTS_NODE) {
      const pm_statements_node_t *st = (const pm_statements_node_t *)b;
      for (size_t i = 0; i < st->body.size && !sc->hit; i++)
        if (PM_NODE_TYPE(st->body.nodes[i]) == PM_DEF_NODE && ((pm_def_node_t *)st->body.nodes[i])->body)
          pm_visit_node(((pm_def_node_t *)st->body.nodes[i])->body, mx_sdef_visit, sc);
    }
    return false;
  }
  if (PM_NODE_TYPE(n) == PM_CLASS_NODE || PM_NODE_TYPE(n) == PM_MODULE_NODE) return false;
  if (PM_NODE_TYPE(n) == PM_STATEMENTS_NODE || PM_NODE_TYPE(n) == PM_PROGRAM_NODE) return true;
  /* a statement run now */
  pm_visit_node(n, mx_sdef_visit, sc);
  return false;
}
static int mx_code_writes(const char *code) {
  pm_parser_t cp;
  pm_parser_init(&cp, (const uint8_t *)code, strlen(code), NULL);
  pm_node_t *root = pm_parse(&cp);
  const pm_parser_t *sv = g_parser;
  g_parser = &cp;
  MxSdefScan sc = { 0, 0 };
  if (cp.error_list.size == 0) pm_visit_node(root, mx_code_defs_visit, &sc);
  else sc.hit = 1;
  g_parser = sv;
  pm_node_destroy(&cp, root);
  pm_parser_free(&cp);
  return sc.hit;
}

static int mx_eval_call(MxCtx *c, pm_call_node_t *n, Mv *out) {
  char *name = mx_name(n->name);
  int ok = 0;
  pm_node_list_t *args = n->arguments ? &n->arguments->arguments : NULL;
  size_t argc = args ? args->size : 0;
  if (n->block) goto done;
  if (!n->receiver || PM_NODE_TYPE(n->receiver) == PM_SELF_NODE) {
    int refuse;
    MxMacro *m = mx_resolve_self(c->modname, c->ext, c->next, name, &refuse);
    /* it runs at run time from the text that keeps it */
    if (refuse) { if (c->iv) mx_forget(c->iv, *c->niv, c->unsure); goto done; }
    /* a macro whose value is a call's the evaluator does not make (its last
       statement residual code) has no value here */
    if (m && c->depth < 16) {
      /* a macro whose value is used: one that leaves residual code (or no
         value the evaluator made) is not evaluated here, and its state
         changes are undone -- the call stays in the text, run at run time */
      MxBuf scratch = {0};
      MxBuf *sv_out = c->out;
      MxVar sv_iv[64]; int sv_niv = *c->niv;
      memcpy(sv_iv, c->iv, sizeof(MxVar) * (size_t)sv_niv);
      c->out = &scratch;
      ok = mx_call_macro(c, m, n->arguments, out) && out->k != MV_UNDEF && scratch.len == 0;
      c->out = sv_out;
      free(scratch.p);
      /* it still runs at run time, from the text that keeps it */
      if (!ok) { memcpy(c->iv, sv_iv, sizeof(MxVar) * (size_t)sv_niv); *c->niv = sv_niv; mx_forget(c->iv, sv_niv, c->unsure); }
      goto done;
    }
    goto done;
  }
  Mv r;
  if (!mx_eval(c, n->receiver, &r)) goto done;
  Mv a0; memset(&a0, 0, sizeof a0);
  if (argc == 1 && !mx_eval(c, args->nodes[0], &a0)) goto done;
  if (argc > 1) goto done;
  if (argc == 0) {
    if (strcmp(name, "nil?") == 0) { *out = mv_nil(); out->k = r.k == MV_NIL ? MV_TRUE : MV_FALSE; ok = 1; }
    else if (strcmp(name, "!") == 0) { *out = mv_nil(); out->k = mv_truthy(r) ? MV_FALSE : MV_TRUE; ok = 1; }
    else if (strcmp(name, "to_s") == 0 && r.k != MV_ARR) {
      MxBuf b = {0}; mv_to_s(r, &b); *out = mv_str(b.p ? b.p : "", b.len, MV_STR); free(b.p); ok = 1;
    }
    else if (strcmp(name, "to_sym") == 0 && (r.k == MV_STR || r.k == MV_SYM)) { *out = r; out->k = MV_SYM; ok = 1; }
    else if (strcmp(name, "inspect") == 0) {
      MxBuf b = {0}; mv_inspect(r, &b); *out = mv_str(b.p ? b.p : "", b.len, MV_STR); free(b.p); ok = 1;
    }
    /* case mapping and length only over ASCII: CRuby's are by character,
       with Unicode case rules */
    else if ((strcmp(name, "downcase") == 0 || strcmp(name, "upcase") == 0 ||
              strcmp(name, "capitalize") == 0) && (r.k == MV_STR || r.k == MV_SYM) && mx_ascii(r.s)) {
      *out = mv_str(r.s, strlen(r.s), r.k);
      for (char *p = out->s; *p; p++) {
        if (name[0] == 'd') *p = (char)tolower((unsigned char)*p);
        else if (name[0] == 'u') *p = (char)toupper((unsigned char)*p);
        else *p = (char)(p == out->s ? toupper((unsigned char)*p) : tolower((unsigned char)*p));
      }
      ok = 1;
    }
    else if (strcmp(name, "compact") == 0 && r.k == MV_ARR) {
      Mv v = r; v.a = calloc(r.n + 1, sizeof(Mv)); v.n = 0;
      for (int i = 0; i < r.n; i++) if (r.a[i].k != MV_NIL) v.a[v.n++] = r.a[i];
      *out = v; ok = 1;
    }
    else if ((strcmp(name, "size") == 0 || strcmp(name, "length") == 0) && r.k != MV_NIL) {
      *out = mv_nil(); out->k = MV_INT;
      out->i = r.k == MV_ARR ? r.n : (r.k == MV_STR || r.k == MV_SYM) ? (long)strlen(r.s) : 0;
      ok = r.k == MV_ARR || ((r.k == MV_STR || r.k == MV_SYM) && mx_ascii(r.s));
    }
    else if ((strcmp(name, "first") == 0 || strcmp(name, "last") == 0) && r.k == MV_ARR) {
      *out = r.n == 0 ? mv_nil() : r.a[name[0] == 'f' ? 0 : r.n - 1]; ok = 1;
    }
    else if (strcmp(name, "join") == 0 && r.k == MV_ARR) {
      /* a nested array is flattened by CRuby: not followed here */
      int flat = 1;
      for (int i = 0; i < r.n; i++) if (r.a[i].k == MV_ARR) flat = 0;
      if (flat) {
        MxBuf b = {0};
        for (int i = 0; i < r.n; i++) mv_to_s(r.a[i], &b);
        *out = mv_str(b.p ? b.p : "", b.len, MV_STR); free(b.p); ok = 1;
      }
    }
    goto done;
  }
  /* one argument */
  if (strcmp(name, "join") == 0 && r.k == MV_ARR && (a0.k == MV_STR || a0.k == MV_NIL)) {
    int flat = 1;
    for (int i = 0; i < r.n; i++) if (r.a[i].k == MV_ARR) flat = 0;
    if (!flat) goto done;
    MxBuf b = {0};
    for (int i = 0; i < r.n; i++) {
      if (i && a0.k == MV_STR) mxb_puts(&b, a0.s);
      mv_to_s(r.a[i], &b);
    }
    *out = mv_str(b.p ? b.p : "", b.len, MV_STR); free(b.p); ok = 1;
  }
  else if (strcmp(name, "==") == 0 || strcmp(name, "!=") == 0) {
    int eq = mv_eq(r, a0);
    *out = mv_nil(); out->k = (eq == (name[0] == '=')) ? MV_TRUE : MV_FALSE; ok = 1;
  }
  /* an Integer result past a C long is a Bignum in CRuby: not followed */
  else if (strcmp(name, "+") == 0 && r.k == MV_INT && a0.k == MV_INT) {
    *out = r; ok = !__builtin_add_overflow(r.i, a0.i, &out->i);
  }
  else if (strcmp(name, "*") == 0 && r.k == MV_INT && a0.k == MV_INT) {
    *out = r; ok = !__builtin_mul_overflow(r.i, a0.i, &out->i);
  }
  else if (strcmp(name, "**") == 0 && r.k == MV_INT && a0.k == MV_INT && a0.i >= 0 && a0.i < 64) {
    long v = 1; int of = 0;
    for (long q = 0; q < a0.i && !of; q++) of = __builtin_mul_overflow(v, r.i, &v);
    *out = r; out->i = v; ok = !of;
  }
  else if (strcmp(name, "-") == 0 && r.k == MV_INT && a0.k == MV_INT) {
    *out = r; ok = !__builtin_sub_overflow(r.i, a0.i, &out->i);
  }
  else if (strcmp(name, "+") == 0 && r.k == MV_STR && a0.k == MV_STR) {
    MxBuf b = {0}; mxb_puts(&b, r.s); mxb_puts(&b, a0.s);
    *out = mv_str(b.p, b.len, MV_STR); free(b.p); ok = 1;
  }
  else if (strcmp(name, "+") == 0 && r.k == MV_ARR && a0.k == MV_ARR) {
    Mv v = r; v.a = calloc(r.n + a0.n + 1, sizeof(Mv)); v.n = 0;
    for (int i = 0; i < r.n; i++) v.a[v.n++] = r.a[i];
    for (int i = 0; i < a0.n; i++) v.a[v.n++] = a0.a[i];
    *out = v; ok = 1;
  }
  else if (strcmp(name, "[]") == 0 && r.k == MV_ARR && a0.k == MV_INT) {
    long ix = a0.i < 0 ? r.n + a0.i : a0.i;
    *out = (ix >= 0 && ix < r.n) ? r.a[ix] : mv_nil(); ok = 1;
  }
done:
  free(name);
  return ok;
}

static int mx_eval(MxCtx *c, pm_node_t *n, Mv *out) {
  if (!n) { *out = mv_nil(); return 1; }
  switch (PM_NODE_TYPE(n)) {
  case PM_NIL_NODE: *out = mv_nil(); return 1;
  case PM_TRUE_NODE: *out = mv_nil(); out->k = MV_TRUE; return 1;
  case PM_FALSE_NODE: *out = mv_nil(); out->k = MV_FALSE; return 1;
  case PM_INTEGER_NODE: {
    pm_integer_node_t *in = (pm_integer_node_t *)n;
    unsigned long uv;
    if (in->value.values) {
      if (in->value.length > 2) return 0;
      uv = (unsigned long)in->value.values[0] |
           (in->value.length > 1 ? (unsigned long)in->value.values[1] << 32 : 0);
      if (uv > 0x7fffffffffffffffUL) return 0;
    }
    else uv = in->value.value;
    *out = mv_nil(); out->k = MV_INT;
    out->i = in->value.negative ? -(long)uv : (long)uv;
    return 1;
  }
  case PM_STRING_NODE: {
    pm_string_node_t *s = (pm_string_node_t *)n;
    *out = mv_str((const char *)pm_string_source(&s->unescaped), pm_string_length(&s->unescaped), MV_STR);
    return 1;
  }
  case PM_SYMBOL_NODE: {
    pm_symbol_node_t *s = (pm_symbol_node_t *)n;
    *out = mv_str((const char *)pm_string_source(&s->unescaped), pm_string_length(&s->unescaped), MV_SYM);
    return 1;
  }
  case PM_INTERPOLATED_STRING_NODE: case PM_INTERPOLATED_SYMBOL_NODE: {
    pm_node_list_t *parts = PM_NODE_TYPE(n) == PM_INTERPOLATED_STRING_NODE
      ? &((pm_interpolated_string_node_t *)n)->parts : &((pm_interpolated_symbol_node_t *)n)->parts;
    MxBuf b = {0};
    if (!mx_eval_parts(c, parts, &b)) { free(b.p); return 0; }
    *out = mv_str(b.p ? b.p : "", b.len, PM_NODE_TYPE(n) == PM_INTERPOLATED_STRING_NODE ? MV_STR : MV_SYM);
    free(b.p);
    return 1;
  }
  case PM_ARRAY_NODE: return mx_eval_list(c, &((pm_array_node_t *)n)->elements, out);
  case PM_LOCAL_VARIABLE_READ_NODE: {
    char *nm = mx_name(((pm_local_variable_read_node_t *)n)->name);
    Mv *v = mx_lookup(c->loc, c->nloc, nm);
    free(nm);
    if (!v) return 0;
    *out = *v; return 1;
  }
  case PM_INSTANCE_VARIABLE_READ_NODE: {
    char *nm = mx_name(((pm_instance_variable_read_node_t *)n)->name);
    Mv *v = mx_lookup(c->iv, *c->niv, nm);
    free(nm);
    /* a value the evaluator cannot know (written under a condition it cannot
       decide, or where it does not look) leaves the call as it is */
    if (v && v->k == MV_UNDEF) return 0;
    if (!v && c->unsure && *c->unsure) return 0;
    *out = v ? *v : mv_nil(); return 1;
  }
  case PM_DEFINED_NODE: {
    pm_node_t *v = ((pm_defined_node_t *)n)->value;
    if (!v || PM_NODE_TYPE(v) != PM_INSTANCE_VARIABLE_READ_NODE) return 0;
    char *nm = mx_name(((pm_instance_variable_read_node_t *)v)->name);
    Mv *iv = mx_lookup(c->iv, *c->niv, nm);
    free(nm);
    if ((iv && iv->k == MV_UNDEF) || (!iv && c->unsure && *c->unsure)) return 0;
    *out = iv ? mv_str("instance-variable", 17, MV_STR) : mv_nil();
    return 1;
  }
  case PM_PARENTHESES_NODE: {
    pm_node_t *b = ((pm_parentheses_node_t *)n)->body;
    if (b && PM_NODE_TYPE(b) == PM_STATEMENTS_NODE) {
      pm_statements_node_t *s = (pm_statements_node_t *)b;
      if (s->body.size != 1) return 0;
      b = s->body.nodes[0];
    }
    return mx_eval(c, b, out);
  }
  case PM_AND_NODE: case PM_OR_NODE: {
    pm_node_t *l = PM_NODE_TYPE(n) == PM_AND_NODE ? ((pm_and_node_t *)n)->left : ((pm_or_node_t *)n)->left;
    pm_node_t *r = PM_NODE_TYPE(n) == PM_AND_NODE ? ((pm_and_node_t *)n)->right : ((pm_or_node_t *)n)->right;
    Mv lv;
    if (!mx_eval(c, l, &lv)) return 0;
    if ((PM_NODE_TYPE(n) == PM_AND_NODE) != mv_truthy(lv)) { *out = lv; return 1; }
    return mx_eval(c, r, out);
  }
  /* __LINE__ / __FILE__ are the macro source's: kept as written */
  case PM_SOURCE_LINE_NODE: case PM_SOURCE_FILE_NODE: return 0;
  case PM_CALL_NODE: return mx_eval_call(c, (pm_call_node_t *)n, out);
  default: return 0;
  }
}

/* the node's source text with every macro local and tracked ivar it reads
   replaced by its value */
/* `scope`: the blocks and lambdas entered since the macro body -- a read is
   the macro's local only at that depth (a block parameter of the same name
   is not). */
typedef struct { MxCtx *c; const uint8_t *from; MxBuf *b; int bad; uint32_t scope; } MxSubst;

static bool mx_subst_visit(const pm_node_t *n, void *data) {
  MxSubst *s = (MxSubst *)data;
  if (s->bad) return false;
  /* children visited out of source order (`x if c`) cannot be spliced */
  if (n->location.start < s->from) { s->bad = 1; return false; }
  if (PM_NODE_TYPE(n) == PM_BLOCK_NODE || PM_NODE_TYPE(n) == PM_LAMBDA_NODE) {
    s->scope++;
    pm_visit_child_nodes(n, mx_subst_visit, s);
    s->scope--;
    return false;
  }
  if (PM_NODE_TYPE(n) == PM_DEF_NODE) { s->bad = 1; return false; }   /* a scope of its own */
  Mv v; int is_var = 0; char *nm = NULL;
  pm_constant_id_t wn = 0; uint32_t wd = 0;
  switch (PM_NODE_TYPE(n)) {
  case PM_LOCAL_VARIABLE_WRITE_NODE: wn = ((pm_local_variable_write_node_t *)n)->name; wd = ((pm_local_variable_write_node_t *)n)->depth; break;
  case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: wn = ((pm_local_variable_operator_write_node_t *)n)->name; wd = ((pm_local_variable_operator_write_node_t *)n)->depth; break;
  case PM_LOCAL_VARIABLE_OR_WRITE_NODE: wn = ((pm_local_variable_or_write_node_t *)n)->name; wd = ((pm_local_variable_or_write_node_t *)n)->depth; break;
  case PM_LOCAL_VARIABLE_AND_WRITE_NODE: wn = ((pm_local_variable_and_write_node_t *)n)->name; wd = ((pm_local_variable_and_write_node_t *)n)->depth; break;
  case PM_LOCAL_VARIABLE_TARGET_NODE: wn = ((pm_local_variable_target_node_t *)n)->name; wd = ((pm_local_variable_target_node_t *)n)->depth; break;
  default: break;
  }
  if (wn && wd == s->scope) {
    /* the text assigning a macro local: its later reads are not the value
       substituted here */
    char *w = mx_name(wn);
    if (mx_lookup(s->c->loc, s->c->nloc, w)) s->bad = 1;
    free(w);
    if (s->bad) return false;
  }
  if (PM_NODE_TYPE(n) == PM_LOCAL_VARIABLE_READ_NODE &&
      ((pm_local_variable_read_node_t *)n)->depth == s->scope) {
    nm = mx_name(((pm_local_variable_read_node_t *)n)->name);
    Mv *p = mx_lookup(s->c->loc, s->c->nloc, nm);
    if (p) { v = *p; is_var = 1; }
  }
  if (nm) free(nm);
  if (!is_var) return true;
  mxb_putn(s->b, (const char *)s->from, (size_t)(n->location.start - s->from));
  mxb_puts(s->b, "(");
  mv_inspect(v, s->b);
  mxb_puts(s->b, ")");
  s->from = n->location.end;
  return false;
}

/* `scope`: the depth of the macro's own locals as seen from `n` (1 in a
   lambda body the macro passes on). */
static int mx_subst_text_at(MxCtx *c, pm_node_t *n, MxBuf *b, uint32_t scope) {
  MxSubst s = { c, n->location.start, b, 0, scope };
  /* `n` itself too: a read, a write of a macro local, or a block or lambda
     handed over whole (whose body is one scope further in) */
  if (mx_subst_visit(n, &s)) pm_visit_child_nodes(n, mx_subst_visit, &s);
  if (s.bad || s.from > n->location.end) return 0;
  mxb_putn(b, (const char *)s.from, (size_t)(n->location.end - s.from));
  return 1;
}

static int mx_subst_text(MxCtx *c, pm_node_t *n, MxBuf *b) { return mx_subst_text_at(c, n, b, 0); }

/* a residual expression: its value when computable, else its text with the
   macro values substituted; `public_send(name, ...)` becomes a plain call */
static int mx_residual(MxCtx *c, pm_node_t *n, MxBuf *b) {
  Mv v;
  if (mx_eval(c, n, &v)) { mv_inspect(v, b); return 1; }
  if (PM_NODE_TYPE(n) == PM_CALL_NODE) {
    pm_call_node_t *cn = (pm_call_node_t *)n;
    char *name = mx_name(cn->name);
    int snd = strcmp(name, "public_send") == 0 || strcmp(name, "send") == 0 || strcmp(name, "__send__") == 0;
    free(name);
    if (snd && (!cn->receiver || PM_NODE_TYPE(cn->receiver) == PM_SELF_NODE) && cn->arguments &&
        cn->arguments->arguments.size >= 1 && !cn->block) {
      Mv mn;
      if (!mx_eval(c, cn->arguments->arguments.nodes[0], &mn) || (mn.k != MV_STR && mn.k != MV_SYM) ||
          !mx_sym_plain(mn.s)) return 0;
      /* `attr=(v)` would read as a local assignment, not a call */
      if (mn.s[0] && mn.s[strlen(mn.s) - 1] == '=') return 0;
      /* a send to a macro, instance_variable_set or a class method of the
         program's writes what the evaluator does not follow */
      if (mx_writer_name(mn.s)) return 0;
      mxb_puts(b, mn.s);
      mxb_puts(b, "(");
      for (size_t i = 1; i < cn->arguments->arguments.size; i++) {
        if (i > 1) mxb_puts(b, ", ");
        if (!mx_residual(c, cn->arguments->arguments.nodes[i], b)) return 0;
      }
      mxb_puts(b, ")");
      return 1;
    }
  }
  return mx_subst_text(c, n, b);
}

static int mx_is_raise(pm_node_t *n) {
  if (PM_NODE_TYPE(n) != PM_CALL_NODE) return 0;
  pm_call_node_t *cn = (pm_call_node_t *)n;
  if (cn->receiver) return 0;
  char *nm = mx_name(cn->name);
  int r = strcmp(nm, "raise") == 0 || strcmp(nm, "fail") == 0;
  free(nm);
  return r;
}

/* A write the evaluator cannot follow may have changed any ivar: every known
   one is unknown now, and so is every one not seen yet. */
static void mx_forget(MxVar *iv, int niv, int *unsure) {
  for (int i = 0; i < niv; i++) iv[i].v = mv_undef();
  if (unsure) *unsure = 1;
}

/* `instance_variable_set(:@x, v)`: the write, when the name and the value
   are computed here, else nothing about the state is known any more. */
static void mx_ivar_set(MxCtx *c, pm_node_list_t *args) {
  Mv nm, v;
  if (args && args->size == 2 && mx_eval(c, args->nodes[0], &nm) &&
      (nm.k == MV_SYM || nm.k == MV_STR) && nm.s[0] == '@' && nm.s[1] != '@' &&
      mx_eval(c, args->nodes[1], &v)) {
    mx_set(c->iv, c->niv, 64, nm.s, v);
    if (mx_lookup(c->iv, *c->niv, nm.s)) return;   /* else the table was full */
  }
  mx_forget(c->iv, *c->niv, c->unsure);
}

static int mx_exec(MxCtx *c, pm_node_t *n) {
  if (c->returned || c->stopped) return 1;
  MxBuf *o = c->out;
  switch (PM_NODE_TYPE(n)) {
  case PM_RETURN_NODE: {
    pm_return_node_t *r = (pm_return_node_t *)n;
    if (r->arguments && r->arguments->arguments.size > 1) return 0;
    if (!mx_eval(c, r->arguments ? r->arguments->arguments.nodes[0] : NULL, &c->ret)) return 0;
    c->returned = 1;
    return 1;
  }
  case PM_LOCAL_VARIABLE_WRITE_NODE: {
    pm_local_variable_write_node_t *w = (pm_local_variable_write_node_t *)n;
    Mv v;
    if (!mx_eval(c, w->value, &v)) return 0;
    char *nm = mx_name(w->name);
    mx_set(c->loc, &c->nloc, 64, nm, v);
    free(nm);
    c->ret = v;
    return 1;
  }
  case PM_INSTANCE_VARIABLE_WRITE_NODE: {
    pm_instance_variable_write_node_t *w = (pm_instance_variable_write_node_t *)n;
    Mv v;
    if (!mx_eval(c, w->value, &v)) return 0;
    char *nm = mx_name(w->name);
    mx_set(c->iv, c->niv, 64, nm, v);
    if (o) { mxb_puts(o, nm); mxb_puts(o, " = "); mv_inspect(v, o); mxb_puts(o, "\n"); }
    free(nm);
    c->ret = v;
    return 1;
  }
  case PM_IF_NODE: case PM_UNLESS_NODE: {
    int is_if = PM_NODE_TYPE(n) == PM_IF_NODE;
    pm_node_t *pred = is_if ? ((pm_if_node_t *)n)->predicate : ((pm_unless_node_t *)n)->predicate;
    Mv pv;
    if (!mx_eval(c, pred, &pv)) return 0;
    int take = mv_truthy(pv) == is_if;
    pm_statements_node_t *then = is_if ? ((pm_if_node_t *)n)->statements : ((pm_unless_node_t *)n)->statements;
    pm_node_t *els = is_if ? ((pm_if_node_t *)n)->subsequent : (pm_node_t *)((pm_unless_node_t *)n)->else_clause;
    if (take) return then ? mx_exec_stmts(c, then) : (c->ret = mv_nil(), 1);
    if (!els) { c->ret = mv_nil(); return 1; }
    if (PM_NODE_TYPE(els) == PM_ELSE_NODE) {
      pm_statements_node_t *es = ((pm_else_node_t *)els)->statements;
      return es ? mx_exec_stmts(c, es) : (c->ret = mv_nil(), 1);
    }
    return mx_exec(c, els);   /* elsif */
  }
  case PM_BEGIN_NODE: {
    pm_begin_node_t *bn = (pm_begin_node_t *)n;
    if (bn->else_clause || bn->ensure_clause) return 0;
    if (!bn->rescue_clause) return bn->statements ? mx_exec_stmts(c, bn->statements) : 1;
    /* how far the body gets, and whether a rescue body runs, is decided at
       run time: an ivar any of them writes is unknown afterwards */
    if (!o) {
      if (mx_undef_writes(n, c->iv, c->niv, c->unsure) && c->ext_unsure) *c->ext_unsure = 1;
      return 1;
    }
    mxb_puts(o, "begin\n");
    size_t body_at = o->len;
    if (bn->statements && !mx_exec_stmts(c, bn->statements)) return 0;
    c->stopped = 0;
    for (pm_rescue_node_t *r = bn->rescue_clause; r; r = r->subsequent) {
      mxb_puts(o, "rescue");
      for (size_t i = 0; i < r->exceptions.size; i++) {
        mxb_puts(o, i ? ", " : " ");
        pm_node_t *e = r->exceptions.nodes[i];
        mxb_putn(o, (const char *)e->location.start, (size_t)(e->location.end - e->location.start));
      }
      if (r->reference) {
        mxb_puts(o, " => ");
        mxb_putn(o, (const char *)r->reference->location.start,
                 (size_t)(r->reference->location.end - r->reference->location.start));
      }
      mxb_puts(o, "\n");
      int sv_fr = c->in_ffi_rescue;
      c->in_ffi_rescue = strstr(o->p + body_at, "attach_function") != NULL;
      if (r->statements && !mx_exec_stmts(c, r->statements)) return 0;
      c->in_ffi_rescue = sv_fr;
      c->stopped = 0;
    }
    mxb_puts(o, "end\n");
    if (mx_undef_writes(n, c->iv, c->niv, c->unsure) && c->ext_unsure) *c->ext_unsure = 1;
    return 1;
  }
  case PM_CALL_NODE: {
    pm_call_node_t *cn = (pm_call_node_t *)n;
    char *name = mx_name(cn->name);
    int self_recv = !cn->receiver || PM_NODE_TYPE(cn->receiver) == PM_SELF_NODE;
    pm_node_list_t *args = cn->arguments ? &cn->arguments->arguments : NULL;
    size_t argc = args ? args->size : 0;
    int ok = 0;
    int refuse = 0;
    MxMacro *m = self_recv ? mx_resolve_self(c->modname, c->ext, c->next, name, &refuse) : NULL;
    if (refuse) { free(name); return 0; }
    if (m) {
      Mv r;
      ok = c->depth < 16 && mx_call_macro(c, m, cn->arguments, &r);
      if (ok) c->ret = r;
    }
    else if (self_recv && argc >= 1 && !cn->block &&
             /* only these evaluate a string as the class body: instance_eval
                defines on the singleton class, and the _exec forms take a
                block, not code */
             (strcmp(name, "module_eval") == 0 || strcmp(name, "class_eval") == 0)) {
      Mv code;
      /* code that writes the state (now, or from a class method it
         defines) is not followed: the macro is left as written */
      if (mx_eval(c, args->nodes[0], &code) && code.k == MV_STR && !mx_code_writes(code.s)) {
        if (o) { mxb_puts(o, code.s); mxb_puts(o, "\n"); }
        c->ret = mv_nil();
        ok = 1;
      }
    }
    else if (self_recv && argc == 2 && strcmp(name, "const_set") == 0 && !cn->block) {
      Mv cnm;
      if (mx_eval(c, args->nodes[0], &cnm) && (cnm.k == MV_STR || cnm.k == MV_SYM) &&
          isupper((unsigned char)cnm.s[0]) && mx_sym_plain(cnm.s)) {
        ok = 1;
        if (o) {
          mxb_puts(o, cnm.s); mxb_puts(o, " = ");
          ok = mx_residual(c, args->nodes[1], o);
          mxb_puts(o, "\n");
        }
      }
    }
    else if (self_recv && argc == 2 && !cn->block &&
             (strcmp(name, "define_singleton_method") == 0 || strcmp(name, "define_method") == 0) &&
             PM_NODE_TYPE(args->nodes[1]) == PM_LAMBDA_NODE &&
             !((pm_lambda_node_t *)args->nodes[1])->parameters) {
      Mv mn;
      /* a class method whose body writes the state, called later, writes
         what is not followed: the macro is left as written */
      MxSdefScan ws = { 0, 0 };
      pm_lambda_node_t *wl = (pm_lambda_node_t *)args->nodes[1];
      if (name[7] == 's' && wl->body) pm_visit_node(wl->body, mx_sdef_visit, &ws);
      if (ws.hit) ok = 0;
      else if (mx_eval(c, args->nodes[0], &mn) && (mn.k == MV_STR || mn.k == MV_SYM) && mx_sym_plain(mn.s)) {
        ok = 1;
        if (o && c->in_ffi_rescue && name[7] == 's') {
          /* standing in for the function attach_function could not find:
             a static def would replace the attached one */
          mxb_puts(o, "__ffi_define_proc(:");
          mxb_puts(o, mn.s);
          mxb_puts(o, ", ");
          ok = mx_residual(c, args->nodes[1], o);
          mxb_puts(o, ")\n");
        }
        else if (o) {
          pm_lambda_node_t *lam = (pm_lambda_node_t *)args->nodes[1];
          /* the body runs when the method is called, on that method's self:
             its text, with only the macro's locals (captured now)
             substituted -- an ivar read or a macro call in it is not
             evaluated now */
          mxb_puts(o, name[7] == 's' ? "def self." : "def ");
          mxb_puts(o, mn.s);
          mxb_puts(o, "\n");
          if (lam->body) {
            if (PM_NODE_TYPE(lam->body) == PM_STATEMENTS_NODE) {
              pm_statements_node_t *st = (pm_statements_node_t *)lam->body;
              for (size_t i = 0; i < st->body.size && ok; i++) {
                ok = mx_subst_text_at(c, st->body.nodes[i], o, 1);
                mxb_puts(o, "\n");
              }
            }
            else { ok = mx_subst_text_at(c, lam->body, o, 1); mxb_puts(o, "\n"); }
          }
          mxb_puts(o, "end\n");
        }
      }
    }
    else {
      /* any other call is residual code -- unless it is made on a value the
         evaluator holds (`@fields << n`, `l.push(n)` with l = @fields): what
         it changes there is not followed, so the macro is not expanded */
      Mv rv;
      if (cn->receiver && PM_NODE_TYPE(cn->receiver) != PM_SELF_NODE &&
          mx_eval(c, cn->receiver, &rv) && (rv.k == MV_ARR || rv.k == MV_STR)) {
        free(name);
        return 0;
      }
      ok = 1;
      c->ret = mv_undef();   /* its value is the run time's */
      if (self_recv && strcmp(name, "instance_variable_set") == 0) mx_ivar_set(c, args);
      /* what else it writes when it runs (a block's ivar writes and macro
         calls, a class method of the program's) is not followed */
      else if (mx_undef_writes(n, c->iv, c->niv, c->unsure) && c->ext_unsure) *c->ext_unsure = 1;
      if (o) {
        if (self_recv && !cn->receiver && !cn->block && argc > 0 && strcmp(name, "public_send") != 0 &&
            strcmp(name, "send") != 0) {
          /* keep the call's shape, arguments substituted one by one */
          mxb_puts(o, name);
          mxb_puts(o, " ");
          for (size_t i = 0; i < argc && ok; i++) {
            if (i) mxb_puts(o, ", ");
            pm_node_t *a = args->nodes[i];
            if (PM_NODE_TYPE(a) == PM_KEYWORD_HASH_NODE) {
              pm_keyword_hash_node_t *kh = (pm_keyword_hash_node_t *)a;
              for (size_t j = 0; j < kh->elements.size && ok; j++) {
                if (j) mxb_puts(o, ", ");
                pm_node_t *el = kh->elements.nodes[j];
                if (PM_NODE_TYPE(el) != PM_ASSOC_NODE) { ok = 0; break; }
                pm_assoc_node_t *as = (pm_assoc_node_t *)el;
                ok = mx_residual(c, as->key, o);
                mxb_puts(o, " => ");
                if (ok) ok = mx_residual(c, as->value, o);
              }
            }
            else ok = mx_residual(c, a, o);
          }
        }
        else ok = mx_residual(c, n, o);
        mxb_puts(o, "\n");
      }
      if (ok && mx_is_raise(n)) c->stopped = 1;
    }
    free(name);
    return ok;
  }
  default: {
    Mv v;
    if (mx_eval(c, n, &v)) { c->ret = v; return 1; }
    return 0;
  }
  }
}

static int mx_exec_stmts(MxCtx *c, pm_statements_node_t *s) {
  if (!s) return 1;
  for (size_t i = 0; i < s->body.size; i++) {
    if (c->returned || c->stopped) break;
    if (!mx_exec(c, s->body.nodes[i])) return 0;
  }
  return 1;
}

/* bind the macro's parameters to the call's (literal) arguments and run it */
static int mx_call_macro(MxCtx *c, MxMacro *m, pm_arguments_node_t *argn, Mv *ret) {
  MxCtx sub; memset(&sub, 0, sizeof sub);
  sub.iv = c->iv; sub.niv = c->niv; sub.out = c->out; sub.depth = c->depth + 1;
  sub.unsure = c->unsure;
  sub.ext = c->ext; sub.next = c->next; sub.ext_unsure = c->ext_unsure;
  sub.modname = m->module;
  pm_node_list_t *args = argn ? &argn->arguments : NULL;
  size_t argc = args ? args->size : 0;
  pm_keyword_hash_node_t *kw = NULL;
  if (argc > 0 && PM_NODE_TYPE(args->nodes[argc - 1]) == PM_KEYWORD_HASH_NODE) {
    kw = (pm_keyword_hash_node_t *)args->nodes[argc - 1];
    argc--;
  }
  for (size_t i = 0; i < argc; i++)
    if (PM_NODE_TYPE(args->nodes[i]) == PM_SPLAT_NODE || PM_NODE_TYPE(args->nodes[i]) == PM_BLOCK_ARGUMENT_NODE)
      return 0;
  pm_parameters_node_t *ps = m->def->parameters;
  size_t nreq = ps ? ps->requireds.size : 0, nopt = ps ? ps->optionals.size : 0;
  if (ps && (ps->rest || ps->posts.size || ps->keyword_rest || ps->block)) return 0;
  if (argc < nreq || argc > nreq + nopt) return 0;
  size_t ai = 0;
  for (size_t i = 0; i < nreq; i++, ai++) {
    Mv v;
    if (PM_NODE_TYPE(ps->requireds.nodes[i]) != PM_REQUIRED_PARAMETER_NODE) return 0;
    if (!mx_eval(c, args->nodes[ai], &v)) return 0;
    char *nm = mx_name(((pm_required_parameter_node_t *)ps->requireds.nodes[i])->name);
    mx_set(sub.loc, &sub.nloc, 64, nm, v); free(nm);
  }
  for (size_t i = 0; i < nopt; i++) {
    pm_optional_parameter_node_t *op = (pm_optional_parameter_node_t *)ps->optionals.nodes[i];
    Mv v;
    if (ai < argc) { if (!mx_eval(c, args->nodes[ai++], &v)) return 0; }
    else if (!mx_eval(&sub, op->value, &v)) return 0;
    char *nm = mx_name(op->name);
    mx_set(sub.loc, &sub.nloc, 64, nm, v); free(nm);
  }
  size_t nkw = ps ? ps->keywords.size : 0;
  if (kw && nkw == 0) return 0;
  if (kw) {
    /* every given keyword must name a parameter */
    for (size_t j = 0; j < kw->elements.size; j++) {
      pm_node_t *el = kw->elements.nodes[j];
      if (PM_NODE_TYPE(el) != PM_ASSOC_NODE) return 0;
      Mv k;
      if (!mx_eval(c, ((pm_assoc_node_t *)el)->key, &k) || k.k != MV_SYM) return 0;
      int found = 0;
      for (size_t i = 0; i < nkw && !found; i++) {
        pm_node_t *kp = ps->keywords.nodes[i];
        pm_constant_id_t kid = PM_NODE_TYPE(kp) == PM_OPTIONAL_KEYWORD_PARAMETER_NODE
          ? ((pm_optional_keyword_parameter_node_t *)kp)->name
          : ((pm_required_keyword_parameter_node_t *)kp)->name;
        char *kn = mx_name(kid);
        size_t kl = strlen(kn);
        if (kl && kn[kl - 1] == ':') kn[kl - 1] = 0;
        found = strcmp(kn, k.s) == 0;
        free(kn);
      }
      if (!found) return 0;
    }
  }
  for (size_t i = 0; i < nkw; i++) {
    pm_node_t *kp = ps->keywords.nodes[i];
    int req = PM_NODE_TYPE(kp) == PM_REQUIRED_KEYWORD_PARAMETER_NODE;
    pm_constant_id_t kid = req ? ((pm_required_keyword_parameter_node_t *)kp)->name
                               : ((pm_optional_keyword_parameter_node_t *)kp)->name;
    char *kn = mx_name(kid);
    size_t kl = strlen(kn);
    if (kl && kn[kl - 1] == ':') kn[kl - 1] = 0;
    Mv v; int have = 0;
    for (size_t j = 0; kw && j < kw->elements.size && !have; j++) {
      pm_assoc_node_t *as = (pm_assoc_node_t *)kw->elements.nodes[j];
      Mv k;
      if (mx_eval(c, as->key, &k) && k.k == MV_SYM && strcmp(k.s, kn) == 0) {
        if (!mx_eval(c, as->value, &v)) { free(kn); return 0; }
        have = 1;
      }
    }
    if (!have) {
      if (req) { free(kn); return 0; }
      if (!mx_eval(&sub, ((pm_optional_keyword_parameter_node_t *)kp)->value, &v)) { free(kn); return 0; }
    }
    mx_set(sub.loc, &sub.nloc, 64, kn, v);
    free(kn);
  }
  sub.ret = mv_nil();
  pm_node_t *body = m->def->body;
  int ok = 1;
  if (body) {
    if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) ok = mx_exec_stmts(&sub, (pm_statements_node_t *)body);
    else if (PM_NODE_TYPE(body) == PM_BEGIN_NODE) ok = mx_exec(&sub, body);
    else ok = mx_exec(&sub, body);
  }
  if (!ok) return 0;
  *ret = sub.ret;
  return 1;
}

/* does the macro (or a macro it calls) do something that only its expansion
   can give the compiler? */
typedef struct { const char *mod; int found; int depth; } MxDyn;
static int mx_macro_dynamic(MxMacro *m, int depth);

static bool mx_dyn_visit(const pm_node_t *n, void *data) {
  MxDyn *d = (MxDyn *)data;
  if (d->found) return false;
  if (PM_NODE_TYPE(n) == PM_CALL_NODE) {
    pm_call_node_t *cn = (pm_call_node_t *)n;
    char *nm = mx_name(cn->name);
    int self_recv = !cn->receiver || PM_NODE_TYPE(cn->receiver) == PM_SELF_NODE;
    if (self_recv) {
      static const char *const DYN[] = { "module_eval", "class_eval", "instance_eval",
        "const_set", "define_method", "define_singleton_method", "public_send", "send",
        "__send__", NULL };
      for (int i = 0; DYN[i]; i++) if (strcmp(nm, DYN[i]) == 0) d->found = 1;
      if (strcmp(nm, "attach_function") == 0 && cn->arguments && cn->arguments->arguments.size >= 1) {
        pm_node_t *a0 = cn->arguments->arguments.nodes[0];
        if (PM_NODE_TYPE(a0) != PM_SYMBOL_NODE && PM_NODE_TYPE(a0) != PM_STRING_NODE) d->found = 1;
      }
      MxMacro *sub = mx_find_macro(d->mod, nm);
      if (sub && d->depth < 8 && mx_macro_dynamic(sub, d->depth + 1)) d->found = 1;
    }
    free(nm);
  }
  return !d->found;
}

static int mx_macro_dynamic(MxMacro *m, int depth) {
  if (m->dynamic >= 0) return m->dynamic;
  if (depth > 8) return 0;
  MxDyn d = { m->module, 0, depth };
  if (m->def->body) pm_visit_node(m->def->body, mx_dyn_visit, &d);
  if (depth == 0) m->dynamic = d.found;
  return d.found;
}

static char *mx_last_name(pm_node_t *cp) {
  if (!cp) return NULL;
  if (PM_NODE_TYPE(cp) == PM_CONSTANT_READ_NODE) return mx_name(((pm_constant_read_node_t *)cp)->name);
  if (PM_NODE_TYPE(cp) == PM_CONSTANT_PATH_NODE) return mx_name(((pm_constant_path_node_t *)cp)->name);
  return NULL;
}

/* collect module instance-method defs */

static void mx_sdef_add(const char *name, pm_node_t *body) {
  g_mx_sdef_bodies = realloc(g_mx_sdef_bodies, sizeof(MxSdef) * (size_t)(g_mx_nsdef_bodies + 1));
  g_mx_sdef_bodies[g_mx_nsdef_bodies].name = strdup(name);
  g_mx_sdef_bodies[g_mx_nsdef_bodies].body = body;
  g_mx_nsdef_bodies++;
}

/* modules with a `self.extended(base)` hook, which runs at each extend */
typedef struct { char *module; pm_def_node_t *def; } MxHook;
static MxHook *g_mx_hooks; static int g_mx_nhooks;

static void mx_hook_add(const char *mname, pm_def_node_t *d) {
  g_mx_hooks = realloc(g_mx_hooks, sizeof(MxHook) * (size_t)(g_mx_nhooks + 1));
  g_mx_hooks[g_mx_nhooks].module = strdup(mname);
  g_mx_hooks[g_mx_nhooks].def = d;
  g_mx_nhooks++;
}

/* The class methods a class or module body defines: `def self.x`, and the
   defs of a `class << self` in it. With the module's name, its `extended`
   hook is recorded too. */
static MxNames g_mx_ihooks;    /* modules with an included/prepended hook */
static MxNames g_mx_inherited; /* classes and modules with an inherited hook */
/* every module and class by its last name and full path: a last name two
   paths share cannot say which one an `extend` means */
static MxNames g_mx_paths;

static void mx_note_hook(const char *owner, const char *dn) {
  if (!owner) return;
  if (strcmp(dn, "included") == 0 || strcmp(dn, "prepended") == 0) mx_names_add(&g_mx_ihooks, owner);
  if (strcmp(dn, "inherited") == 0) mx_names_add(&g_mx_inherited, owner);
}

static int mx_ambiguous(const char *last) {
  size_t ll = strlen(last);
  const char *first = NULL;
  for (int i = 0; i < g_mx_paths.n; i++) {
    const char *p = g_mx_paths.v[i];
    size_t pl = strlen(p);
    if (pl < ll + 2 || strcmp(p + pl - ll, last) != 0 || p[pl - ll - 1] != ':') continue;
    if (!first) first = p;
    else if (strcmp(first, p) != 0) return 1;
  }
  return 0;
}

static void mx_collect_sdefs(pm_statements_node_t *st, const char *mname, const char *owner) {
  for (size_t i = 0; i < st->body.size; i++) {
    pm_node_t *s = st->body.nodes[i];
    if (PM_NODE_TYPE(s) == PM_DEF_NODE && ((pm_def_node_t *)s)->receiver &&
        PM_NODE_TYPE(((pm_def_node_t *)s)->receiver) == PM_SELF_NODE) {
      char *dn = mx_name(((pm_def_node_t *)s)->name);
      mx_names_add(&g_mx_sdefs, dn);
      mx_sdef_add(dn, ((pm_def_node_t *)s)->body);
      mx_note_hook(owner, dn);
      if (mname && strcmp(dn, "extended") == 0) mx_hook_add(mname, (pm_def_node_t *)s);
      free(dn);
    }
    else if (PM_NODE_TYPE(s) == PM_SINGLETON_CLASS_NODE &&
             PM_NODE_TYPE(((pm_singleton_class_node_t *)s)->expression) == PM_SELF_NODE) {
      pm_node_t *sb = ((pm_singleton_class_node_t *)s)->body;
      if (!sb || PM_NODE_TYPE(sb) != PM_STATEMENTS_NODE) continue;
      pm_statements_node_t *ss = (pm_statements_node_t *)sb;
      for (size_t j = 0; j < ss->body.size; j++) {
        if (PM_NODE_TYPE(ss->body.nodes[j]) != PM_DEF_NODE) continue;
        char *dn = mx_name(((pm_def_node_t *)ss->body.nodes[j])->name);
        mx_names_add(&g_mx_sdefs, dn);
        mx_sdef_add(dn, ((pm_def_node_t *)ss->body.nodes[j])->body);
        mx_note_hook(owner, dn);
        if (mname && strcmp(dn, "extended") == 0) mx_hook_add(mname, (pm_def_node_t *)ss->body.nodes[j]);
        free(dn);
      }
    }
  }
}

/* The walk over the program collecting macros, class methods and hooks,
   with the path of the class or module it is in. */
static bool mx_collect_visit(const pm_node_t *n, void *data) {
  const char *path = data ? (const char *)data : "";
  if (PM_NODE_TYPE(n) != PM_CLASS_NODE && PM_NODE_TYPE(n) != PM_MODULE_NODE) return true;
  int is_mod = PM_NODE_TYPE(n) == PM_MODULE_NODE;
  pm_node_t *cp = is_mod ? ((pm_module_node_t *)n)->constant_path : ((pm_class_node_t *)n)->constant_path;
  pm_node_t *body = is_mod ? ((pm_module_node_t *)n)->body : ((pm_class_node_t *)n)->body;
  size_t cl = (size_t)(cp->location.end - cp->location.start);
  char *full = malloc(strlen(path) + cl + 3);
  sprintf(full, "%s::%.*s", path, (int)cl, (const char *)cp->location.start);
  mx_names_add(&g_mx_paths, full);
  char *mname = mx_last_name(cp);
  /* the ffi package's own FFI: its methods are the runtime */
  if (is_mod && strcmp(full, "::FFI") == 0) { free(mname); free(full); return false; }
  if (mname && body && PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
    pm_statements_node_t *st = (pm_statements_node_t *)body;
    mx_collect_sdefs(st, is_mod ? mname : NULL, mname);
    for (size_t i = 0; is_mod && i < st->body.size; i++) {
      pm_node_t *s = st->body.nodes[i];
      if (PM_NODE_TYPE(s) != PM_DEF_NODE || ((pm_def_node_t *)s)->receiver) continue;
      if (g_mx_nmacros == g_mx_cmacros) {
        g_mx_cmacros = g_mx_cmacros ? g_mx_cmacros * 2 : 64;
        g_mx_macros = realloc(g_mx_macros, sizeof(MxMacro) * (size_t)g_mx_cmacros);
      }
      g_mx_macros[g_mx_nmacros].module = strdup(mname);
      g_mx_macros[g_mx_nmacros].def = (pm_def_node_t *)s;
      g_mx_macros[g_mx_nmacros].dynamic = -1;
      g_mx_macros[g_mx_nmacros].expanded = 0;
      g_mx_nmacros++;
    }
  }
  free(mname);
  if (body) pm_visit_node(body, mx_collect_visit, full);
  free(full);
  return false;
}

typedef struct { const uint8_t *start, *end; char *text; } MxEdit;
typedef struct { MxEdit *e; int n, cap; } MxEdits;

/* A class's macro state as the walk reaches each statement: the modules it
   extended so far, its ivars, and the body being walked (the statements a
   `def self.x` called from a nested branch is looked up in). */
typedef struct {
  char *path; char *ext[16]; int next; MxVar iv[64]; int niv; int unsure;
  pm_statements_node_t *top;
  int ext_unsure;   /* an extend was made under a condition: lookup is unknown */
} MxClass;
static MxClass *g_mx_classes; static int g_mx_nclasses;

static MxClass *mx_class_state(const char *path) {
  for (int i = 0; i < g_mx_nclasses; i++) if (strcmp(g_mx_classes[i].path, path) == 0) return &g_mx_classes[i];
  g_mx_classes = realloc(g_mx_classes, sizeof(MxClass) * (size_t)(g_mx_nclasses + 1));
  MxClass *k = &g_mx_classes[g_mx_nclasses++];
  memset(k, 0, sizeof *k);
  k->path = strdup(path);
  return k;
}

static void mx_class_body(pm_statements_node_t *st, MxEdits *ed, MxClass *k);

typedef struct { MxEdits *ed; const char *path; } MxWalk;

/* Calls on a class from outside its body that change what its macros read:
   by the class's last name, where in the source (NULL: from a method, so at
   any time), and whether its macro lookup changes too (an extend, an eval). */
typedef struct { char *name; const uint8_t *pos; int ext; } MxOuter;
static MxOuter *g_mx_outer; static int g_mx_nouter;
typedef struct { int in_def; } MxOuterScan;
static bool mx_outer_visit(const pm_node_t *n, void *data) {
  MxOuterScan *os = data;
  if (PM_NODE_TYPE(n) == PM_DEF_NODE && !os->in_def) {
    MxOuterScan in = { 1 };
    pm_node_t *b = ((pm_def_node_t *)n)->body;
    if (b) pm_visit_node(b, mx_outer_visit, &in);
    return false;
  }
  if (PM_NODE_TYPE(n) != PM_CALL_NODE) return true;
  const pm_call_node_t *cn = (const pm_call_node_t *)n;
  if (!cn->receiver) return true;
  int is_const = PM_NODE_TYPE(cn->receiver) == PM_CONSTANT_READ_NODE ||
                 PM_NODE_TYPE(cn->receiver) == PM_CONSTANT_PATH_NODE;
  if (!is_const && PM_NODE_TYPE(cn->receiver) != PM_LOCAL_VARIABLE_READ_NODE) return true;
  char *cname = mx_name(cn->name);
  int ext = mx_outer_ext_name(cname);
  int writer = mx_writer_name(cname);
  int wr = ext || writer || strcmp(cname, "send") == 0 ||
           strcmp(cname, "public_send") == 0 || strcmp(cname, "__send__") == 0;
  free(cname);
  if (!wr) return true;
  /* a local may hold any class: recorded under "*", and only for a macro or
     a writing class method, in straight-line code (in a method, what a local
     holds and when it runs are not known here: `key.primitive` in a method
     is not taken as a write to every class) */
  if (!is_const && (!writer || os->in_def)) return true;
  char *rn = is_const ? mx_last_name(cn->receiver) : strdup("*");
  if (!rn) return true;
  g_mx_outer = realloc(g_mx_outer, sizeof(MxOuter) * (size_t)(g_mx_nouter + 1));
  g_mx_outer[g_mx_nouter].name = rn;
  g_mx_outer[g_mx_nouter].pos = os->in_def ? NULL : n->location.start;
  g_mx_outer[g_mx_nouter].ext = ext;
  g_mx_nouter++;
  return true;
}

/* A body of a class whose state such a call may have changed before it:
   what was known is not, and with an extend, which module answers is not. */
static void mx_outer_apply(MxClass *k, pm_node_t *cp, const uint8_t *at) {
  char *ln = mx_last_name(cp);
  if (!ln) return;
  for (int i = 0; i < g_mx_nouter; i++) {
    if (strcmp(g_mx_outer[i].name, ln) != 0 && strcmp(g_mx_outer[i].name, "*") != 0) continue;
    if (g_mx_outer[i].pos && g_mx_outer[i].pos > at) continue;
    mx_forget(k->iv, k->niv, &k->unsure);
    if (g_mx_outer[i].ext) k->ext_unsure = 1;
  }
  free(ln);
}

static bool mx_class_visit(const pm_node_t *n, void *data) {
  MxWalk *w = (MxWalk *)data;
  if (PM_NODE_TYPE(n) != PM_CLASS_NODE && PM_NODE_TYPE(n) != PM_MODULE_NODE) return true;
  pm_node_t *cp = PM_NODE_TYPE(n) == PM_CLASS_NODE ? ((pm_class_node_t *)n)->constant_path
                                                  : ((pm_module_node_t *)n)->constant_path;
  pm_node_t *body = PM_NODE_TYPE(n) == PM_CLASS_NODE ? ((pm_class_node_t *)n)->body : ((pm_module_node_t *)n)->body;
  /* the path as written (`A::B` stays `A::B`), under the enclosing one */
  size_t cl = (size_t)(cp->location.end - cp->location.start);
  char *path = malloc(strlen(w->path) + cl + 3);
  sprintf(path, "%s::%.*s", w->path, (int)cl, (const char *)cp->location.start);
  if (body && PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
    MxClass *k = mx_class_state(path);
    mx_outer_apply(k, cp, n->location.start);
    /* the superclass's inherited hook ran as the class was made */
    if (PM_NODE_TYPE(n) == PM_CLASS_NODE && ((pm_class_node_t *)n)->superclass) {
      char *sl = mx_last_name(((pm_class_node_t *)n)->superclass);
      if (!sl || mx_names_has(&g_mx_inherited, sl)) mx_forget(k->iv, k->niv, &k->unsure);
      free(sl);
    }
    k->top = (pm_statements_node_t *)body;
    mx_class_body(k->top, w->ed, k);
  }
  MxWalk sub = { w->ed, path };
  if (body) pm_visit_node(body, mx_class_visit, &sub);
  free(path);
  return false;
}

/* After branches the evaluator cannot choose between: a value every branch
   left the same is kept, any other becomes unknown. */
/* Returns 0 when the table was too full to mark one: then nothing is known. */
static int mx_merge_state(MxVar *acc, int *nacc, const MxVar *br, int nbr) {
  int ok = 1;
  for (int i = 0; i < *nacc; i++) {
    Mv *b = mx_lookup((MxVar *)br, nbr, acc[i].name);
    if (!b || !mv_same(acc[i].v, *b)) acc[i].v = mv_undef();
  }
  for (int j = 0; j < nbr; j++)
    if (!mx_lookup(acc, *nacc, br[j].name)) {
      mx_set(acc, nacc, 64, br[j].name, mv_undef());
      if (!mx_lookup(acc, *nacc, br[j].name)) ok = 0;
    }
  return ok;
}

/* Is a class-body statement a write of an ivar (the evaluator's state) it
   does not run? */
static pm_constant_id_t mx_ivar_write_name(const pm_node_t *s) {
  switch (PM_NODE_TYPE(s)) {
  case PM_INSTANCE_VARIABLE_WRITE_NODE: return ((const pm_instance_variable_write_node_t *)s)->name;
  case PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE: return ((const pm_instance_variable_operator_write_node_t *)s)->name;
  case PM_INSTANCE_VARIABLE_OR_WRITE_NODE: return ((const pm_instance_variable_or_write_node_t *)s)->name;
  case PM_INSTANCE_VARIABLE_AND_WRITE_NODE: return ((const pm_instance_variable_and_write_node_t *)s)->name;
  default: return 0;
  }
}

typedef struct { MxVar *iv; int *niv; int forget; int ext; } MxUndef;

/* Every ivar a statement the walk does not follow writes (under a guard, in a
   `case`, a loop, a block) is unknown afterwards -- and all of them are when
   it calls a macro, instance_variable_set or a class method the program
   defines, which write what the walk does not see. Method and class bodies have a self of their own and are not
   looked into. */
static bool mx_undef_visit(const pm_node_t *n, void *data) {
  MxUndef *u = data;
  switch (PM_NODE_TYPE(n)) {
  case PM_DEF_NODE: case PM_CLASS_NODE: case PM_MODULE_NODE: case PM_SINGLETON_CLASS_NODE:
    return false;
  default: break;
  }
  if (PM_NODE_TYPE(n) == PM_CALL_NODE) {
    const pm_call_node_t *cn = (const pm_call_node_t *)n;
    /* on a local or a block parameter: the class, maybe, held in it (a call
       on another object's method or ivar -- `self.class.primitive`,
       `@key.primitive` -- is taken as that object's) */
    if (cn->receiver && PM_NODE_TYPE(cn->receiver) == PM_LOCAL_VARIABLE_READ_NODE) {
      char *cname = mx_name(cn->name);
      if (mx_writer_name(cname)) u->forget = 1;
      free(cname);
    }
    /* on a constant: the class being walked, maybe (`Box.kind :box`) */
    if (cn->receiver && (PM_NODE_TYPE(cn->receiver) == PM_CONSTANT_READ_NODE ||
                         PM_NODE_TYPE(cn->receiver) == PM_CONSTANT_PATH_NODE)) {
      char *cname = mx_name(cn->name);
      if (mx_outer_ext_name(cname)) { u->forget = 1; u->ext = 1; }
      else if (mx_writer_name(cname) || strcmp(cname, "send") == 0 ||
               strcmp(cname, "public_send") == 0 || strcmp(cname, "__send__") == 0) u->forget = 1;
      free(cname);
    }
    if (!cn->receiver || PM_NODE_TYPE(cn->receiver) == PM_SELF_NODE) {
      char *cname = mx_name(cn->name);
      if (mx_writer_name(cname)) u->forget = 1;
      /* an extend (or an eval) changes which module answers the macros */
      else if (mx_outer_ext_name(cname)) { u->forget = 1; u->ext = 1; }
      /* a send reaching one of those, or a name not known here */
      else if (strcmp(cname, "send") == 0 || strcmp(cname, "public_send") == 0 || strcmp(cname, "__send__") == 0) {
        const pm_node_t *a0 = cn->arguments && cn->arguments->arguments.size ? cn->arguments->arguments.nodes[0] : NULL;
        if (a0 && PM_NODE_TYPE(a0) == PM_SYMBOL_NODE) {
          const pm_string_t *us = &((const pm_symbol_node_t *)a0)->unescaped;
          char *sn = strndup((const char *)pm_string_source(us), pm_string_length(us));
          if (mx_writer_name(sn)) u->forget = 1;
          free(sn);
        }
        else if (a0 && PM_NODE_TYPE(a0) == PM_STRING_NODE) {
          const pm_string_t *us = &((const pm_string_node_t *)a0)->unescaped;
          char *sn = strndup((const char *)pm_string_source(us), pm_string_length(us));
          if (mx_writer_name(sn)) u->forget = 1;
          free(sn);
        }
        else u->forget = 1;
      }
      free(cname);
    }
  }
  pm_constant_id_t wn = mx_ivar_write_name(n);
  if (!wn && PM_NODE_TYPE(n) == PM_INSTANCE_VARIABLE_TARGET_NODE)
    wn = ((const pm_instance_variable_target_node_t *)n)->name;
  if (wn) {
    char *w = mx_name(wn);
    mx_set(u->iv, u->niv, 64, w, mv_undef());
    if (!mx_lookup(u->iv, *u->niv, w)) u->forget = 1;   /* the table was full */
    free(w);
  }
  return true;
}

/* Returns whether it may also change which module answers a macro (an
   extend or eval on a class). */
static int mx_undef_writes(pm_node_t *s, MxVar *iv, int *niv, int *unsure) {
  MxUndef u = { iv, niv, 0, 0 };
  pm_visit_node(s, mx_undef_visit, &u);
  if (u.forget) mx_forget(iv, *niv, unsure);
  return u.ext;
}

/* A literal whose text runs over a line break: joining lines would change
   what it holds (a multi-line string, a %w list, a heredoc). */
typedef struct { const uint8_t *base; int multi; } MxLitScan;
static bool mx_lit_visit(const pm_node_t *n, void *data) {
  MxLitScan *ls = data;
  const uint8_t *st = n->location.start, *en = n->location.end;
  const pm_location_t *op = NULL, *cl = NULL;
  switch (PM_NODE_TYPE(n)) {
  case PM_STRING_NODE: op = &((const pm_string_node_t *)n)->opening_loc; cl = &((const pm_string_node_t *)n)->closing_loc; break;
  case PM_X_STRING_NODE: op = &((const pm_x_string_node_t *)n)->opening_loc; cl = &((const pm_x_string_node_t *)n)->closing_loc; break;
  case PM_INTERPOLATED_STRING_NODE: op = &((const pm_interpolated_string_node_t *)n)->opening_loc; cl = &((const pm_interpolated_string_node_t *)n)->closing_loc; break;
  case PM_INTERPOLATED_X_STRING_NODE: op = &((const pm_interpolated_x_string_node_t *)n)->opening_loc; cl = &((const pm_interpolated_x_string_node_t *)n)->closing_loc; break;
  case PM_REGULAR_EXPRESSION_NODE: op = &((const pm_regular_expression_node_t *)n)->opening_loc; cl = &((const pm_regular_expression_node_t *)n)->closing_loc; break;
  case PM_INTERPOLATED_REGULAR_EXPRESSION_NODE: op = &((const pm_interpolated_regular_expression_node_t *)n)->opening_loc; cl = &((const pm_interpolated_regular_expression_node_t *)n)->closing_loc; break;
  case PM_SYMBOL_NODE: op = &((const pm_symbol_node_t *)n)->opening_loc; cl = &((const pm_symbol_node_t *)n)->closing_loc; break;
  case PM_INTERPOLATED_SYMBOL_NODE: op = &((const pm_interpolated_symbol_node_t *)n)->opening_loc; cl = &((const pm_interpolated_symbol_node_t *)n)->closing_loc; break;
  case PM_ARRAY_NODE: {
    const pm_location_t *ao = &((const pm_array_node_t *)n)->opening_loc;
    if (!ao->start || *ao->start != '%') return true;   /* a [..] list may span lines */
    break;
  }
  default: return true;
  }
  if (op && op->start && op->start < st) st = op->start;
  if (cl && cl->end && cl->end > en) en = cl->end;
  for (const uint8_t *q = st; q < en; q++) if (*q == '\n') { ls->multi = 1; return false; }
  return true;
}

/* The residual code, one statement per line, joined by `; ` on the call's
   own lines -- or NULL when that would not mean the same: trailing comments
   are cut (the one parse of the whole text says where), and a literal that
   spans lines, or a result Prism cannot parse, leaves the call as it is. */
static char *mx_join_residual(const char *t) {
  size_t tl = strlen(t);
  pm_parser_t tp;
  pm_parser_init(&tp, (const uint8_t *)t, tl, NULL);
  pm_node_t *tr = pm_parse(&tp);
  int bad = tp.error_list.size != 0;
  MxLitScan ls = { (const uint8_t *)t, 0 };
  if (!bad) pm_visit_node(tr, mx_lit_visit, &ls);
  char *drop = calloc(tl + 1, 1);
  for (pm_comment_t *cm = (pm_comment_t *)tp.comment_list.head; cm && drop; cm = (pm_comment_t *)cm->node.next)
    for (const uint8_t *q = cm->location.start; q < cm->location.end; q++)
      if (*q != '\n') drop[q - (const uint8_t *)t] = 1;
  pm_node_destroy(&tp, tr);
  pm_parser_free(&tp);
  if (bad || ls.multi || !drop) { free(drop); return NULL; }
  MxBuf rep = {0};
  int first = 1;
  size_t i = 0;
  while (i < tl) {
    size_t e = i;
    while (e < tl && t[e] != '\n') e++;
    size_t a = i, z = e;
    while (z > a && drop[z - 1]) z--;                      /* the trailing comment */
    while (a < z && (t[a] == ' ' || t[a] == '\t')) a++;
    while (z > a && (t[z - 1] == ' ' || t[z - 1] == '\t' || t[z - 1] == '\r')) z--;
    if (z > a && !drop[a]) {
      if (!first) mxb_puts(&rep, "; ");
      mxb_putn(&rep, t + a, z - a);
      first = 0;
    }
    i = e + 1;
  }
  free(drop);
  if (first) mxb_puts(&rep, "nil");
  pm_parser_t cp;
  pm_parser_init(&cp, (const uint8_t *)rep.p, rep.len, NULL);
  pm_node_t *cr = pm_parse(&cp);
  bad = cp.error_list.size != 0;
  pm_node_destroy(&cp, cr);
  pm_parser_free(&cp);
  if (bad) { free(rep.p); return NULL; }
  return rep.p;
}

/* Does a hook statement call anything on the class it was given? */
typedef struct { pm_constant_id_t param; int hit; } MxOnParam;
static bool mx_on_param_visit(const pm_node_t *n, void *data) {
  MxOnParam *op = data;
  if (PM_NODE_TYPE(n) == PM_CALL_NODE) {
    const pm_node_t *r = ((const pm_call_node_t *)n)->receiver;
    if (r && PM_NODE_TYPE(r) == PM_LOCAL_VARIABLE_READ_NODE &&
        ((const pm_local_variable_read_node_t *)r)->name == op->param) op->hit = 1;
  }
  return !op->hit;
}

/* A module's `self.extended(base)` hook, run as `extend` reaches it:
   `base.instance_variable_set(...)` and `base.<macro> ...` are writes the
   state follows (a macro leaving residual code, or anything else done to
   base, forgets it); other calls on base are outside the macros (extend,
   ffi_lib) and write nothing the macros read. */
static void mx_run_hook(pm_def_node_t *d, MxVar *iv, int *niv, int *unsure) {
  pm_parameters_node_t *ps = d->parameters;
  if (!ps || ps->requireds.size != 1 || PM_NODE_TYPE(ps->requireds.nodes[0]) != PM_REQUIRED_PARAMETER_NODE) {
    mx_forget(iv, *niv, unsure);
    return;
  }
  pm_constant_id_t param = ((pm_required_parameter_node_t *)ps->requireds.nodes[0])->name;
  pm_node_t *body = d->body;
  if (!body) return;
  pm_node_t **stmts = &body; size_t ns = 1;
  if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
    stmts = ((pm_statements_node_t *)body)->body.nodes;
    ns = ((pm_statements_node_t *)body)->body.size;
  }
  for (size_t i = 0; i < ns; i++) {
    pm_node_t *s = stmts[i];
    pm_call_node_t *cn = PM_NODE_TYPE(s) == PM_CALL_NODE ? (pm_call_node_t *)s : NULL;
    if (cn && cn->receiver && !cn->block && PM_NODE_TYPE(cn->receiver) == PM_LOCAL_VARIABLE_READ_NODE &&
        ((pm_local_variable_read_node_t *)cn->receiver)->name == param) {
      char *name = mx_name(cn->name);
      MxCtx vc; memset(&vc, 0, sizeof vc);
      vc.iv = iv; vc.niv = niv; vc.unsure = unsure;
      MxMacro *m = mx_find_macro(NULL, name);
      if (strcmp(name, "instance_variable_set") == 0)
        mx_ivar_set(&vc, cn->arguments ? &cn->arguments->arguments : NULL);
      else if (m) {
        MxBuf scratch = {0};
        vc.out = &scratch; vc.modname = m->module;
        MxVar sv[64]; int nsv = *niv;
        memcpy(sv, iv, sizeof(MxVar) * (size_t)nsv);
        Mv r;
        g_mx_full = 0;
        int ok = mx_call_macro(&vc, m, cn->arguments, &r) && !g_mx_full && scratch.len == 0;
        free(scratch.p);
        if (!ok) { memcpy(iv, sv, sizeof(MxVar) * (size_t)nsv); *niv = nsv; mx_forget(iv, *niv, unsure); }
      }
      /* a send (to a macro or instance_variable_set, maybe) or a class
         method of the program's that writes */
      else if (strcmp(name, "send") == 0 || strcmp(name, "public_send") == 0 ||
               strcmp(name, "__send__") == 0 || mx_sdef_writes(name, 0))
        mx_forget(iv, *niv, unsure);
      free(name);
      continue;
    }
    MxOnParam op = { param, 0 };
    pm_visit_node(s, mx_on_param_visit, &op);
    if (op.hit) mx_forget(iv, *niv, unsure);
  }
}

/* `def self.name` among the first `upto` statements of a class body */
static pm_def_node_t *mx_self_def(pm_statements_node_t *st, size_t upto, const char *name) {
  for (size_t i = 0; i < upto && i < st->body.size; i++) {
    pm_node_t *s = st->body.nodes[i];
    if (PM_NODE_TYPE(s) != PM_DEF_NODE) continue;
    pm_def_node_t *d = (pm_def_node_t *)s;
    if (!d->receiver || PM_NODE_TYPE(d->receiver) != PM_SELF_NODE) continue;
    char *dn = mx_name(d->name);
    int eq = strcmp(dn, name) == 0;
    free(dn);
    if (eq) return d;
  }
  return NULL;
}

static void mx_class_body(pm_statements_node_t *st, MxEdits *ed, MxClass *k) {
  char **ext = k->ext; int next = k->next;
  MxVar *iv = k->iv; int niv = k->niv;
  for (size_t i = 0; i < st->body.size; i++) {
    pm_node_t *s = st->body.nodes[i];
    /* `macro :X if COND` / `unless`: the expansion under the same condition */
    pm_node_t *guard = NULL; int guard_unless = 0;
    if ((PM_NODE_TYPE(s) == PM_IF_NODE || PM_NODE_TYPE(s) == PM_UNLESS_NODE)) {
      int is_if = PM_NODE_TYPE(s) == PM_IF_NODE;
      pm_statements_node_t *gs = is_if ? ((pm_if_node_t *)s)->statements : ((pm_unless_node_t *)s)->statements;
      pm_node_t *gelse = is_if ? ((pm_if_node_t *)s)->subsequent : (pm_node_t *)((pm_unless_node_t *)s)->else_clause;
      pm_node_t *gpred = is_if ? ((pm_if_node_t *)s)->predicate : ((pm_unless_node_t *)s)->predicate;
      int modifier = gs && gpred && gpred->location.start > gs->base.location.start;
      /* the condition always runs, an elsif's may: whatever they write is
         not followed */
      if (gpred && mx_undef_writes(gpred, iv, &niv, &k->unsure)) k->ext_unsure = 1;
      for (pm_node_t *ge = gelse; ge && PM_NODE_TYPE(ge) == PM_IF_NODE; ge = ((pm_if_node_t *)ge)->subsequent)
        if (((pm_if_node_t *)ge)->predicate && mx_undef_writes(((pm_if_node_t *)ge)->predicate, iv, &niv, &k->unsure)) k->ext_unsure = 1;
      if (!modifier) {
        /* a block `if`: its statements are class-body statements too, each
           branch run from the state before it; afterwards a value the
           branches do not agree on is unknown (the condition is decided at
           run time) */
        k->next = next; k->niv = niv;
        MxVar pre[64]; int npre = niv;
        memcpy(pre, iv, sizeof(MxVar) * (size_t)niv);
        MxVar acc[64]; int nacc = -1;
        pm_statements_node_t *brs[64]; int nbr = 0, has_else = 0;
        /* an empty branch (NULL here) leaves the state as it was before */
        brs[nbr++] = gs;
        pm_node_t *ge = gelse;
        while (ge && nbr < 64) {
          if (PM_NODE_TYPE(ge) == PM_ELSE_NODE) {
            has_else = 1;
            brs[nbr++] = ((pm_else_node_t *)ge)->statements;
            break;
          }
          if (PM_NODE_TYPE(ge) != PM_IF_NODE) break;
          brs[nbr++] = ((pm_if_node_t *)ge)->statements;
          ge = ((pm_if_node_t *)ge)->subsequent;
        }
        for (int b = 0; b < nbr; b++) {
          memcpy(iv, pre, sizeof(MxVar) * (size_t)npre); k->niv = npre;
          if (brs[b]) mx_class_body(brs[b], ed, k);
          /* an extend only one branch makes: which macro a later call
             reaches is decided at run time */
          while (k->next > next) { free(k->ext[--k->next]); k->ext_unsure = 1; }
          if (nacc < 0) { memcpy(acc, iv, sizeof(MxVar) * (size_t)k->niv); nacc = k->niv; }
          else if (!mx_merge_state(acc, &nacc, iv, k->niv)) k->unsure = 1;
        }
        if (!has_else) {                     /* no branch taken is a branch too */
          if (nacc < 0) { memcpy(acc, pre, sizeof(MxVar) * (size_t)npre); nacc = npre; }
          else if (!mx_merge_state(acc, &nacc, pre, npre)) k->unsure = 1;
        }
        if (nacc >= 0) { memcpy(iv, acc, sizeof(MxVar) * (size_t)nacc); k->niv = nacc; }
        next = k->next; niv = k->niv;
        continue;
      }
      if (!gs || gs->body.size != 1 || gelse || PM_NODE_TYPE(gs->body.nodes[0]) != PM_CALL_NODE) {
        if (mx_undef_writes(s, iv, &niv, &k->unsure)) k->ext_unsure = 1;
        continue;
      }
      guard = is_if ? ((pm_if_node_t *)s)->predicate : ((pm_unless_node_t *)s)->predicate;
      guard_unless = !is_if;
      s = gs->body.nodes[0];
    }
    pm_node_t *whole = guard ? st->body.nodes[i] : s;
    /* the class body writing an ivar itself: the value, when the evaluator
       can compute it, else unknown */
    { pm_constant_id_t wn = mx_ivar_write_name(s);
      if (wn) {
        char *w = mx_name(wn);
        Mv v = mv_undef();
        if (PM_NODE_TYPE(s) == PM_INSTANCE_VARIABLE_WRITE_NODE) {
          MxCtx vc; memset(&vc, 0, sizeof vc);
          vc.iv = iv; vc.niv = &niv; vc.unsure = &k->unsure; vc.ext = ext; vc.next = next;
          Mv e;
          if (mx_eval(&vc, ((pm_instance_variable_write_node_t *)s)->value, &e)) v = e;
        }
        mx_set(iv, &niv, 64, w, v);
        if (!mx_lookup(iv, niv, w)) mx_forget(iv, niv, &k->unsure);   /* the table was full */
        free(w);
        continue;
      } }
    if (PM_NODE_TYPE(s) != PM_CALL_NODE) {
      /* a nested class, module or `class << self` body runs here: a call it
         makes on this class by name (`Box.kind :other`) writes its state */
      if (PM_NODE_TYPE(s) == PM_CLASS_NODE || PM_NODE_TYPE(s) == PM_MODULE_NODE ||
          PM_NODE_TYPE(s) == PM_SINGLETON_CLASS_NODE) {
        const char *klast = strrchr(k->path, ':');
        klast = klast ? klast + 1 : k->path;
        for (int q = 0; q < g_mx_nouter; q++) {
          const uint8_t *qp = g_mx_outer[q].pos;
          if (!qp || qp < s->location.start || qp >= s->location.end) continue;
          if (strcmp(g_mx_outer[q].name, klast) != 0 && strcmp(g_mx_outer[q].name, "*") != 0) continue;
          mx_forget(iv, niv, &k->unsure);
          if (g_mx_outer[q].ext) k->ext_unsure = 1;
        }
      }
      if (mx_undef_writes(s, iv, &niv, &k->unsure)) k->ext_unsure = 1;
      continue;
    }
    pm_call_node_t *cn = (pm_call_node_t *)s;
    char *name = mx_name(cn->name);
    if (strcmp(name, "instance_variable_set") == 0 && !cn->block &&
        (!cn->receiver || PM_NODE_TYPE(cn->receiver) == PM_SELF_NODE)) {
      MxCtx vc; memset(&vc, 0, sizeof vc);
      vc.iv = iv; vc.niv = &niv; vc.unsure = &k->unsure; vc.ext = ext; vc.next = next;
      MxVar before[64]; int nbefore = niv;
      memcpy(before, iv, sizeof(MxVar) * (size_t)niv);
      mx_ivar_set(&vc, cn->arguments ? &cn->arguments->arguments : NULL);
      /* under a guard the write may not happen */
      if (guard && !mx_merge_state(iv, &niv, before, nbefore)) mx_forget(iv, niv, &k->unsure);
      free(name);
      continue;
    }
    if (cn->receiver || cn->block) {
      if (mx_undef_writes(s, iv, &niv, &k->unsure)) k->ext_unsure = 1;
      /* a call on a value the state may hold (`@fields << :a`, or through a
         class-body local) changes what is not followed */
      if (cn->receiver) {
        pm_node_type_t rt = PM_NODE_TYPE(cn->receiver);
        MxCtx vc; memset(&vc, 0, sizeof vc);
        vc.iv = iv; vc.niv = &niv; vc.unsure = &k->unsure; vc.ext = ext; vc.next = next;
        Mv rv;
        if (rt == PM_LOCAL_VARIABLE_READ_NODE || rt == PM_INSTANCE_VARIABLE_READ_NODE ||
            (mx_eval(&vc, cn->receiver, &rv) && (rv.k == MV_ARR || rv.k == MV_STR)))
          mx_forget(iv, niv, &k->unsure);
      }
      /* a macro called with a receiver (`self.kind :box`) writes what the
         evaluator does not follow */
      if (mx_find_macro(NULL, name)) mx_forget(iv, niv, &k->unsure);
      free(name);
      continue;
    }
    if (strcmp(name, "extend") == 0 && cn->arguments && guard) {
      k->ext_unsure = 1;   /* `extend M if cond` */
      free(name);
      continue;
    }
    if (strcmp(name, "extend") == 0 && cn->arguments) {
      /* the last module extended is searched first, and of `extend A, B`
         A before B: the list is kept in reverse lookup order */
      for (size_t j = cn->arguments->arguments.size; j-- > 0; ) {
        char *mn = mx_last_name(cn->arguments->arguments.nodes[j]);
        /* a module not named here, one of two by that name, or one past the
           table: which module answers the macros, and what its hook wrote,
           is not known */
        if (!mn || next >= 16 || mx_ambiguous(mn)) {
          free(mn);
          k->ext_unsure = 1;
          mx_forget(iv, niv, &k->unsure);
          continue;
        }
        int dup = 0;
        for (int q = 0; mn && q < next; q++) if (strcmp(ext[q], mn) == 0) dup = 1;
        /* its `self.extended(base)` hook runs now */
        for (int h = 0; mn && h < g_mx_nhooks; h++)
          if (strcmp(g_mx_hooks[h].module, mn) == 0) mx_run_hook(g_mx_hooks[h].def, iv, &niv, &k->unsure);
        if (mn && !dup) ext[next++] = mn; else free(mn);
      }
      free(name);
      continue;
    }
    if (mx_names_has(&g_mx_sdefs, name) && mx_find_macro(NULL, name)) {
      /* a class method of the program's by a macro's name answers first */
      mx_forget(iv, niv, &k->unsure);
      free(name);
      continue;
    }
    if (k->ext_unsure && mx_find_macro(NULL, name)) {
      /* which module answers is not known here: left as written */
      mx_forget(iv, niv, &k->unsure);
      free(name);
      continue;
    }
    MxMacro *m = NULL;
    for (int e = next - 1; e >= 0 && !m; e--) m = mx_find_macro(ext[e], name);
    if (!m && mx_find_macro(NULL, name)) {
      /* a macro name the recorded extends do not answer: the module may
         reach the class another way (a hook's `base.extend`, an extend not
         named here), and the call writes what is not followed */
      if (getenv("SPINEL_MACRO_DEBUG"))
        fprintf(stderr, "macro call without extend in %s: %s\n", k->path, name);
      mx_forget(iv, niv, &k->unsure);
      free(name);
      continue;
    }
    if (!m && (strcmp(name, "include") == 0 || strcmp(name, "prepend") == 0) && cn->arguments) {
      /* the module's included/prepended hook runs now, writing through its
         base what is not followed (and a module not named here may have one) */
      for (size_t j = 0; j < cn->arguments->arguments.size; j++) {
        char *ln = mx_last_name(cn->arguments->arguments.nodes[j]);
        if (!ln || mx_names_has(&g_mx_ihooks, ln)) mx_forget(iv, niv, &k->unsure);
        free(ln);
      }
    }
    if (!m) {
      /* a class method the body defined itself: what it writes is unknown */
      pm_def_node_t *d = mx_self_def(st, i, name);
      if (!d && k->top && k->top != st) d = mx_self_def(k->top, k->top->body.size, name);
      if (d) {
        if (d->body && mx_undef_writes(d->body, iv, &niv, &k->unsure)) k->ext_unsure = 1;
        if (cn->arguments && mx_undef_writes((pm_node_t *)cn->arguments, iv, &niv, &k->unsure)) k->ext_unsure = 1;
      }
      /* one defined elsewhere (`class << self`, an earlier body, a module):
         what it writes is not known */
      else if (mx_sdef_writes(name, 0)) mx_forget(iv, niv, &k->unsure);
      /* its arguments run too (`puts kind(:box)`, `send :kind, :box`) */
      else if (mx_undef_writes(s, iv, &niv, &k->unsure)) k->ext_unsure = 1;
      free(name);
      continue;
    }
    free(name);
    /* a heredoc argument's body lies outside the call's range */
    { int hd = 0;
      for (const uint8_t *p = s->location.start; p + 2 < s->location.end && !hd; p++)
        if (p[0] == '<' && p[1] == '<' && (p[2] == '~' || p[2] == '-' || isalpha(p[2]) || p[2] == '_' ||
                                           p[2] == '"' || p[2] == '\'' || p[2] == '`'))
          hd = 1;
      if (hd) { mx_forget(iv, niv, &k->unsure); continue; } }
    int dyn = mx_macro_dynamic(m, 0);
    MxBuf out = {0};
    MxCtx c; memset(&c, 0, sizeof c);
    c.iv = iv; c.niv = &niv; c.out = dyn ? &out : NULL; c.modname = m->module;
    c.unsure = &k->unsure;
    c.ext = ext; c.next = next; c.ext_unsure = &k->ext_unsure;
    /* evaluate on a copy of the state: a failed expansion must not leave
       half its writes behind */
    MxVar save[64]; int nsave = niv;
    memcpy(save, iv, sizeof(MxVar) * (size_t)niv);
    Mv r;
    g_mx_full = 0;
    int ok = mx_call_macro(&c, m, cn->arguments, &r) && !g_mx_full;
    if (!ok) {
      /* what it would have done at run time is not known here */
      if (getenv("SPINEL_MACRO_DEBUG"))
        fprintf(stderr, "macro not expanded: %.*s\n", (int)(s->location.end - s->location.start), (const char *)s->location.start);
      memcpy(iv, save, sizeof(MxVar) * (size_t)nsave); niv = nsave;
      mx_forget(iv, niv, &k->unsure);
      free(out.p); continue;
    }
    /* a call under a condition: whatever it changed may not have happened */
    if (guard) {
      MxVar before[64]; int nbefore = nsave;
      memcpy(before, save, sizeof(MxVar) * (size_t)nsave);
      if (!mx_merge_state(iv, &niv, before, nbefore)) mx_forget(iv, niv, &k->unsure);
    }
    if (!dyn) { free(out.p); continue; }
    /* the replacement, on the call's own lines */
    int lines = 0;
    for (const uint8_t *p = whole->location.start; p < whole->location.end; p++) if (*p == '\n') lines++;
    MxBuf rep = {0};
    if (guard) {
      mxb_puts(&rep, guard_unless ? "unless " : "if ");
      mxb_putn(&rep, (const char *)guard->location.start, (size_t)(guard->location.end - guard->location.start));
      mxb_puts(&rep, "; ");
    }
    char *body = mx_join_residual(out.p ? out.p : "");
    if (!body) {
      if (getenv("SPINEL_MACRO_DEBUG"))
        fprintf(stderr, "macro residual not joinable: %.*s\n", (int)(s->location.end - s->location.start), (const char *)s->location.start);
      free(rep.p); free(out.p); continue;
    }
    mxb_puts(&rep, body);
    free(body);
    if (guard) mxb_puts(&rep, "; end");
    /* the guard's own line breaks are in its copied text already */
    if (guard)
      for (const uint8_t *p = guard->location.start; p < guard->location.end; p++) if (*p == '\n') lines--;
    for (int q = 0; q < lines; q++) mxb_puts(&rep, "\n");
    free(out.p);
    if (ed->n == ed->cap) { ed->cap = ed->cap ? ed->cap * 2 : 32; ed->e = realloc(ed->e, sizeof(MxEdit) * (size_t)ed->cap); }
    ed->e[ed->n].start = whole->location.start;
    ed->e[ed->n].end = whole->location.end;
    ed->e[ed->n].text = rep.p;
    ed->n++;
    if (m->expanded < 512) m->sites[m->expanded] = s;
    m->expanded++;
  }
  k->next = next;
  k->niv = niv;
}

/* calls of each dynamic macro's name anywhere in the program */
typedef struct { int *counts; } MxCount;
static bool mx_count_visit(const pm_node_t *n, void *data) {
  MxCount *mc = (MxCount *)data;
  if (PM_NODE_TYPE(n) == PM_CALL_NODE) {
    char *nm = mx_name(((pm_call_node_t *)n)->name);
    for (int i = 0; i < g_mx_nmacros; i++) {
      if (g_mx_macros[i].expanded == 0) continue;
      char *dn = mx_name(g_mx_macros[i].def->name);
      if (strcmp(dn, nm) == 0) {
        mc->counts[i]++;
        if (getenv("SPINEL_MACRO_DEBUG")) {
          int seen = 0;
          for (int q = 0; q < g_mx_macros[i].expanded && q < 512; q++) if (g_mx_macros[i].sites[q] == n) seen = 1;
          if (!seen) {
            int line = 1;
            for (const uint8_t *p = g_parser->start; p < n->location.start; p++) if (*p == '\n') line++;
            fprintf(stderr, "unexpanded call of %s at buffer line %d\n", nm, line);
          }
        }
      }
      free(dn);
    }
    free(nm);
  }
  /* a symbol or string naming it (`method(:m)`, `respond_to?("m")`,
     `public_send("m", ...)`) keeps it too */
  const pm_string_t *us = PM_NODE_TYPE(n) == PM_SYMBOL_NODE ? &((pm_symbol_node_t *)n)->unescaped
                        : PM_NODE_TYPE(n) == PM_STRING_NODE ? &((pm_string_node_t *)n)->unescaped : NULL;
  if (us) {
    for (int i = 0; i < g_mx_nmacros; i++) {
      if (g_mx_macros[i].expanded == 0) continue;
      char *dn = mx_name(g_mx_macros[i].def->name);
      if (strlen(dn) == pm_string_length(us) &&
          memcmp(dn, pm_string_source(us), strlen(dn)) == 0) mc->counts[i] += 1000000;
      free(dn);
    }
  }
  return true;
}

static int mx_edit_cmp(const void *a, const void *b) {
  const MxEdit *x = a, *y = b;
  return x->start < y->start ? -1 : x->start > y->start;
}

/* Expand the class-body macro calls of `source` (the whole program). Returns
   a new buffer, or NULL when nothing was expanded. */
static char *sp_expand_class_macros(const char *source) {
  /* cheap gate: a module_eval / const_set / public_send somewhere */
  if (!strstr(source, "module_eval") && !strstr(source, "class_eval") &&
      !strstr(source, "const_set") && !strstr(source, "public_send") &&
      !strstr(source, "define_singleton_method") && !strstr(source, "define_method"))
    return NULL;
  size_t len = strlen(source);
  if (getenv("SPINEL_MACRO_DUMP")) {
    FILE *df = fopen(getenv("SPINEL_MACRO_DUMP"), "w");
    if (df) { fputs(source, df); fclose(df); }
  }
  pm_parser_t parser;
  pm_parser_init(&parser, (const uint8_t *)source, len, NULL);
  pm_node_t *root = pm_parse(&parser);
  char *result = NULL;
  const pm_parser_t *sv = g_parser;
  g_parser = &parser;
  g_mx_main = &parser;
  if (parser.error_list.size == 0) {
    g_mx_nmacros = 0;
    pm_visit_node(root, mx_collect_visit, NULL);
    MxOuterScan os = { 0 };
    pm_visit_node(root, mx_outer_visit, &os);
    if (g_mx_nmacros > 0) {
      MxEdits ed = {0};
      /* one walk in program order: a macro call sees only the extends and
         the state the body made before it */
      MxWalk w = { &ed, "" };
      pm_visit_node(root, mx_class_visit, &w);
      for (int i = 0; i < g_mx_nclasses; i++) {
        for (int e = 0; e < g_mx_classes[i].next; e++) free(g_mx_classes[i].ext[e]);
        free(g_mx_classes[i].path);
      }
      g_mx_nclasses = 0;
      if (ed.n > 0) {
        /* a macro every call of which was expanded is dropped: what it would
           do at run time is the compiler's to refuse, and nothing calls it */
        MxCount mc = { calloc((size_t)g_mx_nmacros + 1, sizeof(int)) };
        pm_visit_node(root, mx_count_visit, &mc);
        for (int i = 0; i < g_mx_nmacros; i++) {
          MxMacro *m = &g_mx_macros[i];
          if (getenv("SPINEL_MACRO_DEBUG") && m->expanded) {
            char *dn = mx_name(m->def->name);
            fprintf(stderr, "macro %s: %d calls, %d expanded\n", dn, mc.counts[i], m->expanded);
            free(dn);
          }
          if (m->expanded == 0 || mc.counts[i] != m->expanded) continue;
          const uint8_t *ds = m->def->base.location.start, *de = m->def->base.location.end;
          MxBuf blank = {0};
          for (const uint8_t *p = ds; p < de; p++) if (*p == '\n') mxb_puts(&blank, "\n");
          if (!blank.p) mxb_puts(&blank, "");
          if (ed.n == ed.cap) { ed.cap = ed.cap ? ed.cap * 2 : 32; ed.e = realloc(ed.e, sizeof(MxEdit) * (size_t)ed.cap); }
          ed.e[ed.n].start = ds; ed.e[ed.n].end = de; ed.e[ed.n].text = blank.p; ed.n++;
        }
        free(mc.counts);
        qsort(ed.e, (size_t)ed.n, sizeof(MxEdit), mx_edit_cmp);
        MxBuf nb = {0};
        const uint8_t *from = (const uint8_t *)source;
        for (int i = 0; i < ed.n; i++) {
          if (ed.e[i].start < from) continue;   /* nested in an earlier edit */
          mxb_putn(&nb, (const char *)from, (size_t)(ed.e[i].start - from));
          mxb_puts(&nb, ed.e[i].text);
          from = ed.e[i].end;
        }
        mxb_putn(&nb, (const char *)from, (size_t)((const uint8_t *)source + len - from));
        result = nb.p;
        if (getenv("SPINEL_MACRO_DEBUG"))
          for (int i = 0; i < ed.n; i++) fprintf(stderr, "macro: %s\n", ed.e[i].text);
      }
      for (int i = 0; i < ed.n; i++) free(ed.e[i].text);
      free(ed.e);
    }
    for (int i = 0; i < g_mx_nmacros; i++) free(g_mx_macros[i].module);
    g_mx_nmacros = 0;
    mx_names_free(&g_mx_sdefs);
    mx_names_free(&g_mx_ihooks);
    mx_names_free(&g_mx_inherited);
    mx_names_free(&g_mx_paths);
    for (int i = 0; i < g_mx_nouter; i++) free(g_mx_outer[i].name);
    free(g_mx_outer); g_mx_outer = NULL; g_mx_nouter = 0;
    for (int i = 0; i < g_mx_nsdef_bodies; i++) free(g_mx_sdef_bodies[i].name);
    free(g_mx_sdef_bodies); g_mx_sdef_bodies = NULL; g_mx_nsdef_bodies = 0;
    for (int i = 0; i < g_mx_nhooks; i++) free(g_mx_hooks[i].module);
    free(g_mx_hooks); g_mx_hooks = NULL; g_mx_nhooks = 0;
  }
  g_parser = sv;
  g_mx_main = NULL;
  pm_node_destroy(&parser, root);
  pm_parser_free(&parser);
  return result;
}
