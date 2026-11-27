/* Whether a String a class body evaluates (`class_eval "..."`) can be read
 * as the body's own code (included into spinel_parse.c, which uses it to
 * parse such a String in the body's frame). */

/* ---- may a string module_eval / class_eval be read as the body's own code?
   desugar_static_class_eval parses the text a class body evaluates in the
   body's frame, but CRuby runs it in a scope of its own, so reading it as
   the body's code answers differently when the text
   - reads or writes a local, or names one Prism cannot tell from a call
     (`x`, `private`): the eval sees the caller's locals and keeps its own;
   - says a bare `private` / `protected` / `public` / `module_function`:
     it ends with the eval and does not reach the statements after it;
   - says `__LINE__`, `__FILE__` or `__dir__`: they answer the eval's
     arguments, not the line the text was written on;
   - evaluates a string itself (eval, class_eval, instance_eval, ...).
   Inside a `def` only the last two matter: a method body has its own
   scope either way. Such text is left to run time, refused as before. */
typedef struct { int bad; int in_def; } SpSnipScan;
static bool sp_snippet_visit(const pm_node_t *n, void *data) {
  SpSnipScan *s = data;
  if (s->bad) return false;
  switch (PM_NODE_TYPE(n)) {
  case PM_SOURCE_LINE_NODE: case PM_SOURCE_FILE_NODE:
    s->bad = 1; return false;
  case PM_DEF_NODE:
    if (!s->in_def) {
      SpSnipScan in = { 0, 1 };
      pm_visit_node(n, sp_snippet_visit, &in);
      if (in.bad) s->bad = 1;
      return false;
    }
    return true;
  case PM_LOCAL_VARIABLE_READ_NODE: case PM_LOCAL_VARIABLE_WRITE_NODE:
  case PM_LOCAL_VARIABLE_TARGET_NODE: case PM_LOCAL_VARIABLE_AND_WRITE_NODE:
  case PM_LOCAL_VARIABLE_OR_WRITE_NODE: case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE:
    if (!s->in_def) { s->bad = 1; return false; }
    return true;
  case PM_CALL_NODE: {
    const pm_call_node_t *cn = (const pm_call_node_t *)n;
    if (!s->in_def && PM_NODE_FLAG_P(n, PM_CALL_NODE_FLAGS_VARIABLE_CALL)) { s->bad = 1; return false; }
    char *nm = cstr(cn->name);
    int ev = strcmp(nm, "eval") == 0 || strcmp(nm, "class_eval") == 0 || strcmp(nm, "module_eval") == 0 ||
             strcmp(nm, "instance_eval") == 0 || strcmp(nm, "class_exec") == 0 || strcmp(nm, "module_exec") == 0 ||
             strcmp(nm, "instance_exec") == 0 || strcmp(nm, "binding") == 0 || strcmp(nm, "__dir__") == 0;
    int vis = !cn->receiver && !cn->arguments && !cn->block &&
              (strcmp(nm, "private") == 0 || strcmp(nm, "protected") == 0 || strcmp(nm, "public") == 0 ||
               strcmp(nm, "module_function") == 0);
    free(nm);
    if (ev || (vis && !s->in_def)) { s->bad = 1; return false; }
    return true;
  }
  default:
    return true;
  }
}
/* 1 when `src` parses and reads the same spliced into the body (above) */
static int sp_snippet_graftable(const char *src) {
  if (!src) return 0;
  pm_parser_t parser;
  sp_pm_parser_init(&parser, src, strlen(src), NULL);
  pm_node_t *root = pm_parse(&parser);
  int ok = parser.error_list.size == 0;
  if (ok) {
    const pm_parser_t *saved = g_parser;   /* cstr reads the names from it */
    g_parser = &parser;
    SpSnipScan s = { 0, 0 };
    pm_visit_node(root, sp_snippet_visit, &s);
    g_parser = saved;
    ok = !s.bad;
  }
  pm_node_destroy(&parser, root);
  pm_parser_free(&parser);
  return ok;
}
