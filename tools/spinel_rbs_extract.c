/* spinel_rbs_extract -- walk a directory for .rbs files and emit a
 * seed file for spinel_analyze's --rbs path.
 *
 * Reads RBS source through the vendored rbs C parser (vendor/rbs/) and
 * emits a tiny line-oriented seed format consumed by load_rbs_seeds /
 * apply_rbs_seeds in spinel_analyze.rb. Per the design conversation,
 * this is *advisory* seeding: only a subset of RBS maps to spinel's
 * type vocabulary, and anything outside the subset is silently
 * skipped. The analyzer's existing inference still runs on top.
 *
 * Subset supported (everything else dropped without warning):
 *   - Primitives: Integer, Float, String, Symbol, TrueClass,
 *     FalseClass, NilClass, bool, nil; a void return pins nothing
 *   - Nominal class instances → obj_<QualifiedName>
 *   - Array[T] where T is in subset → str_array / int_array /
 *     float_array / sym_array / obj_X_ptr_array / poly_array
 *   - Array[Array[Integer]] / Array[Array[Float]] → int_array_array /
 *     float_array_array (a request the narrowing pass answers, not a pin)
 *   - Hash[K, V] → str_int_hash / str_str_hash / str_poly_hash, and
 *     sym_poly_hash for every Symbol-keyed Hash (the analyzer has no
 *     other Symbol-keyed kind)
 *   - Optional T (T?) → <subset>?    (recursive)
 *   - Union T | nil → T?    (any other union → poly)
 *   - A record `{ name: T, ... }` → sym_poly_hash (str_poly_hash for
 *     String keys, poly for other keys)
 *   - A type alias (`type patch_t = Array[untyped]`, generic ones
 *     excepted) → what its type maps to, wherever the name is referenced
 *   - An `instance` return in a class, and a `self` return in its
 *     instance method → obj_<QualifiedName>; `self` in its singleton
 *     method → class
 *
 * Parameters: required, optional and trailing positionals each take one
 * field of the ptype list, in the Ruby def's order; a keyword takes a
 * `name:type` field, applied by name; the rest parameter takes an empty field (the
 * analyzer types it by construction), `**rest` none. A parameter whose
 * type is outside the subset takes an empty field, and a return outside
 * it reads "-": the method keeps the seeds of everything else.
 *
 * Dropped whole: generics with variables, overloaded methods, proc and
 * untyped function types.
 *
 * Usage:
 *   spinel_rbs_extract [--positions] DIR [DIR ...]
 *
 * Walks each DIR recursively for *.rbs files, writes seed lines to
 * stdout (with --positions, each preceded by `src FILE:LINE`). The `spinel` wrapper captures stdout to a tmpfile and
 * passes it as ARGV[2] to spinel_analyze. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "rbs/parser.h"
#include "rbs/ast.h"
#include "rbs/util/rbs_encoding.h"
#include "rbs/util/rbs_constant_pool.h"
#include "rbs_map.h"

/* --positions: each seed line is preceded by `src FILE:LINE`, its place in
 * the .rbs file, so the analyzer can name it when an inline RBS annotation
 * in the program says something else (docs/inline-rbs.md). */
static int g_positions = 0;
static const char *g_cur_path = NULL;
static size_t *g_cur_newlines = NULL;
static size_t g_cur_newlines_len = 0;

/* Source locations are byte offsets. Index once per file: parent members
 * are emitted before nested declarations, so lookups can move backwards. */
static void index_positions(const char *src, size_t len) {
    size_t count = 0;
    for (size_t i = 0; i < len && src[i]; i++) if (src[i] == '\n') count++;
    if (count == 0) return;
    g_cur_newlines = (size_t *) malloc(count * sizeof(size_t));
    if (g_cur_newlines == NULL) {
        fprintf(stderr, "spinel_rbs_extract: out of memory\n");
        exit(1);
    }
    for (size_t i = 0; i < len && src[i]; i++) {
        if (src[i] == '\n') g_cur_newlines[g_cur_newlines_len++] = i;
    }
}

static void emit_position(FILE *out, int byte_pos) {
    if (!g_positions || g_cur_path == NULL) return;
    size_t lo = 0, hi = g_cur_newlines_len;
    if (byte_pos > 0) {
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (g_cur_newlines[mid] < (size_t) byte_pos) lo = mid + 1;
            else hi = mid;
        }
    }
    fprintf(out, "src %s:%zu\n", g_cur_path, lo + 1);
}

/* ---- overloaded methods ------------------------------------ */

/* The methods some declaration gives more than one overload: several
 * `|`-separated signatures, or a `| ...` that adds to a declaration
 * elsewhere (often another file). Keyed "<scope> <i|c> <name>". Collected
 * over every file before any is emitted, so a declaration whose overloads
 * another file adds is known for one when it is reached. */
static char **g_overloaded = NULL;
static size_t g_noverloaded = 0, g_coverloaded = 0;

static void overloaded_key(char *buf, size_t n, const char *scope, char kind, const char *name) {
    snprintf(buf, n, "%s %c %s", scope, kind, name);
}

static void overloaded_add(const char *scope, char kind, const char *name) {
    char key[1024];
    overloaded_key(key, sizeof key, scope, kind, name);
    for (size_t i = 0; i < g_noverloaded; i++) if (strcmp(g_overloaded[i], key) == 0) return;
    if (g_noverloaded == g_coverloaded) {
        g_coverloaded = g_coverloaded ? g_coverloaded * 2 : 16;
        g_overloaded = (char **) realloc(g_overloaded, g_coverloaded * sizeof(char *));
        if (g_overloaded == NULL) { fprintf(stderr, "spinel_rbs_extract: out of memory\n"); exit(1); }
    }
    g_overloaded[g_noverloaded++] = strdup(key);
}

static bool overloaded_has(const char *scope, char kind, const char *name) {
    char key[1024];
    overloaded_key(key, sizeof key, scope, kind, name);
    for (size_t i = 0; i < g_noverloaded; i++) if (strcmp(g_overloaded[i], key) == 0) return true;
    return false;
}

static bool method_kind_has(rbs_ast_members_method_definition_t *m, char kind) {
    if (m->kind == RBS_METHOD_DEFINITION_KIND_SINGLETON_INSTANCE) return true;
    return kind == 'i' ? m->kind == RBS_METHOD_DEFINITION_KIND_INSTANCE
                       : m->kind == RBS_METHOD_DEFINITION_KIND_SINGLETON;
}

/* Note the methods of +members+ (of class/module +scope+) that are overloaded. */
static void collect_overloaded(rbs_parser_t *p, rbs_node_list_t *members, const char *scope) {
    if (members == NULL) return;
    for (rbs_node_list_node_t *cur = members->head; cur != NULL; cur = cur->next) {
        if (cur->node->type != RBS_AST_MEMBERS_METHOD_DEFINITION) continue;
        rbs_ast_members_method_definition_t *m = (rbs_ast_members_method_definition_t *) cur->node;
        if (!m->overloading && (m->overloads == NULL || m->overloads->length <= 1)) continue;
        sbuf_t mname;
        sbuf_init(&mname);
        name_of_symbol(p, m->name, &mname);
        if (method_kind_has(m, 'i')) overloaded_add(scope, 'i', mname.buf);
        if (method_kind_has(m, 'c')) overloaded_add(scope, 'c', mname.buf);
        sbuf_free(&mname);
    }
}

/* ---- emit one method signature ----------------------------- */

/* Whether the declaration whose members are being emitted is a class
 * (true) or a module (false): a `self` return names an instance of the
 * former and nothing nameable of the latter. */
static bool g_self_scope_is_class = false;

/* Pick the seed-line keyword from rbs's method-definition kind.
 * `meth` lands in the instance table (@cls_meth_*), `cmeth` in the
 * class table (@cls_cmeth_*). SINGLETON_INSTANCE (def self?.foo)
 * emits both so seed_class_method on the analyzer side hits whichever
 * the source actually defined. */
static void emit_method(rbs_parser_t *p, rbs_ast_members_method_definition_t *m,
                        const char *scope, const char *enclosing_scope, FILE *out) {
    if (m->overloads == NULL || m->overloads->head == NULL) return;
    /* An overloaded method is not seeded: a seed pins one signature, and
     * the method takes what every overload takes, so any one overload
     * would pin it narrower than its calls (collect_overloaded). */
    rbs_node_t *first = m->overloads->head->node;
    if (first->type != RBS_AST_MEMBERS_METHOD_DEFINITION_OVERLOAD) return;
    rbs_ast_members_method_definition_overload_t *ov =
        (rbs_ast_members_method_definition_overload_t *) first;

    if (ov->method_type == NULL || ov->method_type->type != RBS_METHOD_TYPE) return;
    rbs_method_type_t *mt = (rbs_method_type_t *) ov->method_type;
    /* type_params on the method (generics) → can't be represented;
     * skip the signature rather than emit a misleading one. */
    if (mt->type_params != NULL && mt->type_params->length > 0) return;
    if (mt->type == NULL || mt->type->type != RBS_TYPES_FUNCTION) {
        /* untyped_function or proc → out of subset. */
        return;
    }
    rbs_types_function_t *fn = (rbs_types_function_t *) mt->type;

    /* One ptype field per parameter of the Ruby def, in the order the
     * analyzer's collect_def_params stores them: required positionals,
     * optional positionals, the rest parameter, trailing positionals,
     * then the keywords, each as `name:type` since a def orders its
     * keywords as it likes and seed_method applies them by name (the
     * required and the optional ones together, in the signature's source
     * order). A `**rest` is the analyzer's last slot and
     * gets no field. A field is empty where the seed has nothing to say:
     * the rest parameter (the analyzer types it a poly array by
     * construction), and a parameter whose type is outside the subset.
     * seed_method reads an empty field as "leave this one alone" and
     * keeps the index aligned, so one such parameter no longer drops the
     * whole signature (every other parameter was losing its seed with it).
     *
     * NOTE: rbs_hash_t.length is not incremented by rbs_hash_set
     * (upstream rbs/ast.c#rbs_hash_set inserts into head/tail without
     * updating length). Detect emptiness via `head == NULL`, not via
     * `length`. */
    sbuf_t ptypes;
    sbuf_init(&ptypes);
    bool first_p = true;
    bool any_type = false;
    const rbs_node_list_t *pos_lists[] = { fn->required_positionals, fn->optional_positionals, NULL, fn->trailing_positionals };
    for (size_t li = 0; li < sizeof(pos_lists)/sizeof(pos_lists[0]); li++) {
        if (li == 2) {   /* the rest parameter's slot */
            if (fn->rest_positionals == NULL) continue;
            if (!first_p) sbuf_append_cstr(&ptypes, ",");
            first_p = false;
            continue;
        }
        const rbs_node_list_t *list = pos_lists[li];
        if (list == NULL) continue;
        rbs_node_list_node_t *cur = list->head;
        while (cur != NULL) {
            if (cur->node->type != RBS_TYPES_FUNCTION_PARAM) { sbuf_free(&ptypes); return; }
            rbs_types_function_param_t *pp = (rbs_types_function_param_t *) cur->node;
            sbuf_t t;
            sbuf_init(&t);
            if (!map_type(p, pp->type, enclosing_scope, &t)) sbuf_set(&t, "", 0);
            if (t.len > 0) any_type = true;
            if (!first_p) sbuf_append_cstr(&ptypes, ",");
            sbuf_append(&ptypes, t.buf, t.len);
            first_p = false;
            sbuf_free(&t);
            cur = cur->next;
        }
    }
    /* The keywords. Ruby matches a keyword argument by name, and a def
     * lists its keywords in any order (`def m(b:, a: 1)`), so no order of
     * the fields can stand for the def's: a keyword field carries its
     * name, `a:int`, and seed_method applies it to the parameter of that
     * name. The fields are written in the order the signature writes the
     * keywords (rbs keeps the required and the optional ones in two
     * hashes; the key's byte offset in the source restores the order), so
     * the seed file reads as the signature does. */
    const rbs_hash_t *kw_hashes[] = { fn->required_keywords, fn->optional_keywords };
    rbs_hash_node_t *kws[64];
    size_t nkw = 0;
    for (size_t ki = 0; ki < sizeof(kw_hashes)/sizeof(kw_hashes[0]); ki++) {
        const rbs_hash_t *hash = kw_hashes[ki];
        if (hash == NULL) continue;
        for (rbs_hash_node_t *cur = hash->head; cur != NULL; cur = cur->next) {
            /* Defensive: an upstream rbs change adding a different
             * value-node type would land here; the signature is dropped
             * rather than misaligned. So is one with more keywords than
             * the table holds. */
            if (cur->value == NULL || cur->value->type != RBS_TYPES_FUNCTION_PARAM
                || cur->key == NULL || nkw == sizeof(kws)/sizeof(kws[0])) {
                sbuf_free(&ptypes); return;
            }
            /* insertion sort by source offset: a few keywords at most */
            size_t at = nkw;
            while (at > 0 && kws[at - 1]->key->location.start_byte > cur->key->location.start_byte) {
                kws[at] = kws[at - 1];
                at--;
            }
            kws[at] = cur;
            nkw++;
        }
    }
    for (size_t i = 0; i < nkw; i++) {
        rbs_types_function_param_t *pp = (rbs_types_function_param_t *) kws[i]->value;
        sbuf_t t;
        sbuf_init(&t);
        if (!map_type(p, pp->type, enclosing_scope, &t)) sbuf_set(&t, "", 0);
        if (t.len > 0) any_type = true;
        sbuf_t kname;
        sbuf_init(&kname);
        name_of_symbol(p, (rbs_ast_symbol_t *) kws[i]->key, &kname);
        if (!first_p) sbuf_append_cstr(&ptypes, ",");
        sbuf_append(&ptypes, kname.buf, kname.len);
        sbuf_append_cstr(&ptypes, ":");
        sbuf_append(&ptypes, t.buf, t.len);
        first_p = false;
        sbuf_free(&kname);
        sbuf_free(&t);
    }

    /* No field with a type (no parameters, or none the seed can type)
     * reads as the one field "-": nothing to pin. */
    if (!any_type) sbuf_set(&ptypes, "-", 1);

    /* The return. `instance` in a class is an instance of that class, and
     * so is `self` in an instance method; `self` in a singleton method
     * (`def self.make: () -> self`) is the class object, which the seed
     * spells `class` as it does `singleton(K)`. A `def self?.x` is both an
     * instance and a singleton method, so its `self` is two types and
     * pins nothing. In a module `self` and `instance` are the including
     * object, which the seed cannot name. Any other type outside the
     * subset pins nothing ("-", as void does) and leaves the parameters'
     * seeds in place: a tuple or a record return used to drop the whole
     * signature. */
    sbuf_t ret;
    sbuf_init(&ret);
    rbs_node_t *rt = fn->return_type;
    bool self_in_class = rt != NULL && g_self_scope_is_class && g_self_scope != NULL && g_self_scope[0] != '\0';
    if (self_in_class && (rt->type == RBS_TYPES_BASES_INSTANCE
                          || (rt->type == RBS_TYPES_BASES_SELF && m->kind == RBS_METHOD_DEFINITION_KIND_INSTANCE))) {
        sbuf_set(&ret, "obj_", 4);
        sbuf_append_cstr(&ret, g_self_scope);
    }
    else if (self_in_class && rt->type == RBS_TYPES_BASES_SELF && m->kind == RBS_METHOD_DEFINITION_KIND_SINGLETON) {
        sbuf_set(&ret, "class", 5);
    }
    else if (!map_type(p, rt, enclosing_scope, &ret)) {
        sbuf_set(&ret, "-", 1);
    }

    sbuf_t mname;
    sbuf_init(&mname);
    name_of_symbol(p, m->name, &mname);

    bool emit_inst = method_kind_has(m, 'i') && !overloaded_has(scope, 'i', mname.buf);
    bool emit_cls  = method_kind_has(m, 'c') && !overloaded_has(scope, 'c', mname.buf);
    if (!emit_inst && !emit_cls) { sbuf_free(&mname); sbuf_free(&ret); sbuf_free(&ptypes); return; }

    emit_position(out, m->base.location.start_byte);
    if (emit_inst) fprintf(out, "meth %s %s %s\n",  mname.buf, ret.buf, ptypes.buf);
    if (emit_cls)  fprintf(out, "cmeth %s %s %s\n", mname.buf, ret.buf, ptypes.buf);

    sbuf_free(&mname);
    sbuf_free(&ret);
    sbuf_free(&ptypes);
}

/* ---- emit attr_accessor / reader / writer ------------------ */

static void emit_attr(rbs_parser_t *p, rbs_ast_symbol_t *name, rbs_node_t *type,
                      const char *enclosing_scope, FILE *out) {
    sbuf_t t;
    sbuf_init(&t);
    if (!map_type(p, type, enclosing_scope, &t)) { sbuf_free(&t); return; }
    sbuf_t n;
    sbuf_init(&n);
    name_of_symbol(p, name, &n);
    emit_position(out, name ? name->base.location.start_byte : 0);
    fprintf(out, "ivar %s %s\n", n.buf, t.buf);
    sbuf_free(&n);
    sbuf_free(&t);
}

/* An attribute's seed is for the instance variable it declares: none for
 * `attr_reader self.x` (the class object's, which the seed format cannot
 * name) or `attr_reader x (): T` (no variable), @y for `x (@y): T`, and
 * @x otherwise. */
static void emit_attr_member(rbs_parser_t *p, rbs_ast_symbol_t *name, rbs_node_t *type,
                             enum rbs_attribute_kind kind, rbs_attr_ivar_name_t ivar,
                             const char *enclosing_scope, FILE *out) {
    if (kind != RBS_ATTRIBUTE_KIND_INSTANCE) return;
    if (ivar.tag == RBS_ATTR_IVAR_NAME_TAG_EMPTY) return;
    if (ivar.tag == RBS_ATTR_IVAR_NAME_TAG_NAME) {
        rbs_constant_t *cn = rbs_constant_pool_id_to_constant(&p->constant_pool, ivar.name);
        if (cn == NULL) return;
        sbuf_t t;
        sbuf_init(&t);
        if (!map_type(p, type, enclosing_scope, &t)) { sbuf_free(&t); return; }
        emit_position(out, name ? name->base.location.start_byte : 0);
        fprintf(out, "ivar %.*s %s\n", (int) cn->length, (const char *) cn->start, t.buf);
        sbuf_free(&t);
        return;
    }
    emit_attr(p, name, type, enclosing_scope, out);
}

/* ---- traverse class/module body ---------------------------- */

/* `qualified_scope` is this declaration's full path (used as the
 * `class X` line). `lookup_scope` is the *parent* path (used to
 * resolve unqualified type references inside the members). For
 * `module ActiveRecord; class RecordInvalid; def record: () -> Base`:
 * qualified_scope = "ActiveRecord_RecordInvalid",
 * lookup_scope    = "ActiveRecord".
 * That makes `Base` resolve to `obj_ActiveRecord_Base` -- the sibling-
 * in-module pattern, which dominates real RBS in practice. Ruby's
 * actual constant lookup walks every enclosing scope outward; this
 * single-level fallback covers the common case without a symbol
 * table. */
static void traverse_members(rbs_parser_t *p, rbs_node_list_t *members,
                             const char *qualified_scope, bool is_class,
                             const char *lookup_scope, FILE *out) {
    if (members == NULL || members->length == 0) return;
    fprintf(out, "class %s\n", qualified_scope);
    const char *sv_self_scope = g_self_scope;
    bool sv_is_class = g_self_scope_is_class;
    g_self_scope = qualified_scope;
    g_self_scope_is_class = is_class;
    const char *sv_self_type = g_self_type;
    rbs_node_list_node_t *cur = members->head;
    while (cur != NULL) {
        rbs_node_t *n = cur->node;
        /* an attribute's or instance variable's `self` is an instance of a class */
        g_self_type = is_class ? qualified_scope : NULL;
        switch (n->type) {
            case RBS_AST_MEMBERS_METHOD_DEFINITION: {
                rbs_ast_members_method_definition_t *m = (rbs_ast_members_method_definition_t *) n;
                g_self_type = is_class && m->kind == RBS_METHOD_DEFINITION_KIND_INSTANCE ? qualified_scope : NULL;
                emit_method(p, m, qualified_scope, lookup_scope, out);
                break;
            }
            case RBS_AST_MEMBERS_ATTR_ACCESSOR: {
                rbs_ast_members_attr_accessor_t *a = (rbs_ast_members_attr_accessor_t *) n;
                emit_attr_member(p, a->name, a->type, a->kind, a->ivar_name, lookup_scope, out);
                break;
            }
            case RBS_AST_MEMBERS_ATTR_READER: {
                rbs_ast_members_attr_reader_t *a = (rbs_ast_members_attr_reader_t *) n;
                emit_attr_member(p, a->name, a->type, a->kind, a->ivar_name, lookup_scope, out);
                break;
            }
            case RBS_AST_MEMBERS_ATTR_WRITER: {
                rbs_ast_members_attr_writer_t *a = (rbs_ast_members_attr_writer_t *) n;
                emit_attr_member(p, a->name, a->type, a->kind, a->ivar_name, lookup_scope, out);
                break;
            }
            case RBS_AST_MEMBERS_INSTANCE_VARIABLE: {
                /* `@name: Type` declarations. seed_class_ivar in
                 * analyze prepends "@" if missing; emit_attr writes
                 * the name as-is (which in RBS includes the leading
                 * @), so the seed line ends up with "@@name" if we
                 * don't strip first. The seed_class_ivar handles
                 * either form, but normalize here so the seed file
                 * matches the attr_* shape (single `@`). */
                rbs_ast_members_instance_variable_t *iv = (rbs_ast_members_instance_variable_t *) n;
                emit_attr(p, iv->name, iv->type, lookup_scope, out);
                break;
            }
            default:
                /* Skip: include / extend / prepend / public / private
                 * / alias / class_variable. */
                break;
        }
        cur = cur->next;
    }
    g_self_scope = sv_self_scope;
    g_self_scope_is_class = sv_is_class;
    g_self_type = sv_self_type;
}

/* Pre-pass: collect every Class/Module declaration's qualified name
 * (with "_" separators) into known_names. Recurses through members so
 * `module A; module B; class C` registers all of A, A_B, A_B_C. The
 * later emit pass uses the set to disambiguate unqualified type
 * references (issue #656). */
static void collect_decl_names(rbs_parser_t *p, rbs_node_t *node,
                                const char *parent_scope) {
    sbuf_t leaf;
    sbuf_init(&leaf);
    rbs_node_list_t *members = NULL;

    if (node->type == RBS_AST_DECLARATIONS_CLASS) {
        rbs_ast_declarations_class_t *c = (rbs_ast_declarations_class_t *) node;
        name_of_type_name(p, c->name, &leaf);
        members = c->members;
    }
else if (node->type == RBS_AST_DECLARATIONS_MODULE) {
        rbs_ast_declarations_module_t *m = (rbs_ast_declarations_module_t *) node;
        name_of_type_name(p, m->name, &leaf);
        members = m->members;
    }
else {
        sbuf_free(&leaf);
        return;
    }

    sbuf_t qualified;
    sbuf_init(&qualified);
    if (parent_scope != NULL && parent_scope[0] != '\0') {
        sbuf_append_cstr(&qualified, parent_scope);
        sbuf_append_cstr(&qualified, "_");
    }
    sbuf_append(&qualified, leaf.buf, leaf.len);
    known_names_add(qualified.buf, qualified.len);
    collect_overloaded(p, members, qualified.buf);

    if (members != NULL) {
        rbs_node_list_node_t *cur = members->head;
        while (cur != NULL) {
            if (cur->node->type == RBS_AST_DECLARATIONS_CLASS
                || cur->node->type == RBS_AST_DECLARATIONS_MODULE) {
                collect_decl_names(p, cur->node, qualified.buf);
            }
            cur = cur->next;
        }
    }

    sbuf_free(&qualified);
    sbuf_free(&leaf);
}

/* Pre-passes over the `type name = T` declarations, at the top level or
 * inside a class or module. The names pass declares each under its
 * qualified name, so a reference resolves to the nearest declaration
 * whatever the order of the files. The alias pass maps T, as a member of
 * the declaring scope is mapped (the scope is g_self_scope, its parent
 * the enclosing scope), and registers the tag. An alias whose type is
 * out of the subset registers nothing; one that names an alias with no
 * tag yet (declared later, or in a file read later) registers nothing on
 * this round, so a fallback on the way (`later | nil` to poly) is not
 * kept, and the caller runs the pass again while a round registers
 * something. An alias in a cycle therefore never registers, in any
 * order, and a reference to it is out of the subset. A generic alias
 * (`type listed[T] = Array[T]`) stays out of the subset too. */
static void collect_aliases(rbs_parser_t *p, rbs_node_t *node,
                            const char *scope, const char *outer_scope, bool declare) {
    if (node->type == RBS_AST_DECLARATIONS_TYPE_ALIAS) {
        rbs_ast_declarations_type_alias_t *a = (rbs_ast_declarations_type_alias_t *) node;
        if (a->type_params != NULL && a->type_params->length > 0) return;
        sbuf_t leaf, qualified, tag;
        sbuf_init(&leaf);
        sbuf_init(&qualified);
        sbuf_init(&tag);
        name_of_type_name(p, a->name, &leaf);
        if (scope != NULL && scope[0] != '\0') {
            sbuf_append_cstr(&qualified, scope);
            sbuf_append_cstr(&qualified, "_");
        }
        sbuf_append(&qualified, leaf.buf, leaf.len);
        if (declare) {
            alias_declare(qualified.buf);
        }
else if (alias_find(qualified.buf) == NULL) {
            const char *sv_self_scope = g_self_scope;
            g_self_scope = (scope != NULL && scope[0] != '\0') ? scope : NULL;
            g_alias_unresolved = false;
            bool ok = map_type(p, a->type, outer_scope, &tag);
            if (ok && !g_alias_unresolved) alias_add(qualified.buf, tag.buf);
            g_alias_unresolved = false;
            g_self_scope = sv_self_scope;
        }
        sbuf_free(&tag);
        sbuf_free(&qualified);
        sbuf_free(&leaf);
        return;
    }
    sbuf_t leaf;
    sbuf_init(&leaf);
    rbs_node_list_t *members = NULL;
    if (node->type == RBS_AST_DECLARATIONS_CLASS) {
        rbs_ast_declarations_class_t *c = (rbs_ast_declarations_class_t *) node;
        name_of_type_name(p, c->name, &leaf);
        members = c->members;
    }
else if (node->type == RBS_AST_DECLARATIONS_MODULE) {
        rbs_ast_declarations_module_t *m = (rbs_ast_declarations_module_t *) node;
        name_of_type_name(p, m->name, &leaf);
        members = m->members;
    }
else {
        sbuf_free(&leaf);
        return;
    }
    sbuf_t qualified;
    sbuf_init(&qualified);
    if (scope != NULL && scope[0] != '\0') {
        sbuf_append_cstr(&qualified, scope);
        sbuf_append_cstr(&qualified, "_");
    }
    sbuf_append(&qualified, leaf.buf, leaf.len);
    if (members != NULL) {
        for (rbs_node_list_node_t *cur = members->head; cur != NULL; cur = cur->next) {
            collect_aliases(p, cur->node, qualified.buf, scope, declare);
        }
    }
    sbuf_free(&qualified);
    sbuf_free(&leaf);
}

/* Recurse into a declaration. For Class/Module: extend the scope path
 * with the declaration's leaf name, emit members, then recurse into
 * any nested Class/Module members so e.g. `module Foo; class Bar; end;
 * end` produces a `class Foo::Bar` block. */
static void traverse_decl(rbs_parser_t *p, rbs_node_t *node,
                          const char *parent_scope, FILE *out) {
    sbuf_t leaf;
    sbuf_init(&leaf);
    rbs_node_list_t *members = NULL;

    if (node->type == RBS_AST_DECLARATIONS_CLASS) {
        rbs_ast_declarations_class_t *c = (rbs_ast_declarations_class_t *) node;
        name_of_type_name(p, c->name, &leaf);
        members = c->members;
    }
else if (node->type == RBS_AST_DECLARATIONS_MODULE) {
        rbs_ast_declarations_module_t *m = (rbs_ast_declarations_module_t *) node;
        name_of_type_name(p, m->name, &leaf);
        members = m->members;
    }
else {
        /* Skip Interface, Constant, Global, TypeAlias, ClassAlias,
         * ModuleAlias, Directives. */
        sbuf_free(&leaf);
        return;
    }

    /* qualified = parent_scope (if any) "_" + leaf -- spinel's
     * nested-class storage uses underscore as separator. */
    sbuf_t qualified;
    sbuf_init(&qualified);
    if (parent_scope != NULL && parent_scope[0] != '\0') {
        sbuf_append_cstr(&qualified, parent_scope);
        sbuf_append_cstr(&qualified, "_");
    }
    sbuf_append(&qualified, leaf.buf, leaf.len);

    traverse_members(p, members, qualified.buf, node->type == RBS_AST_DECLARATIONS_CLASS, parent_scope, out);

    /* Recurse into nested declarations. */
    if (members != NULL) {
        rbs_node_list_node_t *cur = members->head;
        while (cur != NULL) {
            if (cur->node->type == RBS_AST_DECLARATIONS_CLASS
                || cur->node->type == RBS_AST_DECLARATIONS_MODULE) {
                traverse_decl(p, cur->node, qualified.buf, out);
            }
            cur = cur->next;
        }
    }

    sbuf_free(&qualified);
    sbuf_free(&leaf);
}

/* ---- per-file processing ----------------------------------- */

/* Read a file fully into a malloc'd buffer. Returns NULL on failure;
 * sets *len_out to the byte count on success. */
static char *slurp(const char *path, size_t *len_out) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *buf = (char *) malloc((size_t) sz + 1);
    if (buf == NULL) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t) sz, fp);
    fclose(fp);
    buf[got] = '\0';
    *len_out = got;
    return buf;
}

/* Per-pass selector for process_file. Splitting the per-file work
 * into phases (collect names; collect aliases; emit decls) lets walk()
 * run several times over the directory tree so cross-file module
 * reopens have every class registered before any unqualified-Const
 * reference is resolved (issue #658), and every alias before a
 * signature names one. */
typedef enum {
    PHASE_COLLECT_NAMES = 0,
    PHASE_COLLECT_ALIASES = 1,
    PHASE_EMIT = 2
} process_phase_t;

static void process_file_phase(const char *path, FILE *out, process_phase_t phase) {
    size_t len = 0;
    char *src = slurp(path, &len);
    if (src == NULL) {
        fprintf(stderr, "spinel_rbs_extract: cannot read %s\n", path);
        return;
    }
    rbs_string_t str = rbs_string_new(src, src + len);
    rbs_parser_t *p = rbs_parser_new(str, RBS_ENCODING_UTF_8_ENTRY, 0, (int) len);
    if (p == NULL) { free(src); return; }
    rbs_signature_t *sig = NULL;
    bool ok = rbs_parse_signature(p, &sig);
    if (!ok || sig == NULL) {
        if (phase == PHASE_EMIT) {
            fprintf(stderr, "spinel_rbs_extract: parse failed in %s\n", path);
            if (p->error != NULL && p->error->message != NULL) {
                fprintf(stderr, "  %s\n", p->error->message);
            }
        }
        rbs_parser_free(p);
        free(src);
        return;
    }
    g_cur_path = path;
    if (g_positions && phase == PHASE_EMIT) index_positions(src, len);
    if (phase == PHASE_COLLECT_NAMES) {
        rbs_node_list_node_t *pre = sig->declarations->head;
        while (pre != NULL) {
            collect_decl_names(p, pre->node, "");
            collect_aliases(p, pre->node, "", "", true);
            pre = pre->next;
        }
    }
else if (phase == PHASE_COLLECT_ALIASES) {
        rbs_node_list_node_t *pre = sig->declarations->head;
        while (pre != NULL) {
            collect_aliases(p, pre->node, "", "", false);
            pre = pre->next;
        }
    }
else {
        rbs_node_list_node_t *cur = sig->declarations->head;
        while (cur != NULL) {
            traverse_decl(p, cur->node, "", out);
            cur = cur->next;
        }
    }
    g_cur_path = NULL;
    free(g_cur_newlines);
    g_cur_newlines = NULL;
    g_cur_newlines_len = 0;
    rbs_parser_free(p);
    free(src);
}

static void process_file(const char *path, FILE *out) {
    process_file_phase(path, out, PHASE_EMIT);
}

/* ---- directory walk ---------------------------------------- */

static bool ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s), su = strlen(suffix);
    if (su > sl) return false;
    return strcmp(s + sl - su, suffix) == 0;
}

/* The directories on the current path, by identity. stat() follows symlinks,
 * so a link that points at an ancestor resolves to that ancestor's dev/ino and
 * the descent stops there instead of running down the cycle until the path
 * buffer or the kernel's symlink limit ends it -- which is what printed a
 * thirty-levels-deep path and called it `not found` (#4159). Only ANCESTORS
 * are compared, as find -L does: a directory legitimately reachable by two
 * different paths is still visited under each, since neither is inside the
 * other. The chain lives on the C stack, so the walk allocates nothing. */
typedef struct dir_chain_s {
    dev_t dev;
    ino_t ino;
    const struct dir_chain_s *up;
} dir_chain;

static void walk_phase_in(const char *path, FILE *out, process_phase_t phase,
                          const dir_chain *up) {
    struct stat st;
    if (stat(path, &st) != 0) {
        if (phase == PHASE_EMIT) {
            /* say what the filesystem answered: `not found` was printed for
               every reason stat can fail, including the cycle above */
            fprintf(stderr, "spinel_rbs_extract: %s: %s\n", path, strerror(errno));
        }
        return;
    }
    if (S_ISREG(st.st_mode)) {
        if (ends_with(path, ".rbs")) process_file_phase(path, out, phase);
        return;
    }
    if (!S_ISDIR(st.st_mode)) return;
    for (const dir_chain *p = up; p != NULL; p = p->up) {
        /* already on the way in: a symlink closed a loop. Not an error --
           publishing a tree with such a link is ordinary (tree-sitter ships
           lib/src/unicode/unicode -> .) -- so stop quietly. */
        if (p->dev == st.st_dev && p->ino == st.st_ino) return;
    }
    dir_chain here = { st.st_dev, st.st_ino, up };
    DIR *d = opendir(path);
    if (d == NULL) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[4096];
        int n = snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        if (n < 0 || (size_t) n >= sizeof(full)) {
            if (phase == PHASE_EMIT) {
                fprintf(stderr, "spinel_rbs_extract: path too long, skipping: %s/%s\n",
                        path, ent->d_name);
            }
            continue;
        }
        walk_phase_in(full, out, phase, &here);
    }
    closedir(d);
}

static void walk_phase(const char *path, FILE *out, process_phase_t phase) {
    walk_phase_in(path, out, phase, NULL);
}

static void walk(const char *path, FILE *out) { walk_phase(path, out, PHASE_EMIT); }

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--positions") == 0) {
        g_positions = 1;
        argv++;
        argc--;
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: spinel_rbs_extract [--positions] DIR [DIR ...]\n");
        return 1;
    }
    /* Two-pass walk: first phase collects every Class / Module's
     * qualified name across all .rbs files into known_names; second
     * phase emits decls. Splitting the pre-pass from the emit pass
     * lets a bare `Bar` inside `baz.rbs`'s reopen of `module Foo`
     * see `Foo::Bar` from a sibling `bar.rbs` regardless of which
     * file readdir visited first. Issue #658 (followup to #656). */
    for (int i = 1; i < argc; i++) {
        walk_phase(argv[i], stdout, PHASE_COLLECT_NAMES);
    }
    /* The aliases, to a fixpoint: an alias whose type names another alias
     * registers once that one has, whatever the order of the files, and a
     * round that registers nothing ends it (the tags only grow, and the
     * declarations bound them, so it ends). What still has no tag then (a
     * cycle, a type out of the subset) stays out of it. */
    for (;;) {
        size_t before = alias_count();
        for (int i = 1; i < argc; i++) {
            walk_phase(argv[i], stdout, PHASE_COLLECT_ALIASES);
        }
        if (alias_count() == before) break;
    }
    for (int i = 1; i < argc; i++) {
        walk_phase(argv[i], stdout, PHASE_EMIT);
    }
    return 0;
}
