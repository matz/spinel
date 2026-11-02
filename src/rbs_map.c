/* rbs_map.c -- RBS types to Spinel's seed type tags (see rbs_map.h).
 * Moved out of tools/spinel_rbs_extract.c unchanged, so the compiler can map
 * inline RBS annotations exactly as the extractor maps .rbs signatures. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rbs_map.h"
#include "rbs/util/rbs_constant_pool.h"

/* ---- small string buffer ----------------------------------- */


void sbuf_init(sbuf_t *s) { s->buf = NULL; s->len = 0; s->cap = 0; }
void sbuf_free(sbuf_t *s) { free(s->buf); s->buf = NULL; s->len = 0; s->cap = 0; }

void sbuf_grow(sbuf_t *s, size_t need) {
    if (s->cap >= need) return;
    size_t ncap = s->cap == 0 ? 64 : s->cap;
    while (ncap < need) ncap *= 2;
    char *nbuf = (char *) realloc(s->buf, ncap);
    if (nbuf == NULL) {
        fprintf(stderr, "spinel_rbs_extract: out of memory\n");
        exit(1);
    }
    s->buf = nbuf;
    s->cap = ncap;
}

void sbuf_append(sbuf_t *s, const char *src, size_t n) {
    sbuf_grow(s, s->len + n + 1);
    memcpy(s->buf + s->len, src, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

void sbuf_append_cstr(sbuf_t *s, const char *src) {
    sbuf_append(s, src, strlen(src));
}

void sbuf_set(sbuf_t *s, const char *src, size_t n) {
    s->len = 0;
    sbuf_append(s, src, n);
}

/* ---- known-name set (issue #656) ---------------------------- */

/* Set of qualified class/module names defined across all .rbs files
 * traversed. Populated by collect_decl_names in a pre-pass before
 * the emit pass; consulted by resolve_unqualified_name so a bare
 * `Article` written inside `module Views; module Articles` walks
 * the lexical chain (Views_Articles_Article, Views_Article,
 * Article) and picks the first match. Without the set we either
 * always prepend the enclosing scope (wrong when the referent is
 * top-level) or never prepend (wrong for the sibling-in-module
 * case). Issue #656. */
static char **known_names = NULL;
static size_t known_names_len = 0;
static size_t known_names_cap = 0;
/* The qualified name of the declaration whose members are being emitted
 * (`Rails` for the members of `module Rails`). An unqualified type name
 * resolves against it first: RBS, like Ruby, looks a constant up in the
 * innermost enclosing namespace, so `Application` inside `module Rails`
 * is `Rails::Application` when that is declared. */
const char *g_self_scope = NULL;
const char *g_self_type = NULL;

void known_names_add(const char *name, size_t n) {
    /* dedupe (linear; n is small in practice) */
    for (size_t i = 0; i < known_names_len; i++) {
        if (strlen(known_names[i]) == n && memcmp(known_names[i], name, n) == 0) return;
    }
    if (known_names_len >= known_names_cap) {
        size_t ncap = known_names_cap == 0 ? 32 : known_names_cap * 2;
        char **nbuf = (char **) realloc(known_names, ncap * sizeof(char *));
        if (nbuf == NULL) {
            fprintf(stderr, "spinel_rbs_extract: out of memory\n");
            exit(1);
        }
        known_names = nbuf;
        known_names_cap = ncap;
    }
    char *copy = (char *) malloc(n + 1);
    if (copy == NULL) {
        fprintf(stderr, "spinel_rbs_extract: out of memory\n");
        exit(1);
    }
    memcpy(copy, name, n);
    copy[n] = '\0';
    known_names[known_names_len++] = copy;
}

bool known_names_has(const char *name) {
    for (size_t i = 0; i < known_names_len; i++) {
        if (strcmp(known_names[i], name) == 0) return true;
    }
    return false;
}

/* ---- name lookup via rbs constant pool --------------------- */

/* Read the bytes for a constant_id out of the parser's pool into a sbuf.
 * Resets the buffer first. Result is NUL-terminated. */
void name_of_symbol(rbs_parser_t *p, rbs_ast_symbol_t *sym, sbuf_t *out) {
    out->len = 0;
    if (sym == NULL) { sbuf_append(out, "", 0); return; }
    rbs_constant_t *c = rbs_constant_pool_id_to_constant(&p->constant_pool, sym->constant_id);
    if (c == NULL) { sbuf_append(out, "", 0); return; }
    sbuf_set(out, (const char *) c->start, c->length);
}

/* Qualified name from a type_name: namespace path joined with "_"
 * plus the leaf symbol. Spinel registers nested classes with "_"
 * separators (e.g. `module Tep; class Json` → `Tep_Json` in
 * @cls_names; `Tep::Security::Cors` → `Tep_Security_Cors`). RBS
 * spells them with `::`, so we translate here. */
void name_of_type_name(rbs_parser_t *p, rbs_type_name_t *tn, sbuf_t *out) {
    out->len = 0;
    if (tn == NULL) return;
    if (tn->rbs_namespace != NULL && tn->rbs_namespace->path != NULL) {
        rbs_node_list_node_t *cur = tn->rbs_namespace->path->head;
        while (cur != NULL) {
            if (cur->node->type == RBS_AST_SYMBOL) {
                rbs_ast_symbol_t *seg = (rbs_ast_symbol_t *) cur->node;
                rbs_constant_t *c = rbs_constant_pool_id_to_constant(&p->constant_pool, seg->constant_id);
                if (c != NULL) {
                    sbuf_append(out, (const char *) c->start, c->length);
                    sbuf_append_cstr(out, "_");
                }
            }
            cur = cur->next;
        }
    }
    rbs_constant_t *c = rbs_constant_pool_id_to_constant(&p->constant_pool, tn->name->constant_id);
    if (c != NULL) {
        sbuf_append(out, (const char *) c->start, c->length);
    }
}

/* ---- subset type mapping ----------------------------------- */

/* Categorize a primitive name to a spinel scalar tag. NULL on
 * non-primitive (caller treats as nominal `obj_<Name>`). */
static const char *primitive_tag(const char *name, size_t len) {
    if (len == 7 && memcmp(name, "Integer", 7) == 0) return "int";
    if (len == 5 && memcmp(name, "Float", 5) == 0)   return "float";
    if (len == 6 && memcmp(name, "String", 6) == 0)  return "string";
    if (len == 6 && memcmp(name, "Symbol", 6) == 0)  return "symbol";
    if (len == 9 && memcmp(name, "TrueClass", 9) == 0)  return "bool";
    if (len == 10 && memcmp(name, "FalseClass", 10) == 0) return "bool";
    if (len == 8 && memcmp(name, "NilClass", 8) == 0)   return "nil";
    return NULL;
}

/* Map a typed element (already mapped to a spinel scalar/obj tag) into
 * the array variant name spinel uses. NULL if unsupported. */
static const char *array_tag_for_elem(const char *elem) {
    if (strcmp(elem, "int") == 0)    return "int_array";
    if (strcmp(elem, "float") == 0)  return "float_array";
    if (strcmp(elem, "string") == 0) return "str_array";
    if (strcmp(elem, "symbol") == 0) return "sym_array";
    /* Array[Array[Integer]] / Array[Array[Float]]: map_type has already
     * reduced the element, so the nested case is the element tag being an
     * array tag itself. Without these two the pair fell to poly_array, and
     * because a poly_array seed PINS the ivar it stopped the very pass that
     * produces the unboxed table -- so writing the accurate signature made
     * the program slower, with nothing said. Only these two nest: the other
     * array kinds have no table form to ask for. */
    if (strcmp(elem, "int_array") == 0)   return "int_array_array";
    if (strcmp(elem, "float_array") == 0) return "float_array_array";
    {
        size_t l = strlen(elem);
        int is_obj_arr = l >= 10 && strcmp(elem + l - 10, "_ptr_array") == 0;
        if (strncmp(elem, "obj_", 4) == 0 && !is_obj_arr) {
            /* obj_Foo → obj_Foo_ptr_array (heuristic, mirrors spinel's
             * array-of-objects shape). Caller must append _ptr_array. */
            return NULL;
        }
        /* an array OF object arrays has no table form, so it is an
         * `Array[<other>]` like any other: poly_array, per the documented
         * rule, rather than a dropped signature */
    }
    return "poly_array";
}

/* Map (K, V) of a Hash[K, V] to spinel's hash variant name. NULL on
 * unsupported combinations. */
static const char *hash_tag_for_kv(const char *k, const char *v) {
    if (strcmp(k, "string") == 0) {
        if (strcmp(v, "int") == 0)    return "str_int_hash";
        if (strcmp(v, "string") == 0) return "str_str_hash";
        return "str_poly_hash";
    }
    if (strcmp(k, "symbol") == 0) {
        /* The analyzer has one Symbol-keyed Hash kind, the poly-valued one.
         * `sym_int_hash` and `sym_str_hash` named kinds it does not have, so
         * its parser dropped the seed and the slot fell to poly: a box the
         * analysis does not even take for a Hash (`h.keys` on it resolves
         * to no method). A `Hash[Symbol, Integer]` is a Symbol-keyed Hash
         * whose values are boxed, as `Hash[Symbol, untyped]` is. */
        return "sym_poly_hash";
    }
    if (strcmp(k, "int") == 0) {
        if (strcmp(v, "string") == 0) return "int_str_hash";
    }
    /* `Hash[untyped, untyped]` (and any leftover combination where
     * one side is untyped without a matching concrete variant
     * above) -- map to bare "poly" rather than poly_poly_hash. The
     * analyzer treats this slot as sp_RbVal (tagged union), which
     * matches how a top-level `def f(h)` with no caller signal
     * settles. Picking poly_poly_hash here forced a specific tagged
     * struct pointer (sp_PolyPolyHash *) that doesn't accept the
     * sp_RbVal arguments callers actually pass through poly
     * narrowing branches (e.g. `v.is_a?(Hash) ? f(v) : v`). Issue
     * #613. */
    if (strcmp(k, "poly") == 0 || strcmp(v, "poly") == 0) {
        return "poly";
    }
    return NULL;
}

/* Forward decl. Write a spinel type tag into out; return true on
 * success, false to signal "out of subset, caller should skip".
 *
 * `enclosing_scope` (may be NULL/"") is the qualified name of the
 * declaration we're currently inside, e.g. `ActiveRecord` while
 * traversing members of `module ActiveRecord; class RecordInvalid`.
 * Used to resolve unqualified nominal type references: `Base` inside
 * `ActiveRecord` becomes `obj_ActiveRecord_Base`. Without this, the
 * extractor emits `obj_Base` and the seed misses any class actually
 * stored at the qualified name in spinel's tables. */
/* (declared in rbs_map.h) */

/* True if the type_name was written unqualified in source (no `::`
 * separators, no leading `::`). RBS exposes this as an empty
 * namespace path. */
static bool type_name_is_unqualified(rbs_type_name_t *tn) {
    if (tn == NULL || tn->rbs_namespace == NULL) return true;
    if (tn->rbs_namespace->path == NULL) return true;
    return tn->rbs_namespace->path->length == 0;
}

/* Map a ClassInstance node. Handles Array[T], Hash[K,V], primitives,
 * and nominal classes. */
/* A type argument (`V` of `Hash[K, V]`): as map_type, except that `top`
 * (any value) is the boxed value an element of several types is, as
 * `untyped` is. A slot of its own typed `top` pins nothing instead. */
static bool map_type_arg(rbs_parser_t *p, rbs_node_t *node,
                         const char *enclosing_scope, sbuf_t *out) {
    if (node != NULL && node->type == RBS_TYPES_BASES_TOP) { sbuf_set(out, "poly", 4); return true; }
    return map_type(p, node, enclosing_scope, out);
}

static bool map_class_instance(rbs_parser_t *p, rbs_types_class_instance_t *ci,
                                const char *enclosing_scope, sbuf_t *out) {
    sbuf_t name;
    sbuf_init(&name);
    name_of_type_name(p, ci->name, &name);

    /* Primitive lookup uses the leaf name only. After name_of_type_name
     * the separator is "_" (translated from RBS's "::"), so the leaf
     * is the substring after the last "_". Conservative: only treat
     * the trailing segment as the primitive name; a class actually
     * named `Foo_Integer` would shadow the primitive, but that's
     * extremely unlikely in practice. */
    const char *primitive = NULL;
    {
        const char *leaf = name.buf;
        size_t leaf_len = name.len;
        for (size_t i = 0; i < name.len; i++) {
            if (name.buf[i] == '_') {
                leaf = name.buf + i + 1;
                leaf_len = name.len - (i + 1);
            }
        }
        primitive = primitive_tag(leaf, leaf_len);
    }

    size_t argc = ci->args != NULL ? ci->args->length : 0;

    /* `Object` and `BasicObject` take any value, as `untyped` does: a
     * program that reopens `class Object` made the name a class of its
     * own, and the seed pinned the slot to that object, which a Float or
     * a String cannot be stored in (#8407). */
    if (argc == 0 && (strcmp(name.buf, "Object") == 0 || strcmp(name.buf, "BasicObject") == 0)) {
        sbuf_set(out, "poly", 4);
        sbuf_free(&name);
        return true;
    }

    if (primitive != NULL && argc == 0) {
        sbuf_set(out, primitive, strlen(primitive));
        sbuf_free(&name);
        return true;
    }

    /* Array[T] */
    if (name.len >= 5 && strcmp(name.buf + name.len - 5, "Array") == 0 && argc == 1) {
        sbuf_t elem;
        sbuf_init(&elem);
        if (!map_type_arg(p, ci->args->head->node, enclosing_scope, &elem)) {
            sbuf_free(&elem);
            sbuf_free(&name);
            return false;
        }
        /* `Array[Foo]` is obj_Foo_ptr_array. `Array[Array[Foo]]` is NOT
         * obj_Foo_ptr_array_ptr_array -- there is no table of object arrays,
         * and that tag named nothing any consumer accepts, so it was a seed
         * that could only ever be dropped. Nest only where a table exists. */
        if (strncmp(elem.buf, "obj_", 4) == 0
            && !(elem.len >= 10 && strcmp(elem.buf + elem.len - 10, "_ptr_array") == 0)) {
            sbuf_set(out, elem.buf, elem.len);
            sbuf_append_cstr(out, "_ptr_array");
        }
else {
            const char *tag = array_tag_for_elem(elem.buf);
            if (tag == NULL) {
                sbuf_free(&elem);
                sbuf_free(&name);
                return false;
            }
            sbuf_set(out, tag, strlen(tag));
        }
        sbuf_free(&elem);
        sbuf_free(&name);
        return true;
    }

    /* Hash[K, V] */
    if (name.len >= 4 && strcmp(name.buf + name.len - 4, "Hash") == 0 && argc == 2) {
        sbuf_t k, v;
        sbuf_init(&k); sbuf_init(&v);
        if (!map_type_arg(p, ci->args->head->node, enclosing_scope, &k)
            || !map_type_arg(p, ci->args->head->next->node, enclosing_scope, &v)) {
            sbuf_free(&k); sbuf_free(&v); sbuf_free(&name); return false;
        }
        const char *tag = hash_tag_for_kv(k.buf, v.buf);
        sbuf_free(&k); sbuf_free(&v);
        if (tag == NULL) { sbuf_free(&name); return false; }
        sbuf_set(out, tag, strlen(tag));
        sbuf_free(&name);
        return true;
    }

    /* Generic with arity > 0 of an unrecognized container → skip. */
    if (argc > 0) {
        sbuf_free(&name);
        return false;
    }

    /* Nominal class instance: emit obj_<QualifiedName>. Three cases:
     *
     * 1. Source wrote `::Const` (absolute): use the bare name as-is,
     *    never prepend the enclosing scope (issue #656).
     * 2. Source wrote `Const` or a relative path `A::B` inside a class/
     *    module scope: walk the lexical chain from innermost outward
     *    and pick the first qualified name present in known_names.
     *    Fall back to the name as written when no candidate matches
     *    (matches Ruby's top-level resolution).
     * 3. Outside any scope: use the name as written.
     */
    sbuf_set(out, "obj_", 4);
    bool absolute = (ci->name != NULL && ci->name->rbs_namespace != NULL
                     && ci->name->rbs_namespace->absolute);
    /* The declaration's own namespace first: `Application` inside
     * `module Rails` is `Rails_Application` when that is declared, even
     * with a top-level `Application` in scope. */
    bool self_resolved = false;
    if (!absolute && g_self_scope != NULL && g_self_scope[0] != '\0'
        && type_name_is_unqualified(ci->name)) {
        sbuf_t own;
        sbuf_init(&own);
        sbuf_append_cstr(&own, g_self_scope);
        sbuf_append_cstr(&own, "_");
        sbuf_append(&own, name.buf, name.len);
        if (known_names_has(own.buf)) {
            sbuf_append(out, own.buf, own.len);
            self_resolved = true;
        }
        sbuf_free(&own);
    }
    if (self_resolved) {
        /* resolved against the declaration's own namespace */
    }
else if (!absolute && enclosing_scope != NULL && enclosing_scope[0] != '\0') {
        /* Walk the lexical chain, innermost outward: the enclosing scope,
         * then each scope that contains it, then the top level. A
         * qualified name walks it too -- `Mid::Leaf` inside `module
         * Outer` is `Outer::Mid::Leaf` -- as Ruby resolves the first
         * segment of a constant path. `Views_Articles_Helpers` tries
         * `Views_Articles_Helpers_<name>`, `Views_Articles_<name>`,
         * `Views_<name>`, then `<name>`. */
        sbuf_t candidate;
        sbuf_init(&candidate);
        size_t plen = strlen(enclosing_scope);
        bool resolved = false;
        while (plen > 0) {
            candidate.len = 0;
            sbuf_append(&candidate, enclosing_scope, plen);
            sbuf_append_cstr(&candidate, "_");
            sbuf_append(&candidate, name.buf, name.len);
            if (known_names_has(candidate.buf)) {
                sbuf_append(out, candidate.buf, candidate.len);
                resolved = true;
                break;
            }
            /* drop the innermost segment */
            while (plen > 0 && enclosing_scope[plen - 1] != '_') plen--;
            if (plen > 0) plen--;
        }
        if (!resolved) {
            /* Top-level fallback: emit the name as written. */
            sbuf_append(out, name.buf, name.len);
        }
        sbuf_free(&candidate);
    }
else {
        sbuf_append(out, name.buf, name.len);
    }
    sbuf_free(&name);
    return true;
}

/* ---- type aliases ------------------------------------------ */

/* `type patch_t = Array[untyped]`, by qualified name, with the tag the
 * aliased type maps to. The extractor declares every alias name first
 * and fills the tags before its emit pass; a reference (`Array[patch_t]`)
 * then maps as the aliased type would. A name resolves among the DECLARED
 * aliases, so a nearer declaration wins over an outer one whatever the
 * order the files are read in; a declared alias whose tag is not known
 * yet is unresolved (g_alias_unresolved), not an outer one of its name.
 * The inline RBS reader declares none, so a reference stays out of the
 * subset there, as before. */
typedef struct { char *name; char *tag; } alias_entry_t;
static alias_entry_t *aliases = NULL;
static size_t aliases_len = 0;
static size_t aliases_cap = 0;
bool g_alias_unresolved = false;

static alias_entry_t *alias_entry(const char *name) {
    for (size_t i = 0; i < aliases_len; i++) {
        if (strcmp(aliases[i].name, name) == 0) return &aliases[i];
    }
    return NULL;
}

void alias_declare(const char *name) {
    if (alias_entry(name) != NULL) return;
    if (aliases_len >= aliases_cap) {
        size_t ncap = aliases_cap == 0 ? 16 : aliases_cap * 2;
        alias_entry_t *nbuf = (alias_entry_t *) realloc(aliases, ncap * sizeof(alias_entry_t));
        if (nbuf == NULL) {
            fprintf(stderr, "spinel_rbs_extract: out of memory\n");
            exit(1);
        }
        aliases = nbuf;
        aliases_cap = ncap;
    }
    aliases[aliases_len].name = strdup(name);
    aliases[aliases_len].tag = NULL;
    if (aliases[aliases_len].name == NULL) {
        fprintf(stderr, "spinel_rbs_extract: out of memory\n");
        exit(1);
    }
    aliases_len++;
}

void alias_add(const char *name, const char *tag) {
    alias_declare(name);
    alias_entry_t *e = alias_entry(name);
    if (e->tag != NULL) return;
    e->tag = strdup(tag);
    if (e->tag == NULL) {
        fprintf(stderr, "spinel_rbs_extract: out of memory\n");
        exit(1);
    }
}

const char *alias_find(const char *name) {
    alias_entry_t *e = alias_entry(name);
    return e != NULL ? e->tag : NULL;
}

size_t alias_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < aliases_len; i++) {
        if (aliases[i].tag != NULL) n++;
    }
    return n;
}

static bool alias_declared(const char *name) {
    return alias_entry(name) != NULL;
}

/* Resolve a type name as a constant is looked up: an absolute one
 * (`::A::B`) as written; else against the declaration's own namespace
 * (`Inner::row` in a body of `Outer` is `Outer::Inner::row`, as its
 * first segment resolves there), then the enclosing scopes innermost
 * outward, then the top level. The first candidate `has` knows wins;
 * false when none does. `name` is the "_"-joined form. */
static bool resolve_known_name(const char *name, size_t len, bool absolute,
                               const char *enclosing_scope, bool (*has)(const char *), sbuf_t *out) {
    if (absolute) {
        sbuf_set(out, name, len);
        return has(out->buf);
    }
    if (g_self_scope != NULL && g_self_scope[0] != '\0') {
        sbuf_set(out, g_self_scope, strlen(g_self_scope));
        sbuf_append_cstr(out, "_");
        sbuf_append(out, name, len);
        if (has(out->buf)) return true;
    }
    if (enclosing_scope != NULL && enclosing_scope[0] != '\0') {
        size_t plen = strlen(enclosing_scope);
        while (plen > 0) {
            sbuf_set(out, enclosing_scope, plen);
            sbuf_append_cstr(out, "_");
            sbuf_append(out, name, len);
            if (has(out->buf)) return true;
            while (plen > 0 && enclosing_scope[plen - 1] != '_') plen--;
            if (plen > 0) plen--;
        }
    }
    sbuf_set(out, name, len);
    return has(out->buf);
}

/* `patch_t` or `Inner::row`: the tag of the alias the name resolves to.
 * A generic alias (`listed[Integer]`) and a name no declaration gives are
 * out of the subset. A declared alias whose tag is not known yet is out
 * of it for now, and says so in g_alias_unresolved: the extractor's
 * collection then leaves the alias that referenced it for a later round
 * rather than registering what a fallback (`T | nil` to poly) gave. */
static bool map_alias(rbs_parser_t *p, rbs_types_alias_t *a, const char *enclosing_scope, sbuf_t *out) {
    if (a->args != NULL && a->args->length > 0) return false;
    sbuf_t name, qualified;
    sbuf_init(&name);
    sbuf_init(&qualified);
    name_of_type_name(p, a->name, &name);
    bool absolute = a->name != NULL && a->name->rbs_namespace != NULL && a->name->rbs_namespace->absolute;
    bool found = resolve_known_name(name.buf, name.len, absolute, enclosing_scope, alias_declared, &qualified);
    if (found) {
        const char *tag = alias_find(qualified.buf);
        if (tag != NULL) sbuf_set(out, tag, strlen(tag));
        else { g_alias_unresolved = true; found = false; }
    }
    sbuf_free(&qualified);
    sbuf_free(&name);
    return found;
}

/* `{ name: String, ?age: Integer }`: a Hash keyed by the field names,
 * which are Symbols (`name:`, `:name =>`) or Strings (`"name" =>`), with
 * values of several types: the poly-valued Hash kind of that key class.
 * Keys of another class, or of both, have no kind of their own: poly, as
 * a union is. */
static bool map_record(rbs_types_record_t *r, sbuf_t *out) {
    bool sym = true, str = true;
    if (r->all_fields != NULL) {
        for (rbs_hash_node_t *f = r->all_fields->head; f != NULL; f = f->next) {
            if (f->key != NULL && f->key->type == RBS_AST_SYMBOL) str = false;
            else if (f->key != NULL && f->key->type == RBS_AST_STRING) sym = false;
            else { sym = false; str = false; }
        }
    }
    if (sym) sbuf_set(out, "sym_poly_hash", 13);
    else if (str) sbuf_set(out, "str_poly_hash", 13);
    else sbuf_set(out, "poly", 4);
    return true;
}

bool map_type(rbs_parser_t *p, rbs_node_t *node,
                     const char *enclosing_scope, sbuf_t *out) {
    if (node == NULL) return false;
    switch (node->type) {
        case RBS_TYPES_ALIAS:
            return map_alias(p, (rbs_types_alias_t *) node, enclosing_scope, out);
        case RBS_TYPES_RECORD:
            return map_record((rbs_types_record_t *) node, out);
        case RBS_TYPES_BASES_BOOL:
            sbuf_set(out, "bool", 4);
            return true;
        case RBS_TYPES_BASES_NIL:
            sbuf_set(out, "nil", 3);
            return true;
        case RBS_TYPES_BASES_VOID:
            /* void only appears as a return type, and says the value is not
             * meant to be used -- not what it is: the method still answers
             * its body's value, and a caller may use it. "-" pins nothing
             * (the analyzer reads no type from it), so inference keeps the
             * return; it was "nil", which emptied that value. */
            sbuf_set(out, "-", 1);
            return true;
        case RBS_TYPES_BASES_ANY:
            /* `untyped` in RBS == anything. Spinel's nearest is poly
             * (sp_RbVal -- tagged union). Hash[untyped, untyped] uses
             * this in hash_tag_for_kv to land on poly_poly_hash. */
            sbuf_set(out, "poly", 4);
            return true;
        case RBS_TYPES_CLASS_INSTANCE:
            return map_class_instance(p, (rbs_types_class_instance_t *) node, enclosing_scope, out);
        case RBS_TYPES_OPTIONAL: {
            rbs_types_optional_t *opt = (rbs_types_optional_t *) node;
            sbuf_t inner;
            sbuf_init(&inner);
            if (!map_type(p, opt->type, enclosing_scope, &inner)) { sbuf_free(&inner); return false; }
            /* Avoid double-?: nil? → just nil; obj_Foo?? → obj_Foo? */
            if (inner.len > 0 && inner.buf[inner.len - 1] == '?') {
                sbuf_set(out, inner.buf, inner.len);
            }
else if (inner.len == 3 && memcmp(inner.buf, "nil", 3) == 0) {
                sbuf_set(out, "nil", 3);
            }
else {
                sbuf_set(out, inner.buf, inner.len);
                sbuf_append_cstr(out, "?");
            }
            sbuf_free(&inner);
            return true;
        }
        case RBS_TYPES_UNION: {
            /* `T | nil` (the nilable shape) maps to `T?`. Every other
             * union has no single concrete C representation, so it
             * maps to poly (sp_RbVal, the tagged union) -- the same
             * target `untyped` uses. This boxes the value correctly.
             *
             * Previously any non-`T | nil` union returned false, which
             * dropped the *entire* method seed (the caller bails on a
             * false return type), letting inference collapse e.g.
             * {int, string} to a single C type and mis-compile
             * heterogeneous returns (matz/spinel#1255). Mapping to poly
             * is the faithful choice: spinel's only representation for
             * "one of several incompatible types" is the tagged union.
             *
             * This touches only the RBS-seed layer. The self-host build
             * ships no `.rbs`, so the inference-level unify_return_type
             * heuristic (load-bearing for stage-2) is unaffected. */
            rbs_types_union_t *u = (rbs_types_union_t *) node;
            /* a union of singleton(...) types is a Class value whatever the
             * member: the `class` token, not poly -- which reads as a union
             * of instances (matz/spinel#5036) */
            if (u->types != NULL && u->types->length > 0) {
                bool all_cls = true;
                for (rbs_node_list_node_t *e = u->types->head; e; e = e->next)
                    if (e->node->type != RBS_TYPES_CLASS_SINGLETON) { all_cls = false; break; }
                if (all_cls) { sbuf_set(out, "class", 5); return true; }
            }
            if (u->types != NULL && u->types->length == 2) {
                rbs_node_t *a = u->types->head->node;
                rbs_node_t *b = u->types->head->next->node;
                rbs_node_t *t = NULL;
                if (a->type == RBS_TYPES_BASES_NIL) t = b;
                else if (b->type == RBS_TYPES_BASES_NIL) t = a;
                if (t != NULL) {
                    sbuf_t inner;
                    sbuf_init(&inner);
                    if (map_type(p, t, enclosing_scope, &inner)) {
                        sbuf_set(out, inner.buf, inner.len);
                        if (inner.len == 0 || inner.buf[inner.len - 1] != '?') {
                            sbuf_append_cstr(out, "?");
                        }
                        sbuf_free(&inner);
                        return true;
                    }
                    /* `T | nil` where T is itself out-of-subset →
                     * fall through to poly below. */
                    sbuf_free(&inner);
                }
            }
            sbuf_set(out, "poly", 4);
            return true;
        }
        case RBS_TYPES_LITERAL: {
            /* `true` and `false` are the two values of bool, as TrueClass
             * and FalseClass are; other literals (`1`, `:a`, `"s"`) name
             * one value, which no slot type stands for. */
            rbs_node_t *lit = ((rbs_types_literal_t *) node)->literal;
            if (lit == NULL || lit->type != RBS_AST_BOOL) return false;
            sbuf_set(out, "bool", 4);
            return true;
        }
        case RBS_TYPES_BASES_SELF:
        case RBS_TYPES_BASES_INSTANCE: {
            /* `self` (or `instance`): an instance of the class the method is written in,
             * as the extractor's `-> self` return is (a reopened builtin's
             * obj_ tag is read as that builtin's kind); in `class Object`
             * any value, as `Object` is. In a top-level builtin with a
             * kind of its own (`class String`) it is that kind, the token
             * the class's name maps to, so `-> self` there agrees with an
             * `-> String` the .rbs writes; a nested `A::String` or a
             * `My_String` keeps its obj_ tag. */
            if (g_self_type == NULL) return false;
            if (strcmp(g_self_type, "Object") == 0) { sbuf_set(out, "poly", 4); return true; }
            const char *prim = strchr(g_self_type, '_') ? NULL : primitive_tag(g_self_type, strlen(g_self_type));
            if (prim != NULL) { sbuf_set(out, prim, strlen(prim)); return true; }
            sbuf_set(out, "obj_", 4);
            sbuf_append_cstr(out, g_self_type);
            return true;
        }
        case RBS_TYPES_CLASS_SINGLETON:
            /* `singleton(X)`: the class itself, a Class value. It used to
             * fall out of the subset, which dropped the whole method's seed,
             * parameters included (matz/spinel#5036). */
            sbuf_set(out, "class", 5);
            return true;
        default:
            /* Out of subset: Self and Instance (but in a class's instance method) / Top / Bottom / Any /
             * Class / Block / Function / Interface / Intersection /
             * Literal (but true and false) / Proc / Tuple / UntypedFunction / Variable. */
            return false;
    }
}
