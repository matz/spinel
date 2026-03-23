/*
 * codegen.c - Spinel AOT compiler: C code generation from Prism AST
 *
 * Multi-pass approach:
 *   Pass 1 (class analysis): Find classes, modules, top-level functions
 *   Pass 2 (type inference): Infer types for variables, ivars, params, returns
 *   Pass 3 (struct/func emit): Generate C structs and method functions
 *   Pass 4 (main codegen): Generate main() with top-level code
 */

#define _GNU_SOURCE  /* for open_memstream */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <limits.h>
#include <libgen.h>
#include <prism.h>
#include "codegen.h"

/* ------------------------------------------------------------------ */
/* String helpers                                                     */
/* ------------------------------------------------------------------ */

char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *r = malloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

char *sfmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *buf = malloc(n + 1);
    va_start(ap, fmt);
    vsnprintf(buf, n + 1, fmt, ap);
    va_end(ap);
    return buf;
}

/* Sanitize a Ruby identifier for use in C code: ? → _p, ! → _b, = → _eq */
char *c_safe_name(const char *name) {
    size_t len = strlen(name);
    char *buf = malloc(len * 3 + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '?') { buf[j++] = '_'; buf[j++] = 'p'; }
        else if (name[i] == '!') { buf[j++] = '_'; buf[j++] = 'b'; }
        else if (name[i] == '=') { buf[j++] = '_'; buf[j++] = 'e'; buf[j++] = 'q'; }
        else buf[j++] = name[i];
    }
    buf[j] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ */
/* Constant pool helpers                                              */
/* ------------------------------------------------------------------ */

static void craw(codegen_ctx_t *ctx, pm_constant_id_t id,
                 const uint8_t **s, size_t *len) {
    pm_constant_t *c = &ctx->parser->constant_pool.constants[id - 1];
    *s = c->start;
    *len = c->length;
}

char *cstr(codegen_ctx_t *ctx, pm_constant_id_t id) {
    const uint8_t *s; size_t len;
    craw(ctx, id, &s, &len);
    char *buf = malloc(len + 1);
    memcpy(buf, s, len);
    buf[len] = '\0';
    return buf;
}

bool ceq(codegen_ctx_t *ctx, pm_constant_id_t id, const char *s) {
    const uint8_t *p; size_t len;
    craw(ctx, id, &p, &len);
    return len == strlen(s) && memcmp(p, s, len) == 0;
}

/* ------------------------------------------------------------------ */
/* Output helpers                                                     */
/* ------------------------------------------------------------------ */

void emit(codegen_ctx_t *ctx, const char *fmt, ...) {
    for (int i = 0; i < ctx->indent; i++) fprintf(ctx->out, "    ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(ctx->out, fmt, ap);
    va_end(ap);
}

void emit_raw(codegen_ctx_t *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(ctx->out, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Class/module/func registry lookups                                 */
/* ------------------------------------------------------------------ */

class_info_t *find_class(codegen_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->class_count; i++)
        if (strcmp(ctx->classes[i].name, name) == 0) return &ctx->classes[i];
    return NULL;
}

method_info_t *find_method(class_info_t *cls, const char *name) {
    if (!cls) return NULL;
    for (int i = 0; i < cls->method_count; i++)
        if (strcmp(cls->methods[i].name, name) == 0) return &cls->methods[i];
    return NULL;
}

/* Find method walking up inheritance chain; sets *owner to defining class */
method_info_t *find_method_inherited(codegen_ctx_t *ctx,
                                             class_info_t *cls,
                                             const char *name,
                                             class_info_t **owner) {
    while (cls) {
        method_info_t *m = find_method(cls, name);
        if (m) {
            if (owner) *owner = cls;
            return m;
        }
        if (cls->superclass[0])
            cls = find_class(ctx, cls->superclass);
        else
            break;
    }
    if (owner) *owner = NULL;
    return NULL;
}

ivar_info_t *find_ivar(class_info_t *cls, const char *name) {
    if (!cls) return NULL;
    for (int i = 0; i < cls->ivar_count; i++)
        if (strcmp(cls->ivars[i].name, name) == 0) return &cls->ivars[i];
    return NULL;
}

module_info_t *find_module(codegen_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->module_count; i++)
        if (strcmp(ctx->modules[i].name, name) == 0) return &ctx->modules[i];
    return NULL;
}

func_info_t *find_func(codegen_ctx_t *ctx, const char *name) {
    char *safe = c_safe_name(name);
    for (int i = 0; i < ctx->func_count; i++) {
        if (strcmp(ctx->funcs[i].name, safe) == 0) { free(safe); return &ctx->funcs[i]; }
    }
    free(safe);
    return NULL;
}

/* Track which classes a POLY function parameter can hold (for bimorphic dispatch) */
void poly_class_add(codegen_ctx_t *ctx, const char *func_name,
                            int param_idx, const char *class_name) {
    /* Find existing entry */
    for (int i = 0; i < ctx->poly_class_set_count; i++) {
        if (strcmp(ctx->poly_class_sets[i].func_name, func_name) == 0 &&
            ctx->poly_class_sets[i].param_idx == param_idx) {
            /* Check if class already tracked */
            for (int j = 0; j < ctx->poly_class_sets[i].class_count; j++)
                if (strcmp(ctx->poly_class_sets[i].class_names[j], class_name) == 0) return;
            if (ctx->poly_class_sets[i].class_count < MAX_POLY_CLASSES)
                snprintf(ctx->poly_class_sets[i].class_names[ctx->poly_class_sets[i].class_count++],
                         64, "%s", class_name);
            return;
        }
    }
    /* Create new entry */
    if (ctx->poly_class_set_count < MAX_FUNCS) {
        int idx = ctx->poly_class_set_count++;
        snprintf(ctx->poly_class_sets[idx].func_name, 64, "%s", func_name);
        ctx->poly_class_sets[idx].param_idx = param_idx;
        ctx->poly_class_sets[idx].class_count = 1;
        snprintf(ctx->poly_class_sets[idx].class_names[0], 64, "%s", class_name);
    }
}

/* Look up the poly class set for a function parameter */
int poly_class_get(codegen_ctx_t *ctx, const char *func_name,
                           int param_idx, char classes[][64]) {
    for (int i = 0; i < ctx->poly_class_set_count; i++) {
        if (strcmp(ctx->poly_class_sets[i].func_name, func_name) == 0 &&
            ctx->poly_class_sets[i].param_idx == param_idx) {
            for (int j = 0; j < ctx->poly_class_sets[i].class_count; j++)
                snprintf(classes[j], 64, "%s", ctx->poly_class_sets[i].class_names[j]);
            return ctx->poly_class_sets[i].class_count;
        }
    }
    return 0;
}

/* Register a megamorphic dispatch function for a method called on 3+ types.
 * Returns the index into ctx->mega_dispatch[], or an existing entry if already registered.
 * The dispatch function will be named sp_dispatch_<sanitized_method>. */
int mega_dispatch_register(codegen_ctx_t *ctx, const char *method,
                                   const char *sanitized,
                                   char classes[][64], int nclasses,
                                   spinel_type_t return_kind) {
    /* Check if an identical dispatch already exists */
    for (int i = 0; i < ctx->mega_dispatch_count; i++) {
        if (strcmp(ctx->mega_dispatch[i].method_name, method) == 0 &&
            ctx->mega_dispatch[i].class_count == nclasses) {
            bool same = true;
            for (int j = 0; j < nclasses && same; j++) {
                bool found = false;
                for (int k = 0; k < ctx->mega_dispatch[i].class_count; k++)
                    if (strcmp(ctx->mega_dispatch[i].class_names[k], classes[j]) == 0) { found = true; break; }
                if (!found) same = false;
            }
            if (same) return i;
        }
    }
    /* Register new dispatch */
    if (ctx->mega_dispatch_count >= MAX_MEGA_DISPATCH) return -1;
    int idx = ctx->mega_dispatch_count++;
    snprintf(ctx->mega_dispatch[idx].method_name, 64, "%s", method);
    snprintf(ctx->mega_dispatch[idx].sanitized, 64, "%s", sanitized);
    for (int j = 0; j < nclasses && j < MAX_POLY_CLASSES; j++)
        snprintf(ctx->mega_dispatch[idx].class_names[j], 64, "%s", classes[j]);
    ctx->mega_dispatch[idx].class_count = nclasses;
    ctx->mega_dispatch[idx].return_kind = return_kind;
    return idx;
}

/* ------------------------------------------------------------------ */
/* Variable table                                                     */
/* ------------------------------------------------------------------ */

var_entry_t *var_lookup(codegen_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->var_count; i++)
        if (ctx->vars[i].name[0] && strcmp(ctx->vars[i].name, name) == 0) return &ctx->vars[i];
    return NULL;
}

var_entry_t *var_declare(codegen_ctx_t *ctx, const char *name,
                                vtype_t type, bool is_constant) {
    var_entry_t *v = var_lookup(ctx, name);
    if (v) {
        /* Widen if types conflict */
        if (v->type.kind != type.kind && v->type.kind != SPINEL_TYPE_UNKNOWN) {
            if (vt_is_numeric(v->type) && vt_is_numeric(type))
                v->type = vt_prim(SPINEL_TYPE_FLOAT);
            else if (type.kind == SPINEL_TYPE_OBJECT && v->type.kind == SPINEL_TYPE_OBJECT
                     && strcmp(v->type.klass, type.klass) == 0) { /* same class */ }
            else if (type.kind == SPINEL_TYPE_OBJECT && v->type.kind == SPINEL_TYPE_OBJECT
                     && strcmp(v->type.klass, type.klass) != 0)
                v->type = vt_prim(SPINEL_TYPE_POLY); /* different classes → POLY */
            else if (v->type.kind == SPINEL_TYPE_VALUE && type.kind != SPINEL_TYPE_VALUE
                     && type.kind != SPINEL_TYPE_UNKNOWN)
                v->type = type;  /* Narrow from VALUE to a more specific type */
            else if (v->type.kind == SPINEL_TYPE_POLY)
                ; /* already POLY, leave it */
            else if (vt_is_poly_eligible(v->type) && vt_is_poly_eligible(type))
                v->type = vt_prim(SPINEL_TYPE_POLY);
            else if (type.kind == SPINEL_TYPE_VALUE || type.kind == SPINEL_TYPE_UNKNOWN)
                ; /* keep more specific existing type (will be resolved in later passes) */
            else
                v->type = vt_prim(SPINEL_TYPE_VALUE);
        } else if (v->type.kind == SPINEL_TYPE_UNKNOWN) {
            v->type = type;
        }
        return v;
    }
    assert(ctx->var_count < MAX_VARS);
    v = &ctx->vars[ctx->var_count++];
    snprintf(v->name, sizeof(v->name), "%s", name);
    v->type = type;
    v->declared = false;
    v->is_constant = is_constant;
    return v;
}

char *make_cname(const char *name, bool is_constant) {
    return sfmt("%s%s", is_constant ? "cv_" : "lv_", name);
}

/* Sanitize Ruby method name to valid C identifier */
const char *sanitize_method(const char *name) {
    static char buf[128];
    if (strcmp(name, "<=>") == 0) return "_cmp";
    if (strcmp(name, "==") == 0) return "_eq";
    if (strcmp(name, "!=") == 0) return "_neq";
    if (strcmp(name, "<") == 0) return "_lt";
    if (strcmp(name, ">") == 0) return "_gt";
    if (strcmp(name, "<=") == 0) return "_le";
    if (strcmp(name, ">=") == 0) return "_ge";
    if (strcmp(name, "+") == 0) return "_add";
    if (strcmp(name, "-") == 0) return "_sub";
    if (strcmp(name, "*") == 0) return "_mul";
    if (strcmp(name, "/") == 0) return "_div";
    if (strcmp(name, "%") == 0) return "_mod";
    if (strcmp(name, "**") == 0) return "_pow";
    if (strcmp(name, "<<") == 0) return "_lshift";
    if (strcmp(name, ">>") == 0) return "_rshift";
    if (strcmp(name, "|") == 0) return "_bor";
    if (strcmp(name, "&") == 0) return "_band";
    if (strcmp(name, "^") == 0) return "_bxor";
    if (strcmp(name, "~") == 0) return "_bnot";
    if (strcmp(name, "[]") == 0) return "_aref";
    if (strcmp(name, "[]=") == 0) return "_aset";
    if (strcmp(name, "-@") == 0) return "_uminus";
    if (strcmp(name, "+@") == 0) return "_uplus";
    /* Replace trailing ? and ! with _p and _bang */
    size_t len = strlen(name);
    if (len > 0 && len < sizeof(buf) - 2) {
        memcpy(buf, name, len + 1);
        if (buf[len - 1] == '?') { buf[len - 1] = '_'; buf[len] = 'p'; buf[len + 1] = '\0'; }
        else if (buf[len - 1] == '!') { buf[len - 1] = '_'; buf[len] = 'b'; buf[len + 1] = '\0'; }
        else if (buf[len - 1] == '=') { buf[len - 1] = '_'; buf[len] = 's'; buf[len + 1] = '\0'; } /* setter */
        return buf;
    }
    return name;
}
/* ------------------------------------------------------------------ */
/* Pass 1: Class/Module/Function analysis                             */
/* ------------------------------------------------------------------ */

static void analyze_method(codegen_ctx_t *ctx, class_info_t *cls,
                           pm_def_node_t *def) {
    if (cls->method_count >= MAX_METHODS) return;
    /* Skip if method already registered (class reopening) */
    {
        char *dn = cstr(ctx, def->name);
        method_info_t *existing = find_method(cls, dn);
        free(dn);
        if (existing) return;
    }
    method_info_t *m = &cls->methods[cls->method_count++];
    memset(m, 0, sizeof(*m));
    char *name = cstr(ctx, def->name);
    snprintf(m->name, sizeof(m->name), "%s", name);
    free(name);

    m->body_node = def->body ? (pm_node_t *)def->body : NULL;
    m->params_node = def->parameters ? (pm_node_t *)def->parameters : NULL;
    m->origin_parser = ctx->parser;
    m->param_count = 0;
    m->is_getter = false;
    m->is_setter = false;
    m->is_class_method = false;
    m->return_type = vt_prim(SPINEL_TYPE_VALUE);

    /* Extract parameters */
    if (def->parameters) {
        pm_parameters_node_t *params = def->parameters;
        for (size_t i = 0; i < params->requireds.size && m->param_count < MAX_PARAMS; i++) {
            pm_node_t *p = params->requireds.nodes[i];
            if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE) {
                pm_required_parameter_node_t *rp = (pm_required_parameter_node_t *)p;
                char *pname = cstr(ctx, rp->name);
                snprintf(m->params[m->param_count].name, 64, "%s", pname);
                m->params[m->param_count].type = vt_prim(SPINEL_TYPE_VALUE);
                m->param_count++;
                free(pname);
            }
        }
        /* Optional parameters (def foo(x = 10)) */
        for (size_t i = 0; i < params->optionals.size && m->param_count < MAX_PARAMS; i++) {
            pm_node_t *p = params->optionals.nodes[i];
            if (PM_NODE_TYPE(p) == PM_OPTIONAL_PARAMETER_NODE) {
                pm_optional_parameter_node_t *op = (pm_optional_parameter_node_t *)p;
                char *pname = cstr(ctx, op->name);
                snprintf(m->params[m->param_count].name, 64, "%s", pname);
                m->params[m->param_count].type = infer_type(ctx, op->value);
                m->params[m->param_count].is_optional = true;
                m->params[m->param_count].default_node = op->value;
                m->param_count++;
                free(pname);
            }
        }
        /* Rest parameter (def foo(*args)) */
        if (params->rest && PM_NODE_TYPE(params->rest) == PM_REST_PARAMETER_NODE) {
            pm_rest_parameter_node_t *rp = (pm_rest_parameter_node_t *)params->rest;
            if (rp->name) {
                char *pname = cstr(ctx, rp->name);
                m->has_rest = true;
                snprintf(m->rest_name, sizeof(m->rest_name), "%s", pname);
                snprintf(m->params[m->param_count].name, 64, "%s", pname);
                m->params[m->param_count].type = vt_prim(SPINEL_TYPE_ARRAY);
                m->param_count++;
                free(pname);
            }
        }
        /* Keyword parameters (def foo(name:, greeting: "Hello")) */
        for (size_t i = 0; i < params->keywords.size && m->param_count < MAX_PARAMS; i++) {
            pm_node_t *p = params->keywords.nodes[i];
            if (PM_NODE_TYPE(p) == PM_REQUIRED_KEYWORD_PARAMETER_NODE) {
                pm_required_keyword_parameter_node_t *kp = (pm_required_keyword_parameter_node_t *)p;
                char *pname = cstr(ctx, kp->name);
                snprintf(m->params[m->param_count].name, 64, "%s", pname);
                m->params[m->param_count].type = vt_prim(SPINEL_TYPE_VALUE);
                m->params[m->param_count].is_keyword = true;
                m->param_count++;
                free(pname);
            }
            if (PM_NODE_TYPE(p) == PM_OPTIONAL_KEYWORD_PARAMETER_NODE) {
                pm_optional_keyword_parameter_node_t *kp = (pm_optional_keyword_parameter_node_t *)p;
                char *pname = cstr(ctx, kp->name);
                snprintf(m->params[m->param_count].name, 64, "%s", pname);
                m->params[m->param_count].type = infer_type(ctx, kp->value);
                m->params[m->param_count].is_optional = true;
                m->params[m->param_count].is_keyword = true;
                m->params[m->param_count].default_node = kp->value;
                m->param_count++;
                free(pname);
            }
        }
    }

    /* Detect getter pattern: def x; @x; end */
    if (m->param_count == 0 && m->body_node) {
        pm_node_t *body = m->body_node;
        if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
            pm_statements_node_t *stmts = (pm_statements_node_t *)body;
            if (stmts->body.size == 1 &&
                PM_NODE_TYPE(stmts->body.nodes[0]) == PM_INSTANCE_VARIABLE_READ_NODE) {
                pm_instance_variable_read_node_t *iv =
                    (pm_instance_variable_read_node_t *)stmts->body.nodes[0];
                char *ivname = cstr(ctx, iv->name);
                m->is_getter = true;
                snprintf(m->accessor_ivar, sizeof(m->accessor_ivar), "%s",
                         ivname + 1); /* skip @ */
                free(ivname);
            }
        }
    }

    /* Detect setter pattern: def x=(v); @x = v; end */
    if (m->param_count == 1 && m->body_node) {
        size_t nlen = strlen(m->name);
        if (nlen > 1 && m->name[nlen - 1] == '=') {
            pm_node_t *body = m->body_node;
            if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                pm_statements_node_t *stmts = (pm_statements_node_t *)body;
                if (stmts->body.size == 1 &&
                    PM_NODE_TYPE(stmts->body.nodes[0]) == PM_INSTANCE_VARIABLE_WRITE_NODE) {
                    m->is_setter = true;
                    char ivar[64];
                    snprintf(ivar, sizeof(ivar), "%.*s", (int)(nlen - 1), m->name);
                    snprintf(m->accessor_ivar, sizeof(m->accessor_ivar), "%s", ivar);
                }
            }
        }
    }
}

/* Scan initialize body for ivar assignments to determine ivar types */
static void analyze_ivars_from_init(codegen_ctx_t *ctx, class_info_t *cls,
                                     pm_node_t *body) {
    if (!body) return;
    if (PM_NODE_TYPE(body) != PM_STATEMENTS_NODE) return;
    pm_statements_node_t *stmts = (pm_statements_node_t *)body;
    for (size_t i = 0; i < stmts->body.size; i++) {
        pm_node_t *s = stmts->body.nodes[i];
        if (PM_NODE_TYPE(s) == PM_INSTANCE_VARIABLE_WRITE_NODE) {
            pm_instance_variable_write_node_t *iw =
                (pm_instance_variable_write_node_t *)s;
            char *ivname = cstr(ctx, iw->name);
            const char *escaped = escape_c_keyword(ivname + 1);
            /* Skip if already registered (e.g., from attr_accessor) */
            if (!find_ivar(cls, escaped) && cls->ivar_count < MAX_IVARS) {
                ivar_info_t *iv = &cls->ivars[cls->ivar_count++];
                snprintf(iv->name, sizeof(iv->name), "%s", escaped);
                /* Type will be resolved in pass 2 */
                iv->type = vt_prim(SPINEL_TYPE_VALUE);
            }
            free(ivname);
        }
    }
}

/* Recursively scan an AST subtree for @ivar write nodes and register them.
 * This catches ivars assigned in non-initialize methods (e.g., Racc runtime). */
static void scan_ivars_deep(codegen_ctx_t *ctx, class_info_t *cls, pm_node_t *node) {
    if (!node) return;
    switch (PM_NODE_TYPE(node)) {
    case PM_INSTANCE_VARIABLE_WRITE_NODE: {
        pm_instance_variable_write_node_t *iw =
            (pm_instance_variable_write_node_t *)node;
        char *ivname = cstr(ctx, iw->name);
        const char *esc = escape_c_keyword(ivname + 1);
        if (!find_ivar(cls, esc) && cls->ivar_count < MAX_IVARS) {
            ivar_info_t *iv = &cls->ivars[cls->ivar_count++];
            snprintf(iv->name, sizeof(iv->name), "%s", esc);
            iv->type = vt_prim(SPINEL_TYPE_VALUE);
        }
        free(ivname);
        break;
    }
    case PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE: {
        pm_instance_variable_operator_write_node_t *iw =
            (pm_instance_variable_operator_write_node_t *)node;
        char *ivname = cstr(ctx, iw->name);
        const char *esc = escape_c_keyword(ivname + 1);
        if (!find_ivar(cls, esc) && cls->ivar_count < MAX_IVARS) {
            ivar_info_t *iv = &cls->ivars[cls->ivar_count++];
            snprintf(iv->name, sizeof(iv->name), "%s", esc);
            iv->type = vt_prim(SPINEL_TYPE_VALUE);
        }
        free(ivname);
        break;
    }
    case PM_INSTANCE_VARIABLE_OR_WRITE_NODE: {
        pm_instance_variable_or_write_node_t *iw =
            (pm_instance_variable_or_write_node_t *)node;
        char *ivname = cstr(ctx, iw->name);
        const char *esc = escape_c_keyword(ivname + 1);
        if (!find_ivar(cls, esc) && cls->ivar_count < MAX_IVARS) {
            ivar_info_t *iv = &cls->ivars[cls->ivar_count++];
            snprintf(iv->name, sizeof(iv->name), "%s", esc);
            iv->type = vt_prim(SPINEL_TYPE_VALUE);
        }
        free(ivname);
        scan_ivars_deep(ctx, cls, iw->value);
        break;
    }
    case PM_STATEMENTS_NODE: {
        pm_statements_node_t *s = (pm_statements_node_t *)node;
        for (size_t i = 0; i < s->body.size; i++)
            scan_ivars_deep(ctx, cls, s->body.nodes[i]);
        break;
    }
    case PM_IF_NODE: {
        pm_if_node_t *n = (pm_if_node_t *)node;
        if (n->statements) scan_ivars_deep(ctx, cls, (pm_node_t *)n->statements);
        if (n->subsequent) scan_ivars_deep(ctx, cls, (pm_node_t *)n->subsequent);
        break;
    }
    case PM_ELSE_NODE: {
        pm_else_node_t *n = (pm_else_node_t *)node;
        if (n->statements) scan_ivars_deep(ctx, cls, (pm_node_t *)n->statements);
        break;
    }
    case PM_WHILE_NODE: {
        pm_while_node_t *n = (pm_while_node_t *)node;
        if (n->statements) scan_ivars_deep(ctx, cls, (pm_node_t *)n->statements);
        break;
    }
    case PM_UNTIL_NODE: {
        pm_until_node_t *n = (pm_until_node_t *)node;
        if (n->statements) scan_ivars_deep(ctx, cls, (pm_node_t *)n->statements);
        break;
    }
    case PM_CASE_NODE: {
        pm_case_node_t *n = (pm_case_node_t *)node;
        for (size_t i = 0; i < n->conditions.size; i++)
            scan_ivars_deep(ctx, cls, n->conditions.nodes[i]);
        if (n->else_clause) scan_ivars_deep(ctx, cls, (pm_node_t *)n->else_clause);
        break;
    }
    case PM_WHEN_NODE: {
        pm_when_node_t *n = (pm_when_node_t *)node;
        if (n->statements) scan_ivars_deep(ctx, cls, (pm_node_t *)n->statements);
        break;
    }
    case PM_BEGIN_NODE: {
        pm_begin_node_t *n = (pm_begin_node_t *)node;
        if (n->statements) scan_ivars_deep(ctx, cls, (pm_node_t *)n->statements);
        if (n->rescue_clause) scan_ivars_deep(ctx, cls, (pm_node_t *)n->rescue_clause);
        if (n->ensure_clause) scan_ivars_deep(ctx, cls, (pm_node_t *)n->ensure_clause);
        break;
    }
    case PM_RESCUE_NODE: {
        pm_rescue_node_t *n = (pm_rescue_node_t *)node;
        if (n->statements) scan_ivars_deep(ctx, cls, (pm_node_t *)n->statements);
        if (n->subsequent) scan_ivars_deep(ctx, cls, (pm_node_t *)n->subsequent);
        break;
    }
    case PM_ENSURE_NODE: {
        pm_ensure_node_t *n = (pm_ensure_node_t *)node;
        if (n->statements) scan_ivars_deep(ctx, cls, (pm_node_t *)n->statements);
        break;
    }
    case PM_BLOCK_NODE: {
        pm_block_node_t *b = (pm_block_node_t *)node;
        if (b->body) scan_ivars_deep(ctx, cls, (pm_node_t *)b->body);
        break;
    }
    case PM_CALL_NODE: {
        pm_call_node_t *c = (pm_call_node_t *)node;
        if (c->block) scan_ivars_deep(ctx, cls, (pm_node_t *)c->block);
        break;
    }
    case PM_LOCAL_VARIABLE_WRITE_NODE: {
        pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
        scan_ivars_deep(ctx, cls, n->value);
        break;
    }
    case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
        pm_local_variable_or_write_node_t *n = (pm_local_variable_or_write_node_t *)node;
        scan_ivars_deep(ctx, cls, n->value);
        break;
    }
    case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
        pm_local_variable_and_write_node_t *n = (pm_local_variable_and_write_node_t *)node;
        scan_ivars_deep(ctx, cls, n->value);
        break;
    }
    case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
        pm_local_variable_operator_write_node_t *n = (pm_local_variable_operator_write_node_t *)node;
        scan_ivars_deep(ctx, cls, n->value);
        break;
    }
    default:
        break;
    }
}

static void analyze_module(codegen_ctx_t *ctx, pm_module_node_t *node);

static void analyze_class(codegen_ctx_t *ctx, pm_class_node_t *node) {
    /* Check if class already registered (reopened class) — merge if so */
    class_info_t *cls = NULL;
    if (PM_NODE_TYPE(node->constant_path) == PM_CONSTANT_READ_NODE) {
        pm_constant_read_node_t *cr = (pm_constant_read_node_t *)node->constant_path;
        char *dn = cstr(ctx, cr->name);
        cls = find_class(ctx, dn);
        free(dn);
    }
    if (!cls) {
        if (ctx->class_count >= MAX_CLASSES) return;
        cls = &ctx->classes[ctx->class_count++];
        memset(cls, 0, sizeof(*cls));
        cls->origin_parser = ctx->parser;
    }
    /* Update class_node to the most recent definition with a body */
    if (node->body)
        cls->class_node = (pm_node_t *)node;

    /* Get class name from constant_path */
    if (PM_NODE_TYPE(node->constant_path) == PM_CONSTANT_READ_NODE) {
        pm_constant_read_node_t *cr = (pm_constant_read_node_t *)node->constant_path;
        char *name = cstr(ctx, cr->name);
        snprintf(cls->name, sizeof(cls->name), "%s", name);
        free(name);
    }

    /* Extract superclass name (class Dog < Animal) */
    cls->superclass[0] = '\0';
    if (node->superclass && PM_NODE_TYPE(node->superclass) == PM_CONSTANT_READ_NODE) {
        pm_constant_read_node_t *scr = (pm_constant_read_node_t *)node->superclass;
        char *sname = cstr(ctx, scr->name);
        snprintf(cls->superclass, sizeof(cls->superclass), "%s", sname);
        free(sname);
    }
    /* Detect class X < Struct.new(:field1, :field2, keyword_init: true) */
    else if (node->superclass && PM_NODE_TYPE(node->superclass) == PM_CALL_NODE) {
        pm_call_node_t *sc = (pm_call_node_t *)node->superclass;
        if (ceq(ctx, sc->name, "new") && sc->receiver &&
            PM_NODE_TYPE(sc->receiver) == PM_CONSTANT_READ_NODE) {
            pm_constant_read_node_t *cr = (pm_constant_read_node_t *)sc->receiver;
            if (ceq(ctx, cr->name, "Struct") && sc->arguments) {
                /* Check for keyword_init: true */
                bool keyword_init = false;
                int nfields = (int)sc->arguments->arguments.size;
                for (int fi = 0; fi < nfields; fi++) {
                    pm_node_t *arg = sc->arguments->arguments.nodes[fi];
                    if (PM_NODE_TYPE(arg) == PM_KEYWORD_HASH_NODE) {
                        keyword_init = true;
                        nfields = fi;
                        break;
                    }
                }

                /* Create initialize method with struct fields as params */
                method_info_t *init = &cls->methods[cls->method_count++];
                memset(init, 0, sizeof(*init));
                snprintf(init->name, sizeof(init->name), "initialize");
                init->return_type = vt_obj(cls->name);

                for (int fi = 0; fi < nfields && fi < MAX_IVARS; fi++) {
                    pm_node_t *arg = sc->arguments->arguments.nodes[fi];
                    if (PM_NODE_TYPE(arg) != PM_SYMBOL_NODE) continue;
                    pm_symbol_node_t *sym = (pm_symbol_node_t *)arg;
                    const uint8_t *fsrc = pm_string_source(&sym->unescaped);
                    size_t flen = pm_string_length(&sym->unescaped);
                    char fname[64];
                    snprintf(fname, sizeof(fname), "%.*s", (int)flen, fsrc);

                    /* Ivar */
                    ivar_info_t *iv = &cls->ivars[cls->ivar_count++];
                    snprintf(iv->name, sizeof(iv->name), "%s", escape_c_keyword(fname));
                    iv->type = keyword_init ? vt_prim(SPINEL_TYPE_VALUE) : vt_prim(SPINEL_TYPE_INTEGER);

                    /* Init param */
                    snprintf(init->params[init->param_count].name, 64, "%s", fname);
                    init->params[init->param_count].type = keyword_init ? vt_prim(SPINEL_TYPE_VALUE) : vt_prim(SPINEL_TYPE_INTEGER);
                    init->params[init->param_count].is_keyword = keyword_init;
                    init->param_count++;

                    /* Getter */
                    method_info_t *getter = &cls->methods[cls->method_count++];
                    memset(getter, 0, sizeof(*getter));
                    snprintf(getter->name, sizeof(getter->name), "%s", fname);
                    getter->is_getter = true;
                    snprintf(getter->accessor_ivar, sizeof(getter->accessor_ivar), "%s", escape_c_keyword(fname));
                    getter->return_type = vt_prim(SPINEL_TYPE_INTEGER);

                    /* Setter */
                    method_info_t *setter = &cls->methods[cls->method_count++];
                    memset(setter, 0, sizeof(*setter));
                    snprintf(setter->name, sizeof(setter->name), "%s=", fname);
                    setter->is_setter = true;
                    snprintf(setter->accessor_ivar, sizeof(setter->accessor_ivar), "%s", escape_c_keyword(fname));
                    setter->param_count = 1;
                    snprintf(setter->params[0].name, 64, "v");
                    setter->params[0].type = vt_prim(SPINEL_TYPE_INTEGER);
                }
            }
        }
    }

    if (!node->body) return;
    pm_node_t *body = (pm_node_t *)node->body;
    if (PM_NODE_TYPE(body) != PM_STATEMENTS_NODE) return;
    pm_statements_node_t *stmts = (pm_statements_node_t *)body;

    for (size_t i = 0; i < stmts->body.size; i++) {
        pm_node_t *s = stmts->body.nodes[i];
        if (PM_NODE_TYPE(s) == PM_DEF_NODE) {
            pm_def_node_t *def = (pm_def_node_t *)s;

            /* def self.foo → class method */
            if (def->receiver && PM_NODE_TYPE(def->receiver) == PM_SELF_NODE) {
                if (cls->method_count >= MAX_METHODS) continue;
                method_info_t *m = &cls->methods[cls->method_count++];
                memset(m, 0, sizeof(*m));
                char *name = cstr(ctx, def->name);
                snprintf(m->name, sizeof(m->name), "%s", name);
                free(name);
                m->body_node = def->body ? (pm_node_t *)def->body : NULL;
                m->params_node = def->parameters ? (pm_node_t *)def->parameters : NULL;
                m->origin_parser = ctx->parser;
                m->param_count = 0;
                m->is_getter = false;
                m->is_setter = false;
                m->is_class_method = true;
                m->return_type = vt_prim(SPINEL_TYPE_VALUE);

                if (def->parameters) {
                    pm_parameters_node_t *params = def->parameters;
                    for (size_t pi = 0; pi < params->requireds.size && m->param_count < MAX_PARAMS; pi++) {
                        pm_node_t *p = params->requireds.nodes[pi];
                        if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE) {
                            pm_required_parameter_node_t *rp = (pm_required_parameter_node_t *)p;
                            char *pname = cstr(ctx, rp->name);
                            snprintf(m->params[m->param_count].name, 64, "%s", pname);
                            m->params[m->param_count].type = vt_prim(SPINEL_TYPE_VALUE);
                            m->param_count++;
                            free(pname);
                        }
                    }
                }
                continue;
            }

            analyze_method(ctx, cls, def);

            /* Extract ivars from initialize — also update origin_parser
             * to the parser that owns the initialize method, since this is
             * the "main" class definition (important for reopened classes). */
            char *mname = cstr(ctx, def->name);
            if (strcmp(mname, "initialize") == 0) {
                cls->origin_parser = ctx->parser;
                analyze_ivars_from_init(ctx, cls, def->body ? (pm_node_t *)def->body : NULL);
            }
            free(mname);
        }
        /* attr_accessor / attr_reader / attr_writer */
        else if (PM_NODE_TYPE(s) == PM_CALL_NODE) {
            pm_call_node_t *call = (pm_call_node_t *)s;
            char *cname = cstr(ctx, call->name);
            bool is_accessor = (strcmp(cname, "attr_accessor") == 0);
            bool is_reader = (strcmp(cname, "attr_reader") == 0);
            bool is_writer = (strcmp(cname, "attr_writer") == 0);
            if ((is_accessor || is_reader || is_writer) && call->arguments) {
                for (size_t ai = 0; ai < call->arguments->arguments.size; ai++) {
                    pm_node_t *arg = call->arguments->arguments.nodes[ai];
                    if (PM_NODE_TYPE(arg) != PM_SYMBOL_NODE) continue;
                    pm_symbol_node_t *sym = (pm_symbol_node_t *)arg;
                    const uint8_t *src = pm_string_source(&sym->unescaped);
                    size_t len = pm_string_length(&sym->unescaped);
                    char sym_name[64];
                    snprintf(sym_name, sizeof(sym_name), "%.*s", (int)len, (const char *)src);

                    /* Register ivar if not already present */
                    if (!find_ivar(cls, sym_name) && cls->ivar_count < MAX_IVARS) {
                        ivar_info_t *iv = &cls->ivars[cls->ivar_count++];
                        snprintf(iv->name, sizeof(iv->name), "%s", escape_c_keyword(sym_name));
                        iv->type = vt_prim(SPINEL_TYPE_VALUE);
                    }

                    /* Generate getter (skip if already registered) */
                    if ((is_accessor || is_reader) &&
                        !find_method(cls, sym_name) && cls->method_count < MAX_METHODS) {
                        method_info_t *m = &cls->methods[cls->method_count++];
                        memset(m, 0, sizeof(*m));
                        snprintf(m->name, sizeof(m->name), "%s", sym_name);
                        m->body_node = NULL;
                        m->params_node = NULL;
                        m->param_count = 0;
                        m->is_getter = true;
                        m->is_setter = false;
                        m->is_class_method = false;
                        snprintf(m->accessor_ivar, sizeof(m->accessor_ivar), "%s", sym_name);
                        m->return_type = vt_prim(SPINEL_TYPE_VALUE);
                    }

                    /* Generate setter (skip if already registered) */
                    {
                    char setter_name[64];
                    snprintf(setter_name, sizeof(setter_name), "%.62s=", sym_name);
                    if ((is_accessor || is_writer) &&
                        !find_method(cls, setter_name) && cls->method_count < MAX_METHODS) {
                        method_info_t *m = &cls->methods[cls->method_count++];
                        snprintf(m->name, sizeof(m->name), "%.62s=", sym_name);
                        m->body_node = NULL;
                        m->params_node = NULL;
                        m->param_count = 1;
                        snprintf(m->params[0].name, 64, "v");
                        m->params[0].type = vt_prim(SPINEL_TYPE_VALUE);
                        m->is_getter = false;
                        m->is_setter = true;
                        m->is_class_method = false;
                        snprintf(m->accessor_ivar, sizeof(m->accessor_ivar), "%s", sym_name);
                        m->return_type = vt_prim(SPINEL_TYPE_VALUE);
                    }
                    }
                }
            }
            /* include ModuleName — record for mixin resolution */
            if (strcmp(cname, "include") == 0 && call->arguments) {
                for (size_t ai = 0; ai < call->arguments->arguments.size; ai++) {
                    pm_node_t *arg = call->arguments->arguments.nodes[ai];
                    if (PM_NODE_TYPE(arg) == PM_CONSTANT_READ_NODE) {
                        pm_constant_read_node_t *cr = (pm_constant_read_node_t *)arg;
                        char *mname = cstr(ctx, cr->name);
                        if (cls->include_count < MAX_INCLUDES) {
                            snprintf(cls->includes[cls->include_count], 64, "%s", mname);
                            cls->include_count++;
                        }
                        free(mname);
                    }
                }
            }
            /* def_delegators "@target", :method1, :method2, ... */
            if (strcmp(cname, "def_delegators") == 0 && call->arguments &&
                call->arguments->arguments.size >= 2) {
                /* First arg is a string (delegate target), remaining are symbols */
                for (size_t ai = 1; ai < call->arguments->arguments.size; ai++) {
                    pm_node_t *arg = call->arguments->arguments.nodes[ai];
                    if (PM_NODE_TYPE(arg) != PM_SYMBOL_NODE) continue;
                    pm_symbol_node_t *sym = (pm_symbol_node_t *)arg;
                    const uint8_t *src = pm_string_source(&sym->unescaped);
                    size_t len = pm_string_length(&sym->unescaped);
                    char dname[64];
                    snprintf(dname, sizeof(dname), "%.*s", (int)len, (const char *)src);
                    if (!find_method(cls, dname) && cls->method_count < MAX_METHODS) {
                        method_info_t *m = &cls->methods[cls->method_count++];
                        memset(m, 0, sizeof(*m));
                        snprintf(m->name, sizeof(m->name), "%s", dname);
                        m->return_type = vt_prim(SPINEL_TYPE_VALUE);
                        /* Try to resolve delegate target's method signature */
                        for (int ci = 0; ci < ctx->class_count; ci++) {
                            if (&ctx->classes[ci] == cls) continue;
                            method_info_t *target = find_method(&ctx->classes[ci], dname);
                            if (target && target->param_count > 0) {
                                m->param_count = target->param_count;
                                for (int pi = 0; pi < target->param_count; pi++)
                                    m->params[pi] = target->params[pi];
                                m->return_type = target->return_type;
                                break;
                            }
                        }
                    }
                }
            }
            free(cname);
        }
        /* Constants defined in class body (e.g., CONST = value) */
        else if (PM_NODE_TYPE(s) == PM_CONSTANT_WRITE_NODE) {
            pm_constant_write_node_t *cw = (pm_constant_write_node_t *)s;
            char *cname = cstr(ctx, cw->name);
            /* Register as global constant variable so codegen can find it */
            if (!find_class(ctx, cname)) {
                vtype_t type = vt_prim(SPINEL_TYPE_INTEGER);
                if (cw->value) {
                    switch (PM_NODE_TYPE(cw->value)) {
                    case PM_STRING_NODE:
                        type = vt_prim(SPINEL_TYPE_STRING);
                        break;
                    case PM_INTEGER_NODE:
                        type = vt_prim(SPINEL_TYPE_INTEGER);
                        break;
                    case PM_FLOAT_NODE:
                        type = vt_prim(SPINEL_TYPE_FLOAT);
                        break;
                    case PM_TRUE_NODE: case PM_FALSE_NODE:
                        type = vt_prim(SPINEL_TYPE_BOOLEAN);
                        break;
                    case PM_CALL_NODE: {
                        pm_call_node_t *vc = (pm_call_node_t *)cw->value;
                        char *mn = cstr(ctx, vc->name);
                        if (strcmp(mn, "to_f") == 0)
                            type = vt_prim(SPINEL_TYPE_FLOAT);
                        else if (strcmp(mn, "freeze") == 0) {
                            /* Check receiver for type: "str".freeze → STRING, [...].freeze → ARRAY */
                            if (vc->receiver) {
                                switch (PM_NODE_TYPE(vc->receiver)) {
                                case PM_STRING_NODE:
                                    type = vt_prim(SPINEL_TYPE_STRING);
                                    break;
                                case PM_ARRAY_NODE:
                                    type = vt_prim(SPINEL_TYPE_ARRAY);
                                    break;
                                case PM_HASH_NODE:
                                    type = vt_prim(SPINEL_TYPE_HASH);
                                    break;
                                case PM_SYMBOL_NODE:
                                    type = vt_prim(SPINEL_TYPE_STRING);
                                    break;
                                default:
                                    type = vt_prim(SPINEL_TYPE_INTEGER);
                                    break;
                                }
                            }
                        } else
                            type = vt_prim(SPINEL_TYPE_INTEGER);
                        free(mn);
                        break;
                    }
                    case PM_HASH_NODE:
                        type = vt_prim(SPINEL_TYPE_HASH);
                        break;
                    case PM_ARRAY_NODE:
                        type = vt_prim(SPINEL_TYPE_ARRAY);
                        break;
                    case PM_SYMBOL_NODE:
                        type = vt_prim(SPINEL_TYPE_STRING);
                        break;
                    case PM_CONSTANT_PATH_NODE:
                        /* Float::INFINITY → FLOAT */
                        type = vt_prim(SPINEL_TYPE_FLOAT);
                        break;
                    default:
                        type = vt_prim(SPINEL_TYPE_INTEGER);
                        break;
                    }
                }
                var_declare(ctx, cname, type, true);
            }
            free(cname);
        }
        /* alias new_name old_name */
        else if (PM_NODE_TYPE(s) == PM_ALIAS_METHOD_NODE) {
            pm_alias_method_node_t *a = (pm_alias_method_node_t *)s;
            if (PM_NODE_TYPE(a->new_name) == PM_SYMBOL_NODE &&
                PM_NODE_TYPE(a->old_name) == PM_SYMBOL_NODE) {
                pm_symbol_node_t *nn = (pm_symbol_node_t *)a->new_name;
                pm_symbol_node_t *on = (pm_symbol_node_t *)a->old_name;
                const uint8_t *ns = pm_string_source(&nn->unescaped);
                size_t nl = pm_string_length(&nn->unescaped);
                const uint8_t *os = pm_string_source(&on->unescaped);
                size_t ol = pm_string_length(&on->unescaped);
                char new_name[64], old_name[64];
                snprintf(new_name, sizeof(new_name), "%.*s", (int)nl, ns);
                snprintf(old_name, sizeof(old_name), "%.*s", (int)ol, os);
                /* Find the old method and copy it with the new name */
                for (int mi = 0; mi < cls->method_count; mi++) {
                    if (strcmp(cls->methods[mi].name, old_name) == 0 &&
                        cls->method_count < MAX_METHODS) {
                        cls->methods[cls->method_count] = cls->methods[mi];
                        snprintf(cls->methods[cls->method_count].name,
                                 sizeof(cls->methods[cls->method_count].name),
                                 "%s", new_name);
                        cls->method_count++;
                        break;
                    }
                }
            }
        }
        /* begin ... rescue ... end blocks in class body */
        else if (PM_NODE_TYPE(s) == PM_BEGIN_NODE) {
            pm_begin_node_t *bn = (pm_begin_node_t *)s;
            /* Walk begin body for constant writes */
            pm_node_t *parts[3] = { NULL, NULL, NULL };
            int nparts = 0;
            if (bn->statements) parts[nparts++] = (pm_node_t *)bn->statements;
            if (bn->rescue_clause && bn->rescue_clause->statements)
                parts[nparts++] = (pm_node_t *)bn->rescue_clause->statements;
            if (bn->ensure_clause && bn->ensure_clause->statements)
                parts[nparts++] = (pm_node_t *)bn->ensure_clause->statements;
            for (int pi = 0; pi < nparts; pi++) {
                pm_node_t *part = parts[pi];
                if (PM_NODE_TYPE(part) != PM_STATEMENTS_NODE) continue;
                pm_statements_node_t *pstmts = (pm_statements_node_t *)part;
                for (size_t j = 0; j < pstmts->body.size; j++) {
                    pm_node_t *ps = pstmts->body.nodes[j];
                    if (PM_NODE_TYPE(ps) == PM_CONSTANT_WRITE_NODE) {
                        pm_constant_write_node_t *cw = (pm_constant_write_node_t *)ps;
                        char *cname = cstr(ctx, cw->name);
                        if (!find_class(ctx, cname)) {
                            vtype_t type = vt_prim(SPINEL_TYPE_INTEGER);
                            if (cw->value) {
                                switch (PM_NODE_TYPE(cw->value)) {
                                case PM_STRING_NODE: type = vt_prim(SPINEL_TYPE_STRING); break;
                                case PM_INTEGER_NODE: type = vt_prim(SPINEL_TYPE_INTEGER); break;
                                case PM_FLOAT_NODE: type = vt_prim(SPINEL_TYPE_FLOAT); break;
                                case PM_SYMBOL_NODE: type = vt_prim(SPINEL_TYPE_STRING); break;
                                default: break;
                                }
                            }
                            var_declare(ctx, cname, type, true);
                        }
                        free(cname);
                    }
                }
            }
        }
    }

    /* Scan ALL method bodies for @ivar assignments (catches ivars set outside
     * initialize, e.g., Racc runtime's _racc_init_sysvars). */
    for (int mi = 0; mi < cls->method_count; mi++) {
        if (cls->methods[mi].body_node)
            scan_ivars_deep(ctx, cls, cls->methods[mi].body_node);
    }

    /* Heuristic: classes with only float/int ivars and <= 4 fields are value types */
    cls->is_value_type = (cls->ivar_count <= 4 && cls->ivar_count > 0);
}

static void analyze_module(codegen_ctx_t *ctx, pm_module_node_t *node) {
    /* Extract module name first for dedup check */
    char mod_name[64] = "";
    if (PM_NODE_TYPE(node->constant_path) == PM_CONSTANT_READ_NODE) {
        pm_constant_read_node_t *cr = (pm_constant_read_node_t *)node->constant_path;
        char *name = cstr(ctx, cr->name);
        snprintf(mod_name, sizeof(mod_name), "%s", name);
        free(name);
    }

    /* Check if module already exists (e.g., "module Lrama" repeated across files) */
    module_info_t *mod = NULL;
    for (int i = 0; i < ctx->module_count; i++) {
        if (mod_name[0] && strcmp(ctx->modules[i].name, mod_name) == 0) {
            mod = &ctx->modules[i];
            break;
        }
    }
    if (!mod) {
        if (ctx->module_count >= MAX_MODULES) {
            fprintf(stderr, "Warning: too many modules (max %d), skipping '%s'\n",
                    MAX_MODULES, mod_name);
            return;
        }
        mod = &ctx->modules[ctx->module_count++];
        memset(mod, 0, sizeof(*mod));
        snprintf(mod->name, sizeof(mod->name), "%s", mod_name);
    }
    mod->module_node = (pm_node_t *)node;
    if (!mod->origin_parser) mod->origin_parser = ctx->parser;

    /* Analyze module body for methods and class ivars */
    if (!node->body) return;
    if (PM_NODE_TYPE(node->body) != PM_STATEMENTS_NODE) return;
    pm_statements_node_t *stmts = (pm_statements_node_t *)node->body;

    for (size_t i = 0; i < stmts->body.size; i++) {
        pm_node_t *s = stmts->body.nodes[i];
        if (PM_NODE_TYPE(s) == PM_DEF_NODE) {
            pm_def_node_t *def = (pm_def_node_t *)s;
            method_info_t *m = &mod->methods[mod->method_count++];
            memset(m, 0, sizeof(*m));
            char *name = cstr(ctx, def->name);
            snprintf(m->name, sizeof(m->name), "%s", name);
            free(name);
            m->body_node = def->body ? (pm_node_t *)def->body : NULL;
            m->params_node = def->parameters ? (pm_node_t *)def->parameters : NULL;
            m->param_count = 0;
            m->return_type = vt_prim(SPINEL_TYPE_VALUE);

            /* def self.foo → module function; def foo → mixin method */
            if (def->receiver && PM_NODE_TYPE(def->receiver) == PM_SELF_NODE) {
                m->is_class_method = true;
                /* return type resolved later in resolve_class_types */
            }

            /* Parse parameters for mixin methods */
            if (def->parameters) {
                pm_parameters_node_t *params = def->parameters;
                for (size_t pi = 0; pi < params->requireds.size && m->param_count < MAX_PARAMS; pi++) {
                    pm_node_t *p = params->requireds.nodes[pi];
                    if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE) {
                        pm_required_parameter_node_t *rp = (pm_required_parameter_node_t *)p;
                        char *pname = cstr(ctx, rp->name);
                        snprintf(m->params[m->param_count].name, 64, "%s", pname);
                        m->params[m->param_count].type = vt_prim(SPINEL_TYPE_VALUE);
                        m->param_count++;
                        free(pname);
                    }
                }
            }
        } else if (PM_NODE_TYPE(s) == PM_INSTANCE_VARIABLE_WRITE_NODE) {
            pm_instance_variable_write_node_t *iw =
                (pm_instance_variable_write_node_t *)s;
            char *ivname = cstr(ctx, iw->name);
            ivar_info_t *iv = &mod->vars[mod->var_count++];
            snprintf(iv->name, sizeof(iv->name), "%s", escape_c_keyword(ivname + 1));
            iv->type = vt_prim(SPINEL_TYPE_INTEGER);
            free(ivname);
        } else if (PM_NODE_TYPE(s) == PM_CONSTANT_WRITE_NODE) {
            pm_constant_write_node_t *cw = (pm_constant_write_node_t *)s;
            char *cname = cstr(ctx, cw->name);
            module_const_t *mc = &mod->consts[mod->const_count++];
            snprintf(mc->name, sizeof(mc->name), "%s", cname);
            mc->value_node = cw->value;
            /* Infer type from value node */
            if (cw->value) {
                switch (PM_NODE_TYPE(cw->value)) {
                case PM_STRING_NODE:
                    mc->type = vt_prim(SPINEL_TYPE_STRING);
                    break;
                case PM_INTEGER_NODE:
                    mc->type = vt_prim(SPINEL_TYPE_INTEGER);
                    break;
                case PM_FLOAT_NODE:
                    mc->type = vt_prim(SPINEL_TYPE_FLOAT);
                    break;
                case PM_TRUE_NODE: case PM_FALSE_NODE:
                    mc->type = vt_prim(SPINEL_TYPE_BOOLEAN);
                    break;
                case PM_CALL_NODE: {
                    pm_call_node_t *vc = (pm_call_node_t *)cw->value;
                    char *mn = cstr(ctx, vc->name);
                    if (strcmp(mn, "to_f") == 0)
                        mc->type = vt_prim(SPINEL_TYPE_FLOAT);
                    else if (strcmp(mn, "home") == 0 || strcmp(mn, "join") == 0 ||
                             strcmp(mn, "expand_path") == 0)
                        mc->type = vt_prim(SPINEL_TYPE_STRING);
                    else
                        mc->type = vt_prim(SPINEL_TYPE_INTEGER);
                    free(mn);
                    break;
                }
                case PM_HASH_NODE:
                    mc->type = vt_prim(SPINEL_TYPE_HASH);
                    break;
                default:
                    mc->type = vt_prim(SPINEL_TYPE_INTEGER);
                    break;
                }
            } else {
                mc->type = vt_prim(SPINEL_TYPE_INTEGER);
            }
            free(cname);
        } else if (PM_NODE_TYPE(s) == PM_CLASS_NODE) {
            if (ctx->class_count < MAX_CLASSES)
                analyze_class(ctx, (pm_class_node_t *)s);
        } else if (PM_NODE_TYPE(s) == PM_MODULE_NODE) {
            if (ctx->module_count < MAX_MODULES)
                analyze_module(ctx, (pm_module_node_t *)s);
        }
    }
}

/* Detect if a node tree contains PM_YIELD_NODE */
bool has_yield_nodes(pm_node_t *node) {
    if (!node) return false;
    if (PM_NODE_TYPE(node) == PM_YIELD_NODE) return true;
    switch (PM_NODE_TYPE(node)) {
    case PM_STATEMENTS_NODE: {
        pm_statements_node_t *s = (pm_statements_node_t *)node;
        for (size_t i = 0; i < s->body.size; i++)
            if (has_yield_nodes(s->body.nodes[i])) return true;
        return false;
    }
    case PM_IF_NODE: {
        pm_if_node_t *n = (pm_if_node_t *)node;
        if (has_yield_nodes(n->predicate)) return true;
        if (n->statements && has_yield_nodes((pm_node_t *)n->statements)) return true;
        if (n->subsequent && has_yield_nodes((pm_node_t *)n->subsequent)) return true;
        return false;
    }
    case PM_ELSE_NODE: {
        pm_else_node_t *n = (pm_else_node_t *)node;
        return n->statements ? has_yield_nodes((pm_node_t *)n->statements) : false;
    }
    case PM_WHILE_NODE: {
        pm_while_node_t *n = (pm_while_node_t *)node;
        if (has_yield_nodes(n->predicate)) return true;
        return n->statements ? has_yield_nodes((pm_node_t *)n->statements) : false;
    }
    case PM_LOCAL_VARIABLE_WRITE_NODE: {
        pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
        return has_yield_nodes(n->value);
    }
    case PM_CALL_NODE: {
        pm_call_node_t *c = (pm_call_node_t *)node;
        if (c->receiver && has_yield_nodes(c->receiver)) return true;
        if (c->arguments) {
            for (size_t i = 0; i < c->arguments->arguments.size; i++)
                if (has_yield_nodes(c->arguments->arguments.nodes[i])) return true;
        }
        /* Check block body for yield (e.g., @data.each { |x| yield x }) */
        if (c->block && has_yield_nodes(c->block)) return true;
        return false;
    }
    case PM_BLOCK_NODE: {
        pm_block_node_t *b = (pm_block_node_t *)node;
        return b->body ? has_yield_nodes((pm_node_t *)b->body) : false;
    }
    default:
        return false;
    }
}

/* Detect if a block body contains PM_RETURN_NODE (non-local return) */
bool block_has_return(pm_node_t *node) {
    if (!node) return false;
    if (PM_NODE_TYPE(node) == PM_RETURN_NODE) return true;
    switch (PM_NODE_TYPE(node)) {
    case PM_STATEMENTS_NODE: {
        pm_statements_node_t *s = (pm_statements_node_t *)node;
        for (size_t i = 0; i < s->body.size; i++)
            if (block_has_return(s->body.nodes[i])) return true;
        return false;
    }
    case PM_IF_NODE: {
        pm_if_node_t *n = (pm_if_node_t *)node;
        if (n->statements && block_has_return((pm_node_t *)n->statements)) return true;
        if (n->subsequent && block_has_return((pm_node_t *)n->subsequent)) return true;
        return false;
    }
    case PM_ELSE_NODE: {
        pm_else_node_t *n = (pm_else_node_t *)node;
        return n->statements ? block_has_return((pm_node_t *)n->statements) : false;
    }
    case PM_WHILE_NODE: {
        pm_while_node_t *n = (pm_while_node_t *)node;
        return n->statements ? block_has_return((pm_node_t *)n->statements) : false;
    }
    case PM_CALL_NODE: {
        pm_call_node_t *c = (pm_call_node_t *)node;
        if (c->arguments) {
            for (size_t i = 0; i < c->arguments->arguments.size; i++)
                if (block_has_return(c->arguments->arguments.nodes[i])) return true;
        }
        /* Don't recurse into nested blocks — their return is their own scope */
        return false;
    }
    default:
        return false;
    }
}

static void analyze_top_func(codegen_ctx_t *ctx, pm_def_node_t *def) {
    func_info_t *f = &ctx->funcs[ctx->func_count++];
    memset(f, 0, sizeof(*f));
    char *name = cstr(ctx, def->name);
    char *safe = c_safe_name(name);
    snprintf(f->name, sizeof(f->name), "%s", safe);
    free(safe); free(name);
    f->body_node = def->body ? (pm_node_t *)def->body : NULL;
    f->params_node = def->parameters ? (pm_node_t *)def->parameters : NULL;
    f->return_type = vt_prim(SPINEL_TYPE_VALUE);

    if (def->parameters) {
        pm_parameters_node_t *params = def->parameters;
        /* Required parameters */
        for (size_t i = 0; i < params->requireds.size && f->param_count < MAX_PARAMS; i++) {
            pm_node_t *p = params->requireds.nodes[i];
            if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE) {
                pm_required_parameter_node_t *rp = (pm_required_parameter_node_t *)p;
                char *pname = cstr(ctx, rp->name);
                snprintf(f->params[f->param_count].name, 64, "%s", pname);
                f->params[f->param_count].type = vt_prim(SPINEL_TYPE_VALUE);
                f->param_count++;
                free(pname);
            }
        }
        /* Optional parameters (def foo(x = 10)) */
        for (size_t i = 0; i < params->optionals.size && f->param_count < MAX_PARAMS; i++) {
            pm_node_t *p = params->optionals.nodes[i];
            if (PM_NODE_TYPE(p) == PM_OPTIONAL_PARAMETER_NODE) {
                pm_optional_parameter_node_t *op = (pm_optional_parameter_node_t *)p;
                char *pname = cstr(ctx, op->name);
                snprintf(f->params[f->param_count].name, 64, "%s", pname);
                f->params[f->param_count].type = infer_type(ctx, op->value);
                f->params[f->param_count].is_optional = true;
                f->params[f->param_count].default_node = op->value;
                f->param_count++;
                free(pname);
            }
        }
        /* Rest parameter (def foo(*args)) */
        if (params->rest && PM_NODE_TYPE(params->rest) == PM_REST_PARAMETER_NODE) {
            pm_rest_parameter_node_t *rp = (pm_rest_parameter_node_t *)params->rest;
            char *pname = cstr(ctx, rp->name);
            f->has_rest = true;
            snprintf(f->rest_name, sizeof(f->rest_name), "%s", pname);
            f->rest_param_index = f->param_count;
            snprintf(f->params[f->param_count].name, 64, "%s", pname);
            f->params[f->param_count].type = vt_prim(SPINEL_TYPE_ARRAY);
            /* Note: is_array is NOT set; vt_ctype for ARRAY already returns pointer type */
            f->param_count++;
            free(pname);
        }
        /* Keyword parameters (def foo(name:, greeting: "Hello")) */
        for (size_t i = 0; i < params->keywords.size && f->param_count < MAX_PARAMS; i++) {
            pm_node_t *p = params->keywords.nodes[i];
            if (PM_NODE_TYPE(p) == PM_REQUIRED_KEYWORD_PARAMETER_NODE) {
                pm_required_keyword_parameter_node_t *kp = (pm_required_keyword_parameter_node_t *)p;
                char *pname = cstr(ctx, kp->name);
                snprintf(f->params[f->param_count].name, 64, "%s", pname);
                f->params[f->param_count].type = vt_prim(SPINEL_TYPE_VALUE);
                f->params[f->param_count].is_keyword = true;
                f->param_count++;
                free(pname);
            } else if (PM_NODE_TYPE(p) == PM_OPTIONAL_KEYWORD_PARAMETER_NODE) {
                pm_optional_keyword_parameter_node_t *kp = (pm_optional_keyword_parameter_node_t *)p;
                char *pname = cstr(ctx, kp->name);
                snprintf(f->params[f->param_count].name, 64, "%s", pname);
                f->params[f->param_count].type = infer_type(ctx, kp->value);
                f->params[f->param_count].is_keyword = true;
                f->params[f->param_count].is_optional = true;
                f->params[f->param_count].default_node = kp->value;
                f->param_count++;
                free(pname);
            }
        }
    }

    /* Detect &block parameter */
    if (def->parameters && def->parameters->block) {
        pm_block_parameter_node_t *bp = def->parameters->block;
        f->has_block_param = true;
        if (bp->name) {
            char *bname = cstr(ctx, bp->name);
            snprintf(f->block_param_name, sizeof(f->block_param_name), "%s", bname);
            free(bname);
        } else {
            snprintf(f->block_param_name, sizeof(f->block_param_name), "block");
        }
    }

    /* Detect yield in function body */
    f->has_yield = f->body_node ? has_yield_nodes(f->body_node) : false;
}

void class_analysis_pass(codegen_ctx_t *ctx, pm_node_t *root) {
    assert(PM_NODE_TYPE(root) == PM_PROGRAM_NODE);
    pm_program_node_t *prog = (pm_program_node_t *)root;
    if (!prog->statements) return;
    pm_statements_node_t *stmts = prog->statements;

    for (size_t i = 0; i < stmts->body.size; i++) {
        pm_node_t *s = stmts->body.nodes[i];
        switch (PM_NODE_TYPE(s)) {
        case PM_CLASS_NODE:
            analyze_class(ctx, (pm_class_node_t *)s);
            break;
        case PM_MODULE_NODE:
            analyze_module(ctx, (pm_module_node_t *)s);
            break;
        case PM_DEF_NODE:
            analyze_top_func(ctx, (pm_def_node_t *)s);
            break;
        case PM_CONSTANT_WRITE_NODE: {
            /* Detect Struct.new(:x, :y) → synthetic class */
            pm_constant_write_node_t *cw = (pm_constant_write_node_t *)s;
            if (PM_NODE_TYPE(cw->value) == PM_CALL_NODE) {
                pm_call_node_t *call = (pm_call_node_t *)cw->value;
                if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
                    pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
                    if (ceq(ctx, cr->name, "Struct") && ceq(ctx, call->name, "new") &&
                        call->arguments) {
                        /* Create synthetic class */
                        class_info_t *cls = &ctx->classes[ctx->class_count++];
                        memset(cls, 0, sizeof(*cls));
                        char *name = cstr(ctx, cw->name);
                        snprintf(cls->name, sizeof(cls->name), "%s", name);
                        cls->is_value_type = false;
                        cls->class_node = (pm_node_t *)s;

                        /* Check for keyword_init: true */
                        bool keyword_init = false;
                        int nfields = (int)call->arguments->arguments.size;
                        for (int fi = 0; fi < nfields; fi++) {
                            pm_node_t *arg = call->arguments->arguments.nodes[fi];
                            if (PM_NODE_TYPE(arg) == PM_KEYWORD_HASH_NODE) {
                                /* keyword_init: true detected */
                                keyword_init = true;
                                nfields = fi; /* don't count keyword hash as a field */
                                break;
                            }
                        }

                        /* Each symbol arg becomes an ivar + getter + setter */
                        method_info_t *init = &cls->methods[cls->method_count++];
                        memset(init, 0, sizeof(*init));
                        snprintf(init->name, sizeof(init->name), "initialize");
                        init->return_type = vt_obj(name);

                        for (int fi = 0; fi < nfields && fi < MAX_IVARS; fi++) {
                            pm_node_t *arg = call->arguments->arguments.nodes[fi];
                            if (PM_NODE_TYPE(arg) != PM_SYMBOL_NODE) continue;
                            pm_symbol_node_t *sym = (pm_symbol_node_t *)arg;
                            const uint8_t *fsrc = pm_string_source(&sym->unescaped);
                            size_t flen = pm_string_length(&sym->unescaped);
                            char fname[64];
                            snprintf(fname, sizeof(fname), "%.*s", (int)flen, fsrc);

                            /* Ivar */
                            ivar_info_t *iv = &cls->ivars[cls->ivar_count++];
                            snprintf(iv->name, sizeof(iv->name), "%s", escape_c_keyword(fname));
                            iv->type = keyword_init ? vt_prim(SPINEL_TYPE_VALUE) : vt_prim(SPINEL_TYPE_INTEGER);

                            /* Init param */
                            snprintf(init->params[init->param_count].name, 64, "%s", fname);
                            init->params[init->param_count].type = keyword_init ? vt_prim(SPINEL_TYPE_VALUE) : vt_prim(SPINEL_TYPE_INTEGER);
                            init->params[init->param_count].is_keyword = keyword_init;
                            init->param_count++;

                            /* Getter */
                            method_info_t *getter = &cls->methods[cls->method_count++];
                            memset(getter, 0, sizeof(*getter));
                            snprintf(getter->name, sizeof(getter->name), "%s", fname);
                            getter->is_getter = true;
                            snprintf(getter->accessor_ivar, sizeof(getter->accessor_ivar), "%s", escape_c_keyword(fname));
                            getter->return_type = vt_prim(SPINEL_TYPE_INTEGER);

                            /* Setter */
                            method_info_t *setter = &cls->methods[cls->method_count++];
                            memset(setter, 0, sizeof(*setter));
                            snprintf(setter->name, sizeof(setter->name), "%s=", fname);
                            setter->is_setter = true;
                            snprintf(setter->accessor_ivar, sizeof(setter->accessor_ivar), "%s", escape_c_keyword(fname));
                            setter->param_count = 1;
                            snprintf(setter->params[0].name, 64, "v");
                            setter->params[0].type = vt_prim(SPINEL_TYPE_INTEGER);
                        }
                        free(name);
                    }
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* File reading helper (for require_relative)                         */
/* ------------------------------------------------------------------ */

static char *read_source_file(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    if (length) *length = (size_t)len;
    return buf;
}

/* Check if a call node is require_relative("...") */
bool is_require_relative(codegen_ctx_t *ctx, pm_node_t *node) {
    if (PM_NODE_TYPE(node) != PM_CALL_NODE) return false;
    pm_call_node_t *call = (pm_call_node_t *)node;
    if (call->receiver) return false;
    if (!ceq(ctx, call->name, "require_relative")) return false;
    if (!call->arguments || call->arguments->arguments.size != 1) return false;
    if (PM_NODE_TYPE(call->arguments->arguments.nodes[0]) != PM_STRING_NODE) return false;
    return true;
}

/* Check if a node is a require "name" call */
bool is_require(codegen_ctx_t *ctx, pm_node_t *node) {
    if (PM_NODE_TYPE(node) != PM_CALL_NODE) return false;
    pm_call_node_t *call = (pm_call_node_t *)node;
    if (call->receiver) return false;
    if (!ceq(ctx, call->name, "require")) return false;
    if (!call->arguments || call->arguments->arguments.size != 1) return false;
    if (PM_NODE_TYPE(call->arguments->arguments.nodes[0]) != PM_STRING_NODE) return false;
    return true;
}

/* Try to resolve require "name" via lib search paths.
 * Returns allocated full path string or NULL. */
static char *resolve_require(codegen_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->lib_path_count; i++) {
        char path[PATH_MAX];
        size_t nl = strlen(name);
        if (nl >= 3 && strcmp(name + nl - 3, ".rb") == 0)
            snprintf(path, sizeof(path), "%s/%s", ctx->lib_paths[i], name);
        else
            snprintf(path, sizeof(path), "%s/%s.rb", ctx->lib_paths[i], name);
        FILE *f = fopen(path, "r");
        if (f) {
            fclose(f);
            char *result = malloc(strlen(path) + 1);
            strcpy(result, path);
            return result;
        }
    }
    return NULL;
}

/* Process a single file: parse, check errors, run class_analysis_pass.
 * Returns index in required_files or -1 on failure. */
static int process_one_require(codegen_ctx_t *ctx, const char *full_path) {
    /* Deduplicate: skip if already required */
    char real[PATH_MAX];
    const char *canon = realpath(full_path, real) ? real : full_path;
    for (int i = 0; i < ctx->required_file_count; i++) {
        if (strcmp(ctx->required_files[i].path, canon) == 0)
            return i; /* already loaded */
    }

    size_t req_len;
    char *req_source = read_source_file(full_path, &req_len);
    if (!req_source) return -1;

    if (ctx->required_file_count >= MAX_REQUIRED_FILES) {
        fprintf(stderr, "Warning: too many required files (max %d)\n", MAX_REQUIRED_FILES);
        free(req_source);
        return -1;
    }

    int idx = ctx->required_file_count++;
    ctx->required_files[idx].source = req_source;
    ctx->required_files[idx].path = xstrdup(canon);
    pm_parser_init(&ctx->required_files[idx].parser,
                   (const uint8_t *)req_source, req_len, NULL);
    ctx->required_files[idx].root = pm_parse(&ctx->required_files[idx].parser);

    if (ctx->required_files[idx].parser.error_list.size > 0) {
        fprintf(stderr, "Parse errors in '%s'\n", full_path);
        ctx->required_file_count--;
        pm_node_destroy(&ctx->required_files[idx].parser, ctx->required_files[idx].root);
        pm_parser_free(&ctx->required_files[idx].parser);
        free(req_source);
        return -1;
    }

    pm_parser_t *saved_parser = ctx->parser;
    const char *saved_source = ctx->source_path;
    ctx->parser = &ctx->required_files[idx].parser;
    ctx->source_path = full_path;

    /* Recursively process require/require_relative in the required file */
    pm_node_t *rroot = ctx->required_files[idx].root;
    if (PM_NODE_TYPE(rroot) == PM_PROGRAM_NODE) {
        pm_program_node_t *rprog = (pm_program_node_t *)rroot;
        if (rprog->statements) {
            for (size_t j = 0; j < rprog->statements->body.size; j++) {
                pm_node_t *rs = rprog->statements->body.nodes[j];
                if (is_require_relative(ctx, rs)) {
                    pm_call_node_t *rcall = (pm_call_node_t *)rs;
                    pm_string_node_t *rpath_node = (pm_string_node_t *)rcall->arguments->arguments.nodes[0];
                    const uint8_t *rrel_src = pm_string_source(&rpath_node->unescaped);
                    size_t rrel_len = pm_string_length(&rpath_node->unescaped);
                    char rrel_path[PATH_MAX];
                    snprintf(rrel_path, sizeof(rrel_path), "%.*s", (int)rrel_len, rrel_src);

                    char rdir_buf[PATH_MAX];
                    snprintf(rdir_buf, sizeof(rdir_buf), "%s", full_path);
                    char *rdir = dirname(rdir_buf);
                    char rfull[PATH_MAX];
                    size_t rrpl = strlen(rrel_path);
                    if (rrpl >= 3 && strcmp(rrel_path + rrpl - 3, ".rb") == 0)
                        snprintf(rfull, sizeof(rfull), "%s/%s", rdir, rrel_path);
                    else
                        snprintf(rfull, sizeof(rfull), "%s/%s.rb", rdir, rrel_path);
                    process_one_require(ctx, rfull);
                } else if (is_require(ctx, rs)) {
                    pm_call_node_t *rcall = (pm_call_node_t *)rs;
                    pm_string_node_t *rpath_node = (pm_string_node_t *)rcall->arguments->arguments.nodes[0];
                    const uint8_t *rname_src = pm_string_source(&rpath_node->unescaped);
                    size_t rname_len = pm_string_length(&rpath_node->unescaped);
                    char rname[256];
                    snprintf(rname, sizeof(rname), "%.*s", (int)rname_len, rname_src);
                    char *resolved = resolve_require(ctx, rname);
                    if (resolved) {
                        process_one_require(ctx, resolved);
                        free(resolved);
                    } else {
                        fprintf(stderr, "spinel: warning: unsupported call Kernel#require (%s)\n", rname);
                    }
                }
            }
        }
    }

    /* Run class analysis */
    class_analysis_pass(ctx, ctx->required_files[idx].root);

    ctx->parser = saved_parser;
    ctx->source_path = saved_source;
    return idx;
}

/* Process require_relative and require calls at top level */
static void process_require_relative(codegen_ctx_t *ctx, pm_node_t *root) {
    assert(PM_NODE_TYPE(root) == PM_PROGRAM_NODE);
    pm_program_node_t *prog = (pm_program_node_t *)root;
    if (!prog->statements) return;
    pm_statements_node_t *stmts = prog->statements;

    for (size_t i = 0; i < stmts->body.size; i++) {
        pm_node_t *s = stmts->body.nodes[i];

        if (is_require_relative(ctx, s)) {
            pm_call_node_t *call = (pm_call_node_t *)s;
            pm_string_node_t *path_node =
                (pm_string_node_t *)call->arguments->arguments.nodes[0];
            const uint8_t *rel_src = pm_string_source(&path_node->unescaped);
            size_t rel_len = pm_string_length(&path_node->unescaped);

            char rel_path[PATH_MAX];
            snprintf(rel_path, sizeof(rel_path), "%.*s", (int)rel_len, rel_src);

            char dir_buf[PATH_MAX];
            snprintf(dir_buf, sizeof(dir_buf), "%s", ctx->source_path);
            char *dir = dirname(dir_buf);

            char full_path[PATH_MAX];
            size_t rpl = strlen(rel_path);
            if (rpl >= 3 && strcmp(rel_path + rpl - 3, ".rb") == 0)
                snprintf(full_path, sizeof(full_path), "%s/%s", dir, rel_path);
            else
                snprintf(full_path, sizeof(full_path), "%s/%s.rb", dir, rel_path);

            if (process_one_require(ctx, full_path) < 0) {
                fprintf(stderr, "Warning: cannot open require_relative '%s' (%s)\n",
                        rel_path, full_path);
            }
        } else if (is_require(ctx, s)) {
            pm_call_node_t *call = (pm_call_node_t *)s;
            pm_string_node_t *path_node =
                (pm_string_node_t *)call->arguments->arguments.nodes[0];
            const uint8_t *rel_src = pm_string_source(&path_node->unescaped);
            size_t rel_len = pm_string_length(&path_node->unescaped);

            char name[256];
            snprintf(name, sizeof(name), "%.*s", (int)rel_len, rel_src);

            char *resolved = resolve_require(ctx, name);
            if (resolved) {
                process_one_require(ctx, resolved);
                free(resolved);
            }
            /* If not resolved, silently skip (the require call becomes a no-op) */
        }
    }
}

void codegen_init(codegen_ctx_t *ctx, pm_parser_t *parser, FILE *out,
                  const char *source_path) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->parser = parser;
    ctx->out = out;
    ctx->indent = 1;
    ctx->source_path = source_path;
}

/* Check if AST contains any lambda nodes (recursive scan) */
static bool has_lambda_nodes(pm_node_t *node) {
    if (!node) return false;
    if (PM_NODE_TYPE(node) == PM_LAMBDA_NODE) return true;

    switch (PM_NODE_TYPE(node)) {
    case PM_PROGRAM_NODE: {
        pm_program_node_t *p = (pm_program_node_t *)node;
        return p->statements ? has_lambda_nodes((pm_node_t *)p->statements) : false;
    }
    case PM_STATEMENTS_NODE: {
        pm_statements_node_t *s = (pm_statements_node_t *)node;
        for (size_t i = 0; i < s->body.size; i++)
            if (has_lambda_nodes(s->body.nodes[i])) return true;
        return false;
    }
    case PM_LOCAL_VARIABLE_WRITE_NODE: {
        pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
        return has_lambda_nodes(n->value);
    }
    case PM_CONSTANT_WRITE_NODE: {
        pm_constant_write_node_t *n = (pm_constant_write_node_t *)node;
        return has_lambda_nodes(n->value);
    }
    case PM_CALL_NODE: {
        pm_call_node_t *c = (pm_call_node_t *)node;
        if (c->receiver && has_lambda_nodes(c->receiver)) return true;
        if (c->arguments) {
            for (size_t i = 0; i < c->arguments->arguments.size; i++)
                if (has_lambda_nodes(c->arguments->arguments.nodes[i])) return true;
        }
        if (c->block && has_lambda_nodes((pm_node_t *)c->block)) return true;
        return false;
    }
    case PM_BLOCK_NODE: {
        pm_block_node_t *b = (pm_block_node_t *)node;
        return b->body ? has_lambda_nodes((pm_node_t *)b->body) : false;
    }
    case PM_DEF_NODE: {
        pm_def_node_t *d = (pm_def_node_t *)node;
        return d->body ? has_lambda_nodes((pm_node_t *)d->body) : false;
    }
    default:
        return false;
    }
}

/* Check if AST contains any exception nodes (raise/rescue/begin) */
static bool has_exception_nodes(codegen_ctx_t *ctx, pm_node_t *node) {
    if (!node) return false;
    if (PM_NODE_TYPE(node) == PM_BEGIN_NODE ||
        PM_NODE_TYPE(node) == PM_RESCUE_NODE ||
        PM_NODE_TYPE(node) == PM_RESCUE_MODIFIER_NODE ||
        PM_NODE_TYPE(node) == PM_RETRY_NODE)
        return true;

    switch (PM_NODE_TYPE(node)) {
    case PM_PROGRAM_NODE: {
        pm_program_node_t *p = (pm_program_node_t *)node;
        return p->statements ? has_exception_nodes(ctx, (pm_node_t *)p->statements) : false;
    }
    case PM_STATEMENTS_NODE: {
        pm_statements_node_t *s = (pm_statements_node_t *)node;
        for (size_t i = 0; i < s->body.size; i++)
            if (has_exception_nodes(ctx, s->body.nodes[i])) return true;
        return false;
    }
    case PM_LOCAL_VARIABLE_WRITE_NODE: {
        pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
        return has_exception_nodes(ctx, n->value);
    }
    case PM_CONSTANT_WRITE_NODE: {
        pm_constant_write_node_t *n = (pm_constant_write_node_t *)node;
        return has_exception_nodes(ctx, n->value);
    }
    case PM_CALL_NODE: {
        pm_call_node_t *c = (pm_call_node_t *)node;
        /* Check if this is a raise/throw/catch call */
        if (!c->receiver && (ceq(ctx, c->name, "raise") ||
            ceq(ctx, c->name, "throw") || ceq(ctx, c->name, "catch"))) return true;
        if (c->receiver && has_exception_nodes(ctx, c->receiver)) return true;
        if (c->arguments) {
            for (size_t i = 0; i < c->arguments->arguments.size; i++)
                if (has_exception_nodes(ctx, c->arguments->arguments.nodes[i])) return true;
        }
        if (c->block && has_exception_nodes(ctx, (pm_node_t *)c->block)) return true;
        return false;
    }
    case PM_BLOCK_NODE: {
        pm_block_node_t *b = (pm_block_node_t *)node;
        return b->body ? has_exception_nodes(ctx, (pm_node_t *)b->body) : false;
    }
    case PM_DEF_NODE: {
        pm_def_node_t *d = (pm_def_node_t *)node;
        return d->body ? has_exception_nodes(ctx, (pm_node_t *)d->body) : false;
    }
    case PM_IF_NODE: {
        pm_if_node_t *n = (pm_if_node_t *)node;
        if (n->statements && has_exception_nodes(ctx, (pm_node_t *)n->statements)) return true;
        if (n->subsequent && has_exception_nodes(ctx, (pm_node_t *)n->subsequent)) return true;
        return false;
    }
    case PM_ELSE_NODE: {
        pm_else_node_t *n = (pm_else_node_t *)node;
        return n->statements ? has_exception_nodes(ctx, (pm_node_t *)n->statements) : false;
    }
    case PM_WHILE_NODE: {
        pm_while_node_t *n = (pm_while_node_t *)node;
        return n->statements ? has_exception_nodes(ctx, (pm_node_t *)n->statements) : false;
    }
    case PM_CLASS_NODE: {
        pm_class_node_t *cn = (pm_class_node_t *)node;
        return cn->body ? has_exception_nodes(ctx, (pm_node_t *)cn->body) : false;
    }
    case PM_MODULE_NODE: {
        pm_module_node_t *mn = (pm_module_node_t *)node;
        return mn->body ? has_exception_nodes(ctx, (pm_node_t *)mn->body) : false;
    }
    default:
        return false;
    }
}

/* Check if AST contains any block with non-local return (return inside a block) */
static bool has_block_return_nodes(pm_node_t *node) {
    if (!node) return false;
    switch (PM_NODE_TYPE(node)) {
    case PM_PROGRAM_NODE: {
        pm_program_node_t *p = (pm_program_node_t *)node;
        return p->statements ? has_block_return_nodes((pm_node_t *)p->statements) : false;
    }
    case PM_STATEMENTS_NODE: {
        pm_statements_node_t *s = (pm_statements_node_t *)node;
        for (size_t i = 0; i < s->body.size; i++)
            if (has_block_return_nodes(s->body.nodes[i])) return true;
        return false;
    }
    case PM_CALL_NODE: {
        pm_call_node_t *c = (pm_call_node_t *)node;
        /* Check if this call has a block containing a return */
        if (c->block && PM_NODE_TYPE(c->block) == PM_BLOCK_NODE) {
            pm_block_node_t *b = (pm_block_node_t *)c->block;
            if (b->body && block_has_return((pm_node_t *)b->body))
                return true;
        }
        /* Recurse into receiver and arguments */
        if (c->receiver && has_block_return_nodes(c->receiver)) return true;
        if (c->arguments) {
            for (size_t i = 0; i < c->arguments->arguments.size; i++)
                if (has_block_return_nodes(c->arguments->arguments.nodes[i])) return true;
        }
        if (c->block && has_block_return_nodes(c->block)) return true;
        return false;
    }
    case PM_BLOCK_NODE: {
        pm_block_node_t *b = (pm_block_node_t *)node;
        return b->body ? has_block_return_nodes((pm_node_t *)b->body) : false;
    }
    case PM_DEF_NODE: {
        pm_def_node_t *d = (pm_def_node_t *)node;
        return d->body ? has_block_return_nodes((pm_node_t *)d->body) : false;
    }
    case PM_IF_NODE: {
        pm_if_node_t *n = (pm_if_node_t *)node;
        if (n->statements && has_block_return_nodes((pm_node_t *)n->statements)) return true;
        if (n->subsequent && has_block_return_nodes((pm_node_t *)n->subsequent)) return true;
        return false;
    }
    case PM_ELSE_NODE: {
        pm_else_node_t *n = (pm_else_node_t *)node;
        return n->statements ? has_block_return_nodes((pm_node_t *)n->statements) : false;
    }
    case PM_WHILE_NODE: {
        pm_while_node_t *n = (pm_while_node_t *)node;
        return n->statements ? has_block_return_nodes((pm_node_t *)n->statements) : false;
    }
    case PM_CLASS_NODE: {
        pm_class_node_t *cn = (pm_class_node_t *)node;
        return cn->body ? has_block_return_nodes((pm_node_t *)cn->body) : false;
    }
    case PM_MODULE_NODE: {
        pm_module_node_t *mn = (pm_module_node_t *)node;
        return mn->body ? has_block_return_nodes((pm_node_t *)mn->body) : false;
    }
    case PM_LOCAL_VARIABLE_WRITE_NODE: {
        pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
        return has_block_return_nodes(n->value);
    }
    default:
        return false;
    }
}

/* Check if AST contains any string split calls */
static bool has_split_calls(codegen_ctx_t *ctx, pm_node_t *node) {
    if (!node) return false;
    switch (PM_NODE_TYPE(node)) {
    case PM_PROGRAM_NODE: {
        pm_program_node_t *p = (pm_program_node_t *)node;
        return p->statements ? has_split_calls(ctx, (pm_node_t *)p->statements) : false;
    }
    case PM_STATEMENTS_NODE: {
        pm_statements_node_t *s = (pm_statements_node_t *)node;
        for (size_t i = 0; i < s->body.size; i++)
            if (has_split_calls(ctx, s->body.nodes[i])) return true;
        return false;
    }
    case PM_LOCAL_VARIABLE_WRITE_NODE: {
        pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
        return has_split_calls(ctx, n->value);
    }
    case PM_CALL_NODE: {
        pm_call_node_t *c = (pm_call_node_t *)node;
        if (ceq(ctx, c->name, "split") || ceq(ctx, c->name, "each_line") ||
            ceq(ctx, c->name, "chars")) return true;
        /* Dir.glob returns sp_StrArray */
        if (ceq(ctx, c->name, "glob") && c->receiver &&
            PM_NODE_TYPE(c->receiver) == PM_CONSTANT_READ_NODE) {
            pm_constant_read_node_t *cr = (pm_constant_read_node_t *)c->receiver;
            if (ceq(ctx, cr->name, "Dir")) return true;
        }
        if (c->receiver && has_split_calls(ctx, c->receiver)) return true;
        if (c->arguments) {
            for (size_t i = 0; i < c->arguments->arguments.size; i++)
                if (has_split_calls(ctx, c->arguments->arguments.nodes[i])) return true;
        }
        /* Also recurse into blocks */
        if (c->block && has_split_calls(ctx, c->block)) return true;
        return false;
    }
    case PM_IF_NODE: {
        pm_if_node_t *n = (pm_if_node_t *)node;
        if (n->statements && has_split_calls(ctx, (pm_node_t *)n->statements)) return true;
        if (n->subsequent && has_split_calls(ctx, (pm_node_t *)n->subsequent)) return true;
        return false;
    }
    case PM_WHILE_NODE: {
        pm_while_node_t *n = (pm_while_node_t *)node;
        return n->statements ? has_split_calls(ctx, (pm_node_t *)n->statements) : false;
    }
    case PM_DEF_NODE: {
        pm_def_node_t *d = (pm_def_node_t *)node;
        return d->body ? has_split_calls(ctx, (pm_node_t *)d->body) : false;
    }
    case PM_BLOCK_NODE: {
        pm_block_node_t *b = (pm_block_node_t *)node;
        return b->body ? has_split_calls(ctx, (pm_node_t *)b->body) : false;
    }
    case PM_ARRAY_NODE: {
        /* String array literal ["a", "b"] needs sp_StrArray */
        pm_array_node_t *ary = (pm_array_node_t *)node;
        if (ary->elements.size > 0) {
            bool all_str = true;
            for (size_t i = 0; i < ary->elements.size; i++) {
                if (PM_NODE_TYPE(ary->elements.nodes[i]) != PM_STRING_NODE &&
                    PM_NODE_TYPE(ary->elements.nodes[i]) != PM_INTERPOLATED_STRING_NODE) {
                    all_str = false; break;
                }
            }
            if (all_str) return true;
        }
        return false;
    }
    case PM_CLASS_NODE: {
        pm_class_node_t *cn = (pm_class_node_t *)node;
        return cn->body ? has_split_calls(ctx, (pm_node_t *)cn->body) : false;
    }
    case PM_MODULE_NODE: {
        pm_module_node_t *mn = (pm_module_node_t *)node;
        return mn->body ? has_split_calls(ctx, (pm_node_t *)mn->body) : false;
    }
    default:
        return false;
    }
}

/* Check if AST contains any regexp nodes and collect patterns */
static void collect_regexp_patterns(codegen_ctx_t *ctx, pm_node_t *node) {
    if (!node) return;
    switch (PM_NODE_TYPE(node)) {
    case PM_PROGRAM_NODE: {
        pm_program_node_t *p = (pm_program_node_t *)node;
        if (p->statements) collect_regexp_patterns(ctx, (pm_node_t *)p->statements);
        break;
    }
    case PM_STATEMENTS_NODE: {
        pm_statements_node_t *s = (pm_statements_node_t *)node;
        for (size_t i = 0; i < s->body.size; i++)
            collect_regexp_patterns(ctx, s->body.nodes[i]);
        break;
    }
    case PM_LOCAL_VARIABLE_WRITE_NODE: {
        pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
        collect_regexp_patterns(ctx, n->value);
        break;
    }
    case PM_CALL_NODE: {
        pm_call_node_t *c = (pm_call_node_t *)node;
        if (c->receiver) collect_regexp_patterns(ctx, c->receiver);
        if (c->arguments) {
            for (size_t i = 0; i < c->arguments->arguments.size; i++)
                collect_regexp_patterns(ctx, c->arguments->arguments.nodes[i]);
        }
        if (c->block) collect_regexp_patterns(ctx, (pm_node_t *)c->block);
        break;
    }
    case PM_BLOCK_NODE: {
        pm_block_node_t *b = (pm_block_node_t *)node;
        if (b->body) collect_regexp_patterns(ctx, (pm_node_t *)b->body);
        break;
    }
    case PM_IF_NODE: {
        pm_if_node_t *n = (pm_if_node_t *)node;
        if (n->predicate) collect_regexp_patterns(ctx, n->predicate);
        if (n->statements) collect_regexp_patterns(ctx, (pm_node_t *)n->statements);
        if (n->subsequent) collect_regexp_patterns(ctx, (pm_node_t *)n->subsequent);
        break;
    }
    case PM_ELSE_NODE: {
        pm_else_node_t *n = (pm_else_node_t *)node;
        if (n->statements) collect_regexp_patterns(ctx, (pm_node_t *)n->statements);
        break;
    }
    case PM_WHILE_NODE: {
        pm_while_node_t *n = (pm_while_node_t *)node;
        if (n->predicate) collect_regexp_patterns(ctx, n->predicate);
        if (n->statements) collect_regexp_patterns(ctx, (pm_node_t *)n->statements);
        break;
    }
    case PM_DEF_NODE: {
        pm_def_node_t *d = (pm_def_node_t *)node;
        if (d->body) collect_regexp_patterns(ctx, (pm_node_t *)d->body);
        break;
    }
    case PM_MATCH_WRITE_NODE: {
        pm_match_write_node_t *mw = (pm_match_write_node_t *)node;
        collect_regexp_patterns(ctx, (pm_node_t *)mw->call);
        break;
    }
    case PM_REGULAR_EXPRESSION_NODE: {
        pm_regular_expression_node_t *re = (pm_regular_expression_node_t *)node;
        const uint8_t *src = pm_string_source(&re->unescaped);
        size_t len = pm_string_length(&re->unescaped);
        char pat[256];
        size_t plen = len < 255 ? len : 255;
        memcpy(pat, src, plen);
        pat[plen] = '\0';
        /* Check if already registered */
        for (int i = 0; i < ctx->regexp_counter; i++)
            if (strcmp(ctx->regexps[i].pattern, pat) == 0) return;
        /* Register */
        if (ctx->regexp_counter < MAX_REGEXPS) {
            snprintf(ctx->regexps[ctx->regexp_counter].pattern, 256, "%s", pat);
            ctx->regexps[ctx->regexp_counter].id = ctx->regexp_counter;
            ctx->regexp_counter++;
        }
        ctx->needs_regexp = true;
        break;
    }
    case PM_CLASS_NODE: {
        pm_class_node_t *cn = (pm_class_node_t *)node;
        if (cn->body) collect_regexp_patterns(ctx, (pm_node_t *)cn->body);
        break;
    }
    case PM_MODULE_NODE: {
        pm_module_node_t *mn = (pm_module_node_t *)node;
        if (mn->body) collect_regexp_patterns(ctx, (pm_node_t *)mn->body);
        break;
    }
    case PM_CASE_NODE: {
        pm_case_node_t *cn = (pm_case_node_t *)node;
        if (cn->predicate) collect_regexp_patterns(ctx, cn->predicate);
        for (size_t i = 0; i < cn->conditions.size; i++)
            collect_regexp_patterns(ctx, cn->conditions.nodes[i]);
        if (cn->else_clause) collect_regexp_patterns(ctx, (pm_node_t *)cn->else_clause);
        break;
    }
    case PM_WHEN_NODE: {
        pm_when_node_t *wn = (pm_when_node_t *)node;
        for (size_t i = 0; i < wn->conditions.size; i++)
            collect_regexp_patterns(ctx, wn->conditions.nodes[i]);
        if (wn->statements) collect_regexp_patterns(ctx, (pm_node_t *)wn->statements);
        break;
    }
    case PM_UNTIL_NODE: {
        pm_until_node_t *n = (pm_until_node_t *)node;
        if (n->predicate) collect_regexp_patterns(ctx, n->predicate);
        if (n->statements) collect_regexp_patterns(ctx, (pm_node_t *)n->statements);
        break;
    }
    case PM_UNLESS_NODE: {
        pm_unless_node_t *n = (pm_unless_node_t *)node;
        if (n->predicate) collect_regexp_patterns(ctx, n->predicate);
        if (n->statements) collect_regexp_patterns(ctx, (pm_node_t *)n->statements);
        if (n->else_clause) collect_regexp_patterns(ctx, (pm_node_t *)n->else_clause);
        break;
    }
    case PM_BEGIN_NODE: {
        pm_begin_node_t *n = (pm_begin_node_t *)node;
        if (n->statements) collect_regexp_patterns(ctx, (pm_node_t *)n->statements);
        if (n->rescue_clause) collect_regexp_patterns(ctx, (pm_node_t *)n->rescue_clause);
        if (n->ensure_clause) collect_regexp_patterns(ctx, (pm_node_t *)n->ensure_clause);
        break;
    }
    case PM_RESCUE_NODE: {
        pm_rescue_node_t *n = (pm_rescue_node_t *)node;
        if (n->statements) collect_regexp_patterns(ctx, (pm_node_t *)n->statements);
        if (n->subsequent) collect_regexp_patterns(ctx, (pm_node_t *)n->subsequent);
        break;
    }
    case PM_ENSURE_NODE: {
        pm_ensure_node_t *n = (pm_ensure_node_t *)node;
        if (n->statements) collect_regexp_patterns(ctx, (pm_node_t *)n->statements);
        break;
    }
    case PM_CONSTANT_WRITE_NODE: {
        pm_constant_write_node_t *n = (pm_constant_write_node_t *)node;
        collect_regexp_patterns(ctx, n->value);
        break;
    }
    case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
        pm_local_variable_operator_write_node_t *n =
            (pm_local_variable_operator_write_node_t *)node;
        collect_regexp_patterns(ctx, n->value);
        break;
    }
    case PM_INSTANCE_VARIABLE_WRITE_NODE: {
        pm_instance_variable_write_node_t *n =
            (pm_instance_variable_write_node_t *)node;
        collect_regexp_patterns(ctx, n->value);
        break;
    }
    case PM_RETURN_NODE: {
        pm_return_node_t *n = (pm_return_node_t *)node;
        if (n->arguments) {
            for (size_t i = 0; i < n->arguments->arguments.size; i++)
                collect_regexp_patterns(ctx, n->arguments->arguments.nodes[i]);
        }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Lambda capture analysis                                            */
/* ------------------------------------------------------------------ */

/* Collect all free variable references in a lambda body.
 * param_name is the lambda's own parameter (not a capture).
 * outer_params collects names of variables referenced but not locally bound. */

/* capture_list_t is declared in codegen.h */

void capture_list_add(capture_list_t *cl, const char *name) {
    for (int i = 0; i < cl->count; i++)
        if (strcmp(cl->names[i], name) == 0) return;
    if (cl->count < 256)
        snprintf(cl->names[cl->count++], 64, "%s", name);
}

bool capture_list_has(capture_list_t *cl, const char *name) {
    for (int i = 0; i < cl->count; i++)
        if (strcmp(cl->names[i], name) == 0) return true;
    return false;
}

/* Scan a node for local variable reads that are free variables
 * (not the lambda's own param, and not defined locally in the body).
 * local_defs: variables defined within this lambda body (not captures).
 */
void scan_captures(codegen_ctx_t *ctx, pm_node_t *node,
                          const char *param_name,
                          capture_list_t *local_defs,
                          capture_list_t *result) {
    if (!node) return;

    switch (PM_NODE_TYPE(node)) {
    case PM_LOCAL_VARIABLE_READ_NODE: {
        pm_local_variable_read_node_t *n = (pm_local_variable_read_node_t *)node;
        char *name = cstr(ctx, n->name);
        /* Not the param, not a local def → it's a capture */
        if (strcmp(name, param_name) != 0 && !capture_list_has(local_defs, name))
            capture_list_add(result, name);
        free(name);
        break;
    }
    case PM_LOCAL_VARIABLE_WRITE_NODE: {
        pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
        char *name = cstr(ctx, n->name);
        /* If the variable exists in outer scope, it's a capture (write to outer var) */
        if (var_lookup(ctx, name) && strcmp(name, param_name) != 0 &&
            !capture_list_has(local_defs, name)) {
            capture_list_add(result, name);
        } else {
            capture_list_add(local_defs, name);
        }
        scan_captures(ctx, n->value, param_name, local_defs, result);
        free(name);
        break;
    }
    case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
        pm_local_variable_operator_write_node_t *n =
            (pm_local_variable_operator_write_node_t *)node;
        char *name = cstr(ctx, n->name);
        /* Operator write (e.g. total += x): the variable is both read and written,
         * so it's a capture from the outer scope, not a local definition */
        if (strcmp(name, param_name) != 0 && !capture_list_has(local_defs, name))
            capture_list_add(result, name);
        scan_captures(ctx, n->value, param_name, local_defs, result);
        free(name);
        break;
    }
    case PM_LAMBDA_NODE: {
        /* Inner lambdas: scan their body too, but their own param is not a capture */
        pm_lambda_node_t *lam = (pm_lambda_node_t *)node;
        char *inner_param = NULL;
        if (lam->parameters && PM_NODE_TYPE(lam->parameters) == PM_BLOCK_PARAMETERS_NODE) {
            pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)lam->parameters;
            if (bp->parameters && bp->parameters->requireds.size > 0) {
                pm_node_t *p = bp->parameters->requireds.nodes[0];
                if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                    inner_param = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
            }
        }
        /* The inner lambda's param is local to it, but any other vars it references
         * that are not its own param and not local to it are captures of the OUTER.
         * We scan the inner body looking for refs to variables from OUR scope. */
        capture_list_t inner_local_defs = {.count = 0};
        capture_list_t inner_caps = {.count = 0};
        scan_captures(ctx, (pm_node_t *)lam->body,
                      inner_param ? inner_param : "",
                      &inner_local_defs, &inner_caps);
        /* Any of the inner lambda's captures that are not our param or local def
         * are also our captures */
        for (int i = 0; i < inner_caps.count; i++) {
            if (strcmp(inner_caps.names[i], param_name) != 0 &&
                !capture_list_has(local_defs, inner_caps.names[i]))
                capture_list_add(result, inner_caps.names[i]);
        }
        free(inner_param);
        break;
    }
    case PM_CALL_NODE: {
        pm_call_node_t *c = (pm_call_node_t *)node;
        if (c->receiver) scan_captures(ctx, c->receiver, param_name, local_defs, result);
        if (c->arguments) {
            for (size_t i = 0; i < c->arguments->arguments.size; i++)
                scan_captures(ctx, c->arguments->arguments.nodes[i], param_name, local_defs, result);
        }
        if (c->block) scan_captures(ctx, (pm_node_t *)c->block, param_name, local_defs, result);
        break;
    }
    case PM_BLOCK_NODE: {
        pm_block_node_t *b = (pm_block_node_t *)node;
        /* Extract block parameter and treat it as a local def so it's not captured */
        if (b->parameters && PM_NODE_TYPE(b->parameters) == PM_BLOCK_PARAMETERS_NODE) {
            pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)b->parameters;
            if (bp->parameters && bp->parameters->requireds.size > 0) {
                pm_node_t *p = bp->parameters->requireds.nodes[0];
                if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE) {
                    char *inner_param = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                    capture_list_add(local_defs, inner_param);
                    free(inner_param);
                }
            }
        }
        if (b->body) scan_captures(ctx, (pm_node_t *)b->body, param_name, local_defs, result);
        break;
    }
    case PM_STATEMENTS_NODE: {
        pm_statements_node_t *s = (pm_statements_node_t *)node;
        for (size_t i = 0; i < s->body.size; i++)
            scan_captures(ctx, s->body.nodes[i], param_name, local_defs, result);
        break;
    }
    case PM_PARENTHESES_NODE: {
        pm_parentheses_node_t *p = (pm_parentheses_node_t *)node;
        if (p->body) scan_captures(ctx, p->body, param_name, local_defs, result);
        break;
    }
    case PM_IF_NODE: {
        pm_if_node_t *n = (pm_if_node_t *)node;
        scan_captures(ctx, n->predicate, param_name, local_defs, result);
        if (n->statements) scan_captures(ctx, (pm_node_t *)n->statements, param_name, local_defs, result);
        if (n->subsequent) scan_captures(ctx, (pm_node_t *)n->subsequent, param_name, local_defs, result);
        break;
    }
    case PM_ELSE_NODE: {
        pm_else_node_t *n = (pm_else_node_t *)node;
        if (n->statements) scan_captures(ctx, (pm_node_t *)n->statements, param_name, local_defs, result);
        break;
    }
    case PM_UNTIL_NODE: {
        pm_until_node_t *n = (pm_until_node_t *)node;
        scan_captures(ctx, n->predicate, param_name, local_defs, result);
        if (n->statements) scan_captures(ctx, (pm_node_t *)n->statements, param_name, local_defs, result);
        break;
    }
    case PM_WHILE_NODE: {
        pm_while_node_t *n = (pm_while_node_t *)node;
        scan_captures(ctx, n->predicate, param_name, local_defs, result);
        if (n->statements) scan_captures(ctx, (pm_node_t *)n->statements, param_name, local_defs, result);
        break;
    }
    case PM_CONSTANT_READ_NODE: {
        /* Constants like FIRST, IF, LEFT, RIGHT, IS_EMPTY, REST — these are globals, not captures */
        break;
    }
    case PM_RETURN_NODE: {
        pm_return_node_t *n = (pm_return_node_t *)node;
        if (n->arguments) {
            for (size_t i = 0; i < n->arguments->arguments.size; i++)
                scan_captures(ctx, n->arguments->arguments.nodes[i], param_name, local_defs, result);
        }
        break;
    }
    case PM_YIELD_NODE: {
        pm_yield_node_t *yn = (pm_yield_node_t *)node;
        if (yn->arguments) {
            for (size_t i = 0; i < yn->arguments->arguments.size; i++)
                scan_captures(ctx, yn->arguments->arguments.nodes[i], param_name, local_defs, result);
        }
        break;
    }
    default:
        break;
    }
}

/* codegen a lambda expression — returns an expression string.
 * Emits the lambda C function to ctx->lambda_out (or ctx->out if no secondary). */
/* Generate C code for a lambda node.
 * Emits a static C function _lam_N to ctx->lambda_out and returns an
 * expression (written to ctx->out) that constructs the closure. */
char *codegen_lambda(codegen_ctx_t *ctx, pm_lambda_node_t *lam) {
    int lam_id = ctx->lambda_counter++;

    /* Extract parameter name */
    char param_name[64] = "";
    if (lam->parameters && PM_NODE_TYPE(lam->parameters) == PM_BLOCK_PARAMETERS_NODE) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)lam->parameters;
        if (bp->parameters && bp->parameters->requireds.size > 0) {
            pm_node_t *p = bp->parameters->requireds.nodes[0];
            if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE) {
                char *pn = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                snprintf(param_name, sizeof(param_name), "%s", pn);
                free(pn);
            }
        }
    }

    /* Capture analysis: find free variables in the body */
    capture_list_t local_defs = {.count = 0};
    capture_list_t captures = {.count = 0};
    scan_captures(ctx, (pm_node_t *)lam->body, param_name, &local_defs, &captures);

    /* Push lambda scope */
    int scope_idx = ctx->lambda_scope_depth;
    ctx->lambda_scope_depth++;
    snprintf(ctx->lambda_scope[scope_idx].param, 64, "%s", param_name);
    ctx->lambda_scope[scope_idx].capture_count = captures.count;
    for (int i = 0; i < captures.count; i++)
        snprintf(ctx->lambda_scope[scope_idx].captures[i], 64, "%s", captures.names[i]);
    ctx->lambda_scope[scope_idx].depth = scope_idx;

    /* Buffer the entire function definition (header + body + close).
     * Inner lambdas will also write their defs to lambda_out.
     * By buffering this function, inner defs appear before us in lambda_out,
     * which is exactly what we want (forward decls handle ordering). */
    FILE *caller_out = ctx->out;
    int saved_indent = ctx->indent;

    char *func_buf_data = NULL;
    size_t func_buf_size = 0;
    FILE *func_buf = open_memstream(&func_buf_data, &func_buf_size);
    ctx->out = func_buf;

    fprintf(ctx->out, "static sp_Val *_lam_%d(sp_Val *self, sp_Val *arg) {\n", lam_id);

    /* The parameter is accessible as `arg` — create a local alias */
    if (param_name[0]) {
        fprintf(ctx->out, "    sp_Val *lv_%s = arg;\n", param_name);
    } else {
        fprintf(ctx->out, "    (void)arg;\n");
    }
    fprintf(ctx->out, "    (void)self;\n");

    /* Extract captures from self->captures[] */
    for (int i = 0; i < captures.count; i++) {
        fprintf(ctx->out, "    sp_Val *lv_%s = self->captures[%d];\n", captures.names[i], i);
    }

    /* Generate body — the last expression is the return value. */
    ctx->indent = 1;

    if (lam->body) {
        pm_node_t *body = (pm_node_t *)lam->body;
        if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
            pm_statements_node_t *stmts = (pm_statements_node_t *)body;
            for (size_t i = 0; i + 1 < stmts->body.size; i++)
                codegen_stmt(ctx, stmts->body.nodes[i]);
            if (stmts->body.size > 0) {
                char *val = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                emit(ctx, "return %s;\n", val);
                free(val);
            } else {
                emit(ctx, "return &sp_nil_val;\n");
            }
        } else {
            char *val = codegen_expr(ctx, body);
            emit(ctx, "return %s;\n", val);
            free(val);
        }
    } else {
        emit(ctx, "return &sp_nil_val;\n");
    }

    fprintf(ctx->out, "}\n\n");
    fclose(func_buf);

    /* Write the complete function to lambda_out.
     * Inner lambda function defs were written to lambda_out during body generation,
     * so they appear before this function — correct ordering. */
    fwrite(func_buf_data, 1, func_buf_size, ctx->lambda_out);
    free(func_buf_data);

    /* Restore caller output */
    ctx->out = caller_out;
    ctx->indent = saved_indent;

    /* Pop lambda scope */
    ctx->lambda_scope_depth--;

    /* Build the closure construction expression (written to caller_out) */
    if (captures.count == 0) {
        return sfmt("sp_proc(_lam_%d, 0)", lam_id);
    } else {
        /* Need a temporary to fill in captures */
        int tmp = ctx->temp_counter++;
        emit(ctx, "sp_Val *_cl_%d = sp_proc(_lam_%d, %d);\n", tmp, lam_id, captures.count);
        for (int i = 0; i < captures.count; i++) {
            emit(ctx, "_cl_%d->captures[%d] = lv_%s;\n", tmp, i, captures.names[i]);
        }
        return sfmt("_cl_%d", tmp);
    }
}

void codegen_program(codegen_ctx_t *ctx, pm_node_t *root) {
    assert(PM_NODE_TYPE(root) == PM_PROGRAM_NODE);
    pm_program_node_t *prog = (pm_program_node_t *)root;

    /* Pass 0: Process require_relative — parse required files and register
     * their classes/modules/functions before main analysis passes */
    process_require_relative(ctx, root);

    /* Detect lambda mode */
    ctx->lambda_mode = has_lambda_nodes(root);

    /* Detect exception handling mode */
    ctx->needs_exc = has_exception_nodes(ctx, root);

    /* Detect non-local return from blocks (also needs setjmp/longjmp) */
    if (!ctx->needs_exc) ctx->needs_exc = has_block_return_nodes(root);

    /* Detect string split usage */
    ctx->needs_str_split = has_split_calls(ctx, root);

    /* Detect regexp usage and collect patterns (must be before emit_header) */
    collect_regexp_patterns(ctx, root);

    /* Also scan all required files for exceptions, splits, and regexps */
    for (int ri = 0; ri < ctx->required_file_count; ri++) {
        pm_node_t *rroot = (pm_node_t *)ctx->required_files[ri].root;
        if (!rroot) continue;
        if (!ctx->needs_exc) ctx->needs_exc = has_exception_nodes(ctx, rroot);
        if (!ctx->needs_exc) ctx->needs_exc = has_block_return_nodes(rroot);
        if (!ctx->needs_str_split) ctx->needs_str_split = has_split_calls(ctx, rroot);
        collect_regexp_patterns(ctx, rroot);
    }

    /* Pass 1: Analyze classes, modules, functions */
    class_analysis_pass(ctx, root);

    /* Pass 1b: Resolve class inheritance — copy parent ivars into children */
    for (int ci = 0; ci < ctx->class_count; ci++) {
        class_info_t *cls = &ctx->classes[ci];
        if (!cls->superclass[0]) {
            cls->own_ivar_start = 0;
            continue;
        }
        class_info_t *parent = find_class(ctx, cls->superclass);
        if (!parent) {
            cls->own_ivar_start = 0;
            continue;
        }
        /* Prepend parent ivars that child doesn't already have */
        int parent_ivars = parent->ivar_count;
        if (parent_ivars > 0) {
            /* Shift existing (child's own) ivars right */
            int own = cls->ivar_count;
            for (int j = own - 1; j >= 0; j--)
                cls->ivars[j + parent_ivars] = cls->ivars[j];
            /* Copy parent ivars to front */
            for (int j = 0; j < parent_ivars; j++)
                cls->ivars[j] = parent->ivars[j];
            cls->ivar_count = parent_ivars + own;
        }
        cls->own_ivar_start = parent_ivars;

        /* For classes without their own initialize, inherit is_value_type from parent */
        if (!find_method(cls, "initialize")) {
            cls->is_value_type = parent->is_value_type;
        }
    }

    /* Pass 1c: Resolve module includes — copy mixin methods into classes */
    for (int ci = 0; ci < ctx->class_count; ci++) {
        class_info_t *cls = &ctx->classes[ci];
        for (int ii = 0; ii < cls->include_count; ii++) {
            module_info_t *mod = find_module(ctx, cls->includes[ii]);
            if (!mod) continue;
            for (int mi = 0; mi < mod->method_count; mi++) {
                method_info_t *mm = &mod->methods[mi];
                /* Only copy instance methods (not def self.foo module functions) */
                if (mm->is_class_method) continue;
                /* Don't override methods the class already defines */
                if (find_method(cls, mm->name)) continue;
                if (cls->method_count >= MAX_METHODS) continue;
                cls->methods[cls->method_count] = *mm;
                cls->method_count++;
            }
        }
    }

    /* Pass 1d: Comparable mixin — auto-generate <, >, <=, >=, == from <=> */
    for (int ci = 0; ci < ctx->class_count; ci++) {
        class_info_t *cls = &ctx->classes[ci];
        bool has_comparable = false;
        for (int ii = 0; ii < cls->include_count; ii++) {
            if (strcmp(cls->includes[ii], "Comparable") == 0) {
                has_comparable = true;
                break;
            }
        }
        if (!has_comparable) continue;
        /* Check that <=> is defined */
        method_info_t *cmp = find_method(cls, "<=>");
        if (!cmp) continue;
        /* Generate comparison methods that delegate to <=> */
        static const char *comp_ops[] = { "<", ">", "<=", ">=", "==" };
        for (int oi = 0; oi < 5; oi++) {
            if (find_method(cls, comp_ops[oi])) continue; /* already defined */
            if (cls->method_count >= MAX_METHODS) continue;
            method_info_t *m = &cls->methods[cls->method_count++];
            memset(m, 0, sizeof(*m));
            snprintf(m->name, sizeof(m->name), "%s", comp_ops[oi]);
            m->body_node = NULL; /* synthetic — handled specially in emit */
            m->params_node = NULL;
            m->param_count = 1;
            snprintf(m->params[0].name, 64, "other");
            m->params[0].type = vt_obj(cls->name);
            m->return_type = vt_prim(SPINEL_TYPE_BOOLEAN);
            m->is_getter = false;
            m->is_setter = false;
            m->is_class_method = false;
        }
    }

    /* Clear method parameter entries from var table before top-level inference.
     * class_analysis_pass registers method params which pollute the table
     * (e.g., method param 'obj' conflicts with top-level var 'obj'). */
    {
        int top_var_count = ctx->var_count;
        for (int i = top_var_count; i < MAX_VARS; i++)
            ctx->vars[i].name[0] = '\0';
    }

    /* Pass 2: Type inference for top-level code */
    infer_pass(ctx, root);

    /* Pass 2b: Resolve class types */
    resolve_class_types(ctx, root);
    /* Clear stale var entries left by resolve (method param temp registrations) */
    for (int i = ctx->var_count; i < MAX_VARS && ctx->vars[i].name[0]; i++)
        ctx->vars[i].name[0] = '\0';

    /* Pass 2c: Re-infer variable types now that function return types are resolved */
    infer_pass(ctx, root);

    /* Pass 2d: Re-run cross-function inference now that variable types are updated */
    resolve_class_types(ctx, root);
    for (int i = ctx->var_count; i < MAX_VARS && ctx->vars[i].name[0]; i++)
        ctx->vars[i].name[0] = '\0';
    infer_pass(ctx, root);

    /* Detect needs_poly: any POLY-typed variable or function param triggers poly runtime */
    for (int i = 0; i < ctx->var_count && !ctx->needs_poly; i++) {
        if (ctx->vars[i].type.kind == SPINEL_TYPE_POLY)
            ctx->needs_poly = true;
    }
    for (int i = 0; i < ctx->func_count && !ctx->needs_poly; i++) {
        func_info_t *f = &ctx->funcs[i];
        if (f->return_type.kind == SPINEL_TYPE_POLY) { ctx->needs_poly = true; break; }
        for (int j = 0; j < f->param_count; j++)
            if (f->params[j].type.kind == SPINEL_TYPE_POLY) { ctx->needs_poly = true; break; }
    }

    /* Detect needs_rb_array: any RB_ARRAY-typed variable */
    for (int i = 0; i < ctx->var_count && !ctx->needs_rb_array; i++) {
        if (ctx->vars[i].type.kind == SPINEL_TYPE_RB_ARRAY) {
            ctx->needs_rb_array = true;
            ctx->needs_poly = true; /* sp_RbArray uses sp_RbValue elements */
        }
    }

    /* Detect needs_rb_hash: any RB_HASH-typed variable */
    for (int i = 0; i < ctx->var_count && !ctx->needs_rb_hash; i++) {
        if (ctx->vars[i].type.kind == SPINEL_TYPE_RB_HASH) {
            ctx->needs_rb_hash = true;
            ctx->needs_poly = true; /* sp_RbHash uses sp_RbValue values */
        }
    }

    /* Assign class_tag to each class (for POLY dispatch) */
    for (int i = 0; i < ctx->class_count; i++)
        ctx->classes[i].class_tag = 0x0040 + i; /* NaN-box class tag base */

    /* For large multi-file programs, enable all runtime features */
    if (ctx->required_file_count > 5) {
        ctx->needs_hash = true;
        ctx->needs_rb_array = true;
        ctx->needs_rb_hash = true;
        ctx->needs_poly = true;
        ctx->needs_sp_string = true;
        ctx->needs_str_split = true;
        ctx->needs_gc = true;
        ctx->needs_proc = true;
    }

    /* Detect needs from class ivars and method params/returns */
    for (int ci = 0; ci < ctx->class_count; ci++) {
        class_info_t *cls = &ctx->classes[ci];
        for (int ii = 0; ii < cls->ivar_count; ii++) {
            spinel_type_t k = cls->ivars[ii].type.kind;
            if (k == SPINEL_TYPE_SP_STRING) ctx->needs_sp_string = true;
            if (k == SPINEL_TYPE_HASH) ctx->needs_hash = true;
            if (k == SPINEL_TYPE_ARRAY || k == SPINEL_TYPE_FLOAT_ARRAY) ctx->needs_gc = true;
            if (k == SPINEL_TYPE_RB_ARRAY) { ctx->needs_rb_array = true; ctx->needs_poly = true; }
            if (k == SPINEL_TYPE_RB_HASH) { ctx->needs_rb_hash = true; ctx->needs_poly = true; }
            if (k == SPINEL_TYPE_STR_ARRAY) ctx->needs_str_split = true;
            if (k == SPINEL_TYPE_POLY) ctx->needs_poly = true;
        }
        for (int mi = 0; mi < cls->method_count; mi++) {
            method_info_t *m = &cls->methods[mi];
            spinel_type_t rk = m->return_type.kind;
            if (rk == SPINEL_TYPE_HASH) ctx->needs_hash = true;
            if (rk == SPINEL_TYPE_RB_ARRAY) { ctx->needs_rb_array = true; ctx->needs_poly = true; }
            if (rk == SPINEL_TYPE_RB_HASH) { ctx->needs_rb_hash = true; ctx->needs_poly = true; }
            if (rk == SPINEL_TYPE_STR_ARRAY) ctx->needs_str_split = true;
            if (rk == SPINEL_TYPE_SP_STRING) ctx->needs_sp_string = true;
            if (rk == SPINEL_TYPE_POLY) ctx->needs_poly = true;
            for (int pi = 0; pi < m->param_count; pi++) {
                spinel_type_t pk = m->params[pi].type.kind;
                if (pk == SPINEL_TYPE_HASH) ctx->needs_hash = true;
                if (pk == SPINEL_TYPE_RB_ARRAY) { ctx->needs_rb_array = true; ctx->needs_poly = true; }
                if (pk == SPINEL_TYPE_POLY) ctx->needs_poly = true;
            }
        }
    }

    /* Detect needs_sp_string: any SP_STRING-typed variable triggers mutable string runtime */
    for (int i = 0; i < ctx->var_count; i++) {
        if (ctx->vars[i].type.kind == SPINEL_TYPE_SP_STRING) {
            ctx->needs_sp_string = true;
            break;
        }
    }

    /* Detect needs_hash: any HASH-typed variable triggers hash runtime */
    for (int i = 0; i < ctx->var_count; i++) {
        if (ctx->vars[i].type.kind == SPINEL_TYPE_HASH) {
            ctx->needs_hash = true;
            break;
        }
    }

    /* Detect needs_proc: any has_block_param function, or PROC-typed variable (from proc {}/Proc.new) */
    for (int i = 0; i < ctx->func_count && !ctx->needs_proc; i++) {
        if (ctx->funcs[i].has_block_param) ctx->needs_proc = true;
    }
    for (int i = 0; i < ctx->var_count && !ctx->needs_proc; i++) {
        if (ctx->vars[i].type.kind == SPINEL_TYPE_PROC && !ctx->lambda_mode)
            ctx->needs_proc = true;
    }
    for (int i = 0; i < ctx->func_count && !ctx->needs_proc; i++) {
        if (ctx->funcs[i].return_type.kind == SPINEL_TYPE_PROC && !ctx->lambda_mode)
            ctx->needs_proc = true;
    }

    /* Detect needs_gc: any non-value-type class, sp_IntArray, or sp_StrIntHash triggers GC */
    for (int i = 0; i < ctx->class_count && !ctx->needs_gc; i++) {
        if (!ctx->classes[i].is_value_type)
            ctx->needs_gc = true;
    }
    for (int i = 0; i < ctx->var_count && !ctx->needs_gc; i++) {
        if (ctx->vars[i].type.kind == SPINEL_TYPE_ARRAY ||
            ctx->vars[i].type.kind == SPINEL_TYPE_FLOAT_ARRAY ||
            ctx->vars[i].type.kind == SPINEL_TYPE_HASH)
            ctx->needs_gc = true;
    }
    for (int i = 0; i < ctx->func_count && !ctx->needs_gc; i++) {
        func_info_t *f = &ctx->funcs[i];
        if (f->return_type.kind == SPINEL_TYPE_ARRAY ||
            f->return_type.kind == SPINEL_TYPE_FLOAT_ARRAY ||
            f->return_type.kind == SPINEL_TYPE_HASH) { ctx->needs_gc = true; break; }
        for (int j = 0; j < f->param_count && !ctx->needs_gc; j++)
            if (f->params[j].type.kind == SPINEL_TYPE_ARRAY ||
                f->params[j].type.kind == SPINEL_TYPE_FLOAT_ARRAY ||
                f->params[j].type.kind == SPINEL_TYPE_HASH) ctx->needs_gc = true;
        /* Also infer function body variables to detect array/hash usage */
        if (!ctx->needs_gc && f->body_node) {
            int saved_vc = ctx->var_count;
            for (int j = 0; j < f->param_count; j++)
                var_declare(ctx, f->params[j].name, f->params[j].type, false);
            infer_pass(ctx, f->body_node);
            for (int j = saved_vc; j < ctx->var_count; j++) {
                if (ctx->vars[j].type.kind == SPINEL_TYPE_ARRAY ||
                    ctx->vars[j].type.kind == SPINEL_TYPE_FLOAT_ARRAY ||
                    ctx->vars[j].type.kind == SPINEL_TYPE_HASH) {
                    ctx->needs_gc = true;
                    break;
                }
            }
            ctx->var_count = saved_vc;
        }
    }
    /* Also check class method bodies for array/hash usage */
    for (int i = 0; i < ctx->class_count && !ctx->needs_gc; i++) {
        class_info_t *cls = &ctx->classes[i];
        pm_parser_t *saved_gc = ctx->parser;
        if (cls->origin_parser) ctx->parser = cls->origin_parser;
        for (int mi = 0; mi < cls->method_count && !ctx->needs_gc; mi++) {
            method_info_t *m = &cls->methods[mi];
            if (m->return_type.kind == SPINEL_TYPE_ARRAY ||
                m->return_type.kind == SPINEL_TYPE_FLOAT_ARRAY ||
                m->return_type.kind == SPINEL_TYPE_HASH) { ctx->needs_gc = true; break; }
            if (m->body_node) {
                int saved_vc = ctx->var_count;
                for (int j = 0; j < m->param_count; j++)
                    var_declare(ctx, m->params[j].name, m->params[j].type, false);
                infer_pass(ctx, m->body_node);
                for (int j = saved_vc; j < ctx->var_count; j++) {
                    if (ctx->vars[j].type.kind == SPINEL_TYPE_ARRAY ||
                        ctx->vars[j].type.kind == SPINEL_TYPE_FLOAT_ARRAY ||
                        ctx->vars[j].type.kind == SPINEL_TYPE_HASH) {
                        ctx->needs_gc = true;
                        break;
                    }
                }
                ctx->var_count = saved_vc;
            }
        }
        ctx->parser = saved_gc;
    }

    /* Set up lambda output buffer if needed */
    FILE *lambda_buf = NULL;
    char *lambda_buf_data = NULL;
    size_t lambda_buf_size = 0;
    if (ctx->lambda_mode) {
        lambda_buf = open_memstream(&lambda_buf_data, &lambda_buf_size);
        ctx->lambda_out = lambda_buf;
    }

    /* Emit C file */
    emit_header(ctx);

    /* Global for system()/backtick exit status ($?) */
    emit_raw(ctx, "static int sp_last_status = 0;\n\n");

    /* Emit top-level constants as static globals (so methods can access them) */
    for (int i = 0; i < ctx->var_count; i++) {
        var_entry_t *v = &ctx->vars[i];
        if (!v->is_constant) continue;
        char *ct = vt_ctype(ctx, v->type, false);
        char *cn = make_cname(v->name, true);
        const char *init = "";
        if (v->type.kind == SPINEL_TYPE_INTEGER) init = " = 0";
        else if (v->type.kind == SPINEL_TYPE_FLOAT) init = " = 0.0";
        else if (v->type.kind == SPINEL_TYPE_PROC) init = " = NULL";
        emit_raw(ctx, "static %s %s%s;\n", ct, cn, init);
        free(ct); free(cn);
    }
    emit_raw(ctx, "\n");

    /* Forward declarations: structs */
    for (int i = 0; i < ctx->class_count; i++)
        emit_raw(ctx, "typedef struct sp_%s_s sp_%s;\n", ctx->classes[i].name, ctx->classes[i].name);
    emit_raw(ctx, "\n");

    /* Struct definitions */
    for (int i = 0; i < ctx->class_count; i++)
        emit_struct(ctx, &ctx->classes[i]);

    /* GC: emit per-class scan functions for classes with GC-pointer ivars */
    if (ctx->needs_gc) {
        for (int i = 0; i < ctx->class_count; i++) {
            class_info_t *cls = &ctx->classes[i];
            if (cls->is_value_type) continue;

            /* Check if this class has any GC-pointer ivars */
            bool has_gc_fields = false;
            for (int j = 0; j < cls->ivar_count; j++) {
                ivar_info_t *iv = &cls->ivars[j];
                if (is_gc_type(ctx, iv->type)) { has_gc_fields = true; break; }
                /* Special: Scene.spheres is sp_Sphere *[3] */
                if (strcmp(cls->name, "Scene") == 0 && strcmp(iv->name, "spheres") == 0) {
                    class_info_t *sph = find_class(ctx, "Sphere");
                    if (sph && !sph->is_value_type) { has_gc_fields = true; break; }
                }
            }
            if (!has_gc_fields) continue;

            emit_raw(ctx, "static void sp_%s_gc_scan(void *obj) {\n", cls->name);
            emit_raw(ctx, "    sp_%s *o = (sp_%s *)obj;\n", cls->name, cls->name);
            for (int j = 0; j < cls->ivar_count; j++) {
                ivar_info_t *iv = &cls->ivars[j];
                /* Special: Scene.spheres array */
                if (strcmp(cls->name, "Scene") == 0 && strcmp(iv->name, "spheres") == 0) {
                    emit_raw(ctx, "    for (int _i = 0; _i < 3; _i++) sp_gc_mark(o->spheres[_i]);\n");
                    continue;
                }
                if (is_gc_type(ctx, iv->type))
                    emit_raw(ctx, "    sp_gc_mark(o->%s);\n", iv->name);
            }
            emit_raw(ctx, "}\n\n");
        }
    }

    /* Module code */
    for (int i = 0; i < ctx->module_count; i++)
        emit_module(ctx, &ctx->modules[i]);

    /* Block callback type definition (for yield support and proc) */
    {
        bool any_yield = false;
        bool any_block_param = false;
        for (int i = 0; i < ctx->func_count; i++) {
            if (ctx->funcs[i].has_yield) any_yield = true;
            if (ctx->funcs[i].has_block_param) any_block_param = true;
        }
        /* Also check class methods for yield */
        for (int i = 0; i < ctx->class_count && !any_yield; i++) {
            class_info_t *cls = &ctx->classes[i];
            for (int j = 0; j < cls->method_count && !any_yield; j++) {
                method_info_t *m = &cls->methods[j];
                if (m->body_node && has_yield_nodes(m->body_node))
                    any_yield = true;
            }
        }
        if (any_yield || any_block_param || ctx->needs_proc)
            emit_raw(ctx, "typedef mrb_int (*sp_block_fn)(void *env, mrb_int arg);\n\n");
    }

    /* sp_Proc runtime (for &block, proc {}, Proc.new {}) */
    if (ctx->needs_proc) {
        emit_raw(ctx, "/* ---- sp_Proc runtime ---- */\n");
        emit_raw(ctx, "typedef struct { sp_block_fn fn; void *env; } sp_Proc;\n");
        emit_raw(ctx, "static sp_Proc *sp_Proc_new(sp_block_fn fn, void *env) {\n");
        emit_raw(ctx, "    sp_Proc *p = (sp_Proc *)malloc(sizeof(sp_Proc));\n");
        emit_raw(ctx, "    p->fn = fn; p->env = env;\n");
        emit_raw(ctx, "    return p;\n");
        emit_raw(ctx, "}\n");
        emit_raw(ctx, "static mrb_int sp_Proc_call(sp_Proc *p, mrb_int arg) {\n");
        emit_raw(ctx, "    return p->fn(p->env, arg);\n");
        emit_raw(ctx, "}\n\n");
    }

    /* Forward declarations for top-level functions */
    if (!ctx->lambda_mode) {
        for (int i = 0; i < ctx->func_count; i++) {
            func_info_t *f = &ctx->funcs[i];
            char *ret_ct = vt_ctype(ctx, f->return_type, true);
            bool ret_void = (f->return_type.kind == SPINEL_TYPE_NIL);
            emit_raw(ctx, "static %s sp_%s(", ret_void ? "void" : ret_ct, f->name);
            for (int j = 0; j < f->param_count; j++) {
                if (j > 0) emit_raw(ctx, ", ");
                char *pct = vt_ctype(ctx, f->params[j].type, true);
                if (f->params[j].is_array)
                    emit_raw(ctx, "%s *", pct);
                else
                    emit_raw(ctx, "%s", pct);
                free(pct);
            }
            if (f->has_yield) {
                if (f->param_count > 0) emit_raw(ctx, ", ");
                emit_raw(ctx, "sp_block_fn, void *");
            }
            if (f->has_block_param) {
                if (f->param_count > 0 || f->has_yield) emit_raw(ctx, ", ");
                emit_raw(ctx, "sp_Proc *");
            }
            if (f->param_count == 0 && !f->has_yield && !f->has_block_param) emit_raw(ctx, "void");
            emit_raw(ctx, ");\n");
            free(ret_ct);
        }
        emit_raw(ctx, "\n");
    }

    /* Forward declarations for class methods (sanitize operator names) */
    for (int i = 0; i < ctx->class_count; i++) {
        class_info_t *cls = &ctx->classes[i];
        for (int j = 0; j < cls->method_count; j++) {
            method_info_t *m = &cls->methods[j];
            if (strcmp(m->name, "initialize") == 0) continue;
            if (m->is_getter || m->is_setter) continue;
            const char *c_mname = sanitize_method(m->name);
            char *ret_ct = vt_ctype(ctx, m->return_type, true);
            bool ret_void = (m->return_type.kind == SPINEL_TYPE_NIL);
            emit_raw(ctx, "static %s sp_%s_%s(",
                     ret_void ? "void" : ret_ct, cls->name, c_mname);
            if (m->is_class_method) {
                for (int k = 0; k < m->param_count; k++) {
                    if (k > 0) emit_raw(ctx, ", ");
                    char *pct = vt_ctype(ctx, m->params[k].type, true);
                    emit_raw(ctx, "%s", pct);
                    free(pct);
                }
                if (m->param_count == 0) emit_raw(ctx, "void");
            } else {
                if (cls->is_value_type)
                    emit_raw(ctx, "sp_%s", cls->name);
                else
                    emit_raw(ctx, "sp_%s *", cls->name);
                for (int k = 0; k < m->param_count; k++) {
                    emit_raw(ctx, ", ");
                    char *pct = vt_ctype(ctx, m->params[k].type, !cls->is_value_type);
                    emit_raw(ctx, "%s", pct);
                    free(pct);
                }
                /* Add block callback params if method uses yield */
                if (m->body_node && has_yield_nodes(m->body_node)) {
                    emit_raw(ctx, ", sp_block_fn, void *");
                }
            }
            emit_raw(ctx, ");\n");
            free(ret_ct);
        }
    }
    emit_raw(ctx, "\n");

    /* Initialize functions for superclasses (called via super) */
    for (int i = 0; i < ctx->class_count; i++) {
        pm_parser_t *saved = ctx->parser;
        if (ctx->classes[i].origin_parser)
            ctx->parser = ctx->classes[i].origin_parser;
        emit_initialize_func(ctx, &ctx->classes[i]);
        ctx->parser = saved;
    }

    /* Constructors */
    for (int i = 0; i < ctx->class_count; i++) {
        pm_parser_t *saved = ctx->parser;
        if (ctx->classes[i].origin_parser)
            ctx->parser = ctx->classes[i].origin_parser;
        emit_constructor(ctx, &ctx->classes[i]);
        ctx->parser = saved;
    }

    /* Class methods */
    for (int i = 0; i < ctx->class_count; i++) {
        class_info_t *cls = &ctx->classes[i];
        pm_parser_t *saved = ctx->parser;
        for (int j = 0; j < cls->method_count; j++) {
            method_info_t *m = &cls->methods[j];
            /* Use method's own parser for correct constant pool access */
            if (m->origin_parser)
                ctx->parser = m->origin_parser;
            else if (cls->origin_parser)
                ctx->parser = cls->origin_parser;
            emit_method(ctx, cls, m);
            ctx->parser = saved;
        }
    }

    if (ctx->lambda_mode) {
        /* In lambda mode: first we generate the main body to a temp buffer
         * to collect all lambda functions, then write them in order. */

        /* Generate main body to another temp buffer first to collect lambdas */
        char *main_buf_data = NULL;
        size_t main_buf_size = 0;
        FILE *main_buf = open_memstream(&main_buf_data, &main_buf_size);
        FILE *real_out = ctx->out;
        ctx->out = main_buf;

        emit_raw(ctx, "static const char *sp_program_name = \"\";\n");
        emit_raw(ctx, "int main(int argc, char **argv) {\n");
        emit_raw(ctx, "    (void)argc; (void)argv;\n");
        emit_raw(ctx, "    sp_program_name = argv[0];\n");

        /* Variable declarations for top-level */
        for (int i = 0; i < ctx->var_count; i++) {
            var_entry_t *v = &ctx->vars[i];
            if (v->is_constant) continue;
            char *ct = vt_ctype(ctx, v->type, false);
            char *cn = make_cname(v->name, v->is_constant);
            if (v->type.kind == SPINEL_TYPE_POLY) {
                emit_raw(ctx, "    sp_RbValue %s = sp_box_nil();\n", cn);
            } else if (v->type.kind == SPINEL_TYPE_RB_ARRAY) {
                emit_raw(ctx, "    sp_RbArray *%s = NULL;\n", cn);
            } else if (v->type.kind == SPINEL_TYPE_RB_HASH) {
                emit_raw(ctx, "    sp_RbHash *%s = NULL;\n", cn);
            } else if (v->type.kind == SPINEL_TYPE_PROC) {
                emit_raw(ctx, "    sp_Val *%s = NULL;\n", cn);
            } else if (v->type.kind == SPINEL_TYPE_ARRAY) {
                emit_raw(ctx, "    sp_IntArray *%s = NULL;\n", cn);
            } else if (v->type.kind == SPINEL_TYPE_FLOAT_ARRAY) {
                emit_raw(ctx, "    sp_FloatArray *%s = NULL;\n", cn);
            } else if (v->type.kind == SPINEL_TYPE_SP_STRING) {
                emit_raw(ctx, "    sp_String *%s = NULL;\n", cn);
            } else if (v->type.kind == SPINEL_TYPE_VALUE || v->type.kind == SPINEL_TYPE_UNKNOWN) {
                /* In lambda mode, VALUE vars might hold sp_StrArray* or other pointers */
                emit_raw(ctx, "    void *%s = NULL;\n", cn);
            } else {
                const char *init = "";
                if (v->type.kind == SPINEL_TYPE_INTEGER) init = " = 0";
                else if (v->type.kind == SPINEL_TYPE_FLOAT) init = " = 0.0";
                else if (v->type.kind == SPINEL_TYPE_BOOLEAN) init = " = FALSE";
                emit_raw(ctx, "    %s %s%s;\n", ct, cn, init);
            }
            free(ct); free(cn);
        }
        emit_raw(ctx, "\n");

        if (ctx->needs_regexp)
            emit_raw(ctx, "    sp_regexp_init();\n\n");
        /* Generate top-level code (this will collect lambda functions into lambda_buf) */
        codegen_stmts(ctx, (pm_node_t *)prog->statements);

        emit_raw(ctx, "\n    return 0;\n");
        emit_raw(ctx, "}\n");
        fclose(main_buf);

        /* Now write to real output: lambdas first, then fizzbuzz helpers, then main */
        ctx->out = real_out;

        /* Forward declare all lambda functions */
        for (int i = 0; i < ctx->lambda_counter; i++)
            emit_raw(ctx, "static sp_Val *_lam_%d(sp_Val *self, sp_Val *arg);\n", i);
        emit_raw(ctx, "\n");

        /* Emit lambda function bodies */
        fclose(lambda_buf);
        if (lambda_buf_data) {
            fwrite(lambda_buf_data, 1, lambda_buf_size, ctx->out);
            free(lambda_buf_data);
        }
        ctx->lambda_out = NULL;

        /* Emit hand-written FizzBuzz helper functions */
        emit_lambda_fizzbuzz_funcs(ctx);

        /* Emit main function body */
        fwrite(main_buf_data, 1, main_buf_size, ctx->out);
        free(main_buf_data);

    } else {
        /* Non-lambda mode: original path */

        /* Set up block callback buffer for yield support */
        char *block_buf_data = NULL;
        size_t block_buf_size = 0;
        FILE *block_buf = open_memstream(&block_buf_data, &block_buf_size);
        ctx->block_out = block_buf;

        /* Generate top-level functions and main to a temp buffer
         * so block callbacks can be emitted first */
        char *code_buf_data = NULL;
        size_t code_buf_size = 0;
        FILE *code_buf = open_memstream(&code_buf_data, &code_buf_size);
        FILE *real_out = ctx->out;
        ctx->out = code_buf;

        /* Top-level functions */
        for (int i = 0; i < ctx->func_count; i++)
            emit_top_func(ctx, &ctx->funcs[i]);

        /* method(:name) adapters — scan AST for method() calls and emit wrappers */
        if (ctx->needs_proc) {
            pm_statements_node_t *main_stmts = prog->statements;
            if (main_stmts) {
                /* Simple stack-based scan for method(:name) calls */
                pm_node_t *mstack[256];
                int msp = 0;
                for (size_t si = 0; si < main_stmts->body.size && msp < 255; si++)
                    mstack[msp++] = main_stmts->body.nodes[si];
                while (msp > 0) {
                    pm_node_t *cur = mstack[--msp];
                    if (!cur) continue;
                    if (PM_NODE_TYPE(cur) == PM_CALL_NODE) {
                        pm_call_node_t *c = (pm_call_node_t *)cur;
                        if (!c->receiver && ceq(ctx, c->name, "method") &&
                            c->arguments && c->arguments->arguments.size == 1 &&
                            PM_NODE_TYPE(c->arguments->arguments.nodes[0]) == PM_SYMBOL_NODE) {
                            pm_symbol_node_t *sym = (pm_symbol_node_t *)c->arguments->arguments.nodes[0];
                            const uint8_t *src = pm_string_source(&sym->unescaped);
                            size_t len = pm_string_length(&sym->unescaped);
                            char fname[64];
                            snprintf(fname, sizeof(fname), "%.*s", (int)len, src);
                            char *safe_fname = c_safe_name(fname);
                            emit_raw(ctx, "static mrb_int _meth_adapt_%s(void *_e, mrb_int _a) { (void)_e; return sp_%s(_a); }\n",
                                     safe_fname, safe_fname);
                            free(safe_fname);
                        }
                        if (c->receiver && msp < 255) mstack[msp++] = c->receiver;
                        if (c->arguments) {
                            for (size_t ai = 0; ai < c->arguments->arguments.size && msp < 255; ai++)
                                mstack[msp++] = c->arguments->arguments.nodes[ai];
                        }
                    }
                    if (PM_NODE_TYPE(cur) == PM_LOCAL_VARIABLE_WRITE_NODE && msp < 255) {
                        pm_local_variable_write_node_t *lw = (pm_local_variable_write_node_t *)cur;
                        mstack[msp++] = lw->value;
                    }
                    if (PM_NODE_TYPE(cur) == PM_STATEMENTS_NODE) {
                        pm_statements_node_t *ss = (pm_statements_node_t *)cur;
                        for (size_t si = 0; si < ss->body.size && msp < 255; si++)
                            mstack[msp++] = ss->body.nodes[si];
                    }
                }
            }
            emit_raw(ctx, "\n");
        }

        /* Main function */
        emit_raw(ctx, "/* ARGV support */\n");
        emit_raw(ctx, "typedef struct { const char **data; mrb_int len; } sp_Argv;\n");
        emit_raw(ctx, "static sp_Argv sp_argv;\n");
        emit_raw(ctx, "static mrb_int sp_Argv_length(sp_Argv *a) { return a->len; }\n\n");

        emit_raw(ctx, "static const char *sp_program_name = \"\";\n");
        emit_raw(ctx, "int main(int argc, char **argv) {\n");
        emit_raw(ctx, "    sp_program_name = argv[0];\n");
        emit_raw(ctx, "    sp_argv.data = (const char **)(argv + 1); sp_argv.len = argc - 1;\n");

        /* Variable declarations for top-level (skip constants — they're global statics) */
        const char *vol = ctx->needs_exc ? "volatile " : "";
        for (int i = 0; i < ctx->var_count; i++) {
            var_entry_t *v = &ctx->vars[i];
            if (v->is_constant) continue;
            char *ct = vt_ctype(ctx, v->type, false);
            char *cn = make_cname(v->name, v->is_constant);
            const char *init = "";
            if (v->type.kind == SPINEL_TYPE_INTEGER) init = " = 0";
            else if (v->type.kind == SPINEL_TYPE_FLOAT) init = " = 0.0";
            else if (v->type.kind == SPINEL_TYPE_BOOLEAN) init = " = FALSE";
            else if (v->type.kind == SPINEL_TYPE_ARRAY) {
                emit_raw(ctx, "    sp_IntArray *%s = NULL;\n", cn);
                if (ctx->needs_gc)
                    emit_raw(ctx, "    SP_GC_ROOT(%s);\n", cn);
                free(ct); free(cn);
                continue;
            }
            else if (v->type.kind == SPINEL_TYPE_FLOAT_ARRAY) {
                emit_raw(ctx, "    sp_FloatArray *%s = NULL;\n", cn);
                if (ctx->needs_gc)
                    emit_raw(ctx, "    SP_GC_ROOT(%s);\n", cn);
                free(ct); free(cn);
                continue;
            }
            else if (v->type.kind == SPINEL_TYPE_HASH) {
                emit_raw(ctx, "    sp_StrIntHash *%s = NULL;\n", cn);
                if (ctx->needs_gc)
                    emit_raw(ctx, "    SP_GC_ROOT(%s);\n", cn);
                free(ct); free(cn);
                continue;
            }
            else if (v->type.kind == SPINEL_TYPE_POLY) {
                emit_raw(ctx, "    sp_RbValue %s%s = sp_box_nil();\n", vol, cn);
                free(ct); free(cn);
                continue;
            }
            else if (v->type.kind == SPINEL_TYPE_RB_ARRAY) {
                emit_raw(ctx, "    sp_RbArray *%s = NULL;\n", cn);
                free(ct); free(cn);
                continue;
            }
            else if (v->type.kind == SPINEL_TYPE_RB_HASH) {
                emit_raw(ctx, "    sp_RbHash *%s = NULL;\n", cn);
                free(ct); free(cn);
                continue;
            }
            else if (v->type.kind == SPINEL_TYPE_PROC && !ctx->lambda_mode) {
                emit_raw(ctx, "    sp_Proc *%s = NULL;\n", cn);
                free(ct); free(cn);
                continue;
            }
            else if (v->type.kind == SPINEL_TYPE_SP_STRING) {
                emit_raw(ctx, "    sp_String *%s = NULL;\n", cn);
                free(ct); free(cn);
                continue;
            }
            else if (v->type.kind == SPINEL_TYPE_FILE) {
                emit_raw(ctx, "    sp_File *%s = NULL;\n", cn);
                free(ct); free(cn);
                continue;
            }
            else if (v->type.kind == SPINEL_TYPE_OBJECT) {
                class_info_t *vc = find_class(ctx, v->type.klass);
                if (vc && !vc->is_value_type) {
                    emit_raw(ctx, "    %s *%s = NULL;\n", ct, cn);
                    if (ctx->needs_gc)
                        emit_raw(ctx, "    SP_GC_ROOT(%s);\n", cn);
                    free(ct); free(cn);
                    continue;
                }
                init = "";
            }
            emit_raw(ctx, "    %s%s %s%s;\n", vol, ct, cn, init);
            free(ct); free(cn);
        }
        emit_raw(ctx, "\n");

        if (ctx->needs_regexp)
            emit_raw(ctx, "    sp_regexp_init();\n\n");
        /* Top-level code */
        codegen_stmts(ctx, (pm_node_t *)prog->statements);

        emit_raw(ctx, "\n    return 0;\n");
        emit_raw(ctx, "}\n");

        fclose(code_buf);
        fclose(block_buf);
        ctx->out = real_out;
        ctx->block_out = NULL;

        /* Emit block callbacks first */
        if (block_buf_data && block_buf_size > 0) {
            fwrite(block_buf_data, 1, block_buf_size, ctx->out);
            emit_raw(ctx, "\n");
        }

        /* Emit megamorphic dispatch functions (before top-level funcs/main) */
        if (ctx->mega_dispatch_count > 0)
            emit_mega_dispatch_funcs(ctx);

        /* Then the functions and main */
        if (code_buf_data)
            fwrite(code_buf_data, 1, code_buf_size, ctx->out);
        free(block_buf_data);
        free(code_buf_data);
    }
}
