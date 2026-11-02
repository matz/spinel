/* inline_rbs.c -- inline RBS comments (docs/inline-rbs.md), read while the
 * program is parsed. Included by spinel_parse.c, and only when the rbs C
 * parser was fetched (SPINEL_INLINE_RBS); the stub at the end answers for a
 * build without it.
 *
 * The parser calls sp_inline_rbs_run between pm_parse and flatten, when a
 * parse's tree and its comment list are alive together: under
 * --load-model=graph each file's own parse (sp_graph_file), under the inline
 * model the whole spliced program (sp_parse_emit). The pass groups the
 * comments into blocks, attaches each annotation to the node it describes,
 * parses it with the rbs C parser and maps its types with the extractor's own
 * map_type (rbs_map.c). The result is a fact on that node, written by flatten
 * as attributes of the node (docs/internals/AST.md):
 *
 *   S <def> rbs_ret <tag>        S <def> rbs_params <tag>,<tag>,...
 *   S <attr_* call> rbs_ivar <tag>
 *   S <class body> rbs_ivars @x=<tag>@<line>@<file>@<col>,...
 *   I <node> rbs_line <line>     I <node> rbs_file <file id>
 *   I <node> rbs_col <col>       S <node> rbs_names <tag>=<type as written>...
 *
 * The analyzer (apply_inline_rbs) finds the method's Scope by its def node
 * and the class through the node's class body, so no class name is matched.
 *
 * Nothing is applied in part. An annotation is applied whole, refused with a
 * located error (a syntax error, two signatures for one method), or ignored
 * whole with a located warning saying why. */

#include <stdarg.h>
#include "prism.h"

/* ---- diagnostics ------------------------------------------------------ */

static int g_inline_errors = 0;

/* A file's diagnostics are held and printed in source order once the file is
 * done, since the pass that finds them does not visit lines in order. */
typedef struct { int line, col; char *text; } ir_held;
static ir_held *g_held = NULL;
static int g_nheld = 0;
static int g_hold = 0;

/* The file one run of the pass reads: under --load-model=graph each file of
 * the program by itself, with its own parse; under the inline model the
 * spliced program, whose lines the multi-file map takes back to the files
 * they came from. */
typedef struct {
    const char *path;
    int fid;               /* its id in the node table's file list */
    const uint8_t *src;
    size_t len;
    size_t *line_starts;   /* byte offset of each line, index 0 = line 1 */
    int nlines;
    pm_parser_t *pm;
} ir_file;

static void ir_diag(const ir_file *f, int line, int col, int is_error,
                    const char *fmt, ...) RBS_ATTRIBUTE_FORMAT(5, 6);
/* Line `bline` of `f` as the file, line and file id it is reported and
 * stamped with: a line of a file's own parse is that already, and a line of
 * the spliced program is mapped back to the file it came from. */
static void ir_orig(const ir_file *f, int bline, const char **file, int *line, int *fid) {
    *file = f->path;
    *line = bline;
    *fid = sp_line_map_n > 0 && bline <= sp_line_map_n ? sp_line_file[bline] : f->fid;
    if (sp_line_map_n > 0 && bline >= 1 && bline <= sp_line_map_n && sp_line_orig[bline] > 0) {
        *file = sp_file_table[sp_line_file[bline]];
        *line = sp_line_orig[bline];
    }
}

/* "file:line" of a line of `f`, for naming a second position in a message */
static const char *ir_where(const ir_file *f, int bline) {
    static char bufs[4][4200];
    static int k = 0;
    const char *file;
    int line, fid;
    ir_orig(f, bline, &file, &line, &fid);
    k = (k + 1) % 4;
    snprintf(bufs[k], sizeof bufs[k], "%s:%d", file, line);
    return bufs[k];
}

static void ir_diag(const ir_file *f, int line, int col, int is_error,
                    const char *fmt, ...) {
    char head[4200], body[2048];
    const char *file;
    int bline = line, fid;
    ir_orig(f, bline, &file, &line, &fid);
    if (col > 0) snprintf(head, sizeof head, "spinel: %s:%d:%d: %s", file, line, col, is_error ? "" : "warning: ");
    else snprintf(head, sizeof head, "spinel: %s:%d: %s", file, line, is_error ? "" : "warning: ");
    if (is_error) g_inline_errors++;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    if (!g_hold) { fprintf(stderr, "%s%s\n", head, body); return; }
    g_held = (ir_held *) realloc(g_held, sizeof(ir_held) * (size_t) (g_nheld + 1));
    size_t n = strlen(head) + strlen(body) + 1;
    g_held[g_nheld].text = (char *) malloc(n);
    snprintf(g_held[g_nheld].text, n, "%s%s", head, body);
    g_held[g_nheld].line = bline;
    g_held[g_nheld].col = col;
    g_nheld++;
}

static void ir_flush_held(void) {
    /* insertion sort: stable, and a file has few */
    for (int i = 1; i < g_nheld; i++) {
        ir_held h = g_held[i];
        int j = i - 1;
        while (j >= 0 && (g_held[j].line > h.line || (g_held[j].line == h.line && g_held[j].col > h.col))) {
            g_held[j + 1] = g_held[j];
            j--;
        }
        g_held[j + 1] = h;
    }
    for (int i = 0; i < g_nheld; i++) { fprintf(stderr, "%s\n", g_held[i].text); free(g_held[i].text); }
    free(g_held);
    g_held = NULL;
    g_nheld = 0;
}

/* ---- facts, one per annotated node ----------------------------------------- */

typedef struct {
    const pm_node_t *node;
    char *ret;        /* a def: the return tag, NULL if unsaid */
    char *params;     /* a def: the parameter tags, comma-separated, empty if unsaid */
    char *ivar;       /* an attr_* call: the tag */
    sbuf_t ivars;     /* a class body: `@x=<tag>@<line>@<file>@<col>,...` */
    sbuf_t names;     /* `<tag>=<type as written>` per tag, newline-separated */
    const char *file; /* the file of the annotation, */
    int line;         /* its line there, */
    int fid;          /* and the file's id in the node table */
    int col;          /* 1-based column of its `#` */
    int emitted;      /* written as attributes of its node's flattened id */
} ir_nfact;

static ir_nfact *g_nf = NULL;
static int g_nnf = 0, g_cnf = 0;
static int g_nf_sorted = 0;   /* g_nf in node order, for the lookups flatten makes */

static char *ir_strdup(const char *s) {
    if (s == NULL) return NULL;
    size_t n = strlen(s);
    char *d = (char *) malloc(n + 1);
    if (d == NULL) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memcpy(d, s, n + 1);
    return d;
}

static char *ir_strndup(const char *s, size_t n) {
    char *d = (char *) malloc(n + 1);
    if (d == NULL) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static ir_nfact *ir_nfact_for(const ir_file *f, const pm_node_t *node) {
    for (int i = 0; i < g_nnf; i++) if (g_nf[i].node == node) return &g_nf[i];
    if (g_nnf >= g_cnf) {
        g_cnf = g_cnf ? g_cnf * 2 : 32;
        g_nf = (ir_nfact *) realloc(g_nf, sizeof(ir_nfact) * (size_t) g_cnf);
        if (g_nf == NULL) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    ir_nfact *fa = &g_nf[g_nnf++];
    memset(fa, 0, sizeof *fa);
    fa->node = node;
    fa->file = f->path;
    fa->fid = f->fid;
    return fa;
}

/* ---- source positions --------------------------------------------------- */

static int ir_line_of(const ir_file *f, size_t off) {
    int lo = 0, hi = f->nlines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (f->line_starts[mid] <= off) lo = mid;
        else hi = mid - 1;
    }
    return lo + 1;
}

static size_t ir_off(const ir_file *f, const uint8_t *p) { return (size_t) (p - f->src); }

/* ---- the declarations a comment can attach to ---------------------------- */

typedef enum {
    IT_DEF, IT_ATTR, IT_IVAR_WRITE, IT_LOCAL_WRITE, IT_CONST_WRITE,
    IT_CLASS, IT_STMT
} ir_target_kind;

typedef struct ir_ann ir_ann;

typedef struct {
    ir_target_kind kind;
    const pm_node_t *node;
    int start_line;        /* the line the declaration starts on */
    int sig_line;          /* a def: the line of its `)` (or its name); else start_line */
    size_t sig_end;        /* byte offset a trailing annotation must follow */
    int end_line;
    size_t start_off, end_off;
    char *owner;           /* the class the declaration belongs to */
    char *lookup;          /* the namespace a bare type name is looked up in first */
    int is_cmethod;
    int in_def;            /* writes: 0 class body / top level, 1 instance method, 2 class method */
    const char *self_class;   /* a def or attribute of a class's instances: owner, which `self` names; else NULL */
    ir_ann **anns;
    int nanns;
} ir_target;

/* A lexical container a comment can sit in. */
typedef struct {
    size_t start, end;
    int is_def;            /* 0 class/module body, 1 method, 2 singleton class, 3 block */
    char *owner;           /* for a class body: its qualified name */
    char *lookup;
    const pm_node_t *body; /* a class body: its statements, which carry its `# @rbs @x:` facts */
} ir_container;

typedef struct {
    ir_file *f;
    ir_target *targets;
    int ntargets, ctargets;
    ir_container *conts;
    int nconts, cconts;
} ir_walk;

/* the lexical context passed down the walk */
typedef struct {
    ir_walk *w;
    const char *owner;     /* "" at top level */
    const char *lookup;    /* the parent namespace of owner */
    int in_def;            /* 0, 1 instance method, 2 class method */
    int in_class;          /* directly in a class body, not a module's or a block's */
    int in_sclass;         /* a def directly in owner's `class << self`: one of owner's class methods */
} ir_ctx;

static void ir_name_into(const ir_file *f, pm_constant_id_t id, sbuf_t *out) {
    pm_constant_t *c = pm_constant_pool_id_to_constant(&f->pm->constant_pool, id);
    if (c != NULL) sbuf_append(out, (const char *) c->start, c->length);
}

/* `A::B::C` as `A_B_C`; false for a path with a dynamic part. */
static bool ir_const_path(const ir_file *f, const pm_node_t *n, sbuf_t *out) {
    if (n == NULL) return false;
    if (PM_NODE_TYPE_P(n, PM_CONSTANT_READ_NODE)) {
        ir_name_into(f, ((const pm_constant_read_node_t *) n)->name, out);
        return true;
    }
    if (PM_NODE_TYPE_P(n, PM_CONSTANT_PATH_NODE)) {
        const pm_constant_path_node_t *cp = (const pm_constant_path_node_t *) n;
        if (cp->parent != NULL) {
            if (!ir_const_path(f, cp->parent, out)) return false;
            sbuf_append_cstr(out, "_");
        }
        ir_name_into(f, cp->name, out);
        return true;
    }
    return false;
}

static char *ir_join(const char *owner, const char *leaf) {
    sbuf_t s;
    sbuf_init(&s);
    if (owner && owner[0]) { sbuf_append_cstr(&s, owner); sbuf_append_cstr(&s, "_"); }
    sbuf_append_cstr(&s, leaf);
    char *r = ir_strdup(s.buf ? s.buf : "");
    sbuf_free(&s);
    return r;
}

static ir_target *ir_target_new(ir_walk *w, ir_target_kind kind, const pm_node_t *n,
                                const ir_ctx *cx) {
    if (w->ntargets >= w->ctargets) {
        w->ctargets = w->ctargets ? w->ctargets * 2 : 64;
        w->targets = (ir_target *) realloc(w->targets, sizeof(ir_target) * (size_t) w->ctargets);
    }
    ir_target *t = &w->targets[w->ntargets++];
    memset(t, 0, sizeof *t);
    t->kind = kind;
    t->node = n;
    t->start_off = ir_off(w->f, n->location.start);
    t->end_off = ir_off(w->f, n->location.end);
    t->start_line = ir_line_of(w->f, t->start_off);
    t->end_line = ir_line_of(w->f, t->end_off > t->start_off ? t->end_off - 1 : t->end_off);
    t->sig_line = t->start_line;
    t->sig_end = t->end_off;
    t->owner = ir_strdup(cx->owner[0] ? cx->owner : "Object");
    t->lookup = ir_strdup(cx->lookup);
    t->in_def = cx->in_def;
    return t;
}

static void ir_container_add(ir_walk *w, const pm_node_t *n, int is_def,
                             const char *owner, const char *lookup) {
    if (w->nconts >= w->cconts) {
        w->cconts = w->cconts ? w->cconts * 2 : 32;
        w->conts = (ir_container *) realloc(w->conts, sizeof(ir_container) * (size_t) w->cconts);
    }
    ir_container *c = &w->conts[w->nconts++];
    c->start = ir_off(w->f, n->location.start);
    c->end = ir_off(w->f, n->location.end);
    c->is_def = is_def;
    c->owner = ir_strdup(owner);
    c->lookup = ir_strdup(lookup);
    c->body = NULL;
}

static bool ir_is_attr_call(const ir_file *f, const pm_call_node_t *call) {
    if (call->receiver != NULL) return false;
    pm_constant_t *c = pm_constant_pool_id_to_constant(&f->pm->constant_pool, call->name);
    if (c == NULL) return false;
    return (c->length == 11 && memcmp(c->start, "attr_reader", 11) == 0)
        || (c->length == 11 && memcmp(c->start, "attr_writer", 11) == 0)
        || (c->length == 13 && memcmp(c->start, "attr_accessor", 13) == 0);
}

static bool ir_visit(const pm_node_t *n, void *data);

/* Is constant c the word lit? */
static bool ir_tok_is(const pm_constant_t *c, const char *lit) {
    return c != NULL && c->length == strlen(lit) && memcmp(c->start, lit, c->length) == 0;
}

/* A value that makes a class: `Data.define(...)`, `Struct.new(...)`,
   `Class.new(...)` or `Module.new(...)`, a block or not. */
static bool ir_makes_class(const ir_file *f, const pm_node_t *v) {
    if (v == NULL || !PM_NODE_TYPE_P(v, PM_CALL_NODE)) return false;
    const pm_call_node_t *call = (const pm_call_node_t *) v;
    if (call->receiver == NULL || !PM_NODE_TYPE_P(call->receiver, PM_CONSTANT_READ_NODE)) return false;
    pm_constant_t *r = pm_constant_pool_id_to_constant(
        &f->pm->constant_pool, ((const pm_constant_read_node_t *) call->receiver)->name);
    pm_constant_t *m = pm_constant_pool_id_to_constant(&f->pm->constant_pool, call->name);
    return (ir_tok_is(r, "Data") && ir_tok_is(m, "define")) || (ir_tok_is(r, "Struct") && ir_tok_is(m, "new")) ||
           (ir_tok_is(r, "Class") && ir_tok_is(m, "new")) || (ir_tok_is(r, "Module") && ir_tok_is(m, "new"));
}

static void ir_class_body(ir_ctx *cx, const pm_node_t *container, const pm_node_t *body,
                          const char *leaf_path, int is_class) {
    char *owner = ir_join(cx->owner, leaf_path);
    ir_container_add(cx->w, container, 0, owner, cx->owner);
    cx->w->conts[cx->w->nconts - 1].body = body;
    known_names_add(owner, strlen(owner));
    ir_ctx sub = { cx->w, owner, cx->owner, 0, is_class, 0 };
    if (body != NULL) pm_visit_node(body, ir_visit, &sub);
    free(owner);
}

static void ir_statements(ir_ctx *cx, const pm_statements_node_t *st) {
    for (size_t i = 0; i < st->body.size; i++) {
        const pm_node_t *s = st->body.nodes[i];
        /* every statement is a place a trailing annotation can end; the more
           specific kinds are recorded by ir_visit, so record the rest here */
        switch (PM_NODE_TYPE(s)) {
            case PM_DEF_NODE: case PM_INSTANCE_VARIABLE_WRITE_NODE:
            case PM_LOCAL_VARIABLE_WRITE_NODE: case PM_CONSTANT_WRITE_NODE:
            case PM_CONSTANT_PATH_WRITE_NODE: case PM_CLASS_NODE: case PM_MODULE_NODE:
                break;
            case PM_CALL_NODE:
                if (ir_is_attr_call(cx->w->f, (const pm_call_node_t *) s)) break;
                /* fall through */
            default:
                ir_target_new(cx->w, IT_STMT, s, cx);
        }
        pm_visit_node(s, ir_visit, cx);
    }
}

static bool ir_visit(const pm_node_t *n, void *data) {
    ir_ctx *cx = (ir_ctx *) data;
    ir_walk *w = cx->w;
    const ir_file *f = w->f;
    switch (PM_NODE_TYPE(n)) {
        case PM_STATEMENTS_NODE:
            ir_statements(cx, (const pm_statements_node_t *) n);
            return false;
        case PM_CLASS_NODE:
        case PM_MODULE_NODE: {
            const pm_node_t *path = PM_NODE_TYPE_P(n, PM_CLASS_NODE)
                ? ((const pm_class_node_t *) n)->constant_path
                : ((const pm_module_node_t *) n)->constant_path;
            const pm_node_t *body = PM_NODE_TYPE_P(n, PM_CLASS_NODE)
                ? ((const pm_class_node_t *) n)->body
                : ((const pm_module_node_t *) n)->body;
            ir_target_new(w, IT_CLASS, n, cx);
            if (PM_NODE_TYPE_P(n, PM_CLASS_NODE) && ((const pm_class_node_t *) n)->superclass)
                pm_visit_node(((const pm_class_node_t *) n)->superclass, ir_visit, cx);
            sbuf_t p;
            sbuf_init(&p);
            if (ir_const_path(f, path, &p) && p.buf) ir_class_body(cx, n, body, p.buf, PM_NODE_TYPE_P(n, PM_CLASS_NODE));
            sbuf_free(&p);
            return false;
        }
        case PM_SINGLETON_CLASS_NODE: {
            const pm_singleton_class_node_t *sc = (const pm_singleton_class_node_t *) n;
            ir_container_add(w, n, 2, cx->owner, cx->lookup);
            /* `class << self` in a class or module body: a def directly in it
               is a `def self.` of the owner. Its other declarations belong to
               the singleton class, which has no pins. */
            if (cx->owner[0] && !cx->in_def && PM_NODE_TYPE_P(sc->expression, PM_SELF_NODE) &&
                sc->body && PM_NODE_TYPE_P(sc->body, PM_STATEMENTS_NODE)) {
                const pm_statements_node_t *st = (const pm_statements_node_t *) sc->body;
                ir_ctx sub = *cx;
                sub.in_class = 0;
                sub.in_sclass = 1;
                /* only a receiverless def: `def self.m` here is a method of
                   the singleton class's own singleton, which has no pins and
                   keeps the placement warning it had */
                for (size_t i = 0; i < st->body.size; i++)
                    if (PM_NODE_TYPE_P(st->body.nodes[i], PM_DEF_NODE) &&
                        ((const pm_def_node_t *) st->body.nodes[i])->receiver == NULL)
                        ir_visit(st->body.nodes[i], &sub);
            }
            return false;
        }
        case PM_BLOCK_NODE:
        case PM_LAMBDA_NODE:
            /* Generic receiverless calls and lambdas preserve the lexical
               class for declarations, but do not declare class-body ivars. */
            ir_container_add(w, n, 3, cx->owner, cx->lookup);
            if (!cx->in_def && cx->in_class) {
                /* a def in `Struct.new do ... end` or `class_eval do` is not
                   necessarily one of the class's own instance methods */
                ir_ctx sub = *cx;
                sub.in_class = 0;
                pm_visit_child_nodes(n, ir_visit, &sub);
                return false;
            }
            return !cx->in_def;
        case PM_CONSTANT_WRITE_NODE:
            /* `CType = Data.define(...)` makes a class as `class CType` does:
               a relative type name finds it by its scope (#10370) */
            if (!cx->in_def && ir_makes_class(f, ((const pm_constant_write_node_t *) n)->value)) {
                pm_constant_t *cn = pm_constant_pool_id_to_constant(
                    &f->pm->constant_pool, ((const pm_constant_write_node_t *) n)->name);
                if (cn) {
                    char *leaf = ir_strndup((const char *) cn->start, cn->length);
                    char *qn = ir_join(cx->owner, leaf);
                    known_names_add(qn, strlen(qn));
                    free(qn); free(leaf);
                }
            }
            /* fall through */
        case PM_CONSTANT_PATH_WRITE_NODE:
            ir_target_new(w, IT_CONST_WRITE, n, cx);
            return true;
        case PM_DEF_NODE: {
            const pm_def_node_t *d = (const pm_def_node_t *) n;
            ir_container_add(w, n, 1, cx->owner, cx->lookup);
            if (!cx->owner[0] || cx->in_def ||
                (d->receiver && !PM_NODE_TYPE_P(d->receiver, PM_SELF_NODE))) return false;
            ir_target *t = ir_target_new(w, IT_DEF, n, cx);
            t->is_cmethod = d->receiver != NULL || cx->in_sclass;
            if (cx->in_class && !t->is_cmethod) t->self_class = t->owner;
            /* the line a trailing return annotation is written on: after `)`,
               or after the name of a def with no parentheses */
            const uint8_t *se = d->rparen_loc.start ? d->rparen_loc.end : d->name_loc.end;
            if (!d->rparen_loc.start && d->parameters) se = d->parameters->base.location.end;
            t->sig_end = ir_off(f, se);
            t->sig_line = ir_line_of(f, t->sig_end > 0 ? t->sig_end - 1 : 0);
            t->start_line = ir_line_of(f, ir_off(f, d->def_keyword_loc.start));
            ir_ctx sub = *cx;
            sub.in_def = t->is_cmethod ? 2 : 1;
            if (d->body) pm_visit_node(d->body, ir_visit, &sub);
            return false;
        }
        case PM_CALL_NODE: {
            const pm_call_node_t *call = (const pm_call_node_t *) n;
            if (call->receiver != NULL) return false;
            if (ir_is_attr_call(f, call)) {
                if (!cx->owner[0]) return false;
                ir_target *t = ir_target_new(w, IT_ATTR, n, cx);
                if (cx->in_class) t->self_class = t->owner;
                return false;
            }
            pm_constant_t *name = pm_constant_pool_id_to_constant(&f->pm->constant_pool, call->name);
            if (name && ((name->length == 7 && memcmp(name->start, "include", 7) == 0) ||
                         (name->length == 6 && memcmp(name->start, "extend", 6) == 0) ||
                         (name->length == 7 && memcmp(name->start, "prepend", 7) == 0))) return false;
            return true;
        }
        case PM_INSTANCE_VARIABLE_WRITE_NODE:
            ir_target_new(w, IT_IVAR_WRITE, n, cx);
            return true;
        case PM_LOCAL_VARIABLE_WRITE_NODE:
            ir_target_new(w, IT_LOCAL_WRITE, n, cx);
            return true;
        default:
            return true;
    }
}

/* ---- annotations -------------------------------------------------------- */

typedef enum {
    IA_COLON,        /* `#:` leading: a method type */
    IA_RBS,          /* `# @rbs ...` */
    IA_TRAILING,     /* `#:` / `#[` after code on the same line */
} ir_ann_shape;

struct ir_ann {
    ir_ann_shape shape;
    char *text;            /* the joined text the rbs parser reads */
    size_t len;
    int nlines;
    int *line;             /* source line of each joined line */
    int *col;              /* source column (0-based) of each joined line's first byte */
    size_t *joff;          /* offset of each joined line in text */
    int first_line, first_col;   /* the `#` of the first comment */
};

static ir_ann *ir_ann_new(ir_ann_shape shape) {
    ir_ann *a = (ir_ann *) calloc(1, sizeof *a);
    a->shape = shape;
    return a;
}

static void ir_ann_free(ir_ann *a) {
    free(a->text);
    free(a->line);
    free(a->col);
    free(a->joff);
    free(a);
}

static void ir_ann_add_line(ir_ann *a, const char *s, size_t n, int line, int col) {
    a->line = (int *) realloc(a->line, sizeof(int) * (size_t) (a->nlines + 1));
    a->col = (int *) realloc(a->col, sizeof(int) * (size_t) (a->nlines + 1));
    a->joff = (size_t *) realloc(a->joff, sizeof(size_t) * (size_t) (a->nlines + 1));
    size_t at = a->len + (a->nlines ? 1 : 0);
    a->text = (char *) realloc(a->text, at + n + 1);
    if (a->nlines) a->text[a->len] = '\n';
    memcpy(a->text + at, s, n);
    a->text[at + n] = '\0';
    a->len = at + n;
    a->line[a->nlines] = line;
    a->col[a->nlines] = col;
    a->joff[a->nlines] = at;
    if (a->nlines == 0) { a->first_line = line; a->first_col = col; }
    a->nlines++;
}

/* source line and 1-based column of a byte in the joined text */
static void ir_ann_pos(const ir_ann *a, int byte_pos, int *line, int *col) {
    int i = 0;
    while (i + 1 < a->nlines && (size_t) byte_pos >= a->joff[i + 1]) i++;
    *line = a->line[i];
    *col = a->col[i] + (byte_pos - (int) a->joff[i]) + 1;
}

/* ---- the per-file pass ---------------------------------------------------- */

typedef struct {
    size_t start, end;
    int line, col;
    int leading;
} ir_comment;

/* The bytes a continuation line's comment marker takes: `#|` (ruby/rbs's
   continuation of a signature over several lines), or `#` and one space */
static size_t ir_cont_prefix(const char *s, size_t n) {
    return n >= 2 && (s[1] == '|' || s[1] == ' ') ? 2 : 1;
}

static void ir_ann_add_comment(const ir_file *f, ir_ann *a, const ir_comment *cm) {
    const char *s = (const char *) f->src + cm->start;
    size_t n = cm->end - cm->start;
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
    size_t pre = ir_cont_prefix(s, n);
    ir_ann_add_line(a, s + pre, n - pre, cm->line, cm->col + (int) pre);
}

static int ir_find_container(const ir_walk *w, size_t off) {
    int best = -1;
    for (int i = 0; i < w->nconts; i++) {
        if (w->conts[i].start <= off && off < w->conts[i].end) {
            if (best < 0 || (w->conts[i].end - w->conts[i].start) < (w->conts[best].end - w->conts[best].start))
                best = i;
        }
    }
    return best;
}

static void ir_attach(ir_target *t, ir_ann *a) {
    t->anns = (ir_ann **) realloc(t->anns, sizeof(ir_ann *) * (size_t) (t->nanns + 1));
    t->anns[t->nanns++] = a;
}

static rbs_parser_t *ir_parser_for(const ir_ann *a, int start) {
    rbs_string_t str = rbs_string_new(a->text, a->text + a->len);
    return rbs_parser_new(str, RBS_ENCODING_UTF_8_ENTRY, start, (int) a->len);
}

static void ir_syntax_error(const ir_file *f, const ir_ann *a, rbs_parser_t *p) {
    int line = a->first_line, col = a->first_col + 1;
    const char *msg = "malformed annotation";
    if (p->error != NULL) {
        ir_ann_pos(a, p->error->token.range.start.byte_pos, &line, &col);
        if (p->error->message) msg = p->error->message;
    }
    ir_diag(f, line, col, 1, "inline RBS syntax error: %s\n"
            "  the annotation is not applied and the compile stops; correct it, or "
            "make it an ordinary comment (%s)", msg,
            a->shape == IA_RBS ? "one whose text does not start with `@rbs`" : "`# :` is not an annotation");
}

static void ir_warn(const ir_file *f, const ir_ann *a, const char *what, const char *why) {
    ir_diag(f, a->first_line, a->first_col + 1, 0,
            "inline RBS: %s is not applied: %s", what, why);
}

/* Parse a type on its own: `attr_reader :name #: T`. */
static rbs_node_t *ir_parse_type(const ir_file *f, const ir_ann *a, rbs_parser_t **pout) {
    /* the text is ": T" */
    rbs_parser_t *p = ir_parser_for(a, 1);
    *pout = p;
    rbs_node_t *type = NULL;
    if (!rbs_parse_type(p, &type, false, false, false) || p->next_token.type != pEOF) {
        if (p->error == NULL) rbs_parser_set_error(p, p->next_token, true, "unexpected token after the type");
        ir_syntax_error(f, a, p);
        return NULL;
    }
    return type;
}

/* Each tag a declaration maps to, beside the type as the comment wrote it:
 * the analyzer can still find a tag pins nothing (a class outside the
 * program), and says so in the program's words rather than in tags. Filled
 * by ir_map, cleared where a declaration's annotations start, and copied onto
 * the fact that declaration becomes. */
static sbuf_t g_ir_names;

static void ir_names_add(sbuf_t *names, const char *tag, const char *src, size_t n) {
    for (const char *e = names->buf; e && *e; ) {
        const char *nl = strchr(e, '\n'), *eq = strchr(e, '=');
        size_t tl = eq ? (size_t) (eq - e) : 0;
        if (eq && (!nl || eq < nl) && tl == strlen(tag) && strncmp(e, tag, tl) == 0) return;
        e = nl ? nl + 1 : NULL;
    }
    if (names->len) sbuf_append_cstr(names, "\n");
    sbuf_append_cstr(names, tag);
    sbuf_append_cstr(names, "=");
    /* one line, whitespace runs (a type continued over lines) as one space */
    int sp = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = src[i];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { sp = 1; continue; }
        if (sp && names->buf[names->len - 1] != '=') sbuf_append_cstr(names, " ");
        sp = 0;
        sbuf_append(names, &ch, 1);
    }
}

static void ir_names_merge(sbuf_t *into, const sbuf_t *from) {
    for (const char *e = from->buf; e && *e; ) {
        const char *nl = strchr(e, '\n'), *eq = strchr(e, '=');
        size_t el = nl ? (size_t) (nl - e) : strlen(e);
        if (eq && eq < e + el) {
            char *tag = ir_strndup(e, (size_t) (eq - e));
            ir_names_add(into, tag, eq + 1, el - (size_t) (eq - e) - 1);
            free(tag);
        }
        e = nl ? nl + 1 : NULL;
    }
}

/* `top`, or core's `boolish` (`type boolish = top`, which flag parameters
 * are written with): any value at all, which says nothing about the slot.
 * It pins nothing ("-", as a `void` return reads), and the rest of the
 * signature still applies, as --rbs leaves such a slot to inference. A
 * program's own `boolish` alias is mapped as written. */
static bool ir_says_nothing(rbs_parser_t *p, rbs_node_t *type) {
    if (type->type == RBS_TYPES_OPTIONAL) type = ((rbs_types_optional_t *) type)->type;
    if (type->type == RBS_TYPES_BASES_TOP) return true;
    if (type->type != RBS_TYPES_ALIAS) return false;
    rbs_types_alias_t *a = (rbs_types_alias_t *) type;
    if ((a->args && a->args->length > 0) || a->name == NULL ||
        (a->name->rbs_namespace && a->name->rbs_namespace->path && a->name->rbs_namespace->path->length > 0) ||
        (a->name->rbs_namespace && a->name->rbs_namespace->absolute)) return false;
    sbuf_t n; sbuf_init(&n);
    name_of_type_name(p, a->name, &n);
    bool b = n.buf && strcmp(n.buf, "boolish") == 0;
    sbuf_free(&n);
    return b;
}

/* The type variables core RBS declares for a builtin generic
   (`class Array[unchecked out Elem]`), which a reopening's inline
   signatures use without declaring them: NULL when `owner` has none. */
static const char *const *ir_core_type_params(const char *owner) {
    static const char *const elem[] = { "Elem", NULL };
    static const char *const kv[] = { "K", "V", NULL };
    static const char *const set[] = { "A", NULL };
    static const char *const en[] = { "Elem", "Return", NULL };
    if (strcmp(owner, "Array") == 0 || strcmp(owner, "Enumerable") == 0 || strcmp(owner, "Range") == 0 ||
        strcmp(owner, "Struct") == 0) return elem;
    if (strcmp(owner, "Hash") == 0) return kv;
    if (strcmp(owner, "Set") == 0) return set;
    if (strcmp(owner, "Enumerator") == 0 || strcmp(owner, "Enumerator_Lazy") == 0) return en;
    return NULL;
}

/* Does `type` name one of `params` (`Elem`, `Array[Elem]`, `Elem?`)? Such
   a type is the receiver's element type, which differs from receiver to
   receiver, so no one type stands for it. */
static bool ir_names_type_param(rbs_parser_t *p, rbs_node_t *type, const char *const *params) {
    if (type == NULL || params == NULL) return false;
    switch (type->type) {
        case RBS_TYPES_VARIABLE:
            return true;
        case RBS_TYPES_OPTIONAL:
            return ir_names_type_param(p, ((rbs_types_optional_t *) type)->type, params);
        case RBS_TYPES_UNION:
        case RBS_TYPES_INTERSECTION: {
            rbs_node_list_t *l = type->type == RBS_TYPES_UNION ? ((rbs_types_union_t *) type)->types
                                                               : ((rbs_types_intersection_t *) type)->types;
            for (rbs_node_list_node_t *e = l ? l->head : NULL; e; e = e->next)
                if (ir_names_type_param(p, e->node, params)) return true;
            return false;
        }
        case RBS_TYPES_CLASS_INSTANCE: {
            rbs_types_class_instance_t *ci = (rbs_types_class_instance_t *) type;
            for (rbs_node_list_node_t *e = ci->args ? ci->args->head : NULL; e; e = e->next)
                if (ir_names_type_param(p, e->node, params)) return true;
            if (ci->args && ci->args->length > 0) return false;
            if (ci->name == NULL || (ci->name->rbs_namespace &&
                (ci->name->rbs_namespace->absolute ||
                 (ci->name->rbs_namespace->path && ci->name->rbs_namespace->path->length > 0)))) return false;
            sbuf_t n; sbuf_init(&n);
            name_of_type_name(p, ci->name, &n);
            bool hit = false;
            for (int i = 0; params[i] && n.buf && !hit; i++) hit = strcmp(n.buf, params[i]) == 0;
            sbuf_free(&n);
            return hit;
        }
        default:
            return false;
    }
}

/* map_type with the owner's lexical scope, as traverse_members sets it up.
   A type naming one of the owner's core type variables pins nothing ("-"),
   as `top` does, and the rest of the signature applies: --rbs, where
   `class Array[Elem]` declares the variable, leaves that slot alone too. */
static bool ir_map(rbs_parser_t *p, rbs_node_t *type, const ir_target *t, sbuf_t *out) {
    if (ir_names_type_param(p, type, ir_core_type_params(t->owner))) { sbuf_set(out, "-", 1); return true; }
    const char *sv = g_self_scope, *st = g_self_type;
    g_self_scope = strcmp(t->owner, "Object") == 0 ? "" : t->owner;
    g_self_type = t->self_class;
    bool ok = map_type(p, type, t->lookup, out);
    g_self_scope = sv;
    g_self_type = st;
    if (!ok && ir_says_nothing(p, type)) { sbuf_set(out, "-", 1); return true; }
    if (ok && out->buf && type->location.end_byte > type->location.start_byte)
        ir_names_add(&g_ir_names, out->buf, p->lexer->string.start + type->location.start_byte,
                     (size_t) (type->location.end_byte - type->location.start_byte));
    return ok;
}

/* the def's parameters, in order, with what each is */
typedef struct {
    char *name;
    char kind;        /* 'r' required, 'k' keyword, 'o' optional, '*' rest, 'p' post, '&' block, 'K' **, 'f' ..., 'x' other */
} ir_param;

static int ir_def_params(const ir_file *f, const pm_def_node_t *d, ir_param **out) {
    int n = 0;
    ir_param *ps = NULL;
    const pm_parameters_node_t *pn = d->parameters;
    if (pn == NULL) { *out = NULL; return 0; }
#define IR_PUSH(nm, k) do { ps = (ir_param *) realloc(ps, sizeof(ir_param) * (size_t) (n + 1)); \
        ps[n].name = (nm); ps[n].kind = (k); n++; } while (0)
    for (size_t i = 0; i < pn->requireds.size; i++) {
        const pm_node_t *r = pn->requireds.nodes[i];
        sbuf_t s; sbuf_init(&s);
        if (PM_NODE_TYPE_P(r, PM_REQUIRED_PARAMETER_NODE))
            ir_name_into(f, ((const pm_required_parameter_node_t *) r)->name, &s);
        IR_PUSH(ir_strdup(s.buf ? s.buf : ""), PM_NODE_TYPE_P(r, PM_REQUIRED_PARAMETER_NODE) ? 'r' : 'x');
        sbuf_free(&s);
    }
    for (size_t i = 0; i < pn->optionals.size; i++) IR_PUSH(ir_strdup(""), 'o');
    if (pn->rest) IR_PUSH(ir_strdup(""), '*');
    for (size_t i = 0; i < pn->posts.size; i++) IR_PUSH(ir_strdup(""), 'p');
    for (size_t i = 0; i < pn->keywords.size; i++) {
        const pm_node_t *k = pn->keywords.nodes[i];
        sbuf_t s; sbuf_init(&s);
        if (PM_NODE_TYPE_P(k, PM_REQUIRED_KEYWORD_PARAMETER_NODE))
            ir_name_into(f, ((const pm_required_keyword_parameter_node_t *) k)->name, &s);
        else if (PM_NODE_TYPE_P(k, PM_OPTIONAL_KEYWORD_PARAMETER_NODE))
            ir_name_into(f, ((const pm_optional_keyword_parameter_node_t *) k)->name, &s);
        IR_PUSH(ir_strdup(s.buf ? s.buf : ""), 'k');
        sbuf_free(&s);
    }
    if (pn->keyword_rest) IR_PUSH(ir_strdup(""), PM_NODE_TYPE_P(pn->keyword_rest, PM_FORWARDING_PARAMETER_NODE) ? 'f' : 'K');
    if (pn->block) IR_PUSH(ir_strdup(""), '&');
#undef IR_PUSH
    *out = ps;
    return n;
}

static void ir_free_params(ir_param *ps, int n) {
    for (int i = 0; i < n; i++) free(ps[i].name);
    free(ps);
}

static char *ir_sym_name(rbs_parser_t *p, rbs_ast_symbol_t *sym) {
    sbuf_t s; sbuf_init(&s);
    name_of_symbol(p, sym, &s);
    char *r = ir_strdup(s.buf ? s.buf : "");
    sbuf_free(&s);
    return r;
}

/* Has the def any parameter a seed cannot address by position? Seed
 * parameter slots follow the def's own order: required, optional, rest and
 * trailing positionals, then keywords. A destructuring parameter is one slot
 * the signature types as a whole, which the seed cannot split. */
static const char *ir_unseedable_params(const ir_param *ps, int n) {
    for (int i = 0; i < n; i++)
        if (ps[i].kind == 'x') return "the method has a destructuring parameter";
    return NULL;
}

/* The `k`th node of an RBS list, NULL past its end. */
static rbs_node_t *ir_list_at(rbs_node_list_t *l, int k) {
    rbs_node_list_node_t *c = l ? l->head : NULL;
    for (; c && k > 0; k--) c = c->next;
    return c ? c->node : NULL;
}

static int ir_list_len(rbs_node_list_t *l) {
    int n = 0;
    if (l) for (rbs_node_list_node_t *c = l->head; c; c = c->next) n++;
    return n;
}

/* A method type (`(T, k: U) -> R`) against the def: fills ret/ptypes, or
 * says why the whole signature cannot be applied. */
static const char *ir_method_type(rbs_parser_t *p, rbs_node_t *mtn, const ir_target *t,
                                  const ir_param *ps, int np, char **ret, char **ptypes) {
    if (mtn == NULL || mtn->type != RBS_METHOD_TYPE) return "it is not a method type";
    rbs_method_type_t *mt = (rbs_method_type_t *) mtn;
    if (mt->type_params != NULL && mt->type_params->length > 0)
        return "generic methods (type parameters) have no Spinel equivalent";
    if (mt->block != NULL) return "block types are not seeded";
    if (mt->type == NULL || mt->type->type != RBS_TYPES_FUNCTION)
        return "an untyped parameter list (`(?)`) has no Spinel equivalent";
    rbs_types_function_t *fn = (rbs_types_function_t *) mt->type;
    const char *why = ir_unseedable_params(ps, np);
    if (why) return why;
    /* Each positional kind of the def takes the signature's list of that
       kind, one to one: `(Integer, ?String, *Symbol, Float)` for
       `def m(a, b = "", *c, d)`. A rest or `**` slot is typed by
       construction, as the gathered array or hash, and takes no field, as
       --rbs gives it none. One optional past the def's own, where the def
       gathers the rest (`(Symbol, ?untyped)` for `def fetch(key, *default)`),
       describes what the rest gathers: it lands on the rest slot, which
       --rbs leaves as gathered. A def that forwards with `...` hands every
       argument past its named positionals on to the method it calls: those
       take the signature's leading positionals, required ones first, and
       the rest of the signature is the forwarded call's, which --rbs pins
       nothing of either. */
    int nreq = 0, nopt = 0, nrest = 0, npost = 0, nkw = 0, nkwrest = 0, nfwd = 0;
    for (int i = 0; i < np; i++)
        switch (ps[i].kind) {
            case 'r': nreq++; break;
            case 'o': nopt++; break;
            case '*': nrest++; break;
            case 'p': npost++; break;
            case 'k': nkw++; break;
            case 'K': nkwrest++; break;
            case 'f': nfwd++; break;
            default: break;
        }
    int sreq = ir_list_len(fn->required_positionals), sopt = ir_list_len(fn->optional_positionals);
    if (nfwd) {
        if (sreq < nreq || sreq + sopt < nreq + nopt)
            return "the signature's positional parameters do not match the method's";
    }
    else {
        bool opt_on_rest = nrest > 0 && fn->rest_positionals == NULL && sopt == nopt + 1 &&
                           npost == 0 && ir_list_len(fn->trailing_positionals) == 0;
        if (sreq != nreq || (sopt != nopt && !opt_on_rest) ||
            ((fn->rest_positionals != NULL) != (nrest > 0) && !opt_on_rest) || ir_list_len(fn->trailing_positionals) != npost)
            return "the signature's positional parameters do not match the method's";
        if (fn->rest_keywords != NULL && nkwrest == 0)
            return "the signature's keyword parameters do not match the method's";
    }
    for (int i = 0; i < np; i++) {
        sbuf_t tag; sbuf_init(&tag);
        if (ps[i].kind == 'r' || ps[i].kind == 'o' || ps[i].kind == 'p') {
            int k = 0;
            for (int j = 0; j < i; j++) if (ps[j].kind == ps[i].kind) k++;
            rbs_node_list_t *l = ps[i].kind == 'r' ? fn->required_positionals
                               : ps[i].kind == 'o' ? fn->optional_positionals
                                                   : fn->trailing_positionals;
            /* ahead of `...`, an optional takes the signature's next
               positional, a required one while any is left */
            if (nfwd && ps[i].kind == 'o' && nreq + k < sreq) { l = fn->required_positionals; k += nreq; }
            else if (nfwd && ps[i].kind == 'o') k -= sreq - nreq;
            rbs_node_t *n = ir_list_at(l, k);
            rbs_types_function_param_t *fp = (rbs_types_function_param_t *) n;
            if (n == NULL || n->type != RBS_TYPES_FUNCTION_PARAM || !ir_map(p, fp->type, t, &tag)) {
                sbuf_free(&tag);
                return "a parameter type is outside the types Spinel can pin";
            }
        }
        else if (ps[i].kind == 'k') {
            rbs_types_function_param_t *fp = NULL;
            const rbs_hash_t *hs[] = { fn->required_keywords, fn->optional_keywords };
            for (int h = 0; h < 2 && !fp; h++) {
                if (hs[h] == NULL) continue;
                for (rbs_hash_node_t *e = hs[h]->head; e; e = e->next) {
                    char *kn = ir_sym_name(p, (rbs_ast_symbol_t *) e->key);
                    bool eq = strcmp(kn, ps[i].name) == 0;
                    free(kn);
                    if (eq) { fp = (rbs_types_function_param_t *) e->value; break; }
                }
            }
            if (fp == NULL) { sbuf_free(&tag); return "the signature does not name every keyword parameter of the method"; }
            if (!ir_map(p, fp->type, t, &tag)) { sbuf_free(&tag); return "a parameter type is outside the types Spinel can pin"; }
        }
        else { sbuf_free(&tag); continue; }
        ptypes[i] = ir_strdup(tag.buf);
        sbuf_free(&tag);
    }
    int skw = 0;
    const rbs_hash_t *hs2[] = { fn->required_keywords, fn->optional_keywords };
    for (int h = 0; h < 2; h++) if (hs2[h]) for (rbs_hash_node_t *e = hs2[h]->head; e; e = e->next) skw++;
    if (!nfwd && skw != nkw) return "the signature's keyword parameters do not match the method's";
    sbuf_t r; sbuf_init(&r);
    if (!ir_map(p, fn->return_type, t, &r)) { sbuf_free(&r); return "the return type is outside the types Spinel can pin"; }
    *ret = ir_strdup(r.buf);
    sbuf_free(&r);
    return NULL;
}

static void ir_emit_method_fact(const ir_file *f, const ir_target *t, const ir_ann *a,
                                const char *name, char *ret, char **ptypes, int np) {
    (void) f; (void) name;
    ir_nfact *fa = ir_nfact_for(f, t->node);
    fa->ret = ret;
    sbuf_t ps; sbuf_init(&ps);
    int last = -1;
    for (int k = 0; k < np; k++) if (ptypes[k]) last = k;
    for (int k = 0; k <= last; k++) {
        if (k) sbuf_append_cstr(&ps, ",");
        if (ptypes[k]) sbuf_append_cstr(&ps, ptypes[k]);
    }
    fa->params = ir_strdup(ps.buf ? ps.buf : "");
    sbuf_free(&ps);
    for (int k = 0; k < np; k++) free(ptypes[k]);
    free(ptypes);
    ir_orig(f, a->first_line, &fa->file, &fa->line, &fa->fid);
    fa->col = a->first_col + 1;
    ir_names_merge(&fa->names, &g_ir_names);
}

/* An attr_* call: one tag for every name it declares. */
static void ir_emit_ivar_fact(const ir_file *f, const ir_target *t, const char *tag, const ir_ann *a) {
    ir_nfact *fa = ir_nfact_for(f, t->node);
    fa->ivar = ir_strdup(tag);
    ir_orig(f, a->first_line, &fa->file, &fa->line, &fa->fid);
    fa->col = a->first_col + 1;
    ir_names_merge(&fa->names, &g_ir_names);
}

/* The annotations a def carries: one signature (`#:` or `# @rbs (...) ->`),
 * or doc-style lines (`# @rbs x: T`, `# @rbs return: T`, a trailing `#: T`),
 * or `# @rbs skip`. Never a mix: a second signature is an error, since one
 * of the two would have to be dropped without a word. */
static void ir_second_signature(const ir_file *f, const char *name, const ir_ann *later, const ir_ann *earlier) {
    if (later->first_line < earlier->first_line) { const ir_ann *t = later; later = earlier; earlier = t; }
    ir_diag(f, later->first_line, later->first_col + 1, 1,
            "inline RBS: `%s` has a second signature here; the first is at %s\n"
            "  write one signature: a `#:` method type, or `# @rbs` parameter and return lines",
            name, ir_where(f, earlier->first_line));
}

static void ir_apply_def(const ir_file *f, ir_target *t) {
    const pm_def_node_t *d = (const pm_def_node_t *) t->node;
    sbuf_t mn; sbuf_init(&mn);
    ir_name_into(f, d->name, &mn);
    char *name = ir_strdup(mn.buf ? mn.buf : "");
    sbuf_free(&mn);
    sbuf_free(&g_ir_names);
    ir_param *ps = NULL;
    int np = ir_def_params(f, d, &ps);
    int na = t->nanns;
    rbs_parser_t **parsers = (rbs_parser_t **) calloc((size_t) na + 1, sizeof(rbs_parser_t *));
    rbs_node_t **parsed = (rbs_node_t **) calloc((size_t) na + 1, sizeof(rbs_node_t *));
    int bad = 0, skip = 0;

    /* parse every annotation first: a syntax error anywhere stops the compile */
    for (int i = 0; i < na; i++) {
        ir_ann *a = t->anns[i];
        rbs_parser_t *p = parsers[i] = ir_parser_for(a, 0);
        rbs_ast_ruby_annotations_t *an = NULL;
        bool ok = a->shape == IA_TRAILING ? rbs_parse_inline_trailing_annotation(p, &an)
                                          : rbs_parse_inline_leading_annotation(p, &an);
        if (!ok || an == NULL) { ir_syntax_error(f, a, p); bad = 1; continue; }
        parsed[i] = (rbs_node_t *) an;
    }

    /* what kind of signature they make */
    int first_colon = -1, first_rbs_sig = -1, first_doc = -1, ncolon = 0, nrbs_sig = 0;
    for (int i = 0; i < na && !bad; i++) {
        if (!parsed[i]) continue;
        switch (parsed[i]->type) {
            case RBS_AST_RUBY_ANNOTATIONS_COLON_METHOD_TYPE_ANNOTATION:
                if (first_colon < 0) first_colon = i;
                ncolon++;
                break;
            case RBS_AST_RUBY_ANNOTATIONS_METHOD_TYPES_ANNOTATION:
                if (first_rbs_sig < 0) first_rbs_sig = i;
                else { ir_second_signature(f, name, t->anns[i], t->anns[first_rbs_sig]); bad = 1; }
                nrbs_sig++;
                break;
            case RBS_AST_RUBY_ANNOTATIONS_PARAM_TYPE_ANNOTATION:
            case RBS_AST_RUBY_ANNOTATIONS_RETURN_TYPE_ANNOTATION:
            case RBS_AST_RUBY_ANNOTATIONS_NODE_TYPE_ASSERTION:
            case RBS_AST_RUBY_ANNOTATIONS_SPLAT_PARAM_TYPE_ANNOTATION:
            case RBS_AST_RUBY_ANNOTATIONS_DOUBLE_SPLAT_PARAM_TYPE_ANNOTATION:
            case RBS_AST_RUBY_ANNOTATIONS_BLOCK_PARAM_TYPE_ANNOTATION:
                if (first_doc < 0) first_doc = i;
                break;
            case RBS_AST_RUBY_ANNOTATIONS_SKIP_ANNOTATION:
                skip = 1;
                break;
            case RBS_AST_RUBY_ANNOTATIONS_INSTANCE_VARIABLE_ANNOTATION:
                ir_warn(f, t->anns[i], "this instance variable declaration",
                        "it belongs to the method's leading comment block; separate it from the declaration with a blank line");
                break;
            default:
                ir_warn(f, t->anns[i], "this annotation", "it does not describe a method");
                break;
        }
    }
    int first_sig = first_colon >= 0 ? first_colon : first_rbs_sig;
    if (!bad && first_colon >= 0 && first_rbs_sig >= 0) {
        ir_second_signature(f, name, t->anns[first_rbs_sig], t->anns[first_colon]); bad = 1;
    }
    if (!bad && first_sig >= 0 && first_doc >= 0) {
        ir_second_signature(f, name, t->anns[first_doc], t->anns[first_sig]); bad = 1;
    }

    char *ret = NULL;
    char **ptypes = (char **) calloc((size_t) (np ? np : 1), sizeof(char *));
    ir_ann *by = NULL;    /* the annotation a fact is reported at */
    if (!bad && !skip && first_sig >= 0) {
        ir_ann *a = t->anns[first_sig];
        rbs_parser_t *p = parsers[first_sig];
        rbs_node_t *mt = NULL;
        by = a;
        if (ncolon > 1) {
            ir_warn(f, a, "an overloaded signature (several `#:` lines)",
                    "a method has one seeded signature in Spinel, so the overload set is ignored whole");
            skip = 1;
        }
        else if (first_colon >= 0) {
            mt = ((rbs_ast_ruby_annotations_colon_method_type_annotation_t *) parsed[first_sig])->method_type;
        }
        else {
            rbs_ast_ruby_annotations_method_types_annotation_t *m =
                (rbs_ast_ruby_annotations_method_types_annotation_t *) parsed[first_sig];
            int nov = m->overloads ? (int) m->overloads->length : 0;
            bool dot3 = m->dot3_location.end_byte > m->dot3_location.start_byte;
            if (dot3) {
                ir_warn(f, a, nov ? "a signature ending in `| ...`" : "`# @rbs ...` (the overridden method's signature)",
                        "Spinel does not look up the overridden method's types");
                skip = 1;
            }
            else if (nov > 1) {
                ir_warn(f, a, "an overloaded signature",
                        "a method has one seeded signature in Spinel, so the overload set is ignored whole");
                skip = 1;
            }
            else if (nov == 1 && m->overloads->head->node->type == RBS_AST_MEMBERS_METHOD_DEFINITION_OVERLOAD) {
                mt = ((rbs_ast_members_method_definition_overload_t *) m->overloads->head->node)->method_type;
            }
        }
        if (!skip) {
            const char *why = ir_method_type(p, mt, t, ps, np, &ret, ptypes);
            if (why) { ir_warn(f, a, "this signature", why); skip = 1; }
        }
    }
    else if (!bad && !skip && first_doc >= 0) {
        /* doc-style: each line names one slot */
        ir_ann **param_by = (ir_ann **) calloc((size_t) (np ? np : 1), sizeof(ir_ann *));
        ir_ann *ret_by = NULL;
        by = t->anns[first_doc];
        for (int i = 0; i < na && !bad && !skip; i++) {
            rbs_node_t *node = parsed[i];
            if (!node) continue;
            ir_ann *a = t->anns[i];
            rbs_parser_t *p = parsers[i];
            sbuf_t tag; sbuf_init(&tag);
            switch (node->type) {
                case RBS_AST_RUBY_ANNOTATIONS_PARAM_TYPE_ANNOTATION: {
                    rbs_ast_ruby_annotations_param_type_annotation_t *pa =
                        (rbs_ast_ruby_annotations_param_type_annotation_t *) node;
                    char *pname = ir_strndup(a->text + pa->name_location.start_byte,
                                             (size_t) (pa->name_location.end_byte - pa->name_location.start_byte));
                    int idx = -1;
                    for (int k = 0; k < np; k++)
                        if ((ps[k].kind == 'r' || ps[k].kind == 'k') && strcmp(ps[k].name, pname) == 0) idx = k;
                    if (idx < 0) {
                        char what[300];
                        snprintf(what, sizeof what, "the annotation of parameter `%s`", pname);
                        ir_warn(f, a, what, "the method has no required or keyword parameter of that name, so none of its annotations are applied");
                        skip = 1;
                    }
                    else if (param_by[idx]) {
                        ir_diag(f, a->first_line, a->first_col + 1, 1,
                                "inline RBS: parameter `%s` is annotated twice; the first is at %s",
                                pname, ir_where(f, param_by[idx]->first_line));
                        bad = 1;
                    }
                    else if (!ir_map(p, pa->param_type, t, &tag)) {
                        ir_warn(f, a, "a parameter annotation", "its type is outside the types Spinel can pin, so none of the method's annotations are applied");
                        skip = 1;
                    }
                    else { param_by[idx] = a; ptypes[idx] = ir_strdup(tag.buf); }
                    free(pname);
                    break;
                }
                case RBS_AST_RUBY_ANNOTATIONS_RETURN_TYPE_ANNOTATION:
                case RBS_AST_RUBY_ANNOTATIONS_NODE_TYPE_ASSERTION: {
                    rbs_node_t *rt = node->type == RBS_AST_RUBY_ANNOTATIONS_RETURN_TYPE_ANNOTATION
                        ? ((rbs_ast_ruby_annotations_return_type_annotation_t *) node)->return_type
                        : ((rbs_ast_ruby_annotations_node_type_assertion_t *) node)->type;
                    if (ret_by) {
                        ir_diag(f, a->first_line, a->first_col + 1, 1,
                                "inline RBS: the return type of `%s` is annotated twice; the first is at %s",
                                name, ir_where(f, ret_by->first_line));
                        bad = 1;
                    }
                    else if (!ir_map(p, rt, t, &tag)) {
                        ir_warn(f, a, "a return annotation", "its type is outside the types Spinel can pin, so none of the method's annotations are applied");
                        skip = 1;
                    }
                    else { ret_by = a; ret = ir_strdup(tag.buf); }
                    break;
                }
                case RBS_AST_RUBY_ANNOTATIONS_SPLAT_PARAM_TYPE_ANNOTATION:
                case RBS_AST_RUBY_ANNOTATIONS_DOUBLE_SPLAT_PARAM_TYPE_ANNOTATION:
                case RBS_AST_RUBY_ANNOTATIONS_BLOCK_PARAM_TYPE_ANNOTATION:
                    ir_warn(f, a, "a rest or block parameter annotation",
                            "rest and block parameters are not seeded, so none of the method's annotations are applied");
                    skip = 1;
                    break;
                default:
                    break;
            }
            sbuf_free(&tag);
        }
        bool any_param = false;
        for (int i = 0; i < np; i++) if (param_by[i]) any_param = true;
        const char *why = (!bad && !skip && any_param) ? ir_unseedable_params(ps, np) : NULL;
        if (why) { ir_warn(f, by, "the parameter annotations", why); skip = 1; }
        free(param_by);
    }
    if (!bad && !skip && by) {
        ir_emit_method_fact(f, t, by, name, ret, ptypes, np);
        ret = NULL;
        ptypes = NULL;
    }
    for (int i = 0; i < na; i++) if (parsers[i]) rbs_parser_free(parsers[i]);
    free(parsers);
    free(parsed);
    if (ptypes) { for (int i = 0; i < np; i++) free(ptypes[i]); free(ptypes); }
    free(ret);
    free(name);
    ir_free_params(ps, np);
}

/* `attr_reader :a, :b #: T`: one ivar seed per name, which is what
 * `attr_reader a: T` in a .rbs gives. */
static void ir_apply_attr(const ir_file *f, ir_target *t) {
    const pm_call_node_t *call = (const pm_call_node_t *) t->node;
    ir_ann *a = NULL;
    for (int i = 0; i < t->nanns; i++) {
        ir_ann *next = t->anns[i];
        if (next->shape != IA_TRAILING) {
            ir_warn(f, next, "this attribute annotation", "an attribute takes a trailing `#: T` type");
            continue;
        }
        if (a != NULL) {
            ir_diag(f, next->first_line, next->first_col + 1, 1,
                    "inline RBS: this attribute has a second type annotation; the first is at %s",
                    ir_where(f, a->first_line));
            return;
        }
        a = next;
    }
    if (a == NULL) return;
    if (t->in_def) {
        ir_warn(f, a, "this attribute type", "the attribute is declared inside a method body");
        return;
    }
    sbuf_free(&g_ir_names);
    rbs_parser_t *p = NULL;
    rbs_node_t *type = ir_parse_type(f, a, &p);
    if (type == NULL) { rbs_parser_free(p); return; }
    sbuf_t tag; sbuf_init(&tag);
    if (!ir_map(p, type, t, &tag)) {
        ir_warn(f, a, "this attribute type", "it is outside the types Spinel can pin");
        sbuf_free(&tag); rbs_parser_free(p);
        return;
    }
    const pm_arguments_node_t *args = call->arguments;
    int nsym = 0, nother = 0;
    if (args) for (size_t i = 0; i < args->arguments.size; i++) {
        if (PM_NODE_TYPE_P(args->arguments.nodes[i], PM_SYMBOL_NODE)) nsym++; else nother++;
    }
    if (nother > 0 || nsym == 0) {
        ir_warn(f, a, "this attribute type", "the attribute names are not all symbol literals, so there is nothing it can name");
    }
    else {
        ir_emit_ivar_fact(f, t, tag.buf, a);
    }
    sbuf_free(&tag);
    rbs_parser_free(p);
}

/* `# @rbs @x: T` in a class body. */
static void ir_apply_ivar_decl(const ir_file *f, const ir_ann *a, const ir_container *c,
                               const char *owner, const char *lookup) {
    if (c != NULL && !c->is_def && owner[0] && (c->body == NULL || !PM_NODE_TYPE_P(c->body, PM_STATEMENTS_NODE))) {
        ir_warn(f, a, "this instance variable declaration", "the class body has no statements for it to belong to");
        return;
    }
    if (c == NULL || c->is_def || !owner[0]) {
        ir_warn(f, a, "this instance variable declaration",
                c && c->is_def == 1 ? "it is inside a method body; declare it in the class body"
                : c && c->is_def == 2 ? "it is inside `class << self`, whose variables are the class's own, which are not seeded"
                : "it is not inside a class body");
        return;
    }
    rbs_parser_t *p = ir_parser_for(a, 0);
    rbs_ast_ruby_annotations_t *an = NULL;
    if (!rbs_parse_inline_leading_annotation(p, &an) || an == NULL) { ir_syntax_error(f, a, p); rbs_parser_free(p); return; }
    rbs_ast_ruby_annotations_instance_variable_annotation_t *iv =
        (rbs_ast_ruby_annotations_instance_variable_annotation_t *) an;
    ir_target t;
    memset(&t, 0, sizeof t);
    t.owner = (char *) owner;
    t.lookup = (char *) lookup;
    sbuf_free(&g_ir_names);
    sbuf_t tag; sbuf_init(&tag);
    if (!ir_map(p, iv->type, &t, &tag)) ir_warn(f, a, "this instance variable type", "it is outside the types Spinel can pin");
    else {
        char *nm = ir_sym_name(p, iv->ivar_name);
        ir_nfact *fa = ir_nfact_for(f, c->body);
        const char *file;
        int line, fid;
        ir_orig(f, a->first_line, &file, &line, &fid);
        /* the fact's own place, for a fact of declarations only: the first
           one's, which sp_inline_rbs_done names when its class body is
           never flattened (ir_nfact_for sets only its file; a line of 0
           says no annotation gave it a place yet) */
        if (fa->line == 0) { fa->file = file; fa->line = line; fa->fid = fid; fa->col = a->first_col + 1; }
        char ent[512];
        snprintf(ent, sizeof ent, "%s%s=%s@%d@%d@%d", fa->ivars.len ? "," : "", nm, tag.buf,
                 line, fid, a->first_col + 1);
        sbuf_append_cstr(&fa->ivars, ent);
        ir_names_merge(&fa->names, &g_ir_names);
        free(nm);
    }
    sbuf_free(&tag);
    rbs_parser_free(p);
}

static void ir_apply_other(const ir_file *f, ir_target *t) {
    for (int i = 0; i < t->nanns; i++) {
        ir_ann *a = t->anns[i];
        if (a->shape == IA_TRAILING && a->text[0] == '[') {
            ir_warn(f, a, "a type application (`#[...]`)", "generic superclasses and mixins are not modelled");
            continue;
        }
        switch (t->kind) {
            case IT_LOCAL_WRITE:
                ir_warn(f, a, "a type on a local variable", "Spinel infers local variables and cannot pin one");
                break;
            case IT_CONST_WRITE:
                ir_warn(f, a, "a type on a constant", "constants are not seeded; their type is inferred from the value");
                break;
            case IT_CLASS:
                ir_warn(f, a, "an annotation on a class or module", "generic classes, type aliases and module self types are not modelled");
                break;
            default:
                ir_warn(f, a, "a type on an expression", "Spinel does not hold a type for a single expression");
                break;
        }
    }
}

/* Build the comment blocks of one file and attach each annotation. */
static void ir_associate(ir_file *f, ir_walk *w) {
    int ncm = 0;
    ir_comment *cms = NULL;
    for (const pm_comment_t *c = (const pm_comment_t *) f->pm->comment_list.head; c; c = (const pm_comment_t *) c->node.next) {
        size_t s = ir_off(f, c->location.start), e = ir_off(f, c->location.end);
        if (c->type == PM_COMMENT_EMBDOC) continue;
        int line = ir_line_of(f, s);
        size_t ls = f->line_starts[line - 1];
        int leading = 1;
        for (size_t i = ls; i < s; i++) if (f->src[i] != ' ' && f->src[i] != '\t') { leading = 0; break; }
        cms = (ir_comment *) realloc(cms, sizeof(ir_comment) * (size_t) (ncm + 1));
        cms[ncm].start = s; cms[ncm].end = e; cms[ncm].line = line;
        cms[ncm].col = (int) (s - ls); cms[ncm].leading = leading;
        ncm++;
    }

    for (int i = 0; i < ncm; ) {
        ir_comment *cm = &cms[i];
        const char *txt = (const char *) f->src + cm->start;
        size_t tl = cm->end - cm->start;
        while (tl > 0 && (txt[tl - 1] == '\r' || txt[tl - 1] == '\n')) tl--;
        /* A block can begin after code; later comments must be leading,
           consecutive, and at the same column as its first comment. */
        int j = i + 1;
        while (j < ncm && cms[j].leading && cms[j].line == cms[j - 1].line + 1 && cms[j].col == cm->col) j++;
        if (!cm->leading) {
            int first = i;
            i = j;
            if (!(tl >= 2 && (txt[1] == ':' || txt[1] == '['))) continue;
            if (txt[1] == ':' && ir_rdoc_directive(txt + 2, tl - 2)) continue;
            ir_ann *a = ir_ann_new(IA_TRAILING);
            ir_ann_add_line(a, txt + 1, tl - 1, cm->line, cm->col + 1);
            for (int k = first + 1; k < j; k++) ir_ann_add_comment(f, a, &cms[k]);
            a->first_col = cm->col;
            /* the target: a def whose signature this line ends, else the
               outermost statement ending here, before the comment */
            ir_target *best = NULL;
            for (int k = 0; k < w->ntargets; k++) {
                ir_target *t = &w->targets[k];
                if (t->kind == IT_DEF && !((const pm_def_node_t *) t->node)->equal_loc.start &&
                    t->sig_line == cm->line && t->sig_end <= cm->start) { best = t; break; }
            }
            if (!best) for (int k = 0; k < w->ntargets; k++) {
                ir_target *t = &w->targets[k];
                if (t->kind == IT_DEF || t->kind == IT_CLASS) continue;
                if (t->end_line != cm->line || t->end_off > cm->start) continue;
                if (!best || t->end_off > best->end_off ||
                    (t->end_off == best->end_off && t->start_off < best->start_off)) best = t;
            }
            if (!best) for (int k = 0; k < w->ntargets; k++) {
                ir_target *t = &w->targets[k];
                if (t->kind == IT_CLASS && t->start_line == cm->line) { best = t; break; }
            }
            if (best) ir_attach(best, a);
            else {
                ir_warn(f, a, "this annotation", "it does not follow a supported declaration on its line");
                ir_ann_free(a);
            }
            continue;
        }
        int block_end = cms[j - 1].line;
        /* annotations in the block, with their continuation lines */
        ir_ann **anns = NULL;
        int nanns = 0;
        ir_ann *cur = NULL;
        int pending_blank = -1;
        for (int k = i; k < j; k++) {
            const char *s = (const char *) f->src + cms[k].start;
            size_t n = cms[k].end - cms[k].start;
            while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
            ir_cline kind = ir_classify(s, n);
            if (kind == CL_COLON || kind == CL_RBS) {
                cur = ir_ann_new(kind == CL_COLON ? IA_COLON : IA_RBS);
                size_t pre = (kind == CL_RBS && n >= 2 && s[1] == ' ') ? 2 : 1;
                ir_ann_add_line(cur, s + pre, n - pre, cms[k].line, cms[k].col + (int) pre);
                cur->first_col = cms[k].col;
                anns = (ir_ann **) realloc(anns, sizeof(ir_ann *) * (size_t) (nanns + 1));
                anns[nanns++] = cur;
                pending_blank = -1;
            }
            else if (cur != NULL) {
                /* a continuation: `#|` and the rest of the signature, or an
                   indented line after `# ` */
                bool bar = n >= 2 && s[1] == '|';
                size_t pre = ir_cont_prefix(s, n);
                bool blank = true;
                for (size_t z = pre; z < n; z++)
                    if (s[z] != ' ' && s[z] != '\t') { blank = false; break; }
                if (blank) {
                    if (pending_blank < 0) pending_blank = k;
                }
                else if (bar || s[pre] == ' ' || s[pre] == '\t') {
                    /* Internal blank comment lines belong to a continuation;
                       trailing blank comment lines do not. */
                    if (pending_blank >= 0)
                        for (int z = pending_blank; z < k; z++) ir_ann_add_comment(f, cur, &cms[z]);
                    ir_ann_add_comment(f, cur, &cms[k]);
                    pending_blank = -1;
                }
                else { cur = NULL; pending_blank = -1; }
            }
        }
        i = j;
        if (nanns == 0) { free(anns); continue; }
        int ci = ir_find_container(w, cms[j - 1].start);
        const ir_container *cont = ci >= 0 ? &w->conts[ci] : NULL;
        /* A leading block belongs to the declaration on the following line. */
        int next = block_end + 1;
        ir_target *target = NULL;
        if (next <= f->nlines) {
            for (int k = 0; k < w->ntargets && !target; k++) {
                ir_target *t = &w->targets[k];
                if ((t->kind == IT_DEF || t->kind == IT_ATTR || t->kind == IT_CLASS || t->kind == IT_CONST_WRITE) &&
                    t->start_line == next) target = t;
            }
        }
        for (int k = 0; k < nanns; k++) {
            ir_ann *a = anns[k];
            /* `# @rbs @x: T` belongs to the class body it is written in */
            if (a->shape == IA_RBS) {
                const char *r = a->text + 4;
                while (*r == ' ' || *r == '\t') r++;
                if (*r == '@' && target == NULL) {
                    ir_apply_ivar_decl(f, a, cont, cont ? cont->owner : "", cont ? cont->lookup : "");
                    ir_ann_free(a);
                    continue;
                }
            }
            if (cont && cont->is_def == 1 && target == NULL) {
                ir_warn(f, a, "this annotation", "it is inside a method body and precedes no method definition or attribute");
                ir_ann_free(a);
                continue;
            }
            if (target == NULL) {
                ir_warn(f, a, "this annotation",
                        next > f->nlines ? "nothing follows it in the file"
                                         : "this comment block has no supported declaration on the following line");
                ir_ann_free(a);
                continue;
            }
            ir_attach(target, a);
        }
        free(anns);
    }
    free(cms);

    for (int k = 0; k < w->ntargets; k++) {
        ir_target *t = &w->targets[k];
        if (t->nanns == 0) continue;
        switch (t->kind) {
            case IT_DEF: ir_apply_def(f, t); break;
            case IT_ATTR: ir_apply_attr(f, t); break;
            default: ir_apply_other(f, t); break;
        }
    }
}

/* ---- the entry points ------------------------------------------------------ */

/* Does a parse hold an annotation-shaped comment? A program with none costs
 * one look at each comment. */
static bool ir_any_annotation(const pm_parser_t *parser) {
    for (const pm_comment_t *c = (const pm_comment_t *) parser->comment_list.head; c;
         c = (const pm_comment_t *) c->node.next) {
        if (c->type != PM_COMMENT_INLINE || sp_in_builtin(c->location.start)) continue;
        size_t n = (size_t) (c->location.end - c->location.start);
        if (ir_classify((const char *) c->location.start, n) != CL_PROSE ||
            (n >= 2 && c->location.start[1] == '[')) return true;
    }
    return false;
}

/* A parse's tree walked for the declarations and the places an annotation
 * can attach to: the classes and modules it declares join the names a type
 * resolves against (known_names), whichever file's annotation names them. */
static void ir_walk_open(ir_file *f, ir_walk *w, pm_parser_t *parser, pm_node_t *root,
                         const char *source, size_t len, const char *path, int fid) {
    memset(f, 0, sizeof *f);
    f->path = path;
    f->fid = fid;
    f->src = (const uint8_t *) source;
    f->len = len;
    f->pm = parser;
    f->nlines = 1;
    for (size_t i = 0; i < len; i++) if (source[i] == '\n') f->nlines++;
    f->line_starts = (size_t *) malloc(sizeof(size_t) * (size_t) f->nlines);
    if (f->line_starts == NULL) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    f->line_starts[0] = 0;
    int l = 1;
    for (size_t i = 0; i < len; i++) if (source[i] == '\n') f->line_starts[l++] = i + 1;
    memset(w, 0, sizeof *w);
    w->f = f;
    ir_ctx cx = { w, "", "", 0, 0, 0 };
    pm_visit_node(root, ir_visit, &cx);
}

static void ir_walk_close(ir_file *f, ir_walk *w) {
    for (int i = 0; i < w->ntargets; i++) {
        for (int j = 0; j < w->targets[i].nanns; j++) ir_ann_free(w->targets[i].anns[j]);
        free(w->targets[i].owner);
        free(w->targets[i].lookup);
        free(w->targets[i].anns);
    }
    for (int i = 0; i < w->nconts; i++) { free(w->conts[i].owner); free(w->conts[i].lookup); }
    free(w->targets);
    free(w->conts);
    free(f->line_starts);
}

/* Under --load-model=graph, before any file's annotations are read: does
 * the file's parse hold one, and the classes and modules it declares, for
 * a type any file names (RBS resolves a name against the whole program's
 * declarations, wherever they are). */
static int sp_inline_rbs_any(pm_parser_t *parser) {
    return sp_inline_rbs_enabled && ir_any_annotation(parser);
}
static void sp_inline_rbs_declare(pm_parser_t *parser, pm_node_t *root, const char *source, size_t len) {
    if (!sp_inline_rbs_enabled) return;
    ir_file f;
    ir_walk w;
    ir_walk_open(&f, &w, parser, root, source, len, "", 0);
    ir_walk_close(&f, &w);
}

/* Between pm_parse and flatten: attach and parse the annotations of one
 * parse, `source` read from `path` (file `fid` of the node table's list),
 * reporting as it goes. Answers the number of errors; any stops the compile. */
static int sp_inline_rbs_run(pm_parser_t *parser, pm_node_t *root,
                             const char *source, size_t len, const char *path, int fid) {
    if (!sp_inline_rbs_enabled || !ir_any_annotation(parser)) return 0;
    ir_file f;
    ir_walk w;
    ir_walk_open(&f, &w, parser, root, source, len, path, fid);
    g_hold = 1;
    ir_associate(&f, &w);
    g_hold = 0;
    ir_flush_held();
    ir_walk_close(&f, &w);
    return g_inline_errors;
}

static int ir_nfact_cmp(const void *a, const void *b) {
    const pm_node_t *x = ((const ir_nfact *) a)->node, *y = ((const ir_nfact *) b)->node;
    return x < y ? -1 : x > y;
}

/* From flatten: the facts of `node`, under the id it is about to get. */
static void sp_inline_rbs_emit(const pm_node_t *node, int id) {
    if (g_nnf == 0) return;
    if (!g_nf_sorted) { qsort(g_nf, (size_t) g_nnf, sizeof(ir_nfact), ir_nfact_cmp); g_nf_sorted = 1; }
    if (PM_NODE_TYPE_P(node, PM_PROGRAM_NODE)) { emit_int(id, "rbs_any", 1); return; }
    ir_nfact key;
    key.node = node;
    ir_nfact *fa = (ir_nfact *) bsearch(&key, g_nf, (size_t) g_nnf, sizeof(ir_nfact), ir_nfact_cmp);
    if (fa == NULL) return;
    fa->emitted = 1;
    if (fa->ret) emit_str(id, "rbs_ret", fa->ret);
    /* an empty list is still a fact (`def m` with no parameters) */
    if (fa->params) emit_str(id, "rbs_params", fa->params[0] ? fa->params : "-");
    if (fa->ivar) emit_str(id, "rbs_ivar", fa->ivar);
    if (fa->ivars.len) emit_str(id, "rbs_ivars", fa->ivars.buf);
    if (fa->names.len) emit_bytes(id, "rbs_names", fa->names.buf, fa->names.len);
    if (fa->ret || fa->params || fa->ivar) {
        emit_int(id, "rbs_line", fa->line);
        emit_int(id, "rbs_col", fa->col);
        emit_int(id, "rbs_file", fa->fid);
    }
}

/* After flatten: a fact whose node flatten never reached would reach no
 * Scope or class either, so it is reported here rather than lost. */
static void sp_inline_rbs_done(void) {
    for (int i = 0; i < g_nnf; i++) {
        ir_nfact *fa = &g_nf[i];
        if (fa->emitted || !(fa->ret || fa->params || fa->ivar || fa->ivars.len)) continue;
        fprintf(stderr, "spinel: %s:%d: warning: inline RBS: this annotation is not applied: the declaration "
                        "it belongs to is not part of the program Spinel compiles\n", fa->file, fa->line);
    }
}

static void sp_inline_rbs_free(void) {
    for (int i = 0; i < g_nnf; i++) {
        free(g_nf[i].ret);
        free(g_nf[i].params);
        free(g_nf[i].ivar);
        sbuf_free(&g_nf[i].ivars);
        sbuf_free(&g_nf[i].names);
    }
    sbuf_free(&g_ir_names);
    free(g_nf);
    g_nf = NULL;
    g_nnf = g_cnf = 0;
    g_nf_sorted = 0;
}
