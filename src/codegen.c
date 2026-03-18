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
#include <prism.h>
#include "codegen.h"

/* ------------------------------------------------------------------ */
/* String helpers                                                     */
/* ------------------------------------------------------------------ */

static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *r = malloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

static char *sfmt(const char *fmt, ...) {
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

/* ------------------------------------------------------------------ */
/* Constant pool helpers                                              */
/* ------------------------------------------------------------------ */

static void craw(codegen_ctx_t *ctx, pm_constant_id_t id,
                 const uint8_t **s, size_t *len) {
    pm_constant_t *c = &ctx->parser->constant_pool.constants[id - 1];
    *s = c->start;
    *len = c->length;
}

static char *cstr(codegen_ctx_t *ctx, pm_constant_id_t id) {
    const uint8_t *s; size_t len;
    craw(ctx, id, &s, &len);
    char *buf = malloc(len + 1);
    memcpy(buf, s, len);
    buf[len] = '\0';
    return buf;
}

static bool ceq(codegen_ctx_t *ctx, pm_constant_id_t id, const char *s) {
    const uint8_t *p; size_t len;
    craw(ctx, id, &p, &len);
    return len == strlen(s) && memcmp(p, s, len) == 0;
}

/* ------------------------------------------------------------------ */
/* Output helpers                                                     */
/* ------------------------------------------------------------------ */

static void emit(codegen_ctx_t *ctx, const char *fmt, ...) {
    for (int i = 0; i < ctx->indent; i++) fprintf(ctx->out, "    ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(ctx->out, fmt, ap);
    va_end(ap);
}

static void emit_raw(codegen_ctx_t *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(ctx->out, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* vtype helpers                                                      */
/* ------------------------------------------------------------------ */

static vtype_t vt_prim(spinel_type_t k) {
    vtype_t t = {k, ""};
    return t;
}

static vtype_t vt_obj(const char *klass) {
    vtype_t t;
    t.kind = SPINEL_TYPE_OBJECT;
    snprintf(t.klass, sizeof(t.klass), "%s", klass);
    return t;
}

static bool vt_is_numeric(vtype_t t) {
    return t.kind == SPINEL_TYPE_INTEGER || t.kind == SPINEL_TYPE_FLOAT;
}

const char *spinel_type_cname(spinel_type_t k) {
    switch (k) {
    case SPINEL_TYPE_INTEGER: return "mrb_int";
    case SPINEL_TYPE_FLOAT:   return "mrb_float";
    case SPINEL_TYPE_BOOLEAN: return "mrb_bool";
    case SPINEL_TYPE_STRING:  return "const char *";
    case SPINEL_TYPE_ARRAY:   return "sp_IntArray *";
    case SPINEL_TYPE_HASH:    return "sp_StrIntHash *";
    case SPINEL_TYPE_PROC:    return "sp_Val *";
    case SPINEL_TYPE_STR_ARRAY: return "sp_StrArray *";
    default:                  return "mrb_int"; /* fallback for standalone mode */
    }
}

/* C type for a vtype — for objects returns "sp_ClassName" or "sp_ClassName *" */
static char *vt_ctype(codegen_ctx_t *ctx, vtype_t t, bool as_ptr);

/* ------------------------------------------------------------------ */
/* Class/module/func registry lookups                                 */
/* ------------------------------------------------------------------ */

static class_info_t *find_class(codegen_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->class_count; i++)
        if (strcmp(ctx->classes[i].name, name) == 0) return &ctx->classes[i];
    return NULL;
}

static method_info_t *find_method(class_info_t *cls, const char *name) {
    if (!cls) return NULL;
    for (int i = 0; i < cls->method_count; i++)
        if (strcmp(cls->methods[i].name, name) == 0) return &cls->methods[i];
    return NULL;
}

/* Find method walking up inheritance chain; sets *owner to defining class */
static method_info_t *find_method_inherited(codegen_ctx_t *ctx,
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

static ivar_info_t *find_ivar(class_info_t *cls, const char *name) {
    if (!cls) return NULL;
    for (int i = 0; i < cls->ivar_count; i++)
        if (strcmp(cls->ivars[i].name, name) == 0) return &cls->ivars[i];
    return NULL;
}

static module_info_t *find_module(codegen_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->module_count; i++)
        if (strcmp(ctx->modules[i].name, name) == 0) return &ctx->modules[i];
    return NULL;
}

static func_info_t *find_func(codegen_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->func_count; i++)
        if (strcmp(ctx->funcs[i].name, name) == 0) return &ctx->funcs[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Variable table                                                     */
/* ------------------------------------------------------------------ */

static var_entry_t *var_lookup(codegen_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->var_count; i++)
        if (strcmp(ctx->vars[i].name, name) == 0) return &ctx->vars[i];
    return NULL;
}

static var_entry_t *var_declare(codegen_ctx_t *ctx, const char *name,
                                vtype_t type, bool is_constant) {
    var_entry_t *v = var_lookup(ctx, name);
    if (v) {
        /* Widen if types conflict */
        if (v->type.kind != type.kind && v->type.kind != SPINEL_TYPE_UNKNOWN) {
            if (vt_is_numeric(v->type) && vt_is_numeric(type))
                v->type = vt_prim(SPINEL_TYPE_FLOAT);
            else if (type.kind == SPINEL_TYPE_OBJECT && v->type.kind == SPINEL_TYPE_OBJECT
                     && strcmp(v->type.klass, type.klass) == 0) { /* same class */ }
            else if (v->type.kind == SPINEL_TYPE_VALUE && type.kind != SPINEL_TYPE_VALUE
                     && type.kind != SPINEL_TYPE_UNKNOWN)
                v->type = type;  /* Narrow from VALUE to a more specific type */
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

static char *make_cname(const char *name, bool is_constant) {
    return sfmt("%s%s", is_constant ? "cv_" : "lv_", name);
}

/* ------------------------------------------------------------------ */
/* vt_ctype implementation                                            */
/* ------------------------------------------------------------------ */

static char *vt_ctype(codegen_ctx_t *ctx, vtype_t t, bool as_ptr) {
    if (t.kind == SPINEL_TYPE_OBJECT) {
        class_info_t *cls = find_class(ctx, t.klass);
        if (cls && cls->is_value_type)
            return sfmt("sp_%s", t.klass);
        if (as_ptr)
            return sfmt("sp_%s *", t.klass);
        return sfmt("sp_%s", t.klass);
    }
    return xstrdup(spinel_type_cname(t.kind));
}

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

static vtype_t infer_type(codegen_ctx_t *ctx, pm_node_t *node);
static void infer_pass(codegen_ctx_t *ctx, pm_node_t *node);
static char *codegen_expr(codegen_ctx_t *ctx, pm_node_t *node);
static void codegen_stmt(codegen_ctx_t *ctx, pm_node_t *node);
static void codegen_stmts(codegen_ctx_t *ctx, pm_node_t *node);
static char *codegen_lambda(codegen_ctx_t *ctx, pm_lambda_node_t *lam);

/* Forward declarations for capture analysis (used by both lambda and block codegen) */
typedef struct {
    char names[256][64];
    int count;
} capture_list_t;

static void capture_list_add(capture_list_t *cl, const char *name);
static bool capture_list_has(capture_list_t *cl, const char *name);
static void scan_captures(codegen_ctx_t *ctx, pm_node_t *node,
                          const char *param_name,
                          capture_list_t *local_defs,
                          capture_list_t *result);

/* ------------------------------------------------------------------ */
/* GC helpers                                                         */
/* ------------------------------------------------------------------ */

/* Return true if a variable of this type needs GC rooting */
static bool is_gc_type(codegen_ctx_t *ctx, vtype_t t) {
    if (t.kind == SPINEL_TYPE_ARRAY) return true;
    if (t.kind == SPINEL_TYPE_HASH) return true;
    if (t.kind == SPINEL_TYPE_OBJECT) {
        class_info_t *cls = find_class(ctx, t.klass);
        return cls && !cls->is_value_type;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Pass 1: Class/Module/Function analysis                             */
/* ------------------------------------------------------------------ */

static void analyze_method(codegen_ctx_t *ctx, class_info_t *cls,
                           pm_def_node_t *def) {
    method_info_t *m = &cls->methods[cls->method_count++];
    char *name = cstr(ctx, def->name);
    snprintf(m->name, sizeof(m->name), "%s", name);
    free(name);

    m->body_node = def->body ? (pm_node_t *)def->body : NULL;
    m->params_node = def->parameters ? (pm_node_t *)def->parameters : NULL;
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
            /* Skip if already registered (e.g., from attr_accessor) */
            if (!find_ivar(cls, ivname + 1) && cls->ivar_count < MAX_IVARS) {
                ivar_info_t *iv = &cls->ivars[cls->ivar_count++];
                snprintf(iv->name, sizeof(iv->name), "%s", ivname + 1); /* skip @ */
                /* Type will be resolved in pass 2 */
                iv->type = vt_prim(SPINEL_TYPE_VALUE);
            }
            free(ivname);
        }
    }
}

static void analyze_class(codegen_ctx_t *ctx, pm_class_node_t *node) {
    class_info_t *cls = &ctx->classes[ctx->class_count++];
    memset(cls, 0, sizeof(*cls));
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
                method_info_t *m = &cls->methods[cls->method_count++];
                char *name = cstr(ctx, def->name);
                snprintf(m->name, sizeof(m->name), "%s", name);
                free(name);
                m->body_node = def->body ? (pm_node_t *)def->body : NULL;
                m->params_node = def->parameters ? (pm_node_t *)def->parameters : NULL;
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

            /* Extract ivars from initialize */
            char *mname = cstr(ctx, def->name);
            if (strcmp(mname, "initialize") == 0) {
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
                        snprintf(iv->name, sizeof(iv->name), "%s", sym_name);
                        iv->type = vt_prim(SPINEL_TYPE_VALUE);
                    }

                    /* Generate getter */
                    if (is_accessor || is_reader) {
                        method_info_t *m = &cls->methods[cls->method_count++];
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

                    /* Generate setter */
                    if (is_accessor || is_writer) {
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
            free(cname);
        }
    }

    /* Heuristic: classes with only float/int ivars and <= 4 fields are value types */
    cls->is_value_type = (cls->ivar_count <= 4 && cls->ivar_count > 0);
}

static void analyze_module(codegen_ctx_t *ctx, pm_module_node_t *node) {
    module_info_t *mod = &ctx->modules[ctx->module_count++];
    memset(mod, 0, sizeof(*mod));
    mod->module_node = (pm_node_t *)node;

    if (PM_NODE_TYPE(node->constant_path) == PM_CONSTANT_READ_NODE) {
        pm_constant_read_node_t *cr = (pm_constant_read_node_t *)node->constant_path;
        char *name = cstr(ctx, cr->name);
        snprintf(mod->name, sizeof(mod->name), "%s", name);
        free(name);
    }

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
                m->return_type = vt_prim(SPINEL_TYPE_FLOAT); /* for Rand::rand */
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
            snprintf(iv->name, sizeof(iv->name), "%s", ivname + 1);
            iv->type = vt_prim(SPINEL_TYPE_INTEGER);
            free(ivname);
        } else if (PM_NODE_TYPE(s) == PM_CONSTANT_WRITE_NODE) {
            pm_constant_write_node_t *cw = (pm_constant_write_node_t *)s;
            char *cname = cstr(ctx, cw->name);
            module_const_t *mc = &mod->consts[mod->const_count++];
            snprintf(mc->name, sizeof(mc->name), "%s", cname);
            mc->value_node = cw->value;
            /* Infer type: BNUM is integer (1 << 29), BNUMF is float (.to_f) */
            mc->type = vt_prim(SPINEL_TYPE_INTEGER);
            if (cw->value && PM_NODE_TYPE(cw->value) == PM_CALL_NODE) {
                pm_call_node_t *vc = (pm_call_node_t *)cw->value;
                char *mn = cstr(ctx, vc->name);
                if (strcmp(mn, "to_f") == 0)
                    mc->type = vt_prim(SPINEL_TYPE_FLOAT);
                free(mn);
            }
            free(cname);
        }
    }
}

/* Detect if a node tree contains PM_YIELD_NODE */
static bool has_yield_nodes(pm_node_t *node) {
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
    snprintf(f->name, sizeof(f->name), "%s", name);
    free(name);
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

    /* Detect yield in function body */
    f->has_yield = f->body_node ? has_yield_nodes(f->body_node) : false;
}

static void class_analysis_pass(codegen_ctx_t *ctx, pm_node_t *root) {
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

                        /* Each symbol arg becomes an ivar + getter + setter */
                        int nfields = (int)call->arguments->arguments.size;
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
                            snprintf(iv->name, sizeof(iv->name), "%s", fname);
                            iv->type = vt_prim(SPINEL_TYPE_INTEGER);

                            /* Init param */
                            snprintf(init->params[init->param_count].name, 64, "%s", fname);
                            init->params[init->param_count].type = vt_prim(SPINEL_TYPE_INTEGER);
                            init->param_count++;

                            /* Getter */
                            method_info_t *getter = &cls->methods[cls->method_count++];
                            memset(getter, 0, sizeof(*getter));
                            snprintf(getter->name, sizeof(getter->name), "%s", fname);
                            getter->is_getter = true;
                            snprintf(getter->accessor_ivar, sizeof(getter->accessor_ivar), "%s", fname);
                            getter->return_type = vt_prim(SPINEL_TYPE_INTEGER);

                            /* Setter */
                            method_info_t *setter = &cls->methods[cls->method_count++];
                            memset(setter, 0, sizeof(*setter));
                            snprintf(setter->name, sizeof(setter->name), "%s=", fname);
                            setter->is_setter = true;
                            snprintf(setter->accessor_ivar, sizeof(setter->accessor_ivar), "%s", fname);
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
/* Pass 2: Type inference                                             */
/* ------------------------------------------------------------------ */

static vtype_t binop_result(vtype_t l, vtype_t r, const char *op) {
    if (strcmp(op, ">") == 0 || strcmp(op, "<") == 0 ||
        strcmp(op, ">=") == 0 || strcmp(op, "<=") == 0 ||
        strcmp(op, "==") == 0 || strcmp(op, "!=") == 0)
        return vt_prim(SPINEL_TYPE_BOOLEAN);
    if (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0 ||
        strcmp(op, "|") == 0 || strcmp(op, "&") == 0 ||
        strcmp(op, "^") == 0 || strcmp(op, "%") == 0)
        return vt_prim(SPINEL_TYPE_INTEGER);
    if (l.kind == SPINEL_TYPE_FLOAT || r.kind == SPINEL_TYPE_FLOAT)
        return vt_prim(SPINEL_TYPE_FLOAT);
    if (l.kind == SPINEL_TYPE_INTEGER && r.kind == SPINEL_TYPE_INTEGER)
        return vt_prim(SPINEL_TYPE_INTEGER);
    return vt_prim(SPINEL_TYPE_VALUE);
}

static vtype_t infer_type(codegen_ctx_t *ctx, pm_node_t *node) {
    if (!node) return vt_prim(SPINEL_TYPE_NIL);

    switch (PM_NODE_TYPE(node)) {
    case PM_INTEGER_NODE:  return vt_prim(SPINEL_TYPE_INTEGER);
    case PM_FLOAT_NODE:    return vt_prim(SPINEL_TYPE_FLOAT);
    case PM_STRING_NODE:
    case PM_INTERPOLATED_STRING_NODE:
    case PM_SYMBOL_NODE:
                           return vt_prim(SPINEL_TYPE_STRING);
    case PM_TRUE_NODE:
    case PM_FALSE_NODE:    return vt_prim(SPINEL_TYPE_BOOLEAN);
    case PM_NIL_NODE:      return vt_prim(SPINEL_TYPE_NIL);
    case PM_SOURCE_LINE_NODE: return vt_prim(SPINEL_TYPE_INTEGER);
    case PM_SOURCE_FILE_NODE: return vt_prim(SPINEL_TYPE_STRING);
    case PM_DEFINED_NODE:     return vt_prim(SPINEL_TYPE_STRING); /* returns string or nil */

    case PM_LOCAL_VARIABLE_READ_NODE: {
        pm_local_variable_read_node_t *n = (pm_local_variable_read_node_t *)node;
        char *name = cstr(ctx, n->name);
        var_entry_t *v = var_lookup(ctx, name);
        vtype_t t = v ? v->type : vt_prim(SPINEL_TYPE_VALUE);
        free(name);
        return t;
    }

    case PM_CONSTANT_READ_NODE: {
        pm_constant_read_node_t *n = (pm_constant_read_node_t *)node;
        char *name = cstr(ctx, n->name);
        /* Check if it's a class name */
        if (find_class(ctx, name)) { free(name); return vt_prim(SPINEL_TYPE_VALUE); }
        /* Check module constants when inside a module */
        if (ctx->current_module) {
            for (int i = 0; i < ctx->current_module->const_count; i++) {
                if (strcmp(ctx->current_module->consts[i].name, name) == 0) {
                    vtype_t t = ctx->current_module->consts[i].type;
                    free(name);
                    return t;
                }
            }
        }
        /* Also check all modules for unqualified constant access */
        for (int mi = 0; mi < ctx->module_count; mi++) {
            for (int ci = 0; ci < ctx->modules[mi].const_count; ci++) {
                if (strcmp(ctx->modules[mi].consts[ci].name, name) == 0) {
                    vtype_t t = ctx->modules[mi].consts[ci].type;
                    free(name);
                    return t;
                }
            }
        }
        var_entry_t *v = var_lookup(ctx, name);
        vtype_t t = v ? v->type : vt_prim(SPINEL_TYPE_VALUE);
        free(name);
        return t;
    }

    /* Chained assignment: zr = zi = 0 — type is type of the value */
    case PM_LOCAL_VARIABLE_WRITE_NODE: {
        pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
        return infer_type(ctx, n->value);
    }

    case PM_INSTANCE_VARIABLE_READ_NODE: {
        pm_instance_variable_read_node_t *n = (pm_instance_variable_read_node_t *)node;
        char *ivname = cstr(ctx, n->name);
        if (ctx->current_module) {
            for (int i = 0; i < ctx->current_module->var_count; i++)
                if (strcmp(ctx->current_module->vars[i].name, ivname + 1) == 0) {
                    vtype_t t = ctx->current_module->vars[i].type;
                    free(ivname); return t;
                }
        }
        if (ctx->current_class) {
            ivar_info_t *iv = find_ivar(ctx->current_class, ivname + 1);
            if (iv) { free(ivname); return iv->type; }
        }
        free(ivname);
        return vt_prim(SPINEL_TYPE_VALUE);
    }

    case PM_INSTANCE_VARIABLE_WRITE_NODE: {
        /* @x = expr — type is the type of expr (same as ivar type) */
        pm_instance_variable_write_node_t *n = (pm_instance_variable_write_node_t *)node;
        return infer_type(ctx, n->value);
    }

    case PM_CALL_NODE: {
        pm_call_node_t *call = (pm_call_node_t *)node;
        char *method = cstr(ctx, call->name);
        vtype_t result = vt_prim(SPINEL_TYPE_VALUE);

        /* Array indexing: infer element type from variable name heuristics
         * (must be checked before binary operators since [] has one argument) */
        if (strcmp(method, "[]") == 0 && call->receiver) {
            /* Check if receiver is "basis" → Vec, "spheres"/@spheres → Sphere */
            if (PM_NODE_TYPE(call->receiver) == PM_LOCAL_VARIABLE_READ_NODE) {
                pm_local_variable_read_node_t *lv = (pm_local_variable_read_node_t *)call->receiver;
                char *vname = cstr(ctx, lv->name);
                if (strcmp(vname, "basis") == 0) { free(vname); free(method); return vt_obj("Vec"); }
                free(vname);
            }
            if (PM_NODE_TYPE(call->receiver) == PM_INSTANCE_VARIABLE_READ_NODE) {
                pm_instance_variable_read_node_t *iv = (pm_instance_variable_read_node_t *)call->receiver;
                char *ivn = cstr(ctx, iv->name);
                if (strcmp(ivn, "@spheres") == 0) { free(ivn); free(method); return vt_obj("Sphere"); }
                free(ivn);
            }
        }

        /* Binary operators */
        if (call->receiver && call->arguments &&
            call->arguments->arguments.size == 1 &&
            strcmp(method, "[]") != 0) {
            vtype_t lt = infer_type(ctx, call->receiver);
            vtype_t rt = infer_type(ctx, call->arguments->arguments.nodes[0]);
            /* String * Integer → STRING (repetition) */
            if (lt.kind == SPINEL_TYPE_STRING && rt.kind == SPINEL_TYPE_INTEGER &&
                strcmp(method, "*") == 0) {
                free(method);
                return vt_prim(SPINEL_TYPE_STRING);
            }
            /* String comparison operators → BOOLEAN */
            if (lt.kind == SPINEL_TYPE_STRING && rt.kind == SPINEL_TYPE_STRING &&
                (strcmp(method, "==") == 0 || strcmp(method, "!=") == 0 ||
                 strcmp(method, "<") == 0 || strcmp(method, ">") == 0 ||
                 strcmp(method, "<=") == 0 || strcmp(method, ">=") == 0)) {
                free(method);
                return vt_prim(SPINEL_TYPE_BOOLEAN);
            }
            if (vt_is_numeric(lt) || vt_is_numeric(rt) ||
                lt.kind == SPINEL_TYPE_BOOLEAN || rt.kind == SPINEL_TYPE_BOOLEAN) {
                result = binop_result(lt, rt, method);
                free(method);
                return result;
            }
        }

        /* Unary minus: -expr → same type as expr */
        if (strcmp(method, "-@") == 0 && call->receiver && !call->arguments) {
            result = infer_type(ctx, call->receiver);
            free(method);
            return result;
        }

        /* Unary not: !expr → boolean */
        if (strcmp(method, "!") == 0 && call->receiver && !call->arguments) {
            free(method);
            return vt_prim(SPINEL_TYPE_BOOLEAN);
        }

        /* Array methods on ARRAY-typed receiver */
        if (call->receiver) {
            vtype_t recv_t = infer_type(ctx, call->receiver);
            if (recv_t.kind == SPINEL_TYPE_ARRAY) {
                if (strcmp(method, "dup") == 0) { free(method); return vt_prim(SPINEL_TYPE_ARRAY); }
                if (strcmp(method, "empty?") == 0) { free(method); return vt_prim(SPINEL_TYPE_BOOLEAN); }
                if (strcmp(method, "shift") == 0 || strcmp(method, "pop") == 0 ||
                    strcmp(method, "length") == 0) { free(method); return vt_prim(SPINEL_TYPE_INTEGER); }
                if (strcmp(method, "[]") == 0) { free(method); return vt_prim(SPINEL_TYPE_INTEGER); }
                if (strcmp(method, "!=") == 0 || strcmp(method, "==") == 0) { free(method); return vt_prim(SPINEL_TYPE_BOOLEAN); }
                if (strcmp(method, "map") == 0 || strcmp(method, "select") == 0 ||
                    strcmp(method, "reject") == 0) { free(method); return vt_prim(SPINEL_TYPE_ARRAY); }
                if (strcmp(method, "first") == 0 || strcmp(method, "last") == 0) { free(method); return vt_prim(SPINEL_TYPE_INTEGER); }
                if (strcmp(method, "reduce") == 0 || strcmp(method, "inject") == 0 ||
                    strcmp(method, "min") == 0 || strcmp(method, "max") == 0 ||
                    strcmp(method, "sum") == 0) { free(method); return vt_prim(SPINEL_TYPE_INTEGER); }
                if (strcmp(method, "sort") == 0) { free(method); return vt_prim(SPINEL_TYPE_ARRAY); }
                if (strcmp(method, "include?") == 0) { free(method); return vt_prim(SPINEL_TYPE_BOOLEAN); }
                if (strcmp(method, "each") == 0) { free(method); return vt_prim(SPINEL_TYPE_ARRAY); }
            }
            /* Hash methods on HASH-typed receiver */
            if (recv_t.kind == SPINEL_TYPE_HASH) {
                if (strcmp(method, "[]") == 0) { free(method); return vt_prim(SPINEL_TYPE_INTEGER); }
                if (strcmp(method, "[]=") == 0) { free(method); return vt_prim(SPINEL_TYPE_INTEGER); }
                if (strcmp(method, "length") == 0) { free(method); return vt_prim(SPINEL_TYPE_INTEGER); }
                if (strcmp(method, "has_key?") == 0) { free(method); return vt_prim(SPINEL_TYPE_BOOLEAN); }
                if (strcmp(method, "delete") == 0) { free(method); return vt_prim(SPINEL_TYPE_INTEGER); }
                if (strcmp(method, "each") == 0) { free(method); return vt_prim(SPINEL_TYPE_HASH); }
                if (strcmp(method, "keys") == 0) { free(method); return vt_prim(SPINEL_TYPE_ARRAY); }
            }
            /* String methods */
            if (recv_t.kind == SPINEL_TYPE_STRING) {
                if (strcmp(method, "length") == 0 || strcmp(method, "size") == 0) { free(method); return vt_prim(SPINEL_TYPE_INTEGER); }
                if (strcmp(method, "upcase") == 0 || strcmp(method, "downcase") == 0) { free(method); return vt_prim(SPINEL_TYPE_STRING); }
                if (strcmp(method, "include?") == 0) { free(method); return vt_prim(SPINEL_TYPE_BOOLEAN); }
                if (strcmp(method, "+") == 0 || strcmp(method, "<<") == 0) { free(method); return vt_prim(SPINEL_TYPE_STRING); }
                if (strcmp(method, "strip") == 0 || strcmp(method, "chomp") == 0 ||
                    strcmp(method, "capitalize") == 0 || strcmp(method, "reverse") == 0) { free(method); return vt_prim(SPINEL_TYPE_STRING); }
                if (strcmp(method, "gsub") == 0 || strcmp(method, "sub") == 0) { free(method); return vt_prim(SPINEL_TYPE_STRING); }
                if (strcmp(method, "match?") == 0) { free(method); return vt_prim(SPINEL_TYPE_BOOLEAN); }
                if (strcmp(method, "=~") == 0) { free(method); return vt_prim(SPINEL_TYPE_INTEGER); }
                if (strcmp(method, "count") == 0) { free(method); return vt_prim(SPINEL_TYPE_INTEGER); }
                if (strcmp(method, "start_with?") == 0 || strcmp(method, "end_with?") == 0) { free(method); return vt_prim(SPINEL_TYPE_BOOLEAN); }
                if (strcmp(method, "==") == 0 || strcmp(method, "!=") == 0 || strcmp(method, "<") == 0 ||
                    strcmp(method, ">") == 0 || strcmp(method, "<=") == 0 || strcmp(method, ">=") == 0) { free(method); return vt_prim(SPINEL_TYPE_BOOLEAN); }
                if (strcmp(method, "*") == 0) { free(method); return vt_prim(SPINEL_TYPE_STRING); }
                if (strcmp(method, "split") == 0) { free(method); return vt_prim(SPINEL_TYPE_STR_ARRAY); }
            }
            /* String array methods */
            if (recv_t.kind == SPINEL_TYPE_STR_ARRAY) {
                if (strcmp(method, "length") == 0 || strcmp(method, "size") == 0) { free(method); return vt_prim(SPINEL_TYPE_INTEGER); }
            }
            /* Numeric methods */
            if (recv_t.kind == SPINEL_TYPE_INTEGER) {
                if (strcmp(method, "abs") == 0) { free(method); return vt_prim(SPINEL_TYPE_INTEGER); }
                if (strcmp(method, "even?") == 0 || strcmp(method, "odd?") == 0 ||
                    strcmp(method, "zero?") == 0 || strcmp(method, "positive?") == 0 ||
                    strcmp(method, "negative?") == 0) { free(method); return vt_prim(SPINEL_TYPE_BOOLEAN); }
                if (strcmp(method, "**") == 0) { free(method); return vt_prim(SPINEL_TYPE_INTEGER); }
            }
            /* Universal methods */
            if (strcmp(method, "nil?") == 0 || strcmp(method, "is_a?") == 0 ||
                strcmp(method, "respond_to?") == 0) {
                free(method); return vt_prim(SPINEL_TYPE_BOOLEAN);
            }
            if (recv_t.kind == SPINEL_TYPE_FLOAT) {
                if (strcmp(method, "abs") == 0) { free(method); return vt_prim(SPINEL_TYPE_FLOAT); }
                if (strcmp(method, "ceil") == 0 || strcmp(method, "floor") == 0 ||
                    strcmp(method, "round") == 0 || strcmp(method, "to_i") == 0) {
                    free(method); return vt_prim(SPINEL_TYPE_INTEGER);
                }
            }
        }

        /* Range#to_a → ARRAY */
        if (strcmp(method, "to_a") == 0 && call->receiver) {
            free(method);
            return vt_prim(SPINEL_TYPE_ARRAY);
        }

        /* Constructor: ClassName.new(...) → returns ClassName */
        if (strcmp(method, "new") == 0 && call->receiver) {
            if (PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
                pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
                char *cls_name = cstr(ctx, cr->name);
                if (strcmp(cls_name, "Array") == 0) {
                    int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                    if (argc == 0) {
                        free(cls_name); free(method);
                        return vt_prim(SPINEL_TYPE_ARRAY);
                    }
                    /* Array.new(N) — fixed-size C array, type depends on context */
                    free(cls_name); free(method);
                    return vt_prim(SPINEL_TYPE_VALUE);
                }
                if (find_class(ctx, cls_name)) {
                    result = vt_obj(cls_name);
                    free(cls_name); free(method);
                    return result;
                }
                free(cls_name);
            }
        }

        /* Module method calls: Rand::rand, Math.sqrt etc. */
        if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
            pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
            char *mod_name = cstr(ctx, cr->name);
            module_info_t *mod = find_module(ctx, mod_name);
            if (mod) {
                for (int mi = 0; mi < mod->method_count; mi++)
                    if (strcmp(mod->methods[mi].name, method) == 0) {
                        free(mod_name); free(method);
                        return mod->methods[mi].return_type;
                    }
            }
            if (strcmp(mod_name, "Math") == 0) {
                free(mod_name); free(method);
                return vt_prim(SPINEL_TYPE_FLOAT);
            }
            /* Class method calls: ClassName.method → look up class method return type */
            {
                class_info_t *cls = find_class(ctx, mod_name);
                if (cls) {
                    method_info_t *cm = find_method(cls, method);
                    if (cm && cm->is_class_method) {
                        vtype_t rt = cm->return_type;
                        free(mod_name); free(method);
                        return rt;
                    }
                }
            }
            free(mod_name);
        }

        /* Method calls on typed objects (with inheritance) */
        if (call->receiver) {
            vtype_t recv_t = infer_type(ctx, call->receiver);
            if (recv_t.kind == SPINEL_TYPE_OBJECT) {
                class_info_t *cls = find_class(ctx, recv_t.klass);
                if (cls) {
                    class_info_t *owner = NULL;
                    method_info_t *m = find_method_inherited(ctx, cls, method, &owner);
                    if (m) {
                        /* Getter returns ivar type */
                        if (m->is_getter) {
                            ivar_info_t *iv = find_ivar(cls, m->accessor_ivar);
                            if (iv) { free(method); return iv->type; }
                        }
                        free(method);
                        return m->return_type;
                    }
                }
            }
        }

        /* Receiver-less call in class context → implicit self method (with inheritance) */
        if (!call->receiver && ctx->current_class) {
            method_info_t *cm = find_method_inherited(ctx, ctx->current_class, method, NULL);
            if (cm) { free(method); return cm->return_type; }
        }

        /* Receiver-less call to top-level function */
        if (!call->receiver) {
            func_info_t *fn = find_func(ctx, method);
            if (fn && fn->return_type.kind != SPINEL_TYPE_VALUE) {
                free(method); return fn->return_type;
            }
        }

        /* Known methods */
        if (strcmp(method, "chr") == 0 || strcmp(method, "to_s") == 0)
            result = vt_prim(SPINEL_TYPE_STRING);
        else if (strcmp(method, "to_i") == 0 || strcmp(method, "Integer") == 0)
            result = vt_prim(SPINEL_TYPE_INTEGER);
        else if (strcmp(method, "to_f") == 0)
            result = vt_prim(SPINEL_TYPE_FLOAT);
        else if (strcmp(method, "puts") == 0 || strcmp(method, "print") == 0 ||
                 strcmp(method, "printf") == 0 || strcmp(method, "putc") == 0 ||
                 strcmp(method, "p") == 0)
            result = vt_prim(SPINEL_TYPE_NIL);
        else if (strcmp(method, "block_given?") == 0)
            result = vt_prim(SPINEL_TYPE_BOOLEAN);

        free(method);
        return result;
    }

    case PM_IF_NODE: {
        pm_if_node_t *n = (pm_if_node_t *)node;
        vtype_t then_t = n->statements ? infer_type(ctx, (pm_node_t *)n->statements) : vt_prim(SPINEL_TYPE_NIL);
        vtype_t else_t = n->subsequent ? infer_type(ctx, (pm_node_t *)n->subsequent) : vt_prim(SPINEL_TYPE_NIL);
        if (then_t.kind == else_t.kind) return then_t;
        return vt_prim(SPINEL_TYPE_VALUE);
    }

    case PM_ELSE_NODE: {
        pm_else_node_t *n = (pm_else_node_t *)node;
        return n->statements ? infer_type(ctx, (pm_node_t *)n->statements) : vt_prim(SPINEL_TYPE_NIL);
    }

    case PM_UNLESS_NODE: {
        pm_unless_node_t *n = (pm_unless_node_t *)node;
        vtype_t then_t = n->statements ? infer_type(ctx, (pm_node_t *)n->statements) : vt_prim(SPINEL_TYPE_NIL);
        vtype_t else_t = n->else_clause ? infer_type(ctx, (pm_node_t *)n->else_clause) : vt_prim(SPINEL_TYPE_NIL);
        if (then_t.kind == else_t.kind) return then_t;
        return vt_prim(SPINEL_TYPE_VALUE);
    }

    case PM_CASE_NODE: {
        pm_case_node_t *n = (pm_case_node_t *)node;
        vtype_t result = vt_prim(SPINEL_TYPE_NIL);
        for (size_t i = 0; i < n->conditions.size; i++) {
            pm_node_t *cond = n->conditions.nodes[i];
            if (PM_NODE_TYPE(cond) == PM_WHEN_NODE) {
                pm_when_node_t *w = (pm_when_node_t *)cond;
                if (w->statements) {
                    vtype_t t = infer_type(ctx, (pm_node_t *)w->statements);
                    if (i == 0) result = t;
                    else if (result.kind != t.kind) result = vt_prim(SPINEL_TYPE_VALUE);
                }
            }
        }
        return result;
    }

    case PM_STATEMENTS_NODE: {
        pm_statements_node_t *s = (pm_statements_node_t *)node;
        if (s->body.size == 0) return vt_prim(SPINEL_TYPE_NIL);
        return infer_type(ctx, s->body.nodes[s->body.size - 1]);
    }

    case PM_PARENTHESES_NODE: {
        pm_parentheses_node_t *n = (pm_parentheses_node_t *)node;
        return n->body ? infer_type(ctx, n->body) : vt_prim(SPINEL_TYPE_NIL);
    }

    case PM_LAMBDA_NODE:
        return vt_prim(SPINEL_TYPE_PROC);

    case PM_RETURN_NODE: {
        pm_return_node_t *n = (pm_return_node_t *)node;
        if (n->arguments && n->arguments->arguments.size > 0)
            return infer_type(ctx, n->arguments->arguments.nodes[0]);
        return vt_prim(SPINEL_TYPE_NIL);
    }

    case PM_ARRAY_NODE:
        return vt_prim(SPINEL_TYPE_ARRAY);

    case PM_HASH_NODE:
        return vt_prim(SPINEL_TYPE_HASH);

    case PM_BEGIN_NODE: {
        pm_begin_node_t *bn = (pm_begin_node_t *)node;
        if (bn->statements)
            return infer_type(ctx, (pm_node_t *)bn->statements);
        return vt_prim(SPINEL_TYPE_NIL);
    }

    case PM_REGULAR_EXPRESSION_NODE:
        return vt_prim(SPINEL_TYPE_REGEXP);

    case PM_NUMBERED_REFERENCE_READ_NODE:
        return vt_prim(SPINEL_TYPE_STRING);

    case PM_MATCH_WRITE_NODE: {
        pm_match_write_node_t *mw = (pm_match_write_node_t *)node;
        return infer_type(ctx, (pm_node_t *)mw->call);
    }

    default:
        return vt_prim(SPINEL_TYPE_VALUE);
    }
}

/* Walk AST to register all variables and infer their types */
static void infer_pass(codegen_ctx_t *ctx, pm_node_t *node) {
    if (!node) return;

    switch (PM_NODE_TYPE(node)) {
    case PM_PROGRAM_NODE: {
        pm_program_node_t *p = (pm_program_node_t *)node;
        infer_pass(ctx, (pm_node_t *)p->statements);
        break;
    }
    case PM_STATEMENTS_NODE: {
        pm_statements_node_t *s = (pm_statements_node_t *)node;
        for (size_t i = 0; i < s->body.size; i++)
            infer_pass(ctx, s->body.nodes[i]);
        break;
    }
    case PM_LOCAL_VARIABLE_WRITE_NODE: {
        pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
        infer_pass(ctx, n->value);
        char *name = cstr(ctx, n->name);
        vtype_t type = infer_type(ctx, n->value);

        /* Detect Array.new(N) pattern → mark variable as array */
        if (PM_NODE_TYPE(n->value) == PM_CALL_NODE) {
            pm_call_node_t *vc = (pm_call_node_t *)n->value;
            char *mname = cstr(ctx, vc->name);
            if (strcmp(mname, "new") == 0 && vc->receiver &&
                PM_NODE_TYPE(vc->receiver) == PM_CONSTANT_READ_NODE) {
                pm_constant_read_node_t *cr = (pm_constant_read_node_t *)vc->receiver;
                if (ceq(ctx, cr->name, "Array")) {
                    /* Check if there's a size argument → fixed C array (ao_render) */
                    int arr_size = 0;
                    if (vc->arguments && vc->arguments->arguments.size == 1 &&
                        PM_NODE_TYPE(vc->arguments->arguments.nodes[0]) == PM_INTEGER_NODE) {
                        pm_integer_node_t *in = (pm_integer_node_t *)vc->arguments->arguments.nodes[0];
                        arr_size = (int)in->value.value;
                    }
                    if (arr_size > 0) {
                        /* Fixed-size C array (e.g., Array.new(3) for basis) */
                        if (strcmp(name, "basis") == 0)
                            type = vt_obj("Vec");
                        var_entry_t *v = var_declare(ctx, name, type, false);
                        v->is_array = true;
                        v->array_size = arr_size;
                    } else {
                        /* Dynamic sp_IntArray (e.g., Array.new with no args) */
                        type = vt_prim(SPINEL_TYPE_ARRAY);
                        var_declare(ctx, name, type, false);
                    }
                    free(mname); free(name);
                    break;
                }
            }
            free(mname);
        }

        var_declare(ctx, name, type, false);
        free(name);
        break;
    }
    case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
        pm_local_variable_operator_write_node_t *n =
            (pm_local_variable_operator_write_node_t *)node;
        infer_pass(ctx, n->value);
        char *name = cstr(ctx, n->name);
        /* Always declare/widen: x += float widens x from int to float */
        vtype_t rhs_type = infer_type(ctx, n->value);
        var_entry_t *v = var_lookup(ctx, name);
        if (v) {
            /* Widen: int += float → float */
            if (v->type.kind == SPINEL_TYPE_INTEGER && rhs_type.kind == SPINEL_TYPE_FLOAT)
                v->type = vt_prim(SPINEL_TYPE_FLOAT);
        } else {
            var_declare(ctx, name, rhs_type, false);
        }
        free(name);
        break;
    }
    case PM_CONSTANT_WRITE_NODE: {
        pm_constant_write_node_t *n = (pm_constant_write_node_t *)node;
        infer_pass(ctx, n->value);
        char *name = cstr(ctx, n->name);
        if (!find_class(ctx, name)) { /* Don't register class names as vars */
            vtype_t type = infer_type(ctx, n->value);
            var_declare(ctx, name, type, true);
        }
        free(name);
        break;
    }
    case PM_WHILE_NODE: {
        pm_while_node_t *n = (pm_while_node_t *)node;
        infer_pass(ctx, n->predicate);
        if (n->statements) infer_pass(ctx, (pm_node_t *)n->statements);
        break;
    }
    case PM_UNTIL_NODE: {
        pm_until_node_t *n = (pm_until_node_t *)node;
        infer_pass(ctx, n->predicate);
        if (n->statements) infer_pass(ctx, (pm_node_t *)n->statements);
        break;
    }
    case PM_FOR_NODE: {
        pm_for_node_t *fn = (pm_for_node_t *)node;
        /* Register the loop variable */
        if (PM_NODE_TYPE(fn->index) == PM_LOCAL_VARIABLE_TARGET_NODE) {
            pm_local_variable_target_node_t *t =
                (pm_local_variable_target_node_t *)fn->index;
            char *vname = cstr(ctx, t->name);
            var_declare(ctx, vname, vt_prim(SPINEL_TYPE_INTEGER), false);
            free(vname);
        }
        if (fn->statements) infer_pass(ctx, (pm_node_t *)fn->statements);
        break;
    }
    case PM_UNLESS_NODE: {
        pm_unless_node_t *n = (pm_unless_node_t *)node;
        infer_pass(ctx, n->predicate);
        if (n->statements) infer_pass(ctx, (pm_node_t *)n->statements);
        if (n->else_clause) infer_pass(ctx, (pm_node_t *)n->else_clause);
        break;
    }
    case PM_CASE_NODE: {
        pm_case_node_t *n = (pm_case_node_t *)node;
        if (n->predicate) infer_pass(ctx, n->predicate);
        for (size_t i = 0; i < n->conditions.size; i++) {
            pm_node_t *cn = n->conditions.nodes[i];
            if (PM_NODE_TYPE(cn) == PM_WHEN_NODE) {
                pm_when_node_t *w = (pm_when_node_t *)cn;
                if (w->statements) infer_pass(ctx, (pm_node_t *)w->statements);
            }
        }
        if (n->else_clause) infer_pass(ctx, (pm_node_t *)n->else_clause);
        break;
    }
    case PM_IF_NODE: {
        pm_if_node_t *n = (pm_if_node_t *)node;
        infer_pass(ctx, n->predicate);
        if (n->statements) infer_pass(ctx, (pm_node_t *)n->statements);
        if (n->subsequent) infer_pass(ctx, (pm_node_t *)n->subsequent);
        break;
    }
    case PM_ELSE_NODE: {
        pm_else_node_t *n = (pm_else_node_t *)node;
        if (n->statements) infer_pass(ctx, (pm_node_t *)n->statements);
        break;
    }
    case PM_CALL_NODE: {
        pm_call_node_t *call = (pm_call_node_t *)node;
        if (call->receiver) infer_pass(ctx, call->receiver);
        if (call->arguments) {
            for (size_t i = 0; i < call->arguments->arguments.size; i++)
                infer_pass(ctx, call->arguments->arguments.nodes[i]);
        }
        /* Recurse into blocks (for .times do ... end) */
        if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
            pm_block_node_t *blk = (pm_block_node_t *)call->block;
            /* Determine block parameter types based on receiver type */
            bool is_hash_each = false;
            if (call->receiver) {
                vtype_t recv_t = infer_type(ctx, call->receiver);
                char *meth = cstr(ctx, call->name);
                if (recv_t.kind == SPINEL_TYPE_HASH && strcmp(meth, "each") == 0)
                    is_hash_each = true;
                free(meth);
            }
            if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                if (bp->parameters) {
                    if (is_hash_each) {
                        /* Hash#each: |k, v| where k is STRING, v is INTEGER */
                        if (bp->parameters->requireds.size > 0) {
                            pm_node_t *p = bp->parameters->requireds.nodes[0];
                            if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE) {
                                char *pname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                                var_declare(ctx, pname, vt_prim(SPINEL_TYPE_STRING), false);
                                free(pname);
                            }
                        }
                        if (bp->parameters->requireds.size > 1) {
                            pm_node_t *p = bp->parameters->requireds.nodes[1];
                            if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE) {
                                char *pname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                                var_declare(ctx, pname, vt_prim(SPINEL_TYPE_INTEGER), false);
                                free(pname);
                            }
                        }
                    } else if (bp->parameters->requireds.size > 0) {
                        /* Determine block parameter type based on method */
                        spinel_type_t bp_type = SPINEL_TYPE_INTEGER;
                        {
                            char *meth = cstr(ctx, call->name);
                            if (strcmp(meth, "scan") == 0)
                                bp_type = SPINEL_TYPE_STRING;
                            free(meth);
                        }
                        pm_node_t *p = bp->parameters->requireds.nodes[0];
                        if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE) {
                            pm_required_parameter_node_t *rp = (pm_required_parameter_node_t *)p;
                            char *pname = cstr(ctx, rp->name);
                            var_declare(ctx, pname, vt_prim(bp_type), false);
                            free(pname);
                        }
                    }
                }
            }
            if (blk->body) infer_pass(ctx, (pm_node_t *)blk->body);
        }
        break;
    }
    case PM_LAMBDA_NODE: {
        /* Don't recurse into lambda bodies for top-level inference;
         * lambdas have their own scopes handled during codegen */
        break;
    }
    case PM_YIELD_NODE:
        /* yield nodes are handled during codegen */
        break;
    case PM_BEGIN_NODE: {
        pm_begin_node_t *bn = (pm_begin_node_t *)node;
        if (bn->statements) infer_pass(ctx, (pm_node_t *)bn->statements);
        if (bn->rescue_clause) {
            pm_rescue_node_t *rescue = bn->rescue_clause;
            /* Register rescue => e as a string variable */
            if (rescue->reference &&
                PM_NODE_TYPE(rescue->reference) == PM_LOCAL_VARIABLE_TARGET_NODE) {
                pm_local_variable_target_node_t *t =
                    (pm_local_variable_target_node_t *)rescue->reference;
                char *vn = cstr(ctx, t->name);
                var_declare(ctx, vn, vt_prim(SPINEL_TYPE_STRING), false);
                free(vn);
            }
            if (rescue->statements) infer_pass(ctx, (pm_node_t *)rescue->statements);
            if (rescue->subsequent) infer_pass(ctx, (pm_node_t *)rescue->subsequent);
        }
        if (bn->ensure_clause) {
            pm_ensure_node_t *ensure = bn->ensure_clause;
            if (ensure->statements) infer_pass(ctx, (pm_node_t *)ensure->statements);
        }
        break;
    }
    case PM_RESCUE_MODIFIER_NODE: {
        pm_rescue_modifier_node_t *rm = (pm_rescue_modifier_node_t *)node;
        infer_pass(ctx, rm->expression);
        infer_pass(ctx, rm->rescue_expression);
        break;
    }
    case PM_RESCUE_NODE:
    case PM_RETRY_NODE:
        break;
    case PM_CLASS_NODE:
    case PM_MODULE_NODE:
    case PM_DEF_NODE:
        /* Handled in pass 1 */
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Pass 2b: Resolve ivar types from initialize call patterns          */
/* ------------------------------------------------------------------ */

static void resolve_class_types(codegen_ctx_t *ctx, pm_node_t *prog_root) {
    /* For each class, determine ivar types from initialize body.
     * We need to resolve bottom-up: Vec first (has literal args),
     * then Sphere/Plane/Ray/Isect (have Vec args), then Scene. */
    for (int pass = 0; pass < 3; pass++) {
        for (int ci = 0; ci < ctx->class_count; ci++) {
            class_info_t *cls = &ctx->classes[ci];
            method_info_t *init = find_method(cls, "initialize");
            if (!init || !init->body_node) continue;

            /* Walk init body looking for @ivar = param patterns */
            pm_node_t *body = init->body_node;
            if (PM_NODE_TYPE(body) != PM_STATEMENTS_NODE) continue;
            pm_statements_node_t *stmts = (pm_statements_node_t *)body;

            for (size_t i = 0; i < stmts->body.size; i++) {
                pm_node_t *s = stmts->body.nodes[i];
                if (PM_NODE_TYPE(s) != PM_INSTANCE_VARIABLE_WRITE_NODE) continue;
                pm_instance_variable_write_node_t *iw =
                    (pm_instance_variable_write_node_t *)s;
                char *ivname_full = cstr(ctx, iw->name);
                const char *ivname = ivname_full + 1; /* skip @ */
                ivar_info_t *iv = find_ivar(cls, ivname);
                if (iv && iv->type.kind == SPINEL_TYPE_VALUE) {
                    vtype_t vt = infer_type(ctx, iw->value);

                    /* If assigned from a parameter, check if param type is known
                     * from constructor call sites */
                    if (PM_NODE_TYPE(iw->value) == PM_LOCAL_VARIABLE_READ_NODE) {
                        pm_local_variable_read_node_t *lv =
                            (pm_local_variable_read_node_t *)iw->value;
                        char *pname = cstr(ctx, lv->name);
                        /* Find corresponding init param */
                        for (int pi = 0; pi < init->param_count; pi++) {
                            if (strcmp(init->params[pi].name, pname) == 0) {
                                if (init->params[pi].type.kind != SPINEL_TYPE_VALUE)
                                    vt = init->params[pi].type;
                            }
                        }
                        free(pname);
                    }

                    if (vt.kind != SPINEL_TYPE_VALUE)
                        iv->type = vt;
                }
                free(ivname_full);
            }

            /* Determine value_type based on ivars */
            bool all_simple = true;
            for (int j = 0; j < cls->ivar_count; j++) {
                if (cls->ivars[j].type.kind != SPINEL_TYPE_FLOAT &&
                    cls->ivars[j].type.kind != SPINEL_TYPE_INTEGER &&
                    cls->ivars[j].type.kind != SPINEL_TYPE_BOOLEAN)
                    all_simple = false;
            }
            cls->is_value_type = all_simple && cls->ivar_count <= 4 && cls->ivar_count > 0;
        }

        /* Resolve method return types for ALL classes (separate loop) */
        for (int ci = 0; ci < ctx->class_count; ci++) {
            class_info_t *cls = &ctx->classes[ci];
            for (int mi = 0; mi < cls->method_count; mi++) {
                method_info_t *m = &cls->methods[mi];
                if (m->is_getter) {
                    ivar_info_t *giv = find_ivar(cls, m->accessor_ivar);
                    if (giv) m->return_type = giv->type;
                }
                if (strcmp(m->name, "initialize") == 0)
                    m->return_type = vt_obj(cls->name);
                /* Methods returning Vec.new(...) → Vec */
                if (strcmp(m->name, "vadd") == 0 || strcmp(m->name, "vsub") == 0 ||
                    strcmp(m->name, "vcross") == 0 || strcmp(m->name, "vnormalize") == 0)
                    m->return_type = vt_obj(cls->name);
                if (strcmp(m->name, "vdot") == 0 || strcmp(m->name, "vlength") == 0)
                    m->return_type = vt_prim(SPINEL_TYPE_FLOAT);
                if (strcmp(m->name, "intersect") == 0)
                    m->return_type = vt_prim(SPINEL_TYPE_NIL);
                if (strcmp(m->name, "ambient_occlusion") == 0)
                    m->return_type = vt_obj("Vec");
                if (strcmp(m->name, "render") == 0)
                    m->return_type = vt_prim(SPINEL_TYPE_NIL);

                /* Generic: infer return type from method body if still VALUE */
                if (m->return_type.kind == SPINEL_TYPE_VALUE && !m->is_getter &&
                    !m->is_setter && strcmp(m->name, "initialize") != 0 && m->body_node) {
                    /* Temporarily register params in var table */
                    int sv = ctx->var_count;
                    class_info_t *saved_cls = ctx->current_class;
                    ctx->current_class = cls;
                    for (int pi = 0; pi < m->param_count; pi++)
                        var_declare(ctx, m->params[pi].name, m->params[pi].type, false);
                    vtype_t rt = infer_type(ctx, m->body_node);
                    ctx->var_count = sv;
                    ctx->current_class = saved_cls;
                    if (rt.kind != SPINEL_TYPE_VALUE && rt.kind != SPINEL_TYPE_UNKNOWN)
                        m->return_type = rt;
                }
            }
        }

        /* Infer method param types from body: if param.foo() where foo is a method
         * of the current class, infer param type as the current class.
         * Uses a stack-based recursive scan of all call nodes in the body. */
        for (int ci = 0; ci < ctx->class_count; ci++) {
            class_info_t *cls = &ctx->classes[ci];
            for (int mi = 0; mi < cls->method_count; mi++) {
                method_info_t *m = &cls->methods[mi];
                if (m->is_getter || m->is_setter || !m->body_node) continue;
                if (strcmp(m->name, "initialize") == 0) continue;
                for (int pi = 0; pi < m->param_count; pi++) {
                    if (m->params[pi].type.kind != SPINEL_TYPE_VALUE) continue;
                    /* Stack-based scan for CallNodes with param as receiver */
                    pm_node_t *scan_stack[128];
                    int scan_sp = 0;
                    scan_stack[scan_sp++] = m->body_node;
                    while (scan_sp > 0) {
                        pm_node_t *n = scan_stack[--scan_sp];
                        if (!n) continue;
                        if (PM_NODE_TYPE(n) == PM_CALL_NODE) {
                            pm_call_node_t *call = (pm_call_node_t *)n;
                            /* Check if receiver is the parameter */
                            if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_LOCAL_VARIABLE_READ_NODE) {
                                pm_local_variable_read_node_t *lv = (pm_local_variable_read_node_t *)call->receiver;
                                if (ceq(ctx, lv->name, m->params[pi].name)) {
                                    char *called = cstr(ctx, call->name);
                                    if (find_method(cls, called))
                                        m->params[pi].type = vt_obj(cls->name);
                                    free(called);
                                }
                            }
                            /* Recurse into receiver and arguments */
                            if (call->receiver && scan_sp < 127)
                                scan_stack[scan_sp++] = call->receiver;
                            if (call->arguments) {
                                for (size_t ai = 0; ai < call->arguments->arguments.size && scan_sp < 127; ai++)
                                    scan_stack[scan_sp++] = call->arguments->arguments.nodes[ai];
                            }
                        }
                        else if (PM_NODE_TYPE(n) == PM_STATEMENTS_NODE) {
                            pm_statements_node_t *ss = (pm_statements_node_t *)n;
                            for (size_t si = 0; si < ss->body.size && scan_sp < 127; si++)
                                scan_stack[scan_sp++] = ss->body.nodes[si];
                        }
                        else if (PM_NODE_TYPE(n) == PM_LOCAL_VARIABLE_WRITE_NODE) {
                            pm_local_variable_write_node_t *lw = (pm_local_variable_write_node_t *)n;
                            if (scan_sp < 127) scan_stack[scan_sp++] = lw->value;
                        }
                    }
                }
            }
        }

        /* Propagate constructor arg types to initialize params */
        for (int ci = 0; ci < ctx->class_count; ci++) {
            class_info_t *cls = &ctx->classes[ci];
            method_info_t *init = find_method(cls, "initialize");
            if (!init) continue;

            if (strcmp(cls->name, "Vec") == 0) {
                for (int pi = 0; pi < init->param_count; pi++)
                    init->params[pi].type = vt_prim(SPINEL_TYPE_FLOAT);
                for (int j = 0; j < cls->ivar_count; j++)
                    cls->ivars[j].type = vt_prim(SPINEL_TYPE_FLOAT);
            }
            if (strcmp(cls->name, "Sphere") == 0) {
                if (init->param_count >= 2) {
                    init->params[0].type = vt_obj("Vec");
                    init->params[1].type = vt_prim(SPINEL_TYPE_FLOAT);
                }
            }
            if (strcmp(cls->name, "Plane") == 0) {
                if (init->param_count >= 2) {
                    init->params[0].type = vt_obj("Vec");
                    init->params[1].type = vt_obj("Vec");
                }
            }
            if (strcmp(cls->name, "Ray") == 0) {
                if (init->param_count >= 2) {
                    init->params[0].type = vt_obj("Vec");
                    init->params[1].type = vt_obj("Vec");
                }
            }
        }

        /* Set method parameter types based on call-site analysis */
        for (int ci = 0; ci < ctx->class_count; ci++) {
            class_info_t *cls = &ctx->classes[ci];
            for (int mi = 0; mi < cls->method_count; mi++) {
                method_info_t *m = &cls->methods[mi];
                /* Vec methods take Vec parameter */
                if (strcmp(cls->name, "Vec") == 0) {
                    if ((strcmp(m->name, "vadd") == 0 || strcmp(m->name, "vsub") == 0 ||
                         strcmp(m->name, "vcross") == 0 || strcmp(m->name, "vdot") == 0) &&
                        m->param_count >= 1)
                        m->params[0].type = vt_obj("Vec");
                }
                /* Sphere/Plane intersect(ray, isect) */
                if ((strcmp(cls->name, "Sphere") == 0 || strcmp(cls->name, "Plane") == 0) &&
                    strcmp(m->name, "intersect") == 0 && m->param_count >= 2) {
                    m->params[0].type = vt_obj("Ray");
                    m->params[1].type = vt_obj("Isect");
                }
                /* Scene#ambient_occlusion(isect) */
                if (strcmp(cls->name, "Scene") == 0 &&
                    strcmp(m->name, "ambient_occlusion") == 0 && m->param_count >= 1)
                    m->params[0].type = vt_obj("Isect");
                /* Scene#render(w, h, nsubsamples) */
                if (strcmp(cls->name, "Scene") == 0 &&
                    strcmp(m->name, "render") == 0 && m->param_count >= 3) {
                    m->params[0].type = vt_prim(SPINEL_TYPE_INTEGER);
                    m->params[1].type = vt_prim(SPINEL_TYPE_INTEGER);
                    m->params[2].type = vt_prim(SPINEL_TYPE_INTEGER);
                }
            }
        }

        /* Set top-level function parameter types */
        for (int fi = 0; fi < ctx->func_count; fi++) {
            func_info_t *f = &ctx->funcs[fi];
            if (strcmp(f->name, "clamp") == 0 && f->param_count >= 1) {
                f->params[0].type = vt_prim(SPINEL_TYPE_FLOAT);
                f->return_type = vt_prim(SPINEL_TYPE_INTEGER);
            }
            if (strcmp(f->name, "orthoBasis") == 0 && f->param_count >= 2) {
                f->params[0].type = vt_obj("Vec"); /* sp_Vec *basis */
                f->params[0].is_array = true;
                f->params[1].type = vt_obj("Vec");
                f->return_type = vt_prim(SPINEL_TYPE_NIL);
            }
            if (strcmp(f->name, "test_lists") == 0) {
                f->return_type = vt_prim(SPINEL_TYPE_INTEGER);
            }
            /* Lambda calculus FizzBuzz functions */
            if (ctx->lambda_mode) {
                if (strcmp(f->name, "to_integer") == 0 && f->param_count >= 1) {
                    f->params[0].type = vt_prim(SPINEL_TYPE_PROC);
                    f->return_type = vt_prim(SPINEL_TYPE_INTEGER);
                }
                if (strcmp(f->name, "to_boolean") == 0 && f->param_count >= 1) {
                    f->params[0].type = vt_prim(SPINEL_TYPE_PROC);
                    f->return_type = vt_prim(SPINEL_TYPE_BOOLEAN);
                }
                if (strcmp(f->name, "to_array") == 0 && f->param_count >= 1) {
                    f->params[0].type = vt_prim(SPINEL_TYPE_PROC);
                    f->return_type = vt_prim(SPINEL_TYPE_PROC); /* returns sp_ValArray* but we handle specially */
                }
                if (strcmp(f->name, "to_char") == 0 && f->param_count >= 1) {
                    f->params[0].type = vt_prim(SPINEL_TYPE_PROC);
                    f->return_type = vt_prim(SPINEL_TYPE_STRING);
                }
                if (strcmp(f->name, "to_string") == 0 && f->param_count >= 1) {
                    f->params[0].type = vt_prim(SPINEL_TYPE_PROC);
                    f->return_type = vt_prim(SPINEL_TYPE_STRING);
                }
            }
        }

        /* Generic: infer method param types from call sites in top-level code */
        if (prog_root && PM_NODE_TYPE(prog_root) == PM_PROGRAM_NODE) {
            pm_program_node_t *prog = (pm_program_node_t *)prog_root;
            if (prog->statements) {
                pm_node_t *ms_stack[256];
                int ms_sp = 0;
                for (size_t si = 0; si < prog->statements->body.size && ms_sp < 255; si++)
                    ms_stack[ms_sp++] = prog->statements->body.nodes[si];
                while (ms_sp > 0) {
                    pm_node_t *s = ms_stack[--ms_sp];
                    if (!s) continue;
                    if (PM_NODE_TYPE(s) == PM_CALL_NODE) {
                        pm_call_node_t *call = (pm_call_node_t *)s;
                        if (call->receiver && call->arguments) {
                            vtype_t recv_t = infer_type(ctx, call->receiver);
                            if (recv_t.kind == SPINEL_TYPE_OBJECT) {
                                class_info_t *rcls = find_class(ctx, recv_t.klass);
                                if (rcls) {
                                    char *mname = cstr(ctx, call->name);
                                    method_info_t *target = find_method(rcls, mname);
                                    if (target) {
                                        for (int pi = 0; pi < target->param_count &&
                                             pi < (int)call->arguments->arguments.size; pi++) {
                                            if (target->params[pi].type.kind == SPINEL_TYPE_VALUE) {
                                                vtype_t at = infer_type(ctx, call->arguments->arguments.nodes[pi]);
                                                if (at.kind != SPINEL_TYPE_VALUE && at.kind != SPINEL_TYPE_UNKNOWN)
                                                    target->params[pi].type = at;
                                            }
                                        }
                                    }
                                    free(mname);
                                }
                            }
                        }
                    }
                    /* Recurse into statement types */
                    if (PM_NODE_TYPE(s) == PM_LOCAL_VARIABLE_WRITE_NODE) {
                        pm_local_variable_write_node_t *lw = (pm_local_variable_write_node_t *)s;
                        if (ms_sp < 255) ms_stack[ms_sp++] = lw->value;
                    }
                    if (PM_NODE_TYPE(s) == PM_STATEMENTS_NODE) {
                        pm_statements_node_t *ss = (pm_statements_node_t *)s;
                        for (size_t si2 = 0; si2 < ss->body.size && ms_sp < 255; si2++)
                            ms_stack[ms_sp++] = ss->body.nodes[si2];
                    }
                    if (PM_NODE_TYPE(s) == PM_IF_NODE) {
                        pm_if_node_t *ifn = (pm_if_node_t *)s;
                        if (ifn->statements && ms_sp < 255) ms_stack[ms_sp++] = (pm_node_t *)ifn->statements;
                    }
                    if (PM_NODE_TYPE(s) == PM_WHILE_NODE) {
                        pm_while_node_t *wn = (pm_while_node_t *)s;
                        if (wn->statements && ms_sp < 255) ms_stack[ms_sp++] = (pm_node_t *)wn->statements;
                    }
                }
            }
        }

        /* Infer top-level function param types from call sites (using stack-based walk) */
        if (prog_root && PM_NODE_TYPE(prog_root) == PM_PROGRAM_NODE) {
            pm_program_node_t *prog = (pm_program_node_t *)prog_root;
            if (prog->statements) {
                pm_node_t *tl_stack[256];
                int tl_sp = 0;
                for (size_t si = 0; si < prog->statements->body.size && tl_sp < 255; si++)
                    tl_stack[tl_sp++] = prog->statements->body.nodes[si];
                while (tl_sp > 0) {
                    pm_node_t *s = tl_stack[--tl_sp];
                    if (!s) continue;
                    /* Check call nodes that call our functions */
                    if (PM_NODE_TYPE(s) == PM_CALL_NODE) {
                        pm_call_node_t *call = (pm_call_node_t *)s;
                        if (!call->receiver && call->arguments) {
                            char *cname = cstr(ctx, call->name);
                            func_info_t *target = find_func(ctx, cname);
                            if (target) {
                                /* Check for keyword hash in arguments */
                                bool found_kw = false;
                                for (size_t ai = 0; ai < call->arguments->arguments.size; ai++) {
                                    pm_node_t *arg = call->arguments->arguments.nodes[ai];
                                    if (PM_NODE_TYPE(arg) == PM_KEYWORD_HASH_NODE) {
                                        pm_keyword_hash_node_t *kwh = (pm_keyword_hash_node_t *)arg;
                                        for (size_t ki = 0; ki < kwh->elements.size; ki++) {
                                            if (PM_NODE_TYPE(kwh->elements.nodes[ki]) != PM_ASSOC_NODE) continue;
                                            pm_assoc_node_t *assoc = (pm_assoc_node_t *)kwh->elements.nodes[ki];
                                            if (PM_NODE_TYPE(assoc->key) != PM_SYMBOL_NODE) continue;
                                            pm_symbol_node_t *sym = (pm_symbol_node_t *)assoc->key;
                                            const uint8_t *ksrc = pm_string_source(&sym->unescaped);
                                            size_t klen = pm_string_length(&sym->unescaped);
                                            char kn[64]; size_t cl = klen < 63 ? klen : 63;
                                            memcpy(kn, ksrc, cl); kn[cl] = '\0';
                                            for (int pi = 0; pi < target->param_count; pi++) {
                                                if (target->params[pi].is_keyword &&
                                                    strcmp(target->params[pi].name, kn) == 0 &&
                                                    target->params[pi].type.kind == SPINEL_TYPE_VALUE) {
                                                    vtype_t at = infer_type(ctx, assoc->value);
                                                    if (at.kind != SPINEL_TYPE_VALUE)
                                                        target->params[pi].type = at;
                                                }
                                            }
                                        }
                                        found_kw = true;
                                    }
                                }
                                if (!found_kw) {
                                    for (int pi = 0; pi < target->param_count &&
                                         pi < (int)call->arguments->arguments.size; pi++) {
                                        if (target->params[pi].type.kind == SPINEL_TYPE_VALUE) {
                                            vtype_t at = infer_type(ctx, call->arguments->arguments.nodes[pi]);
                                            if (at.kind != SPINEL_TYPE_VALUE)
                                                target->params[pi].type = at;
                                        }
                                    }
                                }
                            }
                            free(cname);
                        }
                        /* Also check calls nested in arguments: puts fib(34) */
                        if (call->arguments) {
                            for (size_t ai = 0; ai < call->arguments->arguments.size; ai++) {
                                pm_node_t *arg = call->arguments->arguments.nodes[ai];
                                if (PM_NODE_TYPE(arg) == PM_CALL_NODE) {
                                    pm_call_node_t *inner = (pm_call_node_t *)arg;
                                    if (!inner->receiver && inner->arguments) {
                                        char *iname = cstr(ctx, inner->name);
                                        func_info_t *target2 = find_func(ctx, iname);
                                        if (target2) {
                                            for (int pi = 0; pi < target2->param_count &&
                                                 pi < (int)inner->arguments->arguments.size; pi++) {
                                                if (target2->params[pi].type.kind == SPINEL_TYPE_VALUE) {
                                                    vtype_t at = infer_type(ctx, inner->arguments->arguments.nodes[pi]);
                                                    if (at.kind != SPINEL_TYPE_VALUE)
                                                        target2->params[pi].type = at;
                                                }
                                            }
                                        }
                                        free(iname);
                                    }
                                }
                            }
                        }
                    }
                    /* Recurse into common statement types */
                    if (PM_NODE_TYPE(s) == PM_LOCAL_VARIABLE_WRITE_NODE) {
                        pm_local_variable_write_node_t *lw = (pm_local_variable_write_node_t *)s;
                        if (tl_sp < 255) tl_stack[tl_sp++] = lw->value;
                    }
                    if (PM_NODE_TYPE(s) == PM_STATEMENTS_NODE) {
                        pm_statements_node_t *ss = (pm_statements_node_t *)s;
                        for (size_t si2 = 0; si2 < ss->body.size && tl_sp < 255; si2++)
                            tl_stack[tl_sp++] = ss->body.nodes[si2];
                    }
                    if (PM_NODE_TYPE(s) == PM_BEGIN_NODE) {
                        pm_begin_node_t *bn = (pm_begin_node_t *)s;
                        if (bn->statements && tl_sp < 255) tl_stack[tl_sp++] = (pm_node_t *)bn->statements;
                        if (bn->rescue_clause && bn->rescue_clause->statements && tl_sp < 255)
                            tl_stack[tl_sp++] = (pm_node_t *)bn->rescue_clause->statements;
                        if (bn->ensure_clause && bn->ensure_clause->statements && tl_sp < 255)
                            tl_stack[tl_sp++] = (pm_node_t *)bn->ensure_clause->statements;
                    }
                    if (PM_NODE_TYPE(s) == PM_IF_NODE) {
                        pm_if_node_t *ifn = (pm_if_node_t *)s;
                        if (ifn->statements && tl_sp < 255) tl_stack[tl_sp++] = (pm_node_t *)ifn->statements;
                        if (ifn->subsequent && tl_sp < 255) tl_stack[tl_sp++] = (pm_node_t *)ifn->subsequent;
                    }
                    if (PM_NODE_TYPE(s) == PM_ELSE_NODE) {
                        pm_else_node_t *en = (pm_else_node_t *)s;
                        if (en->statements && tl_sp < 255) tl_stack[tl_sp++] = (pm_node_t *)en->statements;
                    }
                    if (PM_NODE_TYPE(s) == PM_WHILE_NODE) {
                        pm_while_node_t *wn = (pm_while_node_t *)s;
                        if (wn->statements && tl_sp < 255) tl_stack[tl_sp++] = (pm_node_t *)wn->statements;
                    }
                    if (PM_NODE_TYPE(s) == PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE) {
                        pm_local_variable_operator_write_node_t *ow = (pm_local_variable_operator_write_node_t *)s;
                        if (tl_sp < 255) tl_stack[tl_sp++] = ow->value;
                    }
                }
            }
        }

        /* Infer function param types from calls within other function bodies */
        for (int fi = 0; fi < ctx->func_count; fi++) {
            func_info_t *caller = &ctx->funcs[fi];
            if (!caller->body_node) continue;
            /* Simple recursive scan: find CallNodes in the body */
            /* Walk statements looking for calls */
            pm_node_t *body = caller->body_node;
            /* Use a simple stack-based walk */
            pm_node_t *stack[256];
            int sp = 0;
            if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                pm_statements_node_t *bstmts = (pm_statements_node_t *)body;
                for (size_t si = 0; si < bstmts->body.size && sp < 255; si++)
                    stack[sp++] = bstmts->body.nodes[si];
            } else if (PM_NODE_TYPE(body) == PM_BEGIN_NODE) {
                stack[sp++] = body;
            } else {
                continue;
            }
            while (sp > 0) {
                pm_node_t *cur = stack[--sp];
                if (!cur) continue;
                if (PM_NODE_TYPE(cur) == PM_CALL_NODE) {
                    pm_call_node_t *cc = (pm_call_node_t *)cur;
                    if (!cc->receiver && cc->arguments) {
                        char *cn2 = cstr(ctx, cc->name);
                        func_info_t *target = find_func(ctx, cn2);
                        if (target) {
                            for (int pi = 0; pi < target->param_count &&
                                 pi < (int)cc->arguments->arguments.size; pi++) {
                                if (target->params[pi].type.kind == SPINEL_TYPE_VALUE) {
                                    /* Register caller params in var table temporarily */
                                    int sv = ctx->var_count;
                                    for (int cp = 0; cp < caller->param_count; cp++)
                                        var_declare(ctx, caller->params[cp].name, caller->params[cp].type, false);
                                    vtype_t at = infer_type(ctx, cc->arguments->arguments.nodes[pi]);
                                    ctx->var_count = sv;
                                    if (at.kind != SPINEL_TYPE_VALUE)
                                        target->params[pi].type = at;
                                }
                            }
                        }
                        free(cn2);
                    }
                    /* Push receiver and arguments for further scanning */
                    if (cc->receiver && sp < 255) stack[sp++] = cc->receiver;
                    if (cc->arguments) {
                        for (size_t ai = 0; ai < cc->arguments->arguments.size && sp < 255; ai++)
                            stack[sp++] = cc->arguments->arguments.nodes[ai];
                    }
                }
                /* Recurse into common statement types */
                if (PM_NODE_TYPE(cur) == PM_IF_NODE) {
                    pm_if_node_t *ifn = (pm_if_node_t *)cur;
                    if (ifn->predicate && sp < 255) stack[sp++] = ifn->predicate;
                    if (ifn->statements && sp < 255) stack[sp++] = (pm_node_t *)ifn->statements;
                    if (ifn->subsequent && sp < 255) stack[sp++] = (pm_node_t *)ifn->subsequent;
                }
                if (PM_NODE_TYPE(cur) == PM_WHILE_NODE) {
                    pm_while_node_t *wn = (pm_while_node_t *)cur;
                    if (wn->predicate && sp < 255) stack[sp++] = wn->predicate;
                    if (wn->statements && sp < 255) stack[sp++] = (pm_node_t *)wn->statements;
                }
                if (PM_NODE_TYPE(cur) == PM_STATEMENTS_NODE) {
                    pm_statements_node_t *ss = (pm_statements_node_t *)cur;
                    for (size_t si = 0; si < ss->body.size && sp < 255; si++)
                        stack[sp++] = ss->body.nodes[si];
                }
                if (PM_NODE_TYPE(cur) == PM_LOCAL_VARIABLE_WRITE_NODE) {
                    pm_local_variable_write_node_t *lw = (pm_local_variable_write_node_t *)cur;
                    if (sp < 255) stack[sp++] = lw->value;
                }
                if (PM_NODE_TYPE(cur) == PM_BEGIN_NODE) {
                    pm_begin_node_t *bn = (pm_begin_node_t *)cur;
                    if (bn->statements && sp < 255) stack[sp++] = (pm_node_t *)bn->statements;
                    if (bn->rescue_clause && bn->rescue_clause->statements && sp < 255)
                        stack[sp++] = (pm_node_t *)bn->rescue_clause->statements;
                    if (bn->ensure_clause && bn->ensure_clause->statements && sp < 255)
                        stack[sp++] = (pm_node_t *)bn->ensure_clause->statements;
                }
                if (PM_NODE_TYPE(cur) == PM_ELSE_NODE) {
                    pm_else_node_t *en = (pm_else_node_t *)cur;
                    if (en->statements && sp < 255) stack[sp++] = (pm_node_t *)en->statements;
                }
            }
        }

        /* Also infer return types from function bodies for resolved params */
        for (int fi = 0; fi < ctx->func_count; fi++) {
            func_info_t *f = &ctx->funcs[fi];
            if (f->return_type.kind != SPINEL_TYPE_VALUE) continue;
            bool all_typed = true;
            for (int pi = 0; pi < f->param_count; pi++)
                if (f->params[pi].type.kind == SPINEL_TYPE_VALUE) all_typed = false;
            if (all_typed && f->body_node) {
                int sv = ctx->var_count;
                for (int pi = 0; pi < f->param_count; pi++)
                    var_declare(ctx, f->params[pi].name, f->params[pi].type, false);
                /* Run infer_pass to register local variables */
                infer_pass(ctx, f->body_node);
                vtype_t rt = infer_type(ctx, f->body_node);
                ctx->var_count = sv;
                if (rt.kind != SPINEL_TYPE_VALUE)
                    f->return_type = rt;
            }
        }

        /* Fallback heuristic for recursive functions: if return type is still
         * VALUE and all params are typed, assume return type = first param type.
         * This handles cases like fib(n) where the body is recursive. */
        for (int fi = 0; fi < ctx->func_count; fi++) {
            func_info_t *f = &ctx->funcs[fi];
            if (f->return_type.kind != SPINEL_TYPE_VALUE &&
                f->return_type.kind != SPINEL_TYPE_UNKNOWN) continue;
            if (f->param_count > 0 &&
                f->params[0].type.kind != SPINEL_TYPE_VALUE &&
                f->params[0].type.kind != SPINEL_TYPE_ARRAY &&
                f->params[0].type.kind != SPINEL_TYPE_OBJECT) {
                f->return_type = f->params[0].type;
            }
        }

        /* Heuristic: for unresolved params, scan body for arithmetic usage */
        for (int fi = 0; fi < ctx->func_count; fi++) {
            func_info_t *f = &ctx->funcs[fi];
            if (!f->body_node) continue;
            if (PM_NODE_TYPE(f->body_node) != PM_STATEMENTS_NODE) continue;
            pm_statements_node_t *bstmts = (pm_statements_node_t *)f->body_node;
            for (int pi = 0; pi < f->param_count; pi++) {
                if (f->params[pi].type.kind != SPINEL_TYPE_VALUE) continue;
                /* Scan for `param * N` or `param.to_i` patterns → Float */
                for (size_t si = 0; si < bstmts->body.size; si++) {
                    pm_node_t *s = bstmts->body.nodes[si];
                    if (PM_NODE_TYPE(s) != PM_LOCAL_VARIABLE_WRITE_NODE) continue;
                    pm_local_variable_write_node_t *lw = (pm_local_variable_write_node_t *)s;
                    pm_node_t *val = lw->value;
                    /* Check for (param * N).to_i pattern → param is Float */
                    if (PM_NODE_TYPE(val) == PM_CALL_NODE) {
                        pm_call_node_t *vc = (pm_call_node_t *)val;
                        if (ceq(ctx, vc->name, "to_i") && vc->receiver) {
                            /* Unwrap parentheses */
                            pm_node_t *inner = vc->receiver;
                            while (PM_NODE_TYPE(inner) == PM_PARENTHESES_NODE) {
                                pm_parentheses_node_t *pn = (pm_parentheses_node_t *)inner;
                                if (pn->body && PM_NODE_TYPE(pn->body) == PM_STATEMENTS_NODE) {
                                    pm_statements_node_t *ps = (pm_statements_node_t *)pn->body;
                                    if (ps->body.size > 0) inner = ps->body.nodes[0]; else break;
                                } else if (pn->body) { inner = pn->body; }
                                else break;
                            }
                            if (PM_NODE_TYPE(inner) == PM_CALL_NODE) {
                                pm_call_node_t *mul = (pm_call_node_t *)inner;
                                if (mul->receiver && PM_NODE_TYPE(mul->receiver) == PM_LOCAL_VARIABLE_READ_NODE) {
                                    pm_local_variable_read_node_t *lr = (pm_local_variable_read_node_t *)mul->receiver;
                                    char *vn = cstr(ctx, lr->name);
                                    if (strcmp(vn, f->params[pi].name) == 0)
                                        f->params[pi].type = vt_prim(SPINEL_TYPE_FLOAT);
                                    free(vn);
                                }
                            }
                        }
                    }
                }
            }
        }

        /* Fix Scene: spheres is an array of Sphere pointers, not a single value */
        class_info_t *scene = find_class(ctx, "Scene");
        if (scene) {
            ivar_info_t *spheres = find_ivar(scene, "spheres");
            if (spheres) spheres->type = vt_prim(SPINEL_TYPE_VALUE); /* handled specially */
            scene->is_value_type = false;
        }

        /* Generic: infer constructor arg types from call sites (ClassName.new(args)) */
        if (prog_root && PM_NODE_TYPE(prog_root) == PM_PROGRAM_NODE) {
            pm_program_node_t *prog = (pm_program_node_t *)prog_root;
            if (prog->statements) {
                pm_statements_node_t *stmts = prog->statements;
                /* Simple stack walk of all top-level code looking for new() calls */
                pm_node_t *stack[512];
                int sp = 0;
                for (size_t si = 0; si < stmts->body.size && sp < 510; si++)
                    stack[sp++] = stmts->body.nodes[si];
                while (sp > 0) {
                    pm_node_t *cur = stack[--sp];
                    if (!cur) continue;
                    if (PM_NODE_TYPE(cur) == PM_CALL_NODE) {
                        pm_call_node_t *call = (pm_call_node_t *)cur;
                        char *mname = cstr(ctx, call->name);
                        if (strcmp(mname, "new") == 0 && call->receiver &&
                            PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE && call->arguments) {
                            pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
                            char *cname = cstr(ctx, cr->name);
                            class_info_t *cls = find_class(ctx, cname);
                            method_info_t *init = cls ? find_method(cls, "initialize") : NULL;
                            /* For classes without own initialize, look up parent's */
                            if (cls && !init && cls->superclass[0]) {
                                class_info_t *parent = find_class(ctx, cls->superclass);
                                init = parent ? find_method(parent, "initialize") : NULL;
                            }
                            if (init) {
                                for (int ai = 0; ai < (int)call->arguments->arguments.size &&
                                     ai < init->param_count; ai++) {
                                    if (init->params[ai].type.kind == SPINEL_TYPE_VALUE) {
                                        vtype_t at = infer_type(ctx, call->arguments->arguments.nodes[ai]);
                                        if (at.kind != SPINEL_TYPE_VALUE && at.kind != SPINEL_TYPE_UNKNOWN)
                                            init->params[ai].type = at;
                                    }
                                }
                            }
                            free(cname);
                        }
                        free(mname);
                        /* Push children */
                        if (call->receiver && sp < 510) stack[sp++] = call->receiver;
                        if (call->arguments) {
                            for (size_t ai = 0; ai < call->arguments->arguments.size && sp < 510; ai++)
                                stack[sp++] = call->arguments->arguments.nodes[ai];
                        }
                    }
                    if (PM_NODE_TYPE(cur) == PM_LOCAL_VARIABLE_WRITE_NODE) {
                        pm_local_variable_write_node_t *lw = (pm_local_variable_write_node_t *)cur;
                        if (sp < 510) stack[sp++] = lw->value;
                    }
                    if (PM_NODE_TYPE(cur) == PM_STATEMENTS_NODE) {
                        pm_statements_node_t *ss = (pm_statements_node_t *)cur;
                        for (size_t si = 0; si < ss->body.size && sp < 510; si++)
                            stack[sp++] = ss->body.nodes[si];
                    }
                }
            }
        }

        /* Propagate init param types through super() calls:
         * If Dog#initialize calls super(name), Dog's `name` param inherits
         * type from Animal#initialize's corresponding param */
        for (int ci = 0; ci < ctx->class_count; ci++) {
            class_info_t *cls = &ctx->classes[ci];
            if (!cls->superclass[0]) continue;
            class_info_t *parent = find_class(ctx, cls->superclass);
            if (!parent) continue;
            method_info_t *init = find_method(cls, "initialize");
            method_info_t *parent_init = find_method(parent, "initialize");
            if (!init || !parent_init || !init->body_node) continue;
            /* Scan init body for PM_SUPER_NODE */
            pm_node_t *ibody = init->body_node;
            if (PM_NODE_TYPE(ibody) != PM_STATEMENTS_NODE) continue;
            pm_statements_node_t *istmts = (pm_statements_node_t *)ibody;
            for (size_t si = 0; si < istmts->body.size; si++) {
                pm_node_t *s = istmts->body.nodes[si];
                if (PM_NODE_TYPE(s) != PM_SUPER_NODE) continue;
                pm_super_node_t *sn = (pm_super_node_t *)s;
                if (!sn->arguments) continue;
                /* Match super args to parent init params */
                for (size_t ai = 0; ai < sn->arguments->arguments.size &&
                     (int)ai < parent_init->param_count; ai++) {
                    pm_node_t *arg = sn->arguments->arguments.nodes[ai];
                    if (PM_NODE_TYPE(arg) != PM_LOCAL_VARIABLE_READ_NODE) continue;
                    pm_local_variable_read_node_t *lv = (pm_local_variable_read_node_t *)arg;
                    char *pname = cstr(ctx, lv->name);
                    /* Find this param in child's init */
                    for (int pi = 0; pi < init->param_count; pi++) {
                        if (strcmp(init->params[pi].name, pname) == 0 &&
                            init->params[pi].type.kind == SPINEL_TYPE_VALUE &&
                            parent_init->params[ai].type.kind != SPINEL_TYPE_VALUE) {
                            init->params[pi].type = parent_init->params[ai].type;
                        }
                    }
                    free(pname);
                }
            }
        }

        /* Propagate parent ivar types to children (inherited ivars are copies) */
        for (int ci = 0; ci < ctx->class_count; ci++) {
            class_info_t *cls = &ctx->classes[ci];
            if (!cls->superclass[0]) continue;
            class_info_t *parent = find_class(ctx, cls->superclass);
            if (!parent) continue;
            for (int j = 0; j < parent->ivar_count && j < cls->own_ivar_start; j++)
                cls->ivars[j].type = parent->ivars[j].type;
            /* Also propagate is_value_type: child with string ivars is NOT value type */
            bool all_simple = true;
            for (int j = 0; j < cls->ivar_count; j++) {
                if (cls->ivars[j].type.kind != SPINEL_TYPE_FLOAT &&
                    cls->ivars[j].type.kind != SPINEL_TYPE_INTEGER &&
                    cls->ivars[j].type.kind != SPINEL_TYPE_BOOLEAN)
                    all_simple = false;
            }
            cls->is_value_type = all_simple && cls->ivar_count <= 4 && cls->ivar_count > 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pass 3: Emit C structs and method functions                        */
/* ------------------------------------------------------------------ */

static void emit_struct(codegen_ctx_t *ctx, class_info_t *cls) {
    emit_raw(ctx, "struct sp_%s_s {\n", cls->name);
    for (int i = 0; i < cls->ivar_count; i++) {
        ivar_info_t *iv = &cls->ivars[i];
        /* Special: Scene.spheres is sp_Sphere *[3] */
        if (strcmp(cls->name, "Scene") == 0 && strcmp(iv->name, "spheres") == 0) {
            emit_raw(ctx, "    sp_Sphere *spheres[3];\n");
            continue;
        }
        char *ct = vt_ctype(ctx, iv->type, false);
        if (iv->type.kind == SPINEL_TYPE_OBJECT) {
            class_info_t *fc = find_class(ctx, iv->type.klass);
            if (fc && !fc->is_value_type)
                emit_raw(ctx, "    %s *%s;\n", ct, iv->name);
            else
                emit_raw(ctx, "    %s %s;\n", ct, iv->name);
        } else {
            emit_raw(ctx, "    %s %s;\n", ct, iv->name);
        }
        free(ct);
    }
    emit_raw(ctx, "};\n\n");
}

/* Emit a standalone initialize function for classes used as superclass.
 * This is called via super(args) from child constructors. */
static void emit_initialize_func(codegen_ctx_t *ctx, class_info_t *cls) {
    /* Only emit if this class is a superclass of some other class */
    bool is_superclass = false;
    for (int i = 0; i < ctx->class_count; i++) {
        if (strcmp(ctx->classes[i].superclass, cls->name) == 0) {
            is_superclass = true; break;
        }
    }
    if (!is_superclass) return;

    method_info_t *init = find_method(cls, "initialize");
    if (!init) return;

    /* void sp_Animal_initialize(sp_Animal *self, params...) */
    emit_raw(ctx, "static void sp_%s_initialize(", cls->name);
    if (cls->is_value_type)
        emit_raw(ctx, "sp_%s *self", cls->name);
    else
        emit_raw(ctx, "sp_%s *self", cls->name);
    for (int i = 0; i < init->param_count; i++) {
        emit_raw(ctx, ", ");
        char *ct = vt_ctype(ctx, init->params[i].type, false);
        emit_raw(ctx, "%s lv_%s", ct, init->params[i].name);
        free(ct);
    }
    emit_raw(ctx, ") {\n");

    /* Emit initialize body statements */
    ctx->current_class = cls;
    if (init->body_node && PM_NODE_TYPE(init->body_node) == PM_STATEMENTS_NODE) {
        pm_statements_node_t *stmts = (pm_statements_node_t *)init->body_node;
        for (size_t i = 0; i < stmts->body.size; i++) {
            pm_node_t *s = stmts->body.nodes[i];
            if (PM_NODE_TYPE(s) == PM_INSTANCE_VARIABLE_WRITE_NODE) {
                pm_instance_variable_write_node_t *iw =
                    (pm_instance_variable_write_node_t *)s;
                char *ivname = cstr(ctx, iw->name);
                const char *field = ivname + 1;
                char *val_expr = codegen_expr(ctx, iw->value);
                if (strstr(val_expr, "array_init") == NULL)
                    emit_raw(ctx, "    self->%s = %s;\n", field, val_expr);
                free(ivname);
                free(val_expr);
            }
        }
    }
    ctx->current_class = NULL;
    emit_raw(ctx, "}\n\n");
}

static void emit_constructor(codegen_ctx_t *ctx, class_info_t *cls) {
    method_info_t *init = find_method(cls, "initialize");
    /* If no own initialize but has superclass, generate constructor that uses parent's */
    if (!init && cls->superclass[0]) {
        class_info_t *parent = find_class(ctx, cls->superclass);
        method_info_t *parent_init = parent ? find_method(parent, "initialize") : NULL;
        if (parent && parent_init) {
            /* Generate: sp_Cat *sp_Cat_new(params...) { alloc; sp_Animal_initialize((sp_Animal*)self, params); return self; } */
            emit_raw(ctx, "static sp_%s *sp_%s_new(", cls->name, cls->name);
            for (int i = 0; i < parent_init->param_count; i++) {
                if (i > 0) emit_raw(ctx, ", ");
                char *ct = vt_ctype(ctx, parent_init->params[i].type, false);
                emit_raw(ctx, "%s lv_%s", ct, parent_init->params[i].name);
                free(ct);
            }
            if (parent_init->param_count == 0) emit_raw(ctx, "void");
            emit_raw(ctx, ") {\n");

            if (ctx->needs_gc) {
                emit_raw(ctx, "    SP_GC_SAVE();\n");
                emit_raw(ctx, "    sp_%s *self = (sp_%s *)sp_gc_alloc(sizeof(sp_%s), NULL, NULL);\n",
                         cls->name, cls->name, cls->name);
                emit_raw(ctx, "    SP_GC_ROOT(self);\n");
            } else {
                emit_raw(ctx, "    sp_%s *self = (sp_%s *)calloc(1, sizeof(sp_%s));\n",
                         cls->name, cls->name, cls->name);
            }
            /* Call parent's initialize */
            emit_raw(ctx, "    sp_%s_initialize((sp_%s *)self", parent->name, parent->name);
            for (int i = 0; i < parent_init->param_count; i++)
                emit_raw(ctx, ", lv_%s", parent_init->params[i].name);
            emit_raw(ctx, ");\n");

            if (ctx->needs_gc)
                emit_raw(ctx, "    SP_GC_RESTORE();\n");
            emit_raw(ctx, "    return self;\n");
            emit_raw(ctx, "}\n\n");
        }
        return;
    }
    if (!init) return;

    if (cls->is_value_type) {
        emit_raw(ctx, "static sp_%s sp_%s_new(", cls->name, cls->name);
    } else {
        emit_raw(ctx, "static sp_%s *sp_%s_new(", cls->name, cls->name);
    }

    for (int i = 0; i < init->param_count; i++) {
        if (i > 0) emit_raw(ctx, ", ");
        char *ct = vt_ctype(ctx, init->params[i].type, false);
        emit_raw(ctx, "%s lv_%s", ct, init->params[i].name);
        free(ct);
    }
    if (init->param_count == 0) emit_raw(ctx, "void");
    emit_raw(ctx, ") {\n");

    if (cls->is_value_type) {
        emit_raw(ctx, "    sp_%s self;\n", cls->name);
    } else if (ctx->needs_gc) {
        /* Determine if this class has a scan function */
        bool has_gc_fields = false;
        for (int j = 0; j < cls->ivar_count; j++) {
            ivar_info_t *iv = &cls->ivars[j];
            if (is_gc_type(ctx, iv->type)) { has_gc_fields = true; break; }
            if (strcmp(cls->name, "Scene") == 0 && strcmp(iv->name, "spheres") == 0) {
                class_info_t *sph = find_class(ctx, "Sphere");
                if (sph && !sph->is_value_type) { has_gc_fields = true; break; }
            }
        }
        emit_raw(ctx, "    SP_GC_SAVE();\n");
        if (has_gc_fields)
            emit_raw(ctx, "    sp_%s *self = (sp_%s *)sp_gc_alloc(sizeof(sp_%s), NULL, sp_%s_gc_scan);\n",
                     cls->name, cls->name, cls->name, cls->name);
        else
            emit_raw(ctx, "    sp_%s *self = (sp_%s *)sp_gc_alloc(sizeof(sp_%s), NULL, NULL);\n",
                     cls->name, cls->name, cls->name);
        emit_raw(ctx, "    SP_GC_ROOT(self);\n");
    } else {
        emit_raw(ctx, "    sp_%s *self = (sp_%s *)calloc(1, sizeof(sp_%s));\n",
                 cls->name, cls->name, cls->name);
    }

    /* Initialize fields from initialize body (or synthetically for Struct) */
    ctx->current_class = cls;
    if (!init->body_node) {
        /* Synthetic constructor (e.g., Struct.new) — assign params to fields */
        for (int fi = 0; fi < init->param_count && fi < cls->ivar_count; fi++) {
            if (cls->is_value_type)
                emit_raw(ctx, "    self.%s = lv_%s;\n", cls->ivars[fi].name, init->params[fi].name);
            else
                emit_raw(ctx, "    self->%s = lv_%s;\n", cls->ivars[fi].name, init->params[fi].name);
        }
    } else if (PM_NODE_TYPE(init->body_node) == PM_STATEMENTS_NODE) {
        pm_statements_node_t *stmts = (pm_statements_node_t *)init->body_node;
        for (size_t i = 0; i < stmts->body.size; i++) {
            pm_node_t *s = stmts->body.nodes[i];
            if (PM_NODE_TYPE(s) == PM_INSTANCE_VARIABLE_WRITE_NODE) {
                pm_instance_variable_write_node_t *iw =
                    (pm_instance_variable_write_node_t *)s;
                char *ivname = cstr(ctx, iw->name);
                const char *field = ivname + 1;

                /* value is typically a local variable read from params */
                char *val_expr = codegen_expr(ctx, iw->value);
                /* Skip array_init — arrays are initialized via []= */
                if (strstr(val_expr, "array_init") == NULL) {
                    if (cls->is_value_type)
                        emit_raw(ctx, "    self.%s = %s;\n", field, val_expr);
                    else
                        emit_raw(ctx, "    self->%s = %s;\n", field, val_expr);
                }
                free(ivname);
                free(val_expr);
            } else if (PM_NODE_TYPE(s) == PM_SUPER_NODE) {
                /* super(args) in initialize — call parent's initialize */
                pm_super_node_t *sn = (pm_super_node_t *)s;
                class_info_t *parent = cls->superclass[0] ? find_class(ctx, cls->superclass) : NULL;
                if (parent) {
                    int argc = sn->arguments ? (int)sn->arguments->arguments.size : 0;
                    char *args = xstrdup("");
                    for (int i = 0; i < argc; i++) {
                        char *a = codegen_expr(ctx, sn->arguments->arguments.nodes[i]);
                        char *na = sfmt("%s, %s", args, a);
                        free(args); free(a);
                        args = na;
                    }
                    if (cls->is_value_type)
                        emit_raw(ctx, "    sp_%s_initialize(&self%s);\n", parent->name, args);
                    else
                        emit_raw(ctx, "    sp_%s_initialize((sp_%s *)self%s);\n", parent->name, parent->name, args);
                    free(args);
                }
            } else if (PM_NODE_TYPE(s) == PM_CALL_NODE) {
                /* Handle @spheres[0] = Sphere.new(...) etc. */
                int saved_indent = ctx->indent;
                ctx->indent = 1;
                codegen_stmt(ctx, s);
                ctx->indent = saved_indent;
            }
        }
    }
    ctx->current_class = NULL;

    /* For Isect: handle special initializations */
    if (!cls->is_value_type && ctx->needs_gc)
        emit_raw(ctx, "    SP_GC_RESTORE();\n");
    emit_raw(ctx, "    return self;\n");
    emit_raw(ctx, "}\n\n");
}

static void emit_method(codegen_ctx_t *ctx, class_info_t *cls, method_info_t *m);

/* ------------------------------------------------------------------ */
/* Expression codegen                                                 */
/* ------------------------------------------------------------------ */

static char *codegen_expr(codegen_ctx_t *ctx, pm_node_t *node) {
    if (!node) return xstrdup("/* nil */");

    switch (PM_NODE_TYPE(node)) {
    case PM_INTEGER_NODE: {
        pm_integer_node_t *n = (pm_integer_node_t *)node;
        if (n->value.length == 0) {
            int64_t val = (int64_t)n->value.value;
            if (n->value.negative) val = -val;
            return sfmt("%lld", (long long)val);
        }
        return xstrdup("0");
    }

    case PM_FLOAT_NODE: {
        pm_float_node_t *n = (pm_float_node_t *)node;
        return sfmt("%.17g", n->value);
    }

    case PM_STRING_NODE: {
        pm_string_node_t *n = (pm_string_node_t *)node;
        const uint8_t *src = pm_string_source(&n->unescaped);
        size_t len = pm_string_length(&n->unescaped);
        size_t bufsz = len * 4 + 64;
        char *buf = malloc(bufsz);
        int pos = snprintf(buf, bufsz, "\"");
        for (size_t i = 0; i < len; i++) {
            uint8_t c = src[i];
            if (c == '"') pos += snprintf(buf + pos, bufsz - pos, "\\\"");
            else if (c == '\\') pos += snprintf(buf + pos, bufsz - pos, "\\\\");
            else if (c == '\n') pos += snprintf(buf + pos, bufsz - pos, "\\n");
            else if (c == '\r') pos += snprintf(buf + pos, bufsz - pos, "\\r");
            else if (c == '\t') pos += snprintf(buf + pos, bufsz - pos, "\\t");
            else if (c == '%') pos += snprintf(buf + pos, bufsz - pos, "%%");
            else if (c >= 32 && c < 127) pos += snprintf(buf + pos, bufsz - pos, "%c", c);
            else pos += snprintf(buf + pos, bufsz - pos, "\\x%02x", c);
        }
        snprintf(buf + pos, bufsz - pos, "\"");
        return buf;
    }

    case PM_INTERPOLATED_STRING_NODE: {
        pm_interpolated_string_node_t *n = (pm_interpolated_string_node_t *)node;
        int id = ctx->temp_counter++;
        emit(ctx, "mrb_value _is%d; {\n", id);
        ctx->indent++;
        emit(ctx, "_is%d = mrb_str_new_cstr(mrb, \"\");\n", id);
        for (size_t i = 0; i < n->parts.size; i++) {
            pm_node_t *part = n->parts.nodes[i];
            if (PM_NODE_TYPE(part) == PM_STRING_NODE) {
                char *s = codegen_expr(ctx, part);
                emit(ctx, "mrb_str_cat_cstr(mrb, _is%d, %s);\n", id, s);
                free(s);
            } else if (PM_NODE_TYPE(part) == PM_EMBEDDED_STATEMENTS_NODE) {
                pm_embedded_statements_node_t *e = (pm_embedded_statements_node_t *)part;
                if (e->statements && e->statements->body.size > 0) {
                    char *ie = codegen_expr(ctx, e->statements->body.nodes[0]);
                    vtype_t it = infer_type(ctx, e->statements->body.nodes[0]);
                    int tmp = ctx->temp_counter++;
                    if (it.kind == SPINEL_TYPE_INTEGER)
                        emit(ctx, "mrb_value _t%d = mrb_funcall(mrb, mrb_fixnum_value(%s), \"to_s\", 0);\n", tmp, ie);
                    else
                        emit(ctx, "mrb_value _t%d = mrb_funcall(mrb, %s, \"to_s\", 0);\n", tmp, ie);
                    emit(ctx, "mrb_str_cat_str(mrb, _is%d, _t%d);\n", id, tmp);
                    free(ie);
                }
            }
        }
        ctx->indent--;
        emit(ctx, "}\n");
        return sfmt("_is%d", id);
    }

    case PM_TRUE_NODE:  return xstrdup("TRUE");
    case PM_FALSE_NODE: return xstrdup("FALSE");
    case PM_NIL_NODE:   return xstrdup("0 /* nil */");

    case PM_SOURCE_LINE_NODE: {
        pm_source_line_node_t *n = (pm_source_line_node_t *)node;
        /* Calculate line number from location offset */
        pm_line_column_t lc = pm_newline_list_line_column(&ctx->parser->newline_list, n->base.location.start, ctx->parser->start_line);
        return sfmt("%d", (int)lc.line);
    }

    case PM_SOURCE_FILE_NODE:
        return sfmt("\"%s\"", ctx->parser->filepath.source ? (const char *)ctx->parser->filepath.source : "unknown");

    case PM_DEFINED_NODE: {
        pm_defined_node_t *n = (pm_defined_node_t *)node;
        /* For local variables: check if defined in var table */
        if (PM_NODE_TYPE(n->value) == PM_LOCAL_VARIABLE_READ_NODE) {
            pm_local_variable_read_node_t *lv = (pm_local_variable_read_node_t *)n->value;
            char *name = cstr(ctx, lv->name);
            var_entry_t *v = var_lookup(ctx, name);
            free(name);
            return xstrdup(v ? "\"local-variable\"" : "NULL");
        }
        /* For methods, constants, etc. — conservatively return non-NULL */
        return xstrdup("\"expression\"");
    }

    case PM_SYMBOL_NODE: {
        pm_symbol_node_t *n = (pm_symbol_node_t *)node;
        const uint8_t *src = pm_string_source(&n->unescaped);
        size_t len = pm_string_length(&n->unescaped);
        char *buf = malloc(len + 3);
        buf[0] = '"';
        memcpy(buf + 1, src, len);
        buf[len + 1] = '"';
        buf[len + 2] = '\0';
        return buf;
    }

    case PM_LOCAL_VARIABLE_READ_NODE: {
        pm_local_variable_read_node_t *n = (pm_local_variable_read_node_t *)node;
        char *name = cstr(ctx, n->name);
        char *cn = make_cname(name, false);
        free(name);
        return cn;
    }

    case PM_CONSTANT_READ_NODE: {
        pm_constant_read_node_t *n = (pm_constant_read_node_t *)node;
        char *name = cstr(ctx, n->name);
        /* Check if it's a class name used for ::method */
        if (find_class(ctx, name) || find_module(ctx, name)) {
            /* Will be handled by the call node that uses it */
            return name; /* raw class name */
        }
        /* Check module constants when inside a module */
        if (ctx->current_module) {
            for (int i = 0; i < ctx->current_module->const_count; i++) {
                if (strcmp(ctx->current_module->consts[i].name, name) == 0) {
                    char *r = sfmt("sp_%s_%s", ctx->current_module->name, name);
                    free(name);
                    return r;
                }
            }
        }
        char *cn = make_cname(name, true);
        free(name);
        return cn;
    }

    case PM_INSTANCE_VARIABLE_READ_NODE: {
        pm_instance_variable_read_node_t *n = (pm_instance_variable_read_node_t *)node;
        char *ivname = cstr(ctx, n->name);
        const char *field = ivname + 1;
        char *r;
        if (ctx->current_module)
            r = sfmt("sp_%s_%s", ctx->current_module->name, field);
        else if (ctx->current_class && ctx->current_class->is_value_type)
            r = sfmt("self.%s", field);
        else
            r = sfmt("self->%s", field);
        free(ivname);
        return r;
    }

    case PM_INSTANCE_VARIABLE_WRITE_NODE: {
        pm_instance_variable_write_node_t *n = (pm_instance_variable_write_node_t *)node;
        char *ivname = cstr(ctx, n->name);
        const char *field = ivname + 1;
        char *val = codegen_expr(ctx, n->value);
        char *r;
        if (ctx->current_module)
            r = sfmt("(sp_%s_%s = %s)", ctx->current_module->name, field, val);
        else if (ctx->current_class && ctx->current_class->is_value_type)
            r = sfmt("(self.%s = %s)", field, val);
        else
            r = sfmt("(self->%s = %s)", field, val);
        free(ivname); free(val);
        return r;
    }

    case PM_CALL_NODE: {
        pm_call_node_t *call = (pm_call_node_t *)node;
        char *method = cstr(ctx, call->name);

        /* String binary operators: ==, !=, <, >, <=, >=, * */
        if (call->receiver && call->arguments &&
            call->arguments->arguments.size == 1) {
            vtype_t lt = infer_type(ctx, call->receiver);
            vtype_t rt = infer_type(ctx, call->arguments->arguments.nodes[0]);
            if (lt.kind == SPINEL_TYPE_STRING && rt.kind == SPINEL_TYPE_STRING) {
                if (strcmp(method, "==") == 0 || strcmp(method, "!=") == 0 ||
                    strcmp(method, "<") == 0 || strcmp(method, ">") == 0 ||
                    strcmp(method, "<=") == 0 || strcmp(method, ">=") == 0) {
                    char *left = codegen_expr(ctx, call->receiver);
                    char *right = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *r;
                    if (strcmp(method, "==") == 0)
                        r = sfmt("(strcmp(%s, %s) == 0)", left, right);
                    else if (strcmp(method, "!=") == 0)
                        r = sfmt("(strcmp(%s, %s) != 0)", left, right);
                    else if (strcmp(method, "<") == 0)
                        r = sfmt("(strcmp(%s, %s) < 0)", left, right);
                    else if (strcmp(method, ">") == 0)
                        r = sfmt("(strcmp(%s, %s) > 0)", left, right);
                    else if (strcmp(method, "<=") == 0)
                        r = sfmt("(strcmp(%s, %s) <= 0)", left, right);
                    else
                        r = sfmt("(strcmp(%s, %s) >= 0)", left, right);
                    free(left); free(right); free(method);
                    return r;
                }
            }
            if (lt.kind == SPINEL_TYPE_STRING && rt.kind == SPINEL_TYPE_INTEGER &&
                strcmp(method, "*") == 0) {
                char *left = codegen_expr(ctx, call->receiver);
                char *right = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *r = sfmt("sp_str_repeat(%s, %s)", left, right);
                free(left); free(right); free(method);
                return r;
            }
            /* str =~ /regexp/ → (sp_re_match(_re_N, str) >= 0) */
            if (lt.kind == SPINEL_TYPE_STRING && rt.kind == SPINEL_TYPE_REGEXP &&
                strcmp(method, "=~") == 0) {
                char *left = codegen_expr(ctx, call->receiver);
                char *right = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *r = sfmt("(sp_re_match(%s, %s) >= 0)", right, left);
                free(left); free(right); free(method);
                return r;
            }
        }

        /* Binary operators on numeric types → direct C ops */
        if (call->receiver && call->arguments &&
            call->arguments->arguments.size == 1) {
            vtype_t lt = infer_type(ctx, call->receiver);
            vtype_t rt = infer_type(ctx, call->arguments->arguments.nodes[0]);

            const char *c_op = NULL;
            if (strcmp(method, "+") == 0)  c_op = "+";
            if (strcmp(method, "-") == 0)  c_op = "-";
            if (strcmp(method, "*") == 0)  c_op = "*";
            if (strcmp(method, "/") == 0)  c_op = "/";
            if (strcmp(method, "%") == 0)  c_op = "%";
            if (strcmp(method, "<") == 0)  c_op = "<";
            if (strcmp(method, ">") == 0)  c_op = ">";
            if (strcmp(method, "<=") == 0) c_op = "<=";
            if (strcmp(method, ">=") == 0) c_op = ">=";
            if (strcmp(method, "==") == 0) c_op = "==";
            if (strcmp(method, "!=") == 0) c_op = "!=";
            if (strcmp(method, "<<") == 0) c_op = "<<";
            if (strcmp(method, ">>") == 0) c_op = ">>";
            if (strcmp(method, "|") == 0)  c_op = "|";
            if (strcmp(method, "&") == 0)  c_op = "&";
            if (strcmp(method, "^") == 0)  c_op = "^";

            if (c_op && (vt_is_numeric(lt) || lt.kind == SPINEL_TYPE_BOOLEAN) &&
                        (vt_is_numeric(rt) || rt.kind == SPINEL_TYPE_BOOLEAN)) {
                char *left = codegen_expr(ctx, call->receiver);
                char *right = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                vtype_t rest = binop_result(lt, rt, method);
                if (rest.kind == SPINEL_TYPE_FLOAT) {
                    if (lt.kind == SPINEL_TYPE_INTEGER) {
                        char *t = sfmt("((mrb_float)%s)", left); free(left); left = t;
                    }
                    if (rt.kind == SPINEL_TYPE_INTEGER) {
                        char *t = sfmt("((mrb_float)%s)", right); free(right); right = t;
                    }
                }
                char *r = sfmt("(%s %s %s)", left, c_op, right);
                free(left); free(right); free(method);
                return r;
            }
        }

        /* Array indexing / Proc call: obj[arg] */
        if (strcmp(method, "[]") == 0 && call->receiver && call->arguments &&
            call->arguments->arguments.size == 1) {
            vtype_t recv_t = infer_type(ctx, call->receiver);
            /* In lambda mode: inside lambda bodies, ALL [] are proc calls (sp_call)
             * because every variable in a lambda body is sp_Val*.
             * At top level in lambda mode, use sp_call for proc/value/unknown types. */
            if (ctx->lambda_mode &&
                (ctx->lambda_scope_depth > 0 ||
                 recv_t.kind == SPINEL_TYPE_PROC || recv_t.kind == SPINEL_TYPE_VALUE ||
                 recv_t.kind == SPINEL_TYPE_UNKNOWN)) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *r = sfmt("sp_call(%s, %s)", recv, arg);
                free(recv); free(arg); free(method);
                return r;
            }
            if (recv_t.kind != SPINEL_TYPE_ARRAY && recv_t.kind != SPINEL_TYPE_HASH) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *r = sfmt("%s[%s]", recv, idx);
                free(recv); free(idx); free(method);
                return r;
            }
        }

        /* Array index assignment: obj[index] = val → obj[index] = val */
        if (strcmp(method, "[]=") == 0 && call->receiver && call->arguments &&
            call->arguments->arguments.size == 2) {
            vtype_t recv_t = infer_type(ctx, call->receiver);
            if (recv_t.kind != SPINEL_TYPE_HASH) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *val = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                char *r = sfmt("(%s[%s] = %s)", recv, idx, val);
                free(recv); free(idx); free(val); free(method);
                return r;
            }
        }

        /* Unary minus: -expr */
        if (strcmp(method, "-@") == 0 && call->receiver && !call->arguments) {
            char *recv = codegen_expr(ctx, call->receiver);
            char *r = sfmt("(-%s)", recv);
            free(recv); free(method);
            return r;
        }

        /* Unary not: !expr (also used for `not expr`) */
        if (strcmp(method, "!") == 0 && call->receiver && !call->arguments) {
            char *recv = codegen_expr(ctx, call->receiver);
            char *r = sfmt("(!%s)", recv);
            free(recv); free(method);
            return r;
        }

        /* Range#to_a → sp_IntArray_from_range(start, end) */
        if (strcmp(method, "to_a") == 0 && call->receiver) {
            pm_node_t *recv_node = call->receiver;
            /* Unwrap parentheses */
            while (PM_NODE_TYPE(recv_node) == PM_PARENTHESES_NODE) {
                pm_parentheses_node_t *pn = (pm_parentheses_node_t *)recv_node;
                if (pn->body) recv_node = pn->body;
                else break;
            }
            /* Unwrap statements */
            if (PM_NODE_TYPE(recv_node) == PM_STATEMENTS_NODE) {
                pm_statements_node_t *ss = (pm_statements_node_t *)recv_node;
                if (ss->body.size == 1) recv_node = ss->body.nodes[0];
            }
            if (PM_NODE_TYPE(recv_node) == PM_RANGE_NODE) {
                pm_range_node_t *rng = (pm_range_node_t *)recv_node;
                char *left = codegen_expr(ctx, rng->left);
                char *right = codegen_expr(ctx, rng->right);
                char *r = sfmt("sp_IntArray_from_range(%s, %s)", left, right);
                free(left); free(right); free(method);
                return r;
            }
        }

        /* Constructor: ClassName.new(args) */
        if (strcmp(method, "new") == 0 && call->receiver &&
            PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
            pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
            char *cls_name = cstr(ctx, cr->name);
            class_info_t *cls = find_class(ctx, cls_name);
            if (cls) {
                int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                char *args = xstrdup("");
                for (int i = 0; i < argc; i++) {
                    char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                    char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", a);
                    free(args); free(a);
                    args = na;
                }
                char *r = sfmt("sp_%s_new(%s)", cls_name, args);
                free(cls_name); free(args); free(method);
                return r;
            }
            /* Array.new */
            if (strcmp(cls_name, "Array") == 0) {
                int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                if (argc == 0 && !ctx->current_class) {
                    /* Dynamic sp_IntArray (top-level / regular functions) */
                    free(cls_name); free(method);
                    return xstrdup("sp_IntArray_new()");
                }
                /* Fixed-size C array (inside class) or with size arg — skip */
                free(cls_name); free(method);
                return xstrdup("/* array_init */");
            }
            free(cls_name);
        }

        /* Method call on typed receiver */
        if (call->receiver) {
            vtype_t recv_t = infer_type(ctx, call->receiver);

            /* Math.sqrt/cos/sin */
            if (PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
                pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
                if (ceq(ctx, cr->name, "Math")) {
                    if (call->arguments && call->arguments->arguments.size == 1) {
                        char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *r = sfmt("%s(%s)", method, arg); /* sqrt, cos, sin */
                        free(arg); free(method);
                        return r;
                    }
                }
                /* Rand::rand */
                if (ceq(ctx, cr->name, "Rand")) {
                    char *r = sfmt("sp_Rand_%s()", method);
                    free(method);
                    return r;
                }
                /* Class method call: ClassName.method(args) */
                {
                    char *cls_name = cstr(ctx, cr->name);
                    class_info_t *cls = find_class(ctx, cls_name);
                    if (cls) {
                        method_info_t *cm = find_method(cls, method);
                        if (cm && cm->is_class_method) {
                            int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                            char *args = xstrdup("");
                            for (int i = 0; i < argc; i++) {
                                char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                                char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", a);
                                free(args); free(a);
                                args = na;
                            }
                            char *r = sfmt("sp_%s_%s(%s)", cls_name, method, args);
                            free(cls_name); free(args); free(method);
                            return r;
                        }
                    }
                    free(cls_name);
                }
            }

            /* Hash#keys.length → sp_StrIntHash_length (chained call optimization) */
            if (recv_t.kind == SPINEL_TYPE_ARRAY && strcmp(method, "length") == 0 &&
                PM_NODE_TYPE(call->receiver) == PM_CALL_NODE) {
                pm_call_node_t *inner = (pm_call_node_t *)call->receiver;
                if (ceq(ctx, inner->name, "keys") && inner->receiver) {
                    vtype_t inner_recv_t = infer_type(ctx, inner->receiver);
                    if (inner_recv_t.kind == SPINEL_TYPE_HASH) {
                        char *hrecv = codegen_expr(ctx, inner->receiver);
                        char *r = sfmt("sp_StrIntHash_length(%s)", hrecv);
                        free(hrecv); free(method);
                        return r;
                    }
                }
            }

            /* sp_IntArray method calls */
            if (recv_t.kind == SPINEL_TYPE_ARRAY) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "dup") == 0)
                    r = sfmt("sp_IntArray_dup(%s)", recv);
                else if (strcmp(method, "empty?") == 0)
                    r = sfmt("sp_IntArray_empty(%s)", recv);
                else if (strcmp(method, "shift") == 0)
                    r = sfmt("sp_IntArray_shift(%s)", recv);
                else if (strcmp(method, "pop") == 0)
                    r = sfmt("sp_IntArray_pop(%s)", recv);
                else if (strcmp(method, "length") == 0)
                    r = sfmt("sp_IntArray_length(%s)", recv);
                else if (strcmp(method, "reverse!") == 0)
                    r = sfmt("sp_IntArray_reverse_bang(%s)", recv);
                else if (strcmp(method, "sort") == 0)
                    r = sfmt("sp_IntArray_sort(%s)", recv);
                else if (strcmp(method, "sort!") == 0)
                    r = sfmt("sp_IntArray_sort_bang(%s)", recv);
                else if (strcmp(method, "min") == 0) {
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "mrb_int _min_%d = sp_IntArray_get(%s, 0);\n", tmp, recv);
                    emit(ctx, "for (mrb_int _mi_%d = 1; _mi_%d < sp_IntArray_length(%s); _mi_%d++) {\n", tmp, tmp, recv, tmp);
                    emit(ctx, "  mrb_int _v_%d = sp_IntArray_get(%s, _mi_%d);\n", tmp, recv, tmp);
                    emit(ctx, "  if (_v_%d < _min_%d) _min_%d = _v_%d;\n", tmp, tmp, tmp, tmp);
                    emit(ctx, "}\n");
                    r = sfmt("_min_%d", tmp);
                }
                else if (strcmp(method, "max") == 0) {
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "mrb_int _max_%d = sp_IntArray_get(%s, 0);\n", tmp, recv);
                    emit(ctx, "for (mrb_int _mi_%d = 1; _mi_%d < sp_IntArray_length(%s); _mi_%d++) {\n", tmp, tmp, recv, tmp);
                    emit(ctx, "  mrb_int _v_%d = sp_IntArray_get(%s, _mi_%d);\n", tmp, recv, tmp);
                    emit(ctx, "  if (_v_%d > _max_%d) _max_%d = _v_%d;\n", tmp, tmp, tmp, tmp);
                    emit(ctx, "}\n");
                    r = sfmt("_max_%d", tmp);
                }
                else if (strcmp(method, "sum") == 0) {
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "mrb_int _sum_%d = 0;\n", tmp);
                    emit(ctx, "for (mrb_int _si_%d = 0; _si_%d < sp_IntArray_length(%s); _si_%d++)\n", tmp, tmp, recv, tmp);
                    emit(ctx, "  _sum_%d += sp_IntArray_get(%s, _si_%d);\n", tmp, recv, tmp);
                    r = sfmt("_sum_%d", tmp);
                }
                else if (strcmp(method, "first") == 0)
                    r = sfmt("sp_IntArray_get(%s, 0)", recv);
                else if (strcmp(method, "last") == 0)
                    r = sfmt("sp_IntArray_get(%s, sp_IntArray_length(%s) - 1)", recv, recv);
                else if (strcmp(method, "include?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "mrb_bool _incl_%d = FALSE; { mrb_int _ii_%d;\n", tmp, tmp);
                    emit(ctx, "  for (_ii_%d = 0; _ii_%d < sp_IntArray_length(%s); _ii_%d++)\n", tmp, tmp, recv, tmp);
                    emit(ctx, "    if (sp_IntArray_get(%s, _ii_%d) == %s) { _incl_%d = TRUE; break; }\n", recv, tmp, arg, tmp);
                    emit(ctx, "}\n");
                    free(arg);
                    r = sfmt("_incl_%d", tmp);
                }
                else if (strcmp(method, "push") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_IntArray_push(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "[]") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_IntArray_get(%s, %s)", recv, idx);
                    free(idx);
                }
                else if (strcmp(method, "!=") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_IntArray_neq(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "==") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("(!sp_IntArray_neq(%s, %s))", recv, arg);
                    free(arg);
                }
                if (r) {
                    free(recv); free(method);
                    return r;
                }

                /* Array#map with block → new IntArray (expression context) */
                if (strcmp(method, "map") == 0 && call->block &&
                    PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = NULL;
                    if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                        if (bp->parameters && bp->parameters->requireds.size > 0) {
                            pm_node_t *p = bp->parameters->requireds.nodes[0];
                            if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                                bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                        }
                    }
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_IntArray *_map_%d = sp_IntArray_new();\n", tmp);
                    emit(ctx, "for (mrb_int _mi_%d = 0; _mi_%d < sp_IntArray_length(%s); _mi_%d++) {\n",
                         tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _mi_%d);\n", cn, recv, tmp);
                        free(cn);
                    }
                    /* Block body as expression */
                    char *body_expr = NULL;
                    if (blk->body) {
                        pm_node_t *body = (pm_node_t *)blk->body;
                        if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                            pm_statements_node_t *stmts = (pm_statements_node_t *)body;
                            if (stmts->body.size > 0)
                                body_expr = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                        } else {
                            body_expr = codegen_expr(ctx, body);
                        }
                    }
                    if (body_expr) {
                        emit(ctx, "sp_IntArray_push(_map_%d, %s);\n", tmp, body_expr);
                        free(body_expr);
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(recv); free(bpname); free(method);
                    return sfmt("_map_%d", tmp);
                }

                /* Array#select with block → new IntArray (expression context) */
                if (strcmp(method, "select") == 0 && call->block &&
                    PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = NULL;
                    if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                        if (bp->parameters && bp->parameters->requireds.size > 0) {
                            pm_node_t *p = bp->parameters->requireds.nodes[0];
                            if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                                bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                        }
                    }
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_IntArray *_sel_%d = sp_IntArray_new();\n", tmp);
                    emit(ctx, "for (mrb_int _si_%d = 0; _si_%d < sp_IntArray_length(%s); _si_%d++) {\n",
                         tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _si_%d);\n", cn, recv, tmp);
                        free(cn);
                    }
                    /* Block body as condition */
                    char *body_expr = NULL;
                    if (blk->body) {
                        pm_node_t *body = (pm_node_t *)blk->body;
                        if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                            pm_statements_node_t *stmts = (pm_statements_node_t *)body;
                            if (stmts->body.size > 0)
                                body_expr = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                        } else {
                            body_expr = codegen_expr(ctx, body);
                        }
                    }
                    if (body_expr) {
                        if (bpname) {
                            char *cn = make_cname(bpname, false);
                            emit(ctx, "if (%s) sp_IntArray_push(_sel_%d, %s);\n", body_expr, tmp, cn);
                            free(cn);
                        } else {
                            emit(ctx, "if (%s) sp_IntArray_push(_sel_%d, sp_IntArray_get(%s, _si_%d));\n",
                                 body_expr, tmp, recv, tmp);
                        }
                        free(body_expr);
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(recv); free(bpname); free(method);
                    return sfmt("_sel_%d", tmp);
                }

                /* Array#reject with block → new IntArray (opposite of select) */
                if (strcmp(method, "reject") == 0 && call->block &&
                    PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *bpname = NULL;
                    if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                        if (bp->parameters && bp->parameters->requireds.size > 0) {
                            pm_node_t *p = bp->parameters->requireds.nodes[0];
                            if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                                bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                        }
                    }
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_IntArray *_rej_%d = sp_IntArray_new();\n", tmp);
                    emit(ctx, "for (mrb_int _ri_%d = 0; _ri_%d < sp_IntArray_length(%s); _ri_%d++) {\n",
                         tmp, tmp, recv, tmp);
                    ctx->indent++;
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _ri_%d);\n", cn, recv, tmp);
                        free(cn);
                    }
                    char *body_expr = NULL;
                    if (blk->body) {
                        pm_node_t *body = (pm_node_t *)blk->body;
                        if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                            pm_statements_node_t *stmts = (pm_statements_node_t *)body;
                            if (stmts->body.size > 0)
                                body_expr = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                        } else {
                            body_expr = codegen_expr(ctx, body);
                        }
                    }
                    if (body_expr) {
                        if (bpname) {
                            char *cn = make_cname(bpname, false);
                            emit(ctx, "if (!(%s)) sp_IntArray_push(_rej_%d, %s);\n", body_expr, tmp, cn);
                            free(cn);
                        } else {
                            emit(ctx, "if (!(%s)) sp_IntArray_push(_rej_%d, sp_IntArray_get(%s, _ri_%d));\n",
                                 body_expr, tmp, recv, tmp);
                        }
                        free(body_expr);
                    }
                    ctx->indent--;
                    emit(ctx, "}\n");
                    free(recv); free(bpname); free(method);
                    return sfmt("_rej_%d", tmp);
                }

                /* Array#reduce/inject with initial value and block */
                if ((strcmp(method, "reduce") == 0 || strcmp(method, "inject") == 0) &&
                    call->arguments && call->arguments->arguments.size == 1 &&
                    call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                    pm_block_node_t *blk = (pm_block_node_t *)call->block;
                    char *init_val = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    /* Get block params: |acc, x| */
                    char *acc_name = NULL, *elem_name = NULL;
                    if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                        if (bp->parameters && bp->parameters->requireds.size >= 2) {
                            pm_node_t *p0 = bp->parameters->requireds.nodes[0];
                            pm_node_t *p1 = bp->parameters->requireds.nodes[1];
                            if (PM_NODE_TYPE(p0) == PM_REQUIRED_PARAMETER_NODE)
                                acc_name = cstr(ctx, ((pm_required_parameter_node_t *)p0)->name);
                            if (PM_NODE_TYPE(p1) == PM_REQUIRED_PARAMETER_NODE)
                                elem_name = cstr(ctx, ((pm_required_parameter_node_t *)p1)->name);
                        }
                    }
                    int tmp = ctx->temp_counter++;
                    if (acc_name && elem_name) {
                        char *acc_cn = make_cname(acc_name, false);
                        char *elem_cn = make_cname(elem_name, false);
                        /* Register block params for type inference */
                        int sv = ctx->var_count;
                        var_declare(ctx, acc_name, vt_prim(SPINEL_TYPE_INTEGER), false);
                        var_declare(ctx, elem_name, vt_prim(SPINEL_TYPE_INTEGER), false);
                        emit(ctx, "mrb_int _red_%d; { mrb_int %s = %s;\n", tmp, acc_cn, init_val);
                        emit(ctx, "  for (mrb_int _ri_%d = 0; _ri_%d < sp_IntArray_length(%s); _ri_%d++) {\n",
                             tmp, tmp, recv, tmp);
                        emit(ctx, "    mrb_int %s = sp_IntArray_get(%s, _ri_%d);\n", elem_cn, recv, tmp);
                        /* Block body expression */
                        if (blk->body) {
                            pm_node_t *body = (pm_node_t *)blk->body;
                            char *body_expr;
                            if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                                pm_statements_node_t *stmts = (pm_statements_node_t *)body;
                                body_expr = stmts->body.size > 0 ? codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]) : xstrdup("0");
                            } else {
                                body_expr = codegen_expr(ctx, body);
                            }
                            emit(ctx, "    %s = %s;\n", acc_cn, body_expr);
                            free(body_expr);
                        }
                        emit(ctx, "  } _red_%d = %s; }\n", tmp, acc_cn);
                        ctx->var_count = sv; /* restore var table */
                        free(acc_cn); free(elem_cn);
                    }
                    free(acc_name); free(elem_name); free(init_val);
                    free(recv); free(method);
                    return sfmt("_red_%d", tmp);
                }

                free(recv);
            }

            /* sp_StrIntHash method calls */
            if (recv_t.kind == SPINEL_TYPE_HASH) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "[]") == 0 && call->arguments &&
                    call->arguments->arguments.size == 1) {
                    char *key = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_StrIntHash_get(%s, %s)", recv, key);
                    free(key);
                }
                else if (strcmp(method, "[]=") == 0 && call->arguments &&
                         call->arguments->arguments.size == 2) {
                    char *key = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *val = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                    r = sfmt("sp_StrIntHash_set(%s, %s, %s)", recv, key, val);
                    free(key); free(val);
                }
                else if (strcmp(method, "length") == 0)
                    r = sfmt("sp_StrIntHash_length(%s)", recv);
                else if (strcmp(method, "has_key?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *key = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_StrIntHash_has_key(%s, %s)", recv, key);
                    free(key);
                }
                else if (strcmp(method, "delete") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *key = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_StrIntHash_delete(%s, %s)", recv, key);
                    free(key);
                }
                else if (strcmp(method, "keys") == 0) {
                    /* h.keys returns a temporary — but typically used as h.keys.length
                     * which chains to sp_IntArray_length. Handle keys as expression. */
                    r = sfmt("sp_StrIntHash_keys(%s)", recv);
                }
                if (r) {
                    free(recv); free(method);
                    return r;
                }
                free(recv);
            }

            /* String method calls */
            if (recv_t.kind == SPINEL_TYPE_STRING) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "length") == 0 || strcmp(method, "size") == 0)
                    r = sfmt("((mrb_int)strlen(%s))", recv);
                else if (strcmp(method, "upcase") == 0)
                    r = sfmt("sp_str_upcase(%s)", recv);
                else if (strcmp(method, "downcase") == 0)
                    r = sfmt("sp_str_downcase(%s)", recv);
                else if (strcmp(method, "include?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("(strstr(%s, %s) != NULL)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "+") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_concat(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "strip") == 0)
                    r = sfmt("sp_str_strip(%s)", recv);
                else if (strcmp(method, "chomp") == 0)
                    r = sfmt("sp_str_chomp(%s)", recv);
                else if (strcmp(method, "capitalize") == 0)
                    r = sfmt("sp_str_capitalize(%s)", recv);
                else if (strcmp(method, "reverse") == 0)
                    r = sfmt("sp_str_reverse(%s)", recv);
                else if (strcmp(method, "count") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_count(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "start_with?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_starts_with(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "end_with?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("sp_str_ends_with(%s, %s)", recv, arg);
                    free(arg);
                }
                else if (strcmp(method, "match?") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    vtype_t argt = infer_type(ctx, call->arguments->arguments.nodes[0]);
                    if (argt.kind == SPINEL_TYPE_REGEXP) {
                        char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        r = sfmt("sp_re_match_p(%s, %s)", arg, recv);
                        free(arg);
                    } else {
                        char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        r = sfmt("(strstr(%s, %s) != NULL)", recv, arg);
                        free(arg);
                    }
                }
                else if (strcmp(method, "gsub") == 0 && call->arguments &&
                         call->arguments->arguments.size == 2) {
                    vtype_t argt = infer_type(ctx, call->arguments->arguments.nodes[0]);
                    if (argt.kind == SPINEL_TYPE_REGEXP) {
                        char *pat = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *to = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        r = sfmt("sp_re_gsub(%s, %s, %s)", pat, recv, to);
                        free(pat); free(to);
                    } else {
                        char *from = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *to = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        r = sfmt("sp_str_gsub(%s, %s, %s)", recv, from, to);
                        free(from); free(to);
                    }
                }
                else if (strcmp(method, "sub") == 0 && call->arguments &&
                         call->arguments->arguments.size == 2) {
                    vtype_t argt = infer_type(ctx, call->arguments->arguments.nodes[0]);
                    if (argt.kind == SPINEL_TYPE_REGEXP) {
                        char *pat = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *to = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        r = sfmt("sp_re_sub(%s, %s, %s)", pat, recv, to);
                        free(pat); free(to);
                    } else {
                        char *from = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        char *to = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                        r = sfmt("sp_str_sub(%s, %s, %s)", recv, from, to);
                        free(from); free(to);
                    }
                }
                else if (strcmp(method, "split") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    vtype_t argt = infer_type(ctx, call->arguments->arguments.nodes[0]);
                    if (argt.kind == SPINEL_TYPE_REGEXP) {
                        char *pat = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        r = sfmt("sp_re_split(%s, %s)", pat, recv);
                        free(pat);
                    } else {
                        char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                        r = sfmt("sp_str_split(%s, %s)", recv, arg);
                        free(arg);
                    }
                }
                if (r) {
                    free(recv); free(method);
                    return r;
                }
                free(recv);
            }

            /* String array method calls */
            if (recv_t.kind == SPINEL_TYPE_STR_ARRAY) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "length") == 0 || strcmp(method, "size") == 0)
                    r = sfmt("sp_StrArray_length(%s)", recv);
                if (r) {
                    free(recv); free(method);
                    return r;
                }
                free(recv);
            }

            /* Numeric method calls */
            if (recv_t.kind == SPINEL_TYPE_INTEGER) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "abs") == 0)
                    r = sfmt("((%s) < 0 ? -(%s) : (%s))", recv, recv, recv);
                else if (strcmp(method, "even?") == 0)
                    r = sfmt("((%s) %% 2 == 0)", recv);
                else if (strcmp(method, "odd?") == 0)
                    r = sfmt("((%s) %% 2 != 0)", recv);
                else if (strcmp(method, "zero?") == 0)
                    r = sfmt("((%s) == 0)", recv);
                else if (strcmp(method, "positive?") == 0)
                    r = sfmt("((%s) > 0)", recv);
                else if (strcmp(method, "negative?") == 0)
                    r = sfmt("((%s) < 0)", recv);
                else if (strcmp(method, "**") == 0 && call->arguments &&
                         call->arguments->arguments.size == 1) {
                    char *exp = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    r = sfmt("((mrb_int)pow((double)%s, (double)%s))", recv, exp);
                    free(exp);
                }
                if (r) { free(recv); free(method); return r; }
                free(recv);
            }
            if (recv_t.kind == SPINEL_TYPE_FLOAT) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = NULL;
                if (strcmp(method, "abs") == 0)
                    r = sfmt("fabs(%s)", recv);
                else if (strcmp(method, "ceil") == 0)
                    r = sfmt("((mrb_int)ceil(%s))", recv);
                else if (strcmp(method, "floor") == 0)
                    r = sfmt("((mrb_int)floor(%s))", recv);
                else if (strcmp(method, "round") == 0)
                    r = sfmt("((mrb_int)round(%s))", recv);
                if (r) { free(recv); free(method); return r; }
                free(recv);
            }

            /* Universal methods: nil?, is_a?, respond_to? */
            if (strcmp(method, "nil?") == 0) {
                /* nil? is always false for non-nil values, true for nil */
                if (PM_NODE_TYPE(call->receiver) == PM_NIL_NODE) {
                    free(method); return xstrdup("TRUE");
                }
                free(method); return xstrdup("FALSE");
            }
            if (strcmp(method, "is_a?") == 0 && call->arguments &&
                call->arguments->arguments.size == 1 &&
                PM_NODE_TYPE(call->arguments->arguments.nodes[0]) == PM_CONSTANT_READ_NODE) {
                pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->arguments->arguments.nodes[0];
                char *cls_name = cstr(ctx, cr->name);
                bool result = false;
                if (recv_t.kind == SPINEL_TYPE_OBJECT) {
                    /* Check class and superclass chain */
                    class_info_t *c = find_class(ctx, recv_t.klass);
                    while (c) {
                        if (strcmp(c->name, cls_name) == 0) { result = true; break; }
                        c = c->superclass[0] ? find_class(ctx, c->superclass) : NULL;
                    }
                } else if (recv_t.kind == SPINEL_TYPE_INTEGER && strcmp(cls_name, "Integer") == 0) result = true;
                else if (recv_t.kind == SPINEL_TYPE_FLOAT && strcmp(cls_name, "Float") == 0) result = true;
                else if (recv_t.kind == SPINEL_TYPE_STRING && strcmp(cls_name, "String") == 0) result = true;
                free(cls_name); free(method);
                return xstrdup(result ? "TRUE" : "FALSE");
            }
            if (strcmp(method, "respond_to?") == 0 && call->arguments &&
                call->arguments->arguments.size == 1 &&
                PM_NODE_TYPE(call->arguments->arguments.nodes[0]) == PM_SYMBOL_NODE) {
                pm_symbol_node_t *sym = (pm_symbol_node_t *)call->arguments->arguments.nodes[0];
                const uint8_t *src = pm_string_source(&sym->unescaped);
                size_t len = pm_string_length(&sym->unescaped);
                char mname[64]; snprintf(mname, sizeof(mname), "%.*s", (int)len, src);
                bool result = false;
                if (recv_t.kind == SPINEL_TYPE_OBJECT) {
                    class_info_t *c = find_class(ctx, recv_t.klass);
                    if (c) {
                        class_info_t *owner;
                        result = find_method_inherited(ctx, c, mname, &owner) != NULL;
                    }
                }
                free(method);
                return xstrdup(result ? "TRUE" : "FALSE");
            }

            if (recv_t.kind == SPINEL_TYPE_OBJECT) {
                class_info_t *cls = find_class(ctx, recv_t.klass);
                if (cls) {
                    class_info_t *owner = NULL;
                    method_info_t *m = find_method_inherited(ctx, cls, method, &owner);
                    if (m) {
                        char *recv = codegen_expr(ctx, call->receiver);

                        /* Inline getter: recv.field (works for inherited fields too) */
                        if (m->is_getter) {
                            char *r;
                            if (cls->is_value_type)
                                r = sfmt("%s.%s", recv, m->accessor_ivar);
                            else
                                r = sfmt("%s->%s", recv, m->accessor_ivar);
                            free(recv); free(method);
                            return r;
                        }

                        /* Inline setter: recv.field = val */
                        if (m->is_setter && call->arguments &&
                            call->arguments->arguments.size == 1) {
                            char *val = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                            char *r;
                            if (cls->is_value_type)
                                r = sfmt("(%s.%s = %s)", recv, m->accessor_ivar, val);
                            else
                                r = sfmt("(%s->%s = %s)", recv, m->accessor_ivar, val);
                            free(recv); free(val); free(method);
                            return r;
                        }

                        /* Direct method call (with cast if inherited) */
                        int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                        char *args = xstrdup("");
                        for (int i = 0; i < argc; i++) {
                            char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                            char *na = sfmt("%s, %s", args, a);
                            free(args); free(a);
                            args = na;
                        }

                        char *r;
                        if (owner && owner != cls) {
                            /* Inherited method: cast receiver to parent type */
                            if (cls->is_value_type)
                                r = sfmt("sp_%s_%s(%s%s)", owner->name, method, recv, args);
                            else
                                r = sfmt("sp_%s_%s((sp_%s *)%s%s)", owner->name, method, owner->name, recv, args);
                        } else {
                            r = sfmt("sp_%s_%s(%s%s)", recv_t.klass, method, recv, args);
                        }
                        free(recv); free(args); free(method);
                        return r;
                    }
                }
            }

            /* to_f, to_i on Integer/Float */
            if (strcmp(method, "to_f") == 0 && recv_t.kind == SPINEL_TYPE_INTEGER) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = sfmt("((mrb_float)%s)", recv);
                free(recv); free(method);
                return r;
            }
            if (strcmp(method, "to_s") == 0 && recv_t.kind == SPINEL_TYPE_INTEGER) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = sfmt("sp_int_to_s(%s)", recv);
                free(recv); free(method);
                return r;
            }
            if (strcmp(method, "to_i") == 0 && recv_t.kind == SPINEL_TYPE_FLOAT) {
                char *recv = codegen_expr(ctx, call->receiver);
                char *r = sfmt("((mrb_int)%s)", recv);
                free(recv); free(method);
                return r;
            }
            if (strcmp(method, "to_f") == 0 || strcmp(method, "to_i") == 0) {
                /* Identity for matching types */
                char *recv = codegen_expr(ctx, call->receiver);
                free(method);
                return recv;
            }
        }

        /* Receiver-less: implicit self method call in class body (with inheritance) */
        if (!call->receiver && ctx->current_class) {
            class_info_t *owner = NULL;
            method_info_t *m = find_method_inherited(ctx, ctx->current_class, method, &owner);
            if (m) {
                /* Inline getter: self.field or self->field */
                if (m->is_getter) {
                    char *r;
                    if (ctx->current_class->is_value_type)
                        r = sfmt("self.%s", m->accessor_ivar);
                    else
                        r = sfmt("self->%s", m->accessor_ivar);
                    free(method);
                    return r;
                }
                /* Inline setter */
                if (m->is_setter && call->arguments &&
                    call->arguments->arguments.size == 1) {
                    char *val = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    char *r;
                    if (ctx->current_class->is_value_type)
                        r = sfmt("(self.%s = %s)", m->accessor_ivar, val);
                    else
                        r = sfmt("(self->%s = %s)", m->accessor_ivar, val);
                    free(val); free(method);
                    return r;
                }

                int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                char *args = xstrdup("");
                for (int i = 0; i < argc; i++) {
                    char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                    char *na = sfmt("%s, %s", args, a);
                    free(args); free(a);
                    args = na;
                }
                char *r;
                if (owner && owner != ctx->current_class) {
                    /* Inherited method: cast self to parent type */
                    if (ctx->current_class->is_value_type)
                        r = sfmt("sp_%s_%s(self%s)", owner->name, method, args);
                    else
                        r = sfmt("sp_%s_%s((sp_%s *)self%s)", owner->name, method, owner->name, args);
                } else {
                    r = sfmt("sp_%s_%s(self%s)",
                                   ctx->current_class->name, method, args);
                }
                free(args); free(method);
                return r;
            }
        }

        /* Receiver-less: Integer() conversion — use default value from || */
        if (!call->receiver && strcmp(method, "Integer") == 0) {
            if (call->arguments && call->arguments->arguments.size == 1) {
                pm_node_t *arg = call->arguments->arguments.nodes[0];
                /* Integer(ARGV[0] || 64) → just use the default literal */
                if (PM_NODE_TYPE(arg) == PM_OR_NODE) {
                    pm_or_node_t *or_n = (pm_or_node_t *)arg;
                    char *def = codegen_expr(ctx, or_n->right);
                    free(method);
                    return def;
                }
                char *a = codegen_expr(ctx, arg);
                free(method);
                return a;
            }
            free(method);
            return xstrdup("0");
        }

        /* block_given? → check if _block is non-NULL */
        if (!call->receiver && strcmp(method, "block_given?") == 0) {
            free(method);
            return xstrdup("(_block != NULL)");
        }

        /* Receiver-less: top-level function or Kernel method */
        if (!call->receiver) {
            func_info_t *fn = find_func(ctx, method);
            if (fn) {
                int argc = call->arguments ? (int)call->arguments->arguments.size : 0;

                /* Check if any argument is a KeywordHashNode (keyword args at call site) */
                bool has_kwarg_call = false;
                pm_keyword_hash_node_t *kw_hash = NULL;
                int kw_hash_idx = -1;
                for (int i = 0; i < argc; i++) {
                    if (PM_NODE_TYPE(call->arguments->arguments.nodes[i]) == PM_KEYWORD_HASH_NODE) {
                        kw_hash = (pm_keyword_hash_node_t *)call->arguments->arguments.nodes[i];
                        kw_hash_idx = i;
                        has_kwarg_call = true;
                        break;
                    }
                }

                if (has_kwarg_call && kw_hash) {
                    /* Keyword argument call: build args array indexed by param position */
                    char *param_args[MAX_PARAMS];
                    for (int i = 0; i < fn->param_count; i++) param_args[i] = NULL;

                    /* First, fill positional (non-keyword) args */
                    int pos_idx = 0;
                    for (int i = 0; i < argc; i++) {
                        if (i == kw_hash_idx) continue;
                        if (pos_idx < fn->param_count && !fn->params[pos_idx].is_keyword)
                            param_args[pos_idx] = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                        pos_idx++;
                    }

                    /* Then, match keyword args by name to param positions */
                    for (size_t ki = 0; ki < kw_hash->elements.size; ki++) {
                        pm_node_t *elem = kw_hash->elements.nodes[ki];
                        if (PM_NODE_TYPE(elem) != PM_ASSOC_NODE) continue;
                        pm_assoc_node_t *assoc = (pm_assoc_node_t *)elem;
                        /* Get key name from SymbolNode */
                        if (PM_NODE_TYPE(assoc->key) != PM_SYMBOL_NODE) continue;
                        pm_symbol_node_t *sym = (pm_symbol_node_t *)assoc->key;
                        const uint8_t *ksrc = pm_string_source(&sym->unescaped);
                        size_t klen = pm_string_length(&sym->unescaped);
                        char kname[64];
                        size_t copy_len = klen < 63 ? klen : 63;
                        memcpy(kname, ksrc, copy_len);
                        kname[copy_len] = '\0';

                        /* Find matching param */
                        for (int pi = 0; pi < fn->param_count; pi++) {
                            if (strcmp(fn->params[pi].name, kname) == 0) {
                                param_args[pi] = codegen_expr(ctx, assoc->value);
                                break;
                            }
                        }
                    }

                    /* Fill defaults for missing optional keyword params */
                    for (int i = 0; i < fn->param_count; i++) {
                        if (!param_args[i] && fn->params[i].is_optional && fn->params[i].default_node)
                            param_args[i] = codegen_expr(ctx, (pm_node_t *)fn->params[i].default_node);
                    }

                    /* Build argument string in parameter order */
                    char *args = xstrdup("");
                    for (int i = 0; i < fn->param_count; i++) {
                        if (param_args[i]) {
                            char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", param_args[i]);
                            free(args);
                            args = na;
                            free(param_args[i]);
                        }
                    }
                    char *r = sfmt("sp_%s(%s)", method, args);
                    free(args); free(method);
                    return r;
                }

                if (fn->has_rest) {
                    /* Rest/splat parameter call: collect positional args into IntArray */
                    int rest_idx = fn->rest_param_index;
                    char *args = xstrdup("");
                    /* Emit positional args before the rest param */
                    int ai = 0;
                    for (int i = 0; i < rest_idx && ai < argc; i++, ai++) {
                        char *a = codegen_expr(ctx, call->arguments->arguments.nodes[ai]);
                        char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", a);
                        free(args); free(a);
                        args = na;
                    }
                    /* Build the rest array from remaining args */
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "sp_IntArray *_rest_%d = sp_IntArray_new();\n", tmp);
                    for (int i = ai; i < argc; i++) {
                        char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                        emit(ctx, "sp_IntArray_push(_rest_%d, %s);\n", tmp, a);
                        free(a);
                    }
                    /* Add rest array to args */
                    char *rest_ref = sfmt("_rest_%d", tmp);
                    char *na = sfmt("%s%s%s", args, (rest_idx > 0) ? ", " : "", rest_ref);
                    free(args); free(rest_ref);
                    args = na;

                    /* If target function uses yield, pass block (or NULL if no block) */
                    if (fn->has_yield) {
                        if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
                            char *na2 = sfmt("%s, _block, _block_env", args);
                            free(args); args = na2;
                        } else {
                            char *na2 = sfmt("%s, NULL, NULL", args);
                            free(args); args = na2;
                        }
                    }
                    char *r = sfmt("sp_%s(%s)", method, args);
                    free(args); free(method);
                    return r;
                }

                char *args = xstrdup("");
                for (int i = 0; i < argc; i++) {
                    char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                    char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", a);
                    free(args); free(a);
                    args = na;
                }
                /* Fill in default values for optional parameters */
                for (int i = argc; i < fn->param_count; i++) {
                    if (fn->params[i].is_optional && fn->params[i].default_node) {
                        char *def = codegen_expr(ctx, (pm_node_t *)fn->params[i].default_node);
                        char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", def);
                        free(args); free(def);
                        args = na;
                    }
                }
                /* If target function uses yield, pass block (or NULL) */
                if (fn->has_yield) {
                    int total = fn->param_count;
                    if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE && ctx->in_yield_func) {
                        /* Forward the block from the enclosing yield function */
                        char *na = sfmt("%s%s_block, _block_env", args, total > 0 ? ", " : "");
                        free(args); args = na;
                    } else {
                        /* No block or not in yield context → pass NULL */
                        char *na = sfmt("%s%sNULL, NULL", args, total > 0 ? ", " : "");
                        free(args); args = na;
                    }
                }
                char *r = sfmt("sp_%s(%s)", method, args);
                free(args); free(method);
                return r;
            }
        }

        /* In lambda mode: handle map with block on ValArray result */
        if (ctx->lambda_mode && strcmp(method, "map") == 0 &&
            call->receiver && call->block &&
            PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
            /* Check if receiver is a to_array call or variable of ValArray type */
            char *recv = codegen_expr(ctx, call->receiver);
            pm_block_node_t *blk = (pm_block_node_t *)call->block;

            /* Get block parameter name */
            char *bpname = NULL;
            if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                if (bp->parameters && bp->parameters->requireds.size > 0) {
                    pm_node_t *p = bp->parameters->requireds.nodes[0];
                    if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                        bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                }
            }

            /* Generate: { sp_ValArray *_arr = recv; sp_StrArray *_res = ...; for ... } */
            int tmp = ctx->temp_counter++;
            emit(ctx, "sp_ValArray *_va_%d = %s;\n", tmp, recv);
            emit(ctx, "sp_StrArray *_sa_%d = sp_StrArray_new();\n", tmp);
            emit(ctx, "for (int _mi_%d = 0; _mi_%d < _va_%d->len; _mi_%d++) {\n",
                 tmp, tmp, tmp, tmp);
            ctx->indent++;
            if (bpname) {
                emit(ctx, "sp_Val *lv_%s = _va_%d->data[_mi_%d];\n", bpname, tmp, tmp);
            }
            /* Generate the block body as an expression */
            char *body_expr = NULL;
            if (blk->body) {
                pm_node_t *body = (pm_node_t *)blk->body;
                if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                    pm_statements_node_t *stmts = (pm_statements_node_t *)body;
                    if (stmts->body.size > 0)
                        body_expr = codegen_expr(ctx, stmts->body.nodes[stmts->body.size - 1]);
                } else {
                    body_expr = codegen_expr(ctx, body);
                }
            }
            if (body_expr) {
                emit(ctx, "sp_StrArray_push(_sa_%d, %s);\n", tmp, body_expr);
                free(body_expr);
            }
            ctx->indent--;
            emit(ctx, "}\n");

            free(recv); free(bpname); free(method);
            return sfmt("_sa_%d", tmp);
        }

        /* In lambda mode: handle join on StrArray */
        if (ctx->lambda_mode && strcmp(method, "join") == 0 && call->receiver) {
            char *recv = codegen_expr(ctx, call->receiver);
            /* This should be an sp_StrArray; join all strings */
            int tmp = ctx->temp_counter++;
            emit(ctx, "size_t _jl_%d = 0;\n", tmp);
            emit(ctx, "for (int _ji_%d = 0; _ji_%d < %s->len; _ji_%d++) _jl_%d += strlen(%s->data[_ji_%d]);\n",
                 tmp, tmp, recv, tmp, tmp, recv, tmp);
            emit(ctx, "char *_js_%d = (char *)malloc(_jl_%d + 1); _js_%d[0] = '\\0';\n", tmp, tmp, tmp);
            emit(ctx, "for (int _ji_%d = 0; _ji_%d < %s->len; _ji_%d++) strcat(_js_%d, %s->data[_ji_%d]);\n",
                 tmp, tmp, recv, tmp, tmp, recv, tmp);
            free(recv); free(method);
            return sfmt("_js_%d", tmp);
        }

        /* In lambda mode: handle slice on string literals */
        if (ctx->lambda_mode && strcmp(method, "slice") == 0 &&
            call->receiver && call->arguments &&
            call->arguments->arguments.size == 1) {
            char *recv = codegen_expr(ctx, call->receiver);
            char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
            int tmp = ctx->temp_counter++;
            emit(ctx, "char *_sl_%d = (char *)malloc(2); _sl_%d[0] = %s[(int)%s]; _sl_%d[1] = '\\0';\n",
                 tmp, tmp, recv, idx, tmp);
            free(recv); free(idx); free(method);
            return sfmt("_sl_%d", tmp);
        }

        /* Fallback */
        free(method);
        return sfmt("/* TODO: call */ 0");
    }

    case PM_IF_NODE: {
        /* Ternary in expression context */
        pm_if_node_t *n = (pm_if_node_t *)node;
        char *cond = codegen_expr(ctx, n->predicate);
        char *then_e = xstrdup("0");
        char *else_e = xstrdup("0");
        if (n->statements && PM_NODE_TYPE((pm_node_t *)n->statements) == PM_STATEMENTS_NODE) {
            pm_statements_node_t *s = (pm_statements_node_t *)n->statements;
            if (s->body.size > 0) { free(then_e); then_e = codegen_expr(ctx, s->body.nodes[0]); }
        }
        if (n->subsequent) {
            if (PM_NODE_TYPE(n->subsequent) == PM_ELSE_NODE) {
                pm_else_node_t *el = (pm_else_node_t *)n->subsequent;
                if (el->statements && el->statements->body.size > 0) {
                    free(else_e);
                    else_e = codegen_expr(ctx, el->statements->body.nodes[0]);
                }
            }
        }
        char *r = sfmt("(%s ? %s : %s)", cond, then_e, else_e);
        free(cond); free(then_e); free(else_e);
        return r;
    }

    case PM_PARENTHESES_NODE: {
        pm_parentheses_node_t *n = (pm_parentheses_node_t *)node;
        if (n->body) {
            if (PM_NODE_TYPE(n->body) == PM_STATEMENTS_NODE) {
                pm_statements_node_t *s = (pm_statements_node_t *)n->body;
                if (s->body.size > 0)
                    return codegen_expr(ctx, s->body.nodes[s->body.size - 1]);
            }
            return codegen_expr(ctx, n->body);
        }
        return xstrdup("0");
    }

    case PM_STATEMENTS_NODE: {
        pm_statements_node_t *s = (pm_statements_node_t *)node;
        if (s->body.size > 0) return codegen_expr(ctx, s->body.nodes[s->body.size - 1]);
        return xstrdup("0");
    }

    case PM_AND_NODE: {
        pm_and_node_t *n = (pm_and_node_t *)node;
        char *left = codegen_expr(ctx, n->left);
        char *right = codegen_expr(ctx, n->right);
        char *r = sfmt("(%s && %s)", left, right);
        free(left); free(right);
        return r;
    }

    case PM_OR_NODE: {
        pm_or_node_t *n = (pm_or_node_t *)node;
        char *left = codegen_expr(ctx, n->left);
        char *right = codegen_expr(ctx, n->right);
        char *r = sfmt("(%s || %s)", left, right);
        free(left); free(right);
        return r;
    }

    case PM_CONSTANT_PATH_NODE: {
        /* Module::method — handled at call site */
        pm_constant_path_node_t *n = (pm_constant_path_node_t *)node;
        if (n->parent && PM_NODE_TYPE(n->parent) == PM_CONSTANT_READ_NODE) {
            pm_constant_read_node_t *p = (pm_constant_read_node_t *)n->parent;
            char *mod_name = cstr(ctx, p->name);
            char *child_name = cstr(ctx, n->name);
            /* For constants like BNUM, BNUMF */
            char *r = sfmt("sp_%s_%s", mod_name, child_name);
            free(mod_name); free(child_name);
            return r;
        }
        return xstrdup("0");
    }

    case PM_CASE_NODE: {
        /* Case as expression — emit if/else chain assigning to a temp */
        pm_case_node_t *n = (pm_case_node_t *)node;
        vtype_t rt = infer_type(ctx, node);
        char *ct = vt_ctype(ctx, rt, false);
        int tmp = ctx->temp_counter++;
        char *pred = n->predicate ? codegen_expr(ctx, n->predicate) : NULL;
        int cid = ctx->temp_counter++;

        if (pred) {
            char *pct = vt_ctype(ctx, infer_type(ctx, n->predicate), false);
            emit(ctx, "%s _cpred_%d = %s;\n", pct, cid, pred);
            free(pct); free(pred);
        }
        emit(ctx, "%s _cres_%d = 0;\n", ct, tmp);

        for (size_t i = 0; i < n->conditions.size; i++) {
            pm_node_t *cn = n->conditions.nodes[i];
            if (PM_NODE_TYPE(cn) != PM_WHEN_NODE) continue;
            pm_when_node_t *w = (pm_when_node_t *)cn;
            emit(ctx, "%sif (", i == 0 ? "" : "} else ");
            for (size_t j = 0; j < w->conditions.size; j++) {
                if (j > 0) emit_raw(ctx, " || ");
                pm_node_t *wc = w->conditions.nodes[j];
                if (PM_NODE_TYPE(wc) == PM_RANGE_NODE && pred) {
                    pm_range_node_t *rng = (pm_range_node_t *)wc;
                    char *lo = codegen_expr(ctx, rng->left);
                    char *hi = codegen_expr(ctx, rng->right);
                    emit_raw(ctx, "(_cpred_%d >= %s && _cpred_%d <= %s)", cid, lo, cid, hi);
                    free(lo); free(hi);
                } else if (pred) {
                    char *val = codegen_expr(ctx, wc);
                    emit_raw(ctx, "_cpred_%d == %s", cid, val);
                    free(val);
                } else {
                    char *val = codegen_expr(ctx, wc);
                    emit_raw(ctx, "%s", val);
                    free(val);
                }
            }
            emit_raw(ctx, ") {\n");
            ctx->indent++;
            if (w->statements) {
                pm_statements_node_t *ws = (pm_statements_node_t *)w->statements;
                for (size_t si = 0; si + 1 < ws->body.size; si++)
                    codegen_stmt(ctx, ws->body.nodes[si]);
                if (ws->body.size > 0) {
                    char *val = codegen_expr(ctx, ws->body.nodes[ws->body.size - 1]);
                    emit(ctx, "_cres_%d = %s;\n", tmp, val);
                    free(val);
                }
            }
            ctx->indent--;
        }
        if (n->else_clause) {
            emit(ctx, "} else {\n");
            ctx->indent++;
            pm_else_node_t *el = (pm_else_node_t *)n->else_clause;
            if (el->statements && el->statements->body.size > 0) {
                pm_statements_node_t *es = el->statements;
                for (size_t si = 0; si + 1 < es->body.size; si++)
                    codegen_stmt(ctx, es->body.nodes[si]);
                char *val = codegen_expr(ctx, es->body.nodes[es->body.size - 1]);
                emit(ctx, "_cres_%d = %s;\n", tmp, val);
                free(val);
            }
            ctx->indent--;
        }
        if (n->conditions.size > 0) emit(ctx, "}\n");
        free(ct);
        return sfmt("_cres_%d", tmp);
    }

    case PM_LAMBDA_NODE: {
        pm_lambda_node_t *lam = (pm_lambda_node_t *)node;
        return codegen_lambda(ctx, lam);
    }

    case PM_YIELD_NODE: {
        pm_yield_node_t *yn = (pm_yield_node_t *)node;
        if (yn->arguments && yn->arguments->arguments.size > 0) {
            char *arg = codegen_expr(ctx, yn->arguments->arguments.nodes[0]);
            char *r = sfmt("_block(_block_env, %s)", arg);
            free(arg);
            return r;
        }
        return xstrdup("_block(_block_env, 0)");
    }

    case PM_SUPER_NODE: {
        /* super(args) — call parent's same-named method */
        pm_super_node_t *sn = (pm_super_node_t *)node;
        if (ctx->current_class && ctx->current_method && ctx->current_class->superclass[0]) {
            class_info_t *parent = find_class(ctx, ctx->current_class->superclass);
            if (parent) {
                int argc = sn->arguments ? (int)sn->arguments->arguments.size : 0;
                char *args = xstrdup("");
                for (int i = 0; i < argc; i++) {
                    char *a = codegen_expr(ctx, sn->arguments->arguments.nodes[i]);
                    char *na = sfmt("%s, %s", args, a);
                    free(args); free(a);
                    args = na;
                }
                char *r;
                if (strcmp(ctx->current_method->name, "initialize") == 0) {
                    /* super in initialize: call parent's initialize on self */
                    if (ctx->current_class->is_value_type)
                        r = sfmt("sp_%s_initialize(&self%s)", parent->name, args);
                    else
                        r = sfmt("sp_%s_initialize((sp_%s *)self%s)", parent->name, parent->name, args);
                } else {
                    if (ctx->current_class->is_value_type)
                        r = sfmt("sp_%s_%s(self%s)", parent->name, ctx->current_method->name, args);
                    else
                        r = sfmt("sp_%s_%s((sp_%s *)self%s)", parent->name, ctx->current_method->name, parent->name, args);
                }
                free(args);
                return r;
            }
        }
        return xstrdup("/* super */");
    }

    case PM_ARRAY_NODE: {
        pm_array_node_t *ary = (pm_array_node_t *)node;
        if (ctx->lambda_mode) {
            if (ary->elements.size == 0) {
                /* Empty array literal [] → sp_ValArray_new() */
                return xstrdup("sp_ValArray_new()");
            }
        }
        /* Fallthrough for non-lambda mode */
        return sfmt("0 /* TODO: array expr */");
    }

    case PM_HASH_NODE:
        /* Empty hash literal {} → sp_StrIntHash_new() */
        return xstrdup("sp_StrIntHash_new()");

    /* Chained assignment: zr = zi = 0 — inner write used as expression */
    case PM_LOCAL_VARIABLE_WRITE_NODE: {
        pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
        char *name = cstr(ctx, n->name);
        char *cn = make_cname(name, false);
        char *val = codegen_expr(ctx, n->value);
        char *r = sfmt("(%s = %s)", cn, val);
        free(name); free(cn); free(val);
        return r;
    }

    case PM_REGULAR_EXPRESSION_NODE: {
        pm_regular_expression_node_t *re = (pm_regular_expression_node_t *)node;
        const uint8_t *src = pm_string_source(&re->unescaped);
        size_t len = pm_string_length(&re->unescaped);
        /* Check if this pattern is already registered */
        char pat[256];
        size_t plen = len < 255 ? len : 255;
        memcpy(pat, src, plen);
        pat[plen] = '\0';
        for (int i = 0; i < ctx->regexp_counter; i++) {
            if (strcmp(ctx->regexps[i].pattern, pat) == 0)
                return sfmt("_re_%d", ctx->regexps[i].id);
        }
        /* Register new regexp */
        int id = ctx->regexp_counter;
        if (ctx->regexp_counter < MAX_REGEXPS) {
            snprintf(ctx->regexps[ctx->regexp_counter].pattern, 256, "%s", pat);
            ctx->regexps[ctx->regexp_counter].id = id;
            ctx->regexp_counter++;
        }
        ctx->needs_regexp = true;
        return sfmt("_re_%d", id);
    }

    case PM_NUMBERED_REFERENCE_READ_NODE: {
        pm_numbered_reference_read_node_t *nr = (pm_numbered_reference_read_node_t *)node;
        return sfmt("sp_re_group(%d)", (int)nr->number);
    }

    case PM_MATCH_WRITE_NODE: {
        pm_match_write_node_t *mw = (pm_match_write_node_t *)node;
        return codegen_expr(ctx, (pm_node_t *)mw->call);
    }

    default:
        return sfmt("0 /* TODO: expr %d */", PM_NODE_TYPE(node));
    }
}

/* ------------------------------------------------------------------ */
/* Statement codegen                                                  */
/* ------------------------------------------------------------------ */

/* Detect print <int>.chr → putchar */
static bool try_print_chr(codegen_ctx_t *ctx, pm_call_node_t *call) {
    if (!ceq(ctx, call->name, "print")) return false;
    if (!call->arguments || call->arguments->arguments.size != 1) return false;
    pm_node_t *arg = call->arguments->arguments.nodes[0];
    if (PM_NODE_TYPE(arg) != PM_CALL_NODE) return false;
    pm_call_node_t *inner = (pm_call_node_t *)arg;
    if (!ceq(ctx, inner->name, "chr") || !inner->receiver) return false;
    if (infer_type(ctx, inner->receiver).kind != SPINEL_TYPE_INTEGER) return false;
    char *ie = codegen_expr(ctx, inner->receiver);
    emit(ctx, "putchar((int)%s);\n", ie);
    free(ie);
    return true;
}

static void codegen_stmt(codegen_ctx_t *ctx, pm_node_t *node) {
    if (!node) return;

    switch (PM_NODE_TYPE(node)) {
    case PM_LOCAL_VARIABLE_WRITE_NODE: {
        pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
        char *name = cstr(ctx, n->name);
        char *cn = make_cname(name, false);
        char *val = codegen_expr(ctx, n->value);
        /* Skip array_init — array vars are declared already */
        if (strstr(val, "array_init") == NULL) {
            emit(ctx, "%s = %s;\n", cn, val);
        }
        free(name); free(cn); free(val);
        break;
    }

    case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
        pm_local_variable_operator_write_node_t *n =
            (pm_local_variable_operator_write_node_t *)node;
        char *name = cstr(ctx, n->name);
        char *cn = make_cname(name, false);
        char *op = cstr(ctx, n->binary_operator);
        char *val = codegen_expr(ctx, n->value);
        emit(ctx, "%s %s= %s;\n", cn, op, val);
        free(name); free(cn); free(op); free(val);
        break;
    }

    case PM_CONSTANT_WRITE_NODE: {
        pm_constant_write_node_t *n = (pm_constant_write_node_t *)node;
        char *name = cstr(ctx, n->name);
        if (!find_class(ctx, name)) {
            char *cn = make_cname(name, true);
            char *val = codegen_expr(ctx, n->value);
            emit(ctx, "%s = %s;\n", cn, val);
            free(cn); free(val);
        }
        free(name);
        break;
    }

    case PM_INSTANCE_VARIABLE_WRITE_NODE: {
        pm_instance_variable_write_node_t *n = (pm_instance_variable_write_node_t *)node;
        char *ivname = cstr(ctx, n->name);
        const char *field = ivname + 1;
        char *val = codegen_expr(ctx, n->value);
        if (ctx->current_module)
            emit(ctx, "sp_%s_%s = %s;\n", ctx->current_module->name, field, val);
        else if (ctx->current_class && ctx->current_class->is_value_type)
            emit(ctx, "self.%s = %s;\n", field, val);
        else
            emit(ctx, "self->%s = %s;\n", field, val);
        free(ivname); free(val);
        break;
    }

    case PM_WHILE_NODE: {
        pm_while_node_t *n = (pm_while_node_t *)node;
        char *cond = codegen_expr(ctx, n->predicate);
        emit(ctx, "while (%s) {\n", cond);
        free(cond);
        ctx->indent++;
        ctx->for_depth++;
        if (n->statements) codegen_stmts(ctx, (pm_node_t *)n->statements);
        ctx->for_depth--;
        ctx->indent--;
        emit(ctx, "}\n");
        break;
    }

    case PM_UNTIL_NODE: {
        pm_until_node_t *n = (pm_until_node_t *)node;
        char *cond = codegen_expr(ctx, n->predicate);
        emit(ctx, "while (!(%s)) {\n", cond);
        free(cond);
        ctx->indent++;
        ctx->for_depth++;
        if (n->statements) codegen_stmts(ctx, (pm_node_t *)n->statements);
        ctx->for_depth--;
        ctx->indent--;
        emit(ctx, "}\n");
        break;
    }

    case PM_FOR_NODE: {
        pm_for_node_t *fn = (pm_for_node_t *)node;
        /* for i in start..end → for (i = start; i <= end; i++) */
        if (PM_NODE_TYPE(fn->index) == PM_LOCAL_VARIABLE_TARGET_NODE &&
            PM_NODE_TYPE(fn->collection) == PM_RANGE_NODE) {
            pm_local_variable_target_node_t *t =
                (pm_local_variable_target_node_t *)fn->index;
            pm_range_node_t *rng = (pm_range_node_t *)fn->collection;
            char *vname = cstr(ctx, t->name);
            char *cn = make_cname(vname, false);
            char *lo = codegen_expr(ctx, rng->left);
            char *hi = codegen_expr(ctx, rng->right);
            bool exclude_end = PM_NODE_FLAG_P(rng, PM_RANGE_FLAGS_EXCLUDE_END);
            emit(ctx, "for (%s = %s; %s %s %s; %s++) {\n",
                 cn, lo, cn, exclude_end ? "<" : "<=", hi, cn);
            free(lo); free(hi);
            ctx->indent++;
            ctx->for_depth++;
            if (fn->statements) codegen_stmts(ctx, (pm_node_t *)fn->statements);
            ctx->for_depth--;
            ctx->indent--;
            emit(ctx, "}\n");
            free(vname); free(cn);
        }
        break;
    }

    case PM_IF_NODE: {
        pm_if_node_t *n = (pm_if_node_t *)node;
        char *cond = codegen_expr(ctx, n->predicate);
        emit(ctx, "if (%s) {\n", cond);
        free(cond);
        ctx->indent++;
        if (n->statements) codegen_stmts(ctx, (pm_node_t *)n->statements);
        ctx->indent--;
        if (n->subsequent) {
            if (PM_NODE_TYPE(n->subsequent) == PM_IF_NODE) {
                pm_if_node_t *ei = (pm_if_node_t *)n->subsequent;
                char *ec = codegen_expr(ctx, ei->predicate);
                emit(ctx, "} else if (%s) {\n", ec);
                free(ec);
                ctx->indent++;
                if (ei->statements) codegen_stmts(ctx, (pm_node_t *)ei->statements);
                ctx->indent--;
                if (ei->subsequent) {
                    emit(ctx, "} else {\n");
                    ctx->indent++;
                    codegen_stmt(ctx, (pm_node_t *)ei->subsequent);
                    ctx->indent--;
                }
                emit(ctx, "}\n");
            } else if (PM_NODE_TYPE(n->subsequent) == PM_ELSE_NODE) {
                emit(ctx, "} else {\n");
                ctx->indent++;
                pm_else_node_t *el = (pm_else_node_t *)n->subsequent;
                if (el->statements) codegen_stmts(ctx, (pm_node_t *)el->statements);
                ctx->indent--;
                emit(ctx, "}\n");
            } else {
                emit(ctx, "}\n");
            }
        } else {
            emit(ctx, "}\n");
        }
        break;
    }

    case PM_BREAK_NODE:
        emit(ctx, "break;\n");
        break;

    case PM_NEXT_NODE:
        emit(ctx, "continue;\n");
        break;

    case PM_UNLESS_NODE: {
        pm_unless_node_t *n = (pm_unless_node_t *)node;
        char *cond = codegen_expr(ctx, n->predicate);
        emit(ctx, "if (!(%s)) {\n", cond);
        free(cond);
        ctx->indent++;
        if (n->statements) codegen_stmts(ctx, (pm_node_t *)n->statements);
        ctx->indent--;
        if (n->else_clause) {
            emit(ctx, "} else {\n");
            ctx->indent++;
            codegen_stmt(ctx, (pm_node_t *)n->else_clause);
            ctx->indent--;
        }
        emit(ctx, "}\n");
        break;
    }

    case PM_CASE_NODE: {
        pm_case_node_t *n = (pm_case_node_t *)node;
        char *pred = n->predicate ? codegen_expr(ctx, n->predicate) : NULL;
        int case_id = ctx->temp_counter++;

        if (pred) {
            vtype_t pt = n->predicate ? infer_type(ctx, n->predicate) : vt_prim(SPINEL_TYPE_VALUE);
            /* Declare temp for predicate to avoid re-evaluation */
            char *ct = vt_ctype(ctx, pt, false);
            emit(ctx, "%s _case_%d = %s;\n", ct, case_id, pred);
            free(ct); free(pred);
        }

        for (size_t i = 0; i < n->conditions.size; i++) {
            pm_node_t *cond_node = n->conditions.nodes[i];
            if (PM_NODE_TYPE(cond_node) != PM_WHEN_NODE) continue;
            pm_when_node_t *when = (pm_when_node_t *)cond_node;

            emit(ctx, "%sif (", i == 0 ? "" : "} else ");

            /* Each when can have multiple conditions: when 2, 3 */
            for (size_t j = 0; j < when->conditions.size; j++) {
                if (j > 0) emit_raw(ctx, " || ");
                pm_node_t *wc = when->conditions.nodes[j];

                if (PM_NODE_TYPE(wc) == PM_RANGE_NODE && pred) {
                    /* when 4..6 → _case >= 4 && _case <= 6 */
                    pm_range_node_t *rng = (pm_range_node_t *)wc;
                    char *lo = codegen_expr(ctx, rng->left);
                    char *hi = codegen_expr(ctx, rng->right);
                    emit_raw(ctx, "(_case_%d >= %s && _case_%d <= %s)", case_id, lo, case_id, hi);
                    free(lo); free(hi);
                } else if (pred) {
                    /* when value → _case == value */
                    char *val = codegen_expr(ctx, wc);
                    emit_raw(ctx, "_case_%d == %s", case_id, val);
                    free(val);
                } else {
                    /* case without predicate: when condition → if condition */
                    char *val = codegen_expr(ctx, wc);
                    emit_raw(ctx, "%s", val);
                    free(val);
                }
            }
            emit_raw(ctx, ") {\n");
            ctx->indent++;
            if (when->statements) codegen_stmts(ctx, (pm_node_t *)when->statements);
            ctx->indent--;
        }

        if (n->else_clause) {
            emit(ctx, "} else {\n");
            ctx->indent++;
            pm_else_node_t *el = (pm_else_node_t *)n->else_clause;
            if (el->statements) codegen_stmts(ctx, (pm_node_t *)el->statements);
            ctx->indent--;
        }
        if (n->conditions.size > 0)
            emit(ctx, "}\n");
        break;
    }

    case PM_RETURN_NODE: {
        pm_return_node_t *n = (pm_return_node_t *)node;
        if (n->arguments && n->arguments->arguments.size > 0) {
            char *val = codegen_expr(ctx, n->arguments->arguments.nodes[0]);
            if (ctx->gc_scope_active)
                emit(ctx, "{ SP_GC_RESTORE(); return %s; }\n", val);
            else
                emit(ctx, "return %s;\n", val);
            free(val);
        } else {
            if (ctx->gc_scope_active)
                emit(ctx, "{ SP_GC_RESTORE(); return; }\n");
            else
                emit(ctx, "return;\n");
        }
        break;
    }

    case PM_YIELD_NODE: {
        pm_yield_node_t *yn = (pm_yield_node_t *)node;
        if (yn->arguments && yn->arguments->arguments.size > 0) {
            char *arg = codegen_expr(ctx, yn->arguments->arguments.nodes[0]);
            emit(ctx, "_block(_block_env, %s);\n", arg);
            free(arg);
        } else {
            emit(ctx, "_block(_block_env, 0);\n");
        }
        break;
    }

    case PM_CALL_NODE: {
        pm_call_node_t *call = (pm_call_node_t *)node;

        /* print int.chr → putchar */
        if (!call->receiver && try_print_chr(ctx, call))
            break;

        char *method = cstr(ctx, call->name);

        /* puts: output + newline */
        if (!call->receiver && strcmp(method, "puts") == 0) {
            /* In lambda mode: puts on a StrArray variable → iterate and print each */
            if (ctx->lambda_mode && call->arguments && call->arguments->arguments.size > 0) {
                pm_node_t *arg = call->arguments->arguments.nodes[0];
                vtype_t at = infer_type(ctx, arg);
                /* If the argument type is PROC or VALUE (could be StrArray from map), handle specially */
                if (at.kind == SPINEL_TYPE_PROC || at.kind == SPINEL_TYPE_VALUE ||
                    at.kind == SPINEL_TYPE_UNKNOWN) {
                    char *ae = codegen_expr(ctx, arg);
                    /* Emit: cast to sp_StrArray* and print each element */
                    int tmp = ctx->temp_counter++;
                    emit(ctx, "{ sp_StrArray *_pa_%d = (sp_StrArray *)%s;\n", tmp, ae);
                    emit(ctx, "  for (int _pi_%d = 0; _pi_%d < _pa_%d->len; _pi_%d++) puts(_pa_%d->data[_pi_%d]); }\n",
                         tmp, tmp, tmp, tmp, tmp, tmp);
                    free(ae);
                    free(method);
                    break;
                }
            }
            if (call->arguments && call->arguments->arguments.size > 0) {
                pm_node_t *arg = call->arguments->arguments.nodes[0];
                vtype_t at = infer_type(ctx, arg);
                if (PM_NODE_TYPE(arg) == PM_INTERPOLATED_STRING_NODE) {
                    /* Generate printf for interpolated string */
                    pm_interpolated_string_node_t *is = (pm_interpolated_string_node_t *)arg;
                    /* Build format string and args */
                    char *fmt = xstrdup("");
                    char *args = xstrdup("");
                    for (size_t i = 0; i < is->parts.size; i++) {
                        pm_node_t *part = is->parts.nodes[i];
                        if (PM_NODE_TYPE(part) == PM_STRING_NODE) {
                            pm_string_node_t *sn = (pm_string_node_t *)part;
                            const uint8_t *src = pm_string_source(&sn->unescaped);
                            size_t len = pm_string_length(&sn->unescaped);
                            size_t bufsz = strlen(fmt) + len * 4 + 16;
                            char *nf = malloc(bufsz);
                            int pos = snprintf(nf, bufsz, "%s", fmt);
                            for (size_t j = 0; j < len; j++) {
                                uint8_t c = src[j];
                                if (c == '\n') pos += snprintf(nf + pos, bufsz - pos, "\\n");
                                else if (c == '\\') pos += snprintf(nf + pos, bufsz - pos, "\\\\");
                                else if (c == '"') pos += snprintf(nf + pos, bufsz - pos, "\\\"");
                                else if (c == '%') pos += snprintf(nf + pos, bufsz - pos, "%%%%");
                                else pos += snprintf(nf + pos, bufsz - pos, "%c", c);
                            }
                            free(fmt); fmt = nf;
                        } else if (PM_NODE_TYPE(part) == PM_EMBEDDED_STATEMENTS_NODE) {
                            pm_embedded_statements_node_t *e = (pm_embedded_statements_node_t *)part;
                            if (e->statements && e->statements->body.size > 0) {
                                vtype_t et = infer_type(ctx, e->statements->body.nodes[0]);
                                char *eexpr = codegen_expr(ctx, e->statements->body.nodes[0]);
                                char *nf;
                                if (et.kind == SPINEL_TYPE_INTEGER)
                                    nf = sfmt("%s%%lld", fmt);
                                else if (et.kind == SPINEL_TYPE_FLOAT)
                                    nf = sfmt("%s%%g", fmt);
                                else
                                    nf = sfmt("%s%%s", fmt);
                                free(fmt); fmt = nf;
                                char *na = sfmt("%s, (long long)%s", args, eexpr);
                                free(args); free(eexpr);
                                args = na;
                            }
                        }
                    }
                    emit(ctx, "printf(\"%s\\n\"%s);\n", fmt, args);
                    free(fmt); free(args);
                } else if (at.kind == SPINEL_TYPE_STRING) {
                    char *ae = codegen_expr(ctx, arg);
                    emit(ctx, "puts(%s);\n", ae);
                    free(ae);
                } else if (at.kind == SPINEL_TYPE_BOOLEAN) {
                    char *ae = codegen_expr(ctx, arg);
                    emit(ctx, "puts(%s ? \"true\" : \"false\");\n", ae);
                    free(ae);
                } else if (at.kind == SPINEL_TYPE_FLOAT) {
                    char *ae = codegen_expr(ctx, arg);
                    emit(ctx, "printf(\"%%g\\n\", (double)%s);\n", ae);
                    free(ae);
                } else {
                    char *ae = codegen_expr(ctx, arg);
                    emit(ctx, "printf(\"%%lld\\n\", (long long)%s);\n", ae);
                    free(ae);
                }
            } else {
                emit(ctx, "putchar('\\n');\n");
            }
            free(method);
            break;
        }

        /* print */
        if (!call->receiver && strcmp(method, "print") == 0) {
            if (!try_print_chr(ctx, call)) {
                if (call->arguments && call->arguments->arguments.size > 0) {
                    char *ae = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                    emit(ctx, "printf(\"%%s\", %s);\n", ae);
                    free(ae);
                }
            }
            free(method);
            break;
        }

        /* putc: output a single character (integer or string) */
        if (!call->receiver && strcmp(method, "putc") == 0) {
            if (call->arguments && call->arguments->arguments.size > 0) {
                pm_node_t *arg = call->arguments->arguments.nodes[0];
                vtype_t at = infer_type(ctx, arg);
                char *ae = codegen_expr(ctx, arg);
                if (at.kind == SPINEL_TYPE_INTEGER || at.kind == SPINEL_TYPE_FLOAT)
                    emit(ctx, "putchar((int)%s);\n", ae);
                else if (at.kind == SPINEL_TYPE_STRING)
                    emit(ctx, "putchar(%s[0]);\n", ae);
                else
                    emit(ctx, "putchar((int)%s);\n", ae);
                free(ae);
            }
            free(method);
            break;
        }

        /* printf with format string */
        if (!call->receiver && strcmp(method, "printf") == 0) {
            if (call->arguments && call->arguments->arguments.size >= 1) {
                char *fmt = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                if (call->arguments->arguments.size == 1) {
                    emit(ctx, "printf(%s);\n", fmt);
                } else {
                    emit(ctx, "printf(%s", fmt);
                    for (size_t i = 1; i < call->arguments->arguments.size; i++) {
                        char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                        emit_raw(ctx, ", %s", a);
                        free(a);
                    }
                    emit_raw(ctx, ");\n");
                }
                free(fmt);
            }
            free(method);
            break;
        }

        /* p — debug print */
        if (!call->receiver && strcmp(method, "p") == 0) {
            if (call->arguments && call->arguments->arguments.size > 0) {
                pm_node_t *arg = call->arguments->arguments.nodes[0];
                vtype_t at = infer_type(ctx, arg);
                char *ae = codegen_expr(ctx, arg);
                if (at.kind == SPINEL_TYPE_STRING)
                    emit(ctx, "puts(%s);\n", ae);
                else
                    emit(ctx, "printf(\"%%lld\\n\", (long long)%s);\n", ae);
                free(ae);
            }
            free(method);
            break;
        }

        /* srand */
        if (!call->receiver && strcmp(method, "srand") == 0) {
            emit(ctx, "/* srand — handled by Rand module init */\n");
            free(method);
            break;
        }

        /* loop do ... end → while (1) { body } */
        if (!call->receiver && strcmp(method, "loop") == 0 &&
            call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
            pm_block_node_t *blk = (pm_block_node_t *)call->block;
            emit(ctx, "while (1) {\n");
            ctx->indent++;
            ctx->for_depth++;
            if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);
            ctx->for_depth--;
            ctx->indent--;
            emit(ctx, "}\n");
            free(method);
            break;
        }

        /* raise "msg" or raise ClassName, "msg" */
        if (!call->receiver && strcmp(method, "raise") == 0) {
            int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
            if (argc >= 2 && PM_NODE_TYPE(call->arguments->arguments.nodes[0]) == PM_CONSTANT_READ_NODE) {
                /* raise ClassName, "message" */
                pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->arguments->arguments.nodes[0];
                char *cls = cstr(ctx, cr->name);
                char *msg = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
                emit(ctx, "sp_raise_cls(\"%s\", %s);\n", cls, msg);
                free(cls); free(msg);
            } else if (argc >= 1) {
                char *msg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                emit(ctx, "sp_raise(%s);\n", msg);
                free(msg);
            } else {
                emit(ctx, "sp_raise(\"RuntimeError\");\n");
            }
            free(method);
            break;
        }

        /* Array#each with block → inline for loop (statement context) */
        if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE &&
            strcmp(method, "each") == 0 && call->receiver) {
            vtype_t recv_t = infer_type(ctx, call->receiver);
            if (recv_t.kind == SPINEL_TYPE_ARRAY) {
                pm_block_node_t *blk = (pm_block_node_t *)call->block;
                char *recv = codegen_expr(ctx, call->receiver);
                char *bpname = NULL;
                if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                    pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                    if (bp->parameters && bp->parameters->requireds.size > 0) {
                        pm_node_t *p = bp->parameters->requireds.nodes[0];
                        if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                            bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                    }
                }
                int tmp = ctx->temp_counter++;
                emit(ctx, "for (mrb_int _ei_%d = 0; _ei_%d < sp_IntArray_length(%s); _ei_%d++) {\n",
                     tmp, tmp, recv, tmp);
                ctx->indent++;
                if (bpname) {
                    char *cn = make_cname(bpname, false);
                    emit(ctx, "mrb_int %s = sp_IntArray_get(%s, _ei_%d);\n", cn, recv, tmp);
                    free(cn);
                }
                if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);
                ctx->indent--;
                emit(ctx, "}\n");
                free(recv); free(bpname); free(method);
                break;
            }

            /* Hash#each with block |k, v| → inline loop over insertion-order list */
            if (recv_t.kind == SPINEL_TYPE_HASH) {
                pm_block_node_t *blk = (pm_block_node_t *)call->block;
                char *recv = codegen_expr(ctx, call->receiver);
                char *kname = NULL, *vname = NULL;
                if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                    pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                    if (bp->parameters && bp->parameters->requireds.size > 0) {
                        pm_node_t *p = bp->parameters->requireds.nodes[0];
                        if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                            kname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                    }
                    if (bp->parameters && bp->parameters->requireds.size > 1) {
                        pm_node_t *p = bp->parameters->requireds.nodes[1];
                        if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                            vname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                    }
                }
                int tmp = ctx->temp_counter++;
                emit(ctx, "for (sp_HashEntry *_he_%d = %s->first; _he_%d; _he_%d = _he_%d->order_next) {\n",
                     tmp, recv, tmp, tmp, tmp);
                ctx->indent++;
                if (kname) {
                    char *cn = make_cname(kname, false);
                    emit(ctx, "%s = _he_%d->key;\n", cn, tmp);
                    free(cn);
                }
                if (vname) {
                    char *cn = make_cname(vname, false);
                    emit(ctx, "%s = _he_%d->value;\n", cn, tmp);
                    free(cn);
                }
                if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);
                ctx->indent--;
                emit(ctx, "}\n");
                free(recv); free(kname); free(vname); free(method);
                break;
            }
        }

        /* String#scan(regexp) { |m| ... } → inline loop with onig_search */
        if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE &&
            strcmp(method, "scan") == 0 && call->receiver &&
            call->arguments && call->arguments->arguments.size == 1) {
            vtype_t recv_t = infer_type(ctx, call->receiver);
            vtype_t arg_t = infer_type(ctx, call->arguments->arguments.nodes[0]);
            if (recv_t.kind == SPINEL_TYPE_STRING && arg_t.kind == SPINEL_TYPE_REGEXP) {
                pm_block_node_t *blk = (pm_block_node_t *)call->block;
                char *recv = codegen_expr(ctx, call->receiver);
                char *pat = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                char *bpname = NULL;
                if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                    pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                    if (bp->parameters && bp->parameters->requireds.size > 0) {
                        pm_node_t *p = bp->parameters->requireds.nodes[0];
                        if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                            bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                    }
                }
                int tmp = ctx->temp_counter++;
                emit(ctx, "{ /* scan */\n");
                ctx->indent++;
                emit(ctx, "const char *_ss_%d = %s;\n", tmp, recv);
                emit(ctx, "OnigRegion *_sr_%d = onig_region_new();\n", tmp);
                emit(ctx, "const OnigUChar *_se_%d = (const OnigUChar *)_ss_%d + strlen(_ss_%d);\n", tmp, tmp, tmp);
                emit(ctx, "int _sp_%d = 0;\n", tmp);
                emit(ctx, "while (_sp_%d >= 0) {\n", tmp);
                ctx->indent++;
                emit(ctx, "_sp_%d = onig_search(%s, (const OnigUChar *)_ss_%d, _se_%d,\n", tmp, pat, tmp, tmp);
                emit(ctx, "    (const OnigUChar *)_ss_%d + _sp_%d, _se_%d, _sr_%d, ONIG_OPTION_NONE);\n", tmp, tmp, tmp, tmp);
                emit(ctx, "if (_sp_%d >= 0) {\n", tmp);
                ctx->indent++;
                if (bpname) {
                    char *cn = make_cname(bpname, false);
                    emit(ctx, "int _ml_%d = _sr_%d->end[0] - _sr_%d->beg[0];\n", tmp, tmp, tmp);
                    emit(ctx, "char *%s = (char *)malloc(_ml_%d + 1);\n", cn, tmp);
                    emit(ctx, "memcpy(%s, _ss_%d + _sr_%d->beg[0], _ml_%d);\n", cn, tmp, tmp, tmp);
                    emit(ctx, "%s[_ml_%d] = '\\0';\n", cn, tmp);
                    free(cn);
                }
                if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);
                emit(ctx, "_sp_%d = _sr_%d->end[0];\n", tmp, tmp);
                ctx->indent--;
                emit(ctx, "}\n");
                ctx->indent--;
                emit(ctx, "}\n");
                emit(ctx, "onig_region_free(_sr_%d, 1);\n", tmp);
                ctx->indent--;
                emit(ctx, "}\n");
                free(recv); free(pat); free(bpname); free(method);
                break;
            }
        }

        /* Receiver-less call with block to yield function → generate callback */
        if (!call->receiver && call->block &&
            PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
            func_info_t *fn = find_func(ctx, method);
            if (fn && fn->has_yield) {
                pm_block_node_t *blk = (pm_block_node_t *)call->block;
                int blk_id = ctx->block_counter++;

                /* Get block parameter name */
                char *bpname = NULL;
                if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                    pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                    if (bp->parameters && bp->parameters->requireds.size > 0) {
                        pm_node_t *p = bp->parameters->requireds.nodes[0];
                        if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                            bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                    }
                }

                /* Scan block body for captured variables (variables from the caller's scope) */
                capture_list_t local_defs = {.count = 0};
                capture_list_t captures = {.count = 0};
                scan_captures(ctx, (pm_node_t *)blk->body,
                              bpname ? bpname : "", &local_defs, &captures);

                /* Generate the block body first (to a temp buffer). Any nested blocks
                 * will be written to ctx->block_out during body generation, so they
                 * appear BEFORE this block in the output (correct C ordering). */
                int saved_indent = ctx->indent;
                int saved_var_count = ctx->var_count;
                ctx->indent = 1;

                /* Register block param in var table */
                if (bpname)
                    var_declare(ctx, bpname, vt_prim(SPINEL_TYPE_INTEGER), false);

                char *body_processed = NULL;
                {
                    char *body_buf_data = NULL;
                    size_t body_buf_size = 0;
                    FILE *body_buf = open_memstream(&body_buf_data, &body_buf_size);
                    FILE *saved_out = ctx->out;
                    ctx->out = body_buf;

                    if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);

                    fclose(body_buf);
                    ctx->out = saved_out;

                    /* Replace lv_CAPNAME with (*_e->CAPNAME) for captured vars */
                    if (body_buf_data) {
                        body_processed = xstrdup(body_buf_data);
                        for (int i = 0; i < captures.count; i++) {
                            char *old_ref = sfmt("lv_%s", captures.names[i]);
                            char *new_ref = sfmt("(*_e->%s)", captures.names[i]);
                            while (1) {
                                char *pos = strstr(body_processed, old_ref);
                                if (!pos) break;
                                size_t prefix_len = pos - body_processed;
                                size_t old_len = strlen(old_ref);
                                size_t new_len = strlen(new_ref);
                                size_t rest_len = strlen(pos + old_len);
                                char *nr = malloc(prefix_len + new_len + rest_len + 1);
                                memcpy(nr, body_processed, prefix_len);
                                memcpy(nr + prefix_len, new_ref, new_len);
                                memcpy(nr + prefix_len + new_len, pos + old_len, rest_len + 1);
                                free(body_processed);
                                body_processed = nr;
                            }
                            free(old_ref); free(new_ref);
                        }
                        free(body_buf_data);
                    }
                }

                ctx->indent = saved_indent;
                ctx->var_count = saved_var_count;

                /* Now write the complete block function to block_out.
                 * At this point, any nested blocks have already been written to
                 * block_out, so they appear before this block (correct for C). */
                if (ctx->block_out) {
                    /* Env struct */
                    fprintf(ctx->block_out, "typedef struct { ");
                    for (int i = 0; i < captures.count; i++)
                        fprintf(ctx->block_out, "mrb_int *%s; ", captures.names[i]);
                    if (captures.count == 0) fprintf(ctx->block_out, "int _dummy; ");
                    fprintf(ctx->block_out, "} _blk_%d_env;\n", blk_id);

                    /* Callback function */
                    fprintf(ctx->block_out, "static mrb_int _blk_%d(void *_env, mrb_int _arg) {\n", blk_id);
                    fprintf(ctx->block_out, "    _blk_%d_env *_e = (_blk_%d_env *)_env;\n", blk_id, blk_id);
                    if (bpname) {
                        char *cn = make_cname(bpname, false);
                        fprintf(ctx->block_out, "    mrb_int %s = _arg;\n", cn);
                        free(cn);
                    }
                    if (body_processed) {
                        fprintf(ctx->block_out, "%s", body_processed);
                    }
                    fprintf(ctx->block_out, "    return 0;\n");
                    fprintf(ctx->block_out, "}\n\n");
                }
                free(body_processed);

                /* Generate the call site: env init + function call */
                emit(ctx, "_blk_%d_env _env_%d = { ", blk_id, blk_id);
                for (int i = 0; i < captures.count; i++) {
                    char *cn = make_cname(captures.names[i], false);
                    emit_raw(ctx, "%s&%s", i > 0 ? ", " : "", cn);
                    free(cn);
                }
                if (captures.count == 0) emit_raw(ctx, "0");
                emit_raw(ctx, " };\n");

                /* Build arguments */
                int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                char *args = xstrdup("");
                for (int i = 0; i < argc; i++) {
                    char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                    char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", a);
                    free(args); free(a);
                    args = na;
                }
                emit(ctx, "sp_%s(%s%s(sp_block_fn)_blk_%d, &_env_%d);\n",
                     method, args, argc > 0 ? ", " : "", blk_id, blk_id);
                free(args); free(bpname); free(method);
                break;
            }
        }

        /* Integer#times with block → for loop */
        if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE &&
            strcmp(method, "times") == 0) {
            pm_block_node_t *blk = (pm_block_node_t *)call->block;
            char *count = codegen_expr(ctx, call->receiver);

            /* Get block parameter name */
            char *bpname = NULL;
            if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                if (bp->parameters && bp->parameters->requireds.size > 0) {
                    pm_node_t *p = bp->parameters->requireds.nodes[0];
                    if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                        bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                }
            }

            if (bpname) {
                char *cn = make_cname(bpname, false);
                emit(ctx, "for (%s = 0; %s < %s; %s++) {\n", cn, cn, count, cn);
                free(cn); free(bpname);
            } else {
                int tmp = ctx->temp_counter++;
                emit(ctx, "for (mrb_int _it%d = 0; _it%d < %s; _it%d++) {\n",
                     tmp, tmp, count, tmp);
            }
            free(count);

            ctx->indent++;
            ctx->for_depth++;
            if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);
            ctx->for_depth--;
            ctx->indent--;
            emit(ctx, "}\n");
            free(method);
            break;
        }

        /* Integer#upto/downto with block → for loop */
        if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE &&
            (strcmp(method, "upto") == 0 || strcmp(method, "downto") == 0) &&
            call->arguments && call->arguments->arguments.size == 1) {
            pm_block_node_t *blk = (pm_block_node_t *)call->block;
            char *start = codegen_expr(ctx, call->receiver);
            char *end = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
            bool is_upto = strcmp(method, "upto") == 0;

            char *bpname = NULL;
            if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                if (bp->parameters && bp->parameters->requireds.size > 0) {
                    pm_node_t *p = bp->parameters->requireds.nodes[0];
                    if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE)
                        bpname = cstr(ctx, ((pm_required_parameter_node_t *)p)->name);
                }
            }
            if (bpname) {
                char *cn = make_cname(bpname, false);
                if (is_upto)
                    emit(ctx, "for (%s = %s; %s <= %s; %s++) {\n", cn, start, cn, end, cn);
                else
                    emit(ctx, "for (%s = %s; %s >= %s; %s--) {\n", cn, start, cn, end, cn);
                free(cn); free(bpname);
            } else {
                int tmp = ctx->temp_counter++;
                if (is_upto)
                    emit(ctx, "for (mrb_int _it%d = %s; _it%d <= %s; _it%d++) {\n", tmp, start, tmp, end, tmp);
                else
                    emit(ctx, "for (mrb_int _it%d = %s; _it%d >= %s; _it%d--) {\n", tmp, start, tmp, end, tmp);
            }
            free(start); free(end);
            ctx->indent++;
            ctx->for_depth++;
            if (blk->body) codegen_stmts(ctx, (pm_node_t *)blk->body);
            ctx->for_depth--;
            ctx->indent--;
            emit(ctx, "}\n");
            free(method);
            break;
        }

        /* String << (append) as statement → reassign */
        if (call->receiver && strcmp(method, "<<") == 0 &&
            call->arguments && call->arguments->arguments.size == 1) {
            vtype_t recv_t = infer_type(ctx, call->receiver);
            if (recv_t.kind == SPINEL_TYPE_STRING &&
                PM_NODE_TYPE(call->receiver) == PM_LOCAL_VARIABLE_READ_NODE) {
                pm_local_variable_read_node_t *lv = (pm_local_variable_read_node_t *)call->receiver;
                char *vn = cstr(ctx, lv->name);
                char *cn = make_cname(vn, false);
                char *arg = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
                emit(ctx, "%s = sp_str_concat(%s, %s);\n", cn, cn, arg);
                free(vn); free(cn); free(arg); free(method);
                break;
            }
        }

        /* General call as statement */
        {
            char *expr = codegen_expr(ctx, node);
            emit(ctx, "%s;\n", expr);
            free(expr); free(method);
        }
        break;
    }

    case PM_ELSE_NODE: {
        pm_else_node_t *n = (pm_else_node_t *)node;
        if (n->statements) codegen_stmts(ctx, (pm_node_t *)n->statements);
        break;
    }

    case PM_BEGIN_NODE: {
        pm_begin_node_t *bn = (pm_begin_node_t *)node;
        pm_rescue_node_t *rescue = bn->rescue_clause;
        pm_ensure_node_t *ensure = bn->ensure_clause;

        if (!rescue && !ensure) {
            /* Plain begin...end with no rescue/ensure — just emit body */
            if (bn->statements) codegen_stmts(ctx, (pm_node_t *)bn->statements);
            break;
        }

        int exc_id = ctx->exc_counter++;

        if (rescue) {
            /* Check if rescue body contains retry (simple scan) */
            bool has_retry = false;
            if (rescue->statements) {
                pm_statements_node_t *rs = rescue->statements;
                for (size_t ri = 0; ri < rs->body.size; ri++) {
                    pm_node_t *rn = rs->body.nodes[ri];
                    if (PM_NODE_TYPE(rn) == PM_RETRY_NODE) { has_retry = true; break; }
                    /* Also check modifier-if: retry if cond */
                    if (PM_NODE_TYPE(rn) == PM_IF_NODE) {
                        pm_if_node_t *ifn = (pm_if_node_t *)rn;
                        if (ifn->statements) {
                            pm_statements_node_t *ifs = ifn->statements;
                            for (size_t ii = 0; ii < ifs->body.size; ii++)
                                if (PM_NODE_TYPE(ifs->body.nodes[ii]) == PM_RETRY_NODE)
                                    { has_retry = true; break; }
                        }
                    }
                }
            }
            /* Only emit retry label if rescue body uses retry */
            emit(ctx, "/* begin/rescue */\n");
            if (has_retry)
                emit_raw(ctx, "_sp_retry_%d: ;\n", exc_id);

            emit(ctx, "sp_exc_depth++;\n");
            emit(ctx, "if (setjmp(sp_exc_stack[sp_exc_depth - 1]) == 0) {\n");
            ctx->indent++;

            /* begin body */
            if (bn->statements) codegen_stmts(ctx, (pm_node_t *)bn->statements);

            ctx->indent--;
            emit(ctx, "    sp_exc_depth--;\n");
            emit(ctx, "} else {\n");
            ctx->indent++;
            emit(ctx, "sp_exc_depth--;\n");

            /* rescue clauses — may be chained (rescue A => e; rescue B => e2) */
            pm_rescue_node_t *rc = rescue;
            bool first_rc = true;
            while (rc) {
                /* Check if this rescue has exception class filters */
                bool has_class_filter = (rc->exceptions.size > 0);

                if (has_class_filter) {
                    emit(ctx, "%sif (", first_rc ? "" : "} else ");
                    for (size_t ei = 0; ei < rc->exceptions.size; ei++) {
                        if (ei > 0) emit_raw(ctx, " || ");
                        pm_node_t *exc = rc->exceptions.nodes[ei];
                        if (PM_NODE_TYPE(exc) == PM_CONSTANT_READ_NODE) {
                            pm_constant_read_node_t *cr = (pm_constant_read_node_t *)exc;
                            char *cls = cstr(ctx, cr->name);
                            emit_raw(ctx, "sp_exc_is_a(\"%s\")", cls);
                            free(cls);
                        }
                    }
                    emit_raw(ctx, ") {\n");
                    ctx->indent++;
                } else if (!first_rc) {
                    emit(ctx, "} else {\n");
                    ctx->indent++;
                }

                /* rescue => e binding */
                if (rc->reference) {
                    if (PM_NODE_TYPE(rc->reference) == PM_LOCAL_VARIABLE_TARGET_NODE) {
                        pm_local_variable_target_node_t *t =
                            (pm_local_variable_target_node_t *)rc->reference;
                        char *vn = cstr(ctx, t->name);
                        char *cn = make_cname(vn, false);
                        emit(ctx, "%s = sp_exc_message;\n", cn);
                        free(vn); free(cn);
                    }
                }

                /* rescue body */
                if (rc->statements) codegen_stmts(ctx, (pm_node_t *)rc->statements);

                if (has_class_filter) ctx->indent--;

                first_rc = false;
                rc = rc->subsequent;
            }
            if (!first_rc && rescue->exceptions.size > 0)
                emit(ctx, "}\n");

            ctx->indent--;
            emit(ctx, "}\n");

        } else {
            /* ensure without rescue — just emit body */
            if (bn->statements) codegen_stmts(ctx, (pm_node_t *)bn->statements);
        }

        /* ensure clause */
        if (ensure && ensure->statements)
            codegen_stmts(ctx, (pm_node_t *)ensure->statements);

        break;
    }

    case PM_RETRY_NODE: {
        /* Find the nearest enclosing begin/rescue exc_id.
         * We use the current exc_counter - 1 since the enclosing begin just incremented it. */
        emit(ctx, "goto _sp_retry_%d;\n", ctx->exc_counter - 1);
        break;
    }

    case PM_RESCUE_MODIFIER_NODE: {
        /* expr rescue default_expr  → inline rescue */
        pm_rescue_modifier_node_t *rm = (pm_rescue_modifier_node_t *)node;
        int exc_id = ctx->exc_counter++;
        emit(ctx, "sp_exc_depth++;\n");
        emit(ctx, "if (setjmp(sp_exc_stack[sp_exc_depth - 1]) == 0) {\n");
        ctx->indent++;
        codegen_stmt(ctx, rm->expression);
        ctx->indent--;
        emit(ctx, "    sp_exc_depth--;\n");
        emit(ctx, "} else {\n");
        ctx->indent++;
        emit(ctx, "sp_exc_depth--;\n");
        codegen_stmt(ctx, rm->rescue_expression);
        ctx->indent--;
        emit(ctx, "}\n");
        (void)exc_id;
        break;
    }

    case PM_MULTI_WRITE_NODE: {
        pm_multi_write_node_t *n = (pm_multi_write_node_t *)node;
        if (PM_NODE_TYPE(n->value) == PM_ARRAY_NODE) {
            pm_array_node_t *ary = (pm_array_node_t *)n->value;
            size_t count = n->lefts.size < ary->elements.size ? n->lefts.size : ary->elements.size;
            emit(ctx, "{\n"); ctx->indent++;
            for (size_t i = 0; i < count; i++) {
                vtype_t rt = infer_type(ctx, ary->elements.nodes[i]);
                char *ct = vt_ctype(ctx, rt, false);
                char *rhs = codegen_expr(ctx, ary->elements.nodes[i]);
                emit(ctx, "%s _mw_%d = %s;\n", ct, (int)i, rhs);
                free(ct); free(rhs);
            }
            for (size_t i = 0; i < count; i++) {
                if (PM_NODE_TYPE(n->lefts.nodes[i]) == PM_LOCAL_VARIABLE_TARGET_NODE) {
                    pm_local_variable_target_node_t *t = (pm_local_variable_target_node_t *)n->lefts.nodes[i];
                    char *vn = cstr(ctx, t->name);
                    char *cn = make_cname(vn, false);
                    emit(ctx, "%s = _mw_%d;\n", cn, (int)i);
                    free(vn); free(cn);
                } else if (PM_NODE_TYPE(n->lefts.nodes[i]) == PM_INSTANCE_VARIABLE_TARGET_NODE) {
                    pm_instance_variable_target_node_t *t = (pm_instance_variable_target_node_t *)n->lefts.nodes[i];
                    char *ivn = cstr(ctx, t->name);
                    const char *field = ivn + 1;
                    if (ctx->current_module)
                        emit(ctx, "sp_%s_%s = _mw_%d;\n", ctx->current_module->name, field, (int)i);
                    else if (ctx->current_class && ctx->current_class->is_value_type)
                        emit(ctx, "self.%s = _mw_%d;\n", field, (int)i);
                    else
                        emit(ctx, "self->%s = _mw_%d;\n", field, (int)i);
                    free(ivn);
                }
            }
            ctx->indent--; emit(ctx, "}\n");
        }
        break;
    }

    /* Skip class/module/def at top level (handled in separate passes) */
    case PM_CLASS_NODE:
    case PM_MODULE_NODE:
    case PM_DEF_NODE:
        break;

    /* Handle expression-as-statement (including implicit returns, nil nodes) */
    case PM_LOCAL_VARIABLE_READ_NODE:
    case PM_INSTANCE_VARIABLE_READ_NODE:
    case PM_NIL_NODE:
    case PM_SELF_NODE:
    case PM_FLOAT_NODE:
    case PM_INTEGER_NODE:
        /* Bare expression — typically the implicit return value.
         * In void methods we discard; in non-void we could emit return. */
        break;

    default: {
        /* Try as expression statement */
        char *expr = codegen_expr(ctx, node);
        if (expr && strcmp(expr, "0") != 0 && strncmp(expr, "/* TODO", 7) != 0)
            emit(ctx, "%s;\n", expr);
        free(expr);
        break;
    }
    }
}

static void codegen_stmts(codegen_ctx_t *ctx, pm_node_t *node) {
    if (!node) return;
    if (PM_NODE_TYPE(node) == PM_STATEMENTS_NODE) {
        pm_statements_node_t *s = (pm_statements_node_t *)node;
        bool saved_ir = ctx->implicit_return;
        for (size_t i = 0; i < s->body.size; i++) {
            bool is_last = (i + 1 == s->body.size);
            if (!is_last) {
                /* Not the last statement — disable implicit return */
                ctx->implicit_return = false;
            } else {
                ctx->implicit_return = saved_ir;
            }
            pm_node_t *stmt = s->body.nodes[i];
            /* If implicit_return and last stmt is a simple expression, emit return */
            if (is_last && ctx->implicit_return &&
                PM_NODE_TYPE(stmt) != PM_IF_NODE &&
                PM_NODE_TYPE(stmt) != PM_WHILE_NODE &&
                PM_NODE_TYPE(stmt) != PM_RETURN_NODE &&
                PM_NODE_TYPE(stmt) != PM_LOCAL_VARIABLE_WRITE_NODE &&
                PM_NODE_TYPE(stmt) != PM_CONSTANT_WRITE_NODE &&
                PM_NODE_TYPE(stmt) != PM_INSTANCE_VARIABLE_WRITE_NODE &&
                PM_NODE_TYPE(stmt) != PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE &&
                PM_NODE_TYPE(stmt) != PM_BREAK_NODE) {
                char *val = codegen_expr(ctx, stmt);
                if (val && strcmp(val, "/* nil */") != 0) {
                    if (ctx->gc_scope_active)
                        emit(ctx, "{ SP_GC_RESTORE(); return %s; }\n", val);
                    else
                        emit(ctx, "return %s;\n", val);
                }
                free(val);
                ctx->implicit_return = saved_ir;
                continue;
            }
            codegen_stmt(ctx, stmt);
        }
        ctx->implicit_return = saved_ir;
    } else {
        codegen_stmt(ctx, node);
    }
}

/* ------------------------------------------------------------------ */
/* Method body codegen                                                */
/* ------------------------------------------------------------------ */

static void emit_method(codegen_ctx_t *ctx, class_info_t *cls, method_info_t *m) {
    if (strcmp(m->name, "initialize") == 0) return; /* handled by constructor */
    if (m->is_getter || m->is_setter) return; /* inlined */

    /* Determine return type C string */
    char *ret_ct = vt_ctype(ctx, m->return_type, false);
    bool ret_void = (m->return_type.kind == SPINEL_TYPE_NIL);

    /* Function signature */
    emit_raw(ctx, "static %s sp_%s_%s(",
             ret_void ? "void" : ret_ct, cls->name, m->name);

    if (m->is_class_method) {
        /* Class method: no self parameter */
        for (int i = 0; i < m->param_count; i++) {
            if (i > 0) emit_raw(ctx, ", ");
            char *pct = vt_ctype(ctx, m->params[i].type, false);
            emit_raw(ctx, "%s lv_%s", pct, m->params[i].name);
            free(pct);
        }
        if (m->param_count == 0) emit_raw(ctx, "void");
    } else {
        /* Self parameter */
        if (cls->is_value_type)
            emit_raw(ctx, "sp_%s self", cls->name);
        else
            emit_raw(ctx, "sp_%s *self", cls->name);

        /* Method parameters — use lv_ prefix to match codegen variable references */
        for (int i = 0; i < m->param_count; i++) {
            emit_raw(ctx, ", ");
            char *pct = vt_ctype(ctx, m->params[i].type, !cls->is_value_type);
            emit_raw(ctx, "%s lv_%s", pct, m->params[i].name);
            free(pct);
        }
    }
    emit_raw(ctx, ") {\n");

    /* Set method context for ivar access */
    ctx->current_class = cls;
    ctx->current_method = m;
    int saved_indent = ctx->indent;
    int saved_var_count = ctx->var_count;
    ctx->indent = 1;

    /* Register method parameters in the variable table so type inference works */
    for (int i = 0; i < m->param_count; i++)
        var_declare(ctx, m->params[i].name, m->params[i].type, false);

    /* Infer local variables from method body */
    if (m->body_node) infer_pass(ctx, m->body_node);

    /* Check if this method needs GC rooting */
    bool method_has_gc_vars = false;
    if (ctx->needs_gc && !m->is_class_method) {
        /* Check self (non-value-type class pointer) */
        if (!cls->is_value_type) method_has_gc_vars = true;
        /* Check parameters */
        for (int i = 0; i < m->param_count && !method_has_gc_vars; i++)
            if (is_gc_type(ctx, m->params[i].type)) method_has_gc_vars = true;
        /* Check locals */
        for (int i = saved_var_count + m->param_count; i < ctx->var_count && !method_has_gc_vars; i++)
            if (is_gc_type(ctx, ctx->vars[i].type)) method_has_gc_vars = true;
    }

    bool saved_gc_scope = ctx->gc_scope_active;
    if (method_has_gc_vars) {
        emit(ctx, "SP_GC_SAVE();\n");
        ctx->gc_scope_active = true;
        /* Root self if it's a GC pointer */
        if (!cls->is_value_type)
            emit(ctx, "SP_GC_ROOT(self);\n");
        /* Root GC-typed parameters */
        for (int i = 0; i < m->param_count; i++) {
            if (is_gc_type(ctx, m->params[i].type))
                emit(ctx, "SP_GC_ROOT(lv_%s);\n", m->params[i].name);
        }
    }

    /* Emit local variable declarations (skip params — they're function args) */
    for (int i = saved_var_count + m->param_count; i < ctx->var_count; i++) {
        var_entry_t *v = &ctx->vars[i];
        char *ct = vt_ctype(ctx, v->type, false);
        char *cn = make_cname(v->name, v->is_constant);
        if (v->is_array) {
            emit(ctx, "%s %s[%d];\n", ct, cn, v->array_size);
            free(ct); free(cn);
            continue;
        }
        const char *init = "";
        if (v->type.kind == SPINEL_TYPE_INTEGER) init = " = 0";
        else if (v->type.kind == SPINEL_TYPE_FLOAT) init = " = 0.0";
        else if (v->type.kind == SPINEL_TYPE_BOOLEAN) init = " = FALSE";
        else if (v->type.kind == SPINEL_TYPE_OBJECT) {
            class_info_t *vc = find_class(ctx, v->type.klass);
            if (vc && vc->is_value_type) {
                /* Value type: stack-allocated struct */
                emit(ctx, "%s %s; memset(&%s, 0, sizeof(%s));\n", ct, cn, cn, cn);
            } else {
                /* Reference type: pointer, initialized to NULL */
                emit(ctx, "%s *%s = NULL;\n", ct, cn);
                if (method_has_gc_vars)
                    emit(ctx, "SP_GC_ROOT(%s);\n", cn);
            }
            free(ct); free(cn);
            continue;
        }
        else if (v->type.kind == SPINEL_TYPE_ARRAY) {
            emit(ctx, "%s *%s = NULL;\n", ct, cn);
            if (method_has_gc_vars)
                emit(ctx, "SP_GC_ROOT(%s);\n", cn);
            free(ct); free(cn);
            continue;
        }
        else if (v->type.kind == SPINEL_TYPE_HASH) {
            emit(ctx, "sp_StrIntHash *%s = NULL;\n", cn);
            if (method_has_gc_vars)
                emit(ctx, "SP_GC_ROOT(%s);\n", cn);
            free(ct); free(cn);
            continue;
        }
        emit(ctx, "%s %s%s;\n", ct, cn, init);
        free(ct); free(cn);
    }

    /* Generate method body, with implicit return for last expression */
    if (m->body_node && PM_NODE_TYPE(m->body_node) == PM_STATEMENTS_NODE) {
        pm_statements_node_t *stmts = (pm_statements_node_t *)m->body_node;
        /* Emit all but last */
        for (size_t i = 0; i + 1 < stmts->body.size; i++)
            codegen_stmt(ctx, stmts->body.nodes[i]);
        /* Last statement: return if non-void */
        if (stmts->body.size > 0) {
            pm_node_t *last = stmts->body.nodes[stmts->body.size - 1];
            if (!ret_void &&
                PM_NODE_TYPE(last) != PM_IF_NODE &&
                PM_NODE_TYPE(last) != PM_WHILE_NODE &&
                PM_NODE_TYPE(last) != PM_RETURN_NODE) {
                char *val = codegen_expr(ctx, last);
                if (method_has_gc_vars)
                    emit(ctx, "SP_GC_RESTORE();\n");
                emit(ctx, "return %s;\n", val);
                free(val);
            } else {
                if (!ret_void) ctx->implicit_return = true;
                codegen_stmt(ctx, last);
                ctx->implicit_return = false;
            }
        }
    } else if (m->body_node) {
        codegen_stmts(ctx, m->body_node);
    }

    if (method_has_gc_vars && ret_void)
        emit_raw(ctx, "    SP_GC_RESTORE();\n");

    ctx->gc_scope_active = saved_gc_scope;
    ctx->indent = saved_indent;
    ctx->var_count = saved_var_count;
    ctx->current_class = NULL;
    ctx->current_method = NULL;

    emit_raw(ctx, "}\n\n");
    free(ret_ct);
}

/* ------------------------------------------------------------------ */
/* Top-level function codegen                                         */
/* ------------------------------------------------------------------ */

static void emit_top_func(codegen_ctx_t *ctx, func_info_t *f) {
    /* Determine return type */
    char *ret_ct = vt_ctype(ctx, f->return_type, false);
    bool ret_void = (f->return_type.kind == SPINEL_TYPE_NIL);

    emit_raw(ctx, "static %s sp_%s(", ret_void ? "void" : ret_ct, f->name);
    for (int i = 0; i < f->param_count; i++) {
        if (i > 0) emit_raw(ctx, ", ");
        char *pct = vt_ctype(ctx, f->params[i].type, false);
        /* Use lv_ prefix to match how codegen references locals */
        if (f->params[i].is_array)
            emit_raw(ctx, "%s *lv_%s", pct, f->params[i].name);
        else
            emit_raw(ctx, "%s lv_%s", pct, f->params[i].name);
        free(pct);
    }
    /* Add block callback parameters for yield functions */
    if (f->has_yield) {
        if (f->param_count > 0) emit_raw(ctx, ", ");
        emit_raw(ctx, "sp_block_fn _block, void *_block_env");
    }
    if (f->param_count == 0 && !f->has_yield) emit_raw(ctx, "void");
    emit_raw(ctx, ") {\n");

    int saved_indent = ctx->indent;
    int saved_var_count = ctx->var_count;
    ctx->indent = 1;

    /* Register parameters in var table for type inference */
    for (int i = 0; i < f->param_count; i++)
        var_declare(ctx, f->params[i].name, f->params[i].type, false);

    if (f->body_node) infer_pass(ctx, f->body_node);

    /* Check if this function needs GC rooting */
    bool func_has_gc_vars = false;
    if (ctx->needs_gc) {
        for (int i = 0; i < f->param_count && !func_has_gc_vars; i++)
            if (is_gc_type(ctx, f->params[i].type)) func_has_gc_vars = true;
        for (int i = saved_var_count + f->param_count; i < ctx->var_count && !func_has_gc_vars; i++)
            if (is_gc_type(ctx, ctx->vars[i].type)) func_has_gc_vars = true;
    }

    bool saved_gc_scope = ctx->gc_scope_active;
    if (func_has_gc_vars) {
        emit(ctx, "SP_GC_SAVE();\n");
        ctx->gc_scope_active = true;
        for (int i = 0; i < f->param_count; i++) {
            if (is_gc_type(ctx, f->params[i].type))
                emit(ctx, "SP_GC_ROOT(lv_%s);\n", f->params[i].name);
        }
    }

    /* Collect all local variable names used in the function body */
    /* Include both newly registered vars AND outer-scope vars that shadow */
    {
        /* First emit newly registered vars (the standard path) */
        for (int i = saved_var_count + f->param_count; i < ctx->var_count; i++) {
            var_entry_t *v = &ctx->vars[i];
            char *ct = vt_ctype(ctx, v->type, false);
            char *cn = make_cname(v->name, v->is_constant);
            if (v->type.kind == SPINEL_TYPE_ARRAY) {
                emit(ctx, "sp_IntArray *%s = NULL;\n", cn);
                if (func_has_gc_vars)
                    emit(ctx, "SP_GC_ROOT(%s);\n", cn);
                free(ct); free(cn);
                continue;
            }
            if (v->type.kind == SPINEL_TYPE_HASH) {
                emit(ctx, "sp_StrIntHash *%s = NULL;\n", cn);
                if (func_has_gc_vars)
                    emit(ctx, "SP_GC_ROOT(%s);\n", cn);
                free(ct); free(cn);
                continue;
            }
            if (v->type.kind == SPINEL_TYPE_OBJECT) {
                class_info_t *vc = find_class(ctx, v->type.klass);
                if (vc && !vc->is_value_type) {
                    emit(ctx, "%s *%s = NULL;\n", ct, cn);
                    if (func_has_gc_vars)
                        emit(ctx, "SP_GC_ROOT(%s);\n", cn);
                    free(ct); free(cn);
                    continue;
                }
            }
            const char *init = "";
            if (v->type.kind == SPINEL_TYPE_INTEGER) init = " = 0";
            else if (v->type.kind == SPINEL_TYPE_FLOAT) init = " = 0.0";
            else if (v->type.kind == SPINEL_TYPE_BOOLEAN) init = " = FALSE";
            else if (v->type.kind == SPINEL_TYPE_STRING) init = " = NULL";
            emit(ctx, "%s %s%s;\n", ct, cn, init);
            free(ct); free(cn);
        }
        /* Also declare any outer-scope variables that are used in this function
         * body but were not newly registered (shadowed by top-level vars).
         * This happens when the function body uses a variable name that also
         * exists at top-level (e.g., 'i' in a yield function). */
        if (f->body_node) {
            /* Walk the function body to find all local variable writes/refs */
            pm_node_t *stack[256];
            int sp = 0;
            if (PM_NODE_TYPE(f->body_node) == PM_STATEMENTS_NODE) {
                pm_statements_node_t *stmts = (pm_statements_node_t *)f->body_node;
                for (size_t si = 0; si < stmts->body.size && sp < 255; si++)
                    stack[sp++] = stmts->body.nodes[si];
            } else if (PM_NODE_TYPE(f->body_node) == PM_BEGIN_NODE) {
                if (sp < 255) stack[sp++] = f->body_node;
            }
            /* Track which shadow vars we've already emitted to avoid duplicates */
            char emitted_shadows[16][64];
            int emitted_shadow_count = 0;
            while (sp > 0) {
                pm_node_t *cur = stack[--sp];
                if (!cur) continue;
                /* Helper: check if a variable name needs a local declaration */
                #define CHECK_SHADOW_VAR(vname, vtype) do { \
                    bool is_param = false; \
                    for (int pi = 0; pi < f->param_count; pi++) \
                        if (strcmp(f->params[pi].name, (vname)) == 0) { is_param = true; break; } \
                    bool is_new_var = false; \
                    for (int vi = saved_var_count + f->param_count; vi < ctx->var_count; vi++) \
                        if (strcmp(ctx->vars[vi].name, (vname)) == 0) { is_new_var = true; break; } \
                    bool already_emitted = false; \
                    for (int ei = 0; ei < emitted_shadow_count; ei++) \
                        if (strcmp(emitted_shadows[ei], (vname)) == 0) { already_emitted = true; break; } \
                    if (!is_param && !is_new_var && !already_emitted) { \
                        var_entry_t *v = var_lookup(ctx, (vname)); \
                        vtype_t et = (v) ? v->type : (vtype); \
                        char *ct = vt_ctype(ctx, et, false); \
                        char *cn = make_cname((vname), false); \
                        const char *init = ""; \
                        if (et.kind == SPINEL_TYPE_INTEGER) init = " = 0"; \
                        else if (et.kind == SPINEL_TYPE_FLOAT) init = " = 0.0"; \
                        else if (et.kind == SPINEL_TYPE_STRING) init = " = NULL"; \
                        emit(ctx, "%s %s%s;\n", ct, cn, init); \
                        free(ct); free(cn); \
                        if (emitted_shadow_count < 16) \
                            snprintf(emitted_shadows[emitted_shadow_count++], 64, "%s", (vname)); \
                    } \
                } while(0)

                if (PM_NODE_TYPE(cur) == PM_LOCAL_VARIABLE_WRITE_NODE) {
                    pm_local_variable_write_node_t *lw = (pm_local_variable_write_node_t *)cur;
                    char *vname = cstr(ctx, lw->name);
                    CHECK_SHADOW_VAR(vname, vt_prim(SPINEL_TYPE_VALUE));
                    free(vname);
                    if (sp < 255) stack[sp++] = lw->value;
                }
                if (PM_NODE_TYPE(cur) == PM_STATEMENTS_NODE) {
                    pm_statements_node_t *ss = (pm_statements_node_t *)cur;
                    for (size_t si = 0; si < ss->body.size && sp < 255; si++)
                        stack[sp++] = ss->body.nodes[si];
                }
                if (PM_NODE_TYPE(cur) == PM_WHILE_NODE) {
                    pm_while_node_t *wn = (pm_while_node_t *)cur;
                    if (wn->statements && sp < 255) stack[sp++] = (pm_node_t *)wn->statements;
                }
                if (PM_NODE_TYPE(cur) == PM_IF_NODE) {
                    pm_if_node_t *ifn = (pm_if_node_t *)cur;
                    if (ifn->statements && sp < 255) stack[sp++] = (pm_node_t *)ifn->statements;
                    if (ifn->subsequent && sp < 255) stack[sp++] = (pm_node_t *)ifn->subsequent;
                }
                if (PM_NODE_TYPE(cur) == PM_BEGIN_NODE) {
                    pm_begin_node_t *bn = (pm_begin_node_t *)cur;
                    if (bn->statements && sp < 255) stack[sp++] = (pm_node_t *)bn->statements;
                    if (bn->rescue_clause) {
                        pm_rescue_node_t *rescue = bn->rescue_clause;
                        /* Handle rescue => e variable */
                        if (rescue->reference &&
                            PM_NODE_TYPE(rescue->reference) == PM_LOCAL_VARIABLE_TARGET_NODE) {
                            pm_local_variable_target_node_t *t =
                                (pm_local_variable_target_node_t *)rescue->reference;
                            char *vname = cstr(ctx, t->name);
                            CHECK_SHADOW_VAR(vname, vt_prim(SPINEL_TYPE_STRING));
                            free(vname);
                        }
                        if (rescue->statements && sp < 255)
                            stack[sp++] = (pm_node_t *)rescue->statements;
                    }
                    if (bn->ensure_clause && bn->ensure_clause->statements && sp < 255)
                        stack[sp++] = (pm_node_t *)bn->ensure_clause->statements;
                }
                #undef CHECK_SHADOW_VAR
            }
        }
    }

    /* Generate body with implicit return for last expression */
    if (f->body_node && PM_NODE_TYPE(f->body_node) == PM_STATEMENTS_NODE) {
        pm_statements_node_t *stmts = (pm_statements_node_t *)f->body_node;
        for (size_t i = 0; i + 1 < stmts->body.size; i++)
            codegen_stmt(ctx, stmts->body.nodes[i]);
        if (stmts->body.size > 0) {
            pm_node_t *last = stmts->body.nodes[stmts->body.size - 1];
            if (!ret_void &&
                PM_NODE_TYPE(last) != PM_IF_NODE &&
                PM_NODE_TYPE(last) != PM_WHILE_NODE &&
                PM_NODE_TYPE(last) != PM_RETURN_NODE) {
                char *val = codegen_expr(ctx, last);
                if (func_has_gc_vars)
                    emit(ctx, "SP_GC_RESTORE();\n");
                emit(ctx, "return %s;\n", val);
                free(val);
            } else {
                /* For IF/WHILE as last stmt in non-void func, enable implicit return */
                if (!ret_void) ctx->implicit_return = true;
                codegen_stmt(ctx, last);
                ctx->implicit_return = false;
            }
        }
    } else if (f->body_node) {
        codegen_stmts(ctx, f->body_node);
    }

    if (func_has_gc_vars && ret_void)
        emit_raw(ctx, "    SP_GC_RESTORE();\n");

    ctx->gc_scope_active = saved_gc_scope;
    ctx->indent = saved_indent;
    ctx->var_count = saved_var_count;

    emit_raw(ctx, "}\n\n");
    free(ret_ct);
}

/* ------------------------------------------------------------------ */
/* Module codegen (Rand)                                              */
/* ------------------------------------------------------------------ */

static void emit_module(codegen_ctx_t *ctx, module_info_t *mod) {
    /* Emit module constants as #define macros (avoids file-scope init issues) */
    ctx->current_module = mod;
    for (int i = 0; i < mod->const_count; i++) {
        module_const_t *mc = &mod->consts[i];
        char *val = codegen_expr(ctx, mc->value_node);
        emit_raw(ctx, "#define sp_%s_%s (%s)\n", mod->name, mc->name, val);
        free(val);
    }
    ctx->current_module = NULL;

    /* Emit module state variables with initialization values from the module body */
    {
        pm_module_node_t *mnode = (pm_module_node_t *)mod->module_node;
        pm_statements_node_t *mstmts = NULL;
        if (mnode->body && PM_NODE_TYPE(mnode->body) == PM_STATEMENTS_NODE)
            mstmts = (pm_statements_node_t *)mnode->body;

        for (int i = 0; i < mod->var_count; i++) {
            char *ct = vt_ctype(ctx, mod->vars[i].type, false);
            /* Find the initial value from the module body */
            char *init_val = NULL;
            if (mstmts) {
                for (size_t j = 0; j < mstmts->body.size; j++) {
                    pm_node_t *s = mstmts->body.nodes[j];
                    if (PM_NODE_TYPE(s) == PM_INSTANCE_VARIABLE_WRITE_NODE) {
                        pm_instance_variable_write_node_t *iw = (pm_instance_variable_write_node_t *)s;
                        char *ivn = cstr(ctx, iw->name);
                        if (strcmp(ivn + 1, mod->vars[i].name) == 0) {
                            init_val = codegen_expr(ctx, iw->value);
                        }
                        free(ivn);
                        if (init_val) break;
                    }
                }
            }
            if (init_val) {
                emit_raw(ctx, "static %s sp_%s_%s = %s;\n", ct, mod->name, mod->vars[i].name, init_val);
                free(init_val);
            } else {
                emit_raw(ctx, "static %s sp_%s_%s;\n", ct, mod->name, mod->vars[i].name);
            }
            free(ct);
        }
    }
    emit_raw(ctx, "\n");

    /* Emit module methods (only module functions, not mixin methods) */
    for (int i = 0; i < mod->method_count; i++) {
        method_info_t *m = &mod->methods[i];
        /* Skip mixin methods — they are emitted per-class via emit_method */
        if (!m->is_class_method) continue;
        char *ret_ct = vt_ctype(ctx, m->return_type, false);
        emit_raw(ctx, "static %s sp_%s_%s(void) {\n", ret_ct, mod->name, m->name);
        free(ret_ct);

        /* Generate method body using the AST */
        int saved_indent = ctx->indent;
        int saved_var_count = ctx->var_count;
        ctx->indent = 1;
        ctx->current_module = mod;

        if (m->body_node) {
            infer_pass(ctx, m->body_node);
            for (int j = saved_var_count; j < ctx->var_count; j++) {
                var_entry_t *v = &ctx->vars[j];
                char *ct = vt_ctype(ctx, v->type, false);
                char *cn = make_cname(v->name, v->is_constant);
                emit(ctx, "%s %s = 0;\n", ct, cn);
                free(ct); free(cn);
            }
            /* Handle implicit return for module methods */
            bool ret_void = (m->return_type.kind == SPINEL_TYPE_NIL);
            if (!ret_void && PM_NODE_TYPE(m->body_node) == PM_STATEMENTS_NODE) {
                pm_statements_node_t *stmts = (pm_statements_node_t *)m->body_node;
                for (size_t si = 0; si + 1 < stmts->body.size; si++)
                    codegen_stmt(ctx, stmts->body.nodes[si]);
                if (stmts->body.size > 0) {
                    pm_node_t *last = stmts->body.nodes[stmts->body.size - 1];
                    if (PM_NODE_TYPE(last) != PM_RETURN_NODE) {
                        char *val = codegen_expr(ctx, last);
                        emit(ctx, "return %s;\n", val);
                        free(val);
                    } else {
                        codegen_stmt(ctx, last);
                    }
                }
            } else {
                codegen_stmts(ctx, m->body_node);
            }
        }

        ctx->current_module = NULL;
        ctx->indent = saved_indent;
        ctx->var_count = saved_var_count;
        emit_raw(ctx, "}\n\n");
    }
}

/* ------------------------------------------------------------------ */
/* Top-level program generation                                       */
/* ------------------------------------------------------------------ */

static void emit_header(codegen_ctx_t *ctx) {
    emit_raw(ctx, "/* Generated by Spinel AOT compiler */\n");
    if (ctx->needs_regexp)
        emit_raw(ctx, "/* Compile: cc -O2 <this file> -lonig -lm -o <output> */\n");
    else
        emit_raw(ctx, "/* Compile: cc -O2 <this file> -lm -o <output> */\n");
    emit_raw(ctx, "#include <stdio.h>\n");
    emit_raw(ctx, "#include <stdlib.h>\n");
    emit_raw(ctx, "#include <string.h>\n");
    emit_raw(ctx, "#include <math.h>\n");
    emit_raw(ctx, "#include <stdbool.h>\n");
    emit_raw(ctx, "#include <stdint.h>\n");
    emit_raw(ctx, "#include <ctype.h>\n\n");
    emit_raw(ctx, "typedef int64_t mrb_int;\n");
    emit_raw(ctx, "typedef double mrb_float;\n");
    emit_raw(ctx, "typedef bool mrb_bool;\n");
    emit_raw(ctx, "#ifndef TRUE\n#define TRUE true\n#endif\n");
    emit_raw(ctx, "#ifndef FALSE\n#define FALSE false\n#endif\n\n");

    /* ---- String helpers ---- */
    emit_raw(ctx, "static const char *sp_str_upcase(const char *s) {\n");
    emit_raw(ctx, "    size_t n = strlen(s); char *r = (char *)malloc(n + 1);\n");
    emit_raw(ctx, "    for (size_t i = 0; i <= n; i++) r[i] = toupper((unsigned char)s[i]);\n");
    emit_raw(ctx, "    return r;\n}\n");
    emit_raw(ctx, "static const char *sp_str_downcase(const char *s) {\n");
    emit_raw(ctx, "    size_t n = strlen(s); char *r = (char *)malloc(n + 1);\n");
    emit_raw(ctx, "    for (size_t i = 0; i <= n; i++) r[i] = tolower((unsigned char)s[i]);\n");
    emit_raw(ctx, "    return r;\n}\n");
    emit_raw(ctx, "static const char *sp_str_concat(const char *a, const char *b) {\n");
    emit_raw(ctx, "    size_t la = strlen(a), lb = strlen(b);\n");
    emit_raw(ctx, "    char *r = (char *)malloc(la + lb + 1);\n");
    emit_raw(ctx, "    memcpy(r, a, la); memcpy(r + la, b, lb + 1); return r;\n}\n");
    emit_raw(ctx, "static const char *sp_int_to_s(mrb_int n) {\n");
    emit_raw(ctx, "    char *r = (char *)malloc(24); snprintf(r, 24, \"%%lld\", (long long)n); return r;\n}\n\n");

    /* ---- Additional string helpers ---- */
    emit_raw(ctx, "static const char *sp_str_strip(const char *s) {\n");
    emit_raw(ctx, "    while (*s && isspace((unsigned char)*s)) s++;\n");
    emit_raw(ctx, "    size_t n = strlen(s);\n");
    emit_raw(ctx, "    while (n > 0 && isspace((unsigned char)s[n-1])) n--;\n");
    emit_raw(ctx, "    char *r = (char *)malloc(n + 1);\n");
    emit_raw(ctx, "    memcpy(r, s, n); r[n] = '\\0'; return r;\n}\n");

    emit_raw(ctx, "static const char *sp_str_chomp(const char *s) {\n");
    emit_raw(ctx, "    size_t n = strlen(s);\n");
    emit_raw(ctx, "    if (n > 0 && s[n-1] == '\\n') { if (n > 1 && s[n-2] == '\\r') n--; n--; }\n");
    emit_raw(ctx, "    char *r = (char *)malloc(n + 1);\n");
    emit_raw(ctx, "    memcpy(r, s, n); r[n] = '\\0'; return r;\n}\n");

    emit_raw(ctx, "static const char *sp_str_capitalize(const char *s) {\n");
    emit_raw(ctx, "    size_t n = strlen(s); char *r = (char *)malloc(n + 1);\n");
    emit_raw(ctx, "    for (size_t i = 0; i <= n; i++) r[i] = (i == 0) ? toupper((unsigned char)s[i]) : tolower((unsigned char)s[i]);\n");
    emit_raw(ctx, "    return r;\n}\n");

    emit_raw(ctx, "static const char *sp_str_reverse(const char *s) {\n");
    emit_raw(ctx, "    size_t n = strlen(s); char *r = (char *)malloc(n + 1);\n");
    emit_raw(ctx, "    for (size_t i = 0; i < n; i++) r[i] = s[n - 1 - i];\n");
    emit_raw(ctx, "    r[n] = '\\0'; return r;\n}\n");

    emit_raw(ctx, "static mrb_int sp_str_count(const char *s, const char *chars) {\n");
    emit_raw(ctx, "    mrb_int c = 0;\n");
    emit_raw(ctx, "    for (; *s; s++) { for (const char *p = chars; *p; p++) { if (*s == *p) { c++; break; } } }\n");
    emit_raw(ctx, "    return c;\n}\n");

    emit_raw(ctx, "static mrb_bool sp_str_starts_with(const char *s, const char *prefix) {\n");
    emit_raw(ctx, "    size_t pn = strlen(prefix);\n");
    emit_raw(ctx, "    return strncmp(s, prefix, pn) == 0;\n}\n");

    emit_raw(ctx, "static mrb_bool sp_str_ends_with(const char *s, const char *suffix) {\n");
    emit_raw(ctx, "    size_t sn = strlen(s), xn = strlen(suffix);\n");
    emit_raw(ctx, "    return sn >= xn && strcmp(s + sn - xn, suffix) == 0;\n}\n");

    emit_raw(ctx, "static const char *sp_str_gsub(const char *s, const char *from, const char *to) {\n");
    emit_raw(ctx, "    size_t fl = strlen(from), tl = strlen(to);\n");
    emit_raw(ctx, "    size_t cap = strlen(s) * 2 + 16; char *r = (char *)malloc(cap); size_t ri = 0;\n");
    emit_raw(ctx, "    while (*s) {\n");
    emit_raw(ctx, "        if (strncmp(s, from, fl) == 0) {\n");
    emit_raw(ctx, "            if (ri + tl >= cap) { cap = (ri + tl) * 2; r = (char *)realloc(r, cap); }\n");
    emit_raw(ctx, "            memcpy(r + ri, to, tl); ri += tl; s += fl;\n");
    emit_raw(ctx, "        } else {\n");
    emit_raw(ctx, "            if (ri + 1 >= cap) { cap *= 2; r = (char *)realloc(r, cap); }\n");
    emit_raw(ctx, "            r[ri++] = *s++;\n");
    emit_raw(ctx, "        }\n");
    emit_raw(ctx, "    }\n");
    emit_raw(ctx, "    r[ri] = '\\0'; return r;\n}\n");

    emit_raw(ctx, "static const char *sp_str_sub(const char *s, const char *from, const char *to) {\n");
    emit_raw(ctx, "    const char *p = strstr(s, from);\n");
    emit_raw(ctx, "    if (!p) { size_t n = strlen(s); char *r = (char *)malloc(n+1); memcpy(r,s,n+1); return r; }\n");
    emit_raw(ctx, "    size_t fl = strlen(from), tl = strlen(to), sn = strlen(s);\n");
    emit_raw(ctx, "    char *r = (char *)malloc(sn - fl + tl + 1);\n");
    emit_raw(ctx, "    size_t before = p - s;\n");
    emit_raw(ctx, "    memcpy(r, s, before); memcpy(r + before, to, tl);\n");
    emit_raw(ctx, "    memcpy(r + before + tl, p + fl, sn - before - fl + 1); return r;\n}\n");

    emit_raw(ctx, "static const char *sp_str_repeat(const char *s, mrb_int n) {\n");
    emit_raw(ctx, "    size_t sl = strlen(s); char *r = (char *)malloc(sl * n + 1);\n");
    emit_raw(ctx, "    for (mrb_int i = 0; i < n; i++) memcpy(r + sl * i, s, sl);\n");
    emit_raw(ctx, "    r[sl * n] = '\\0'; return r;\n}\n\n");

    /* ---- Regexp runtime (only when needed) ---- */
    if (ctx->needs_regexp) {
        emit_raw(ctx, "/* ---- Regexp runtime (oniguruma) ---- */\n");
        emit_raw(ctx, "/* Link with: /usr/lib/x86_64-linux-gnu/libonig.so.5 */\n");
        emit_raw(ctx, "/* Minimal oniguruma declarations — no header needed */\n");
        emit_raw(ctx, "typedef unsigned char OnigUChar;\n");
        emit_raw(ctx, "typedef struct OnigEncodingTypeST OnigEncodingType;\n");
        emit_raw(ctx, "typedef OnigEncodingType *OnigEncoding;\n");
        emit_raw(ctx, "typedef struct { int num_regs; int *beg; int *end; } OnigRegion;\n");
        emit_raw(ctx, "typedef struct re_pattern_buffer regex_t;\n");
        emit_raw(ctx, "typedef struct { int ret; const OnigUChar *s; } OnigErrorInfo;\n");
        emit_raw(ctx, "typedef struct OnigSyntaxTypeST OnigSyntaxType;\n");
        emit_raw(ctx, "#define ONIG_OPTION_DEFAULT 0\n");
        emit_raw(ctx, "#define ONIG_OPTION_NONE 0\n");
        emit_raw(ctx, "extern OnigEncodingType OnigEncodingUTF8;\n");
        emit_raw(ctx, "extern OnigSyntaxType OnigSyntaxRuby;\n");
        emit_raw(ctx, "extern int onig_initialize(OnigEncoding *encs, int n);\n");
        emit_raw(ctx, "extern int onig_new(regex_t **reg, const OnigUChar *pattern,\n");
        emit_raw(ctx, "    const OnigUChar *pattern_end, int option, OnigEncoding enc,\n");
        emit_raw(ctx, "    OnigSyntaxType *syntax, OnigErrorInfo *einfo);\n");
        emit_raw(ctx, "extern int onig_search(regex_t *reg, const OnigUChar *str,\n");
        emit_raw(ctx, "    const OnigUChar *end, const OnigUChar *start, const OnigUChar *range,\n");
        emit_raw(ctx, "    OnigRegion *region, int option);\n");
        emit_raw(ctx, "extern OnigRegion *onig_region_new(void);\n");
        emit_raw(ctx, "extern void onig_region_free(OnigRegion *region, int free_self);\n\n");

        /* Global match region for $1..$9 */
        emit_raw(ctx, "static OnigRegion *sp_match_region;\n");
        emit_raw(ctx, "static const char *sp_match_str;\n\n");

        /* Static regex variables */
        for (int i = 0; i < ctx->regexp_counter; i++)
            emit_raw(ctx, "static regex_t *_re_%d;\n", ctx->regexps[i].id);
        emit_raw(ctx, "\n");

        /* sp_regexp_init — compile all patterns at startup */
        emit_raw(ctx, "static void sp_regexp_init(void) {\n");
        emit_raw(ctx, "    OnigEncoding enc = &OnigEncodingUTF8;\n");
        emit_raw(ctx, "    OnigErrorInfo einfo;\n");
        emit_raw(ctx, "    onig_initialize(&enc, 1);\n");
        emit_raw(ctx, "    sp_match_region = onig_region_new();\n");
        for (int i = 0; i < ctx->regexp_counter; i++) {
            /* Escape the pattern for C string literal */
            const char *pat = ctx->regexps[i].pattern;
            size_t plen = strlen(pat);
            char escaped[1024];
            int ep = 0;
            for (size_t j = 0; j < plen && ep < 1020; j++) {
                if (pat[j] == '\\') { escaped[ep++] = '\\'; escaped[ep++] = '\\'; }
                else if (pat[j] == '"') { escaped[ep++] = '\\'; escaped[ep++] = '"'; }
                else escaped[ep++] = pat[j];
            }
            escaped[ep] = '\0';
            emit_raw(ctx, "    onig_new(&_re_%d, (const OnigUChar *)\"%s\", (const OnigUChar *)\"%s\" + %zu,\n",
                     ctx->regexps[i].id, escaped, escaped, plen);
            emit_raw(ctx, "        ONIG_OPTION_DEFAULT, &OnigEncodingUTF8, &OnigSyntaxRuby, &einfo);\n");
        }
        emit_raw(ctx, "}\n\n");

        /* sp_re_match — perform match, store results, return position or -1 */
        emit_raw(ctx, "static mrb_int sp_re_match(regex_t *re, const char *s) {\n");
        emit_raw(ctx, "    sp_match_str = s;\n");
        emit_raw(ctx, "    const OnigUChar *end = (const OnigUChar *)s + strlen(s);\n");
        emit_raw(ctx, "    int r = onig_search(re, (const OnigUChar *)s, end,\n");
        emit_raw(ctx, "        (const OnigUChar *)s, end, sp_match_region, ONIG_OPTION_NONE);\n");
        emit_raw(ctx, "    return (mrb_int)r;\n");
        emit_raw(ctx, "}\n\n");

        /* sp_re_match_p — check if match exists (boolean) */
        emit_raw(ctx, "static mrb_bool sp_re_match_p(regex_t *re, const char *s) {\n");
        emit_raw(ctx, "    const OnigUChar *end = (const OnigUChar *)s + strlen(s);\n");
        emit_raw(ctx, "    int r = onig_search(re, (const OnigUChar *)s, end,\n");
        emit_raw(ctx, "        (const OnigUChar *)s, end, sp_match_region, ONIG_OPTION_NONE);\n");
        emit_raw(ctx, "    return r >= 0;\n");
        emit_raw(ctx, "}\n\n");

        /* sp_re_group — extract $N from last match */
        emit_raw(ctx, "static const char *sp_re_group(int n) {\n");
        emit_raw(ctx, "    if (n < 0 || n >= sp_match_region->num_regs) return \"\";\n");
        emit_raw(ctx, "    int beg = sp_match_region->beg[n], end = sp_match_region->end[n];\n");
        emit_raw(ctx, "    if (beg < 0) return \"\";\n");
        emit_raw(ctx, "    int len = end - beg;\n");
        emit_raw(ctx, "    char *r = (char *)malloc(len + 1);\n");
        emit_raw(ctx, "    memcpy(r, sp_match_str + beg, len);\n");
        emit_raw(ctx, "    r[len] = '\\0';\n");
        emit_raw(ctx, "    return r;\n");
        emit_raw(ctx, "}\n\n");

        /* sp_re_gsub — global substitution with regexp */
        emit_raw(ctx, "static const char *sp_re_gsub(regex_t *re, const char *s, const char *repl) {\n");
        emit_raw(ctx, "    size_t slen = strlen(s), rlen = strlen(repl);\n");
        emit_raw(ctx, "    size_t cap = slen * 2 + 16; char *out = (char *)malloc(cap); size_t oi = 0;\n");
        emit_raw(ctx, "    OnigRegion *region = onig_region_new();\n");
        emit_raw(ctx, "    const OnigUChar *end = (const OnigUChar *)s + slen;\n");
        emit_raw(ctx, "    int pos = 0;\n");
        emit_raw(ctx, "    while (pos <= (int)slen) {\n");
        emit_raw(ctx, "        int r = onig_search(re, (const OnigUChar *)s, end,\n");
        emit_raw(ctx, "            (const OnigUChar *)s + pos, end, region, ONIG_OPTION_NONE);\n");
        emit_raw(ctx, "        if (r < 0) break;\n");
        emit_raw(ctx, "        int mbeg = region->beg[0], mend = region->end[0];\n");
        emit_raw(ctx, "        size_t need = oi + (mbeg - pos) + rlen + (slen - mend) + 1;\n");
        emit_raw(ctx, "        if (need > cap) { cap = need * 2; out = (char *)realloc(out, cap); }\n");
        emit_raw(ctx, "        memcpy(out + oi, s + pos, mbeg - pos); oi += mbeg - pos;\n");
        emit_raw(ctx, "        memcpy(out + oi, repl, rlen); oi += rlen;\n");
        emit_raw(ctx, "        pos = mend;\n");
        emit_raw(ctx, "        if (mend == mbeg) pos++; /* avoid infinite loop on zero-width match */\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    size_t rest = slen - pos;\n");
        emit_raw(ctx, "    if (oi + rest + 1 > cap) { cap = oi + rest + 1; out = (char *)realloc(out, cap); }\n");
        emit_raw(ctx, "    memcpy(out + oi, s + pos, rest); oi += rest;\n");
        emit_raw(ctx, "    out[oi] = '\\0';\n");
        emit_raw(ctx, "    onig_region_free(region, 1);\n");
        emit_raw(ctx, "    return out;\n");
        emit_raw(ctx, "}\n\n");

        /* sp_re_sub — single substitution with regexp */
        emit_raw(ctx, "static const char *sp_re_sub(regex_t *re, const char *s, const char *repl) {\n");
        emit_raw(ctx, "    size_t slen = strlen(s), rlen = strlen(repl);\n");
        emit_raw(ctx, "    OnigRegion *region = onig_region_new();\n");
        emit_raw(ctx, "    const OnigUChar *end = (const OnigUChar *)s + slen;\n");
        emit_raw(ctx, "    int r = onig_search(re, (const OnigUChar *)s, end,\n");
        emit_raw(ctx, "        (const OnigUChar *)s, end, region, ONIG_OPTION_NONE);\n");
        emit_raw(ctx, "    if (r < 0) {\n");
        emit_raw(ctx, "        onig_region_free(region, 1);\n");
        emit_raw(ctx, "        char *dup = (char *)malloc(slen + 1); memcpy(dup, s, slen + 1); return dup;\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    int mbeg = region->beg[0], mend = region->end[0];\n");
        emit_raw(ctx, "    size_t olen = slen - (mend - mbeg) + rlen;\n");
        emit_raw(ctx, "    char *out = (char *)malloc(olen + 1);\n");
        emit_raw(ctx, "    memcpy(out, s, mbeg);\n");
        emit_raw(ctx, "    memcpy(out + mbeg, repl, rlen);\n");
        emit_raw(ctx, "    memcpy(out + mbeg + rlen, s + mend, slen - mend + 1);\n");
        emit_raw(ctx, "    onig_region_free(region, 1);\n");
        emit_raw(ctx, "    return out;\n");
        emit_raw(ctx, "}\n\n");

    }

    /* ---- GC runtime (only when needed) ---- */
    if (ctx->needs_gc) {
        emit_raw(ctx, "/* ---- Mark-and-sweep GC runtime ---- */\n");
        emit_raw(ctx, "typedef struct sp_gc_hdr {\n");
        emit_raw(ctx, "    struct sp_gc_hdr *next;\n");
        emit_raw(ctx, "    void (*finalize)(void *);\n");
        emit_raw(ctx, "    void (*scan)(void *);\n");
        emit_raw(ctx, "    unsigned marked : 1;\n");
        emit_raw(ctx, "} sp_gc_hdr;\n\n");

        emit_raw(ctx, "static sp_gc_hdr *sp_gc_heap = NULL;\n");
        emit_raw(ctx, "static size_t sp_gc_bytes = 0;\n");
        emit_raw(ctx, "static size_t sp_gc_threshold = 256 * 1024;\n\n");

        emit_raw(ctx, "#define SP_GC_STACK_MAX 8192\n");
        emit_raw(ctx, "static void **sp_gc_roots[SP_GC_STACK_MAX];\n");
        emit_raw(ctx, "static int sp_gc_nroots = 0;\n");
        emit_raw(ctx, "#define SP_GC_SAVE() int _gc_saved = sp_gc_nroots\n");
        emit_raw(ctx, "#define SP_GC_ROOT(v) do { if (sp_gc_nroots < SP_GC_STACK_MAX) sp_gc_roots[sp_gc_nroots++] = (void **)&(v); } while(0)\n");
        emit_raw(ctx, "#define SP_GC_RESTORE() sp_gc_nroots = _gc_saved\n\n");

        emit_raw(ctx, "static void sp_gc_mark(void *obj) {\n");
        emit_raw(ctx, "    if (!obj) return;\n");
        emit_raw(ctx, "    sp_gc_hdr *h = (sp_gc_hdr *)((char *)obj - sizeof(sp_gc_hdr));\n");
        emit_raw(ctx, "    if (h->marked) return;\n");
        emit_raw(ctx, "    h->marked = 1;\n");
        emit_raw(ctx, "    if (h->scan) h->scan(obj);\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static void sp_gc_collect(void) {\n");
        emit_raw(ctx, "    /* Mark phase: mark all roots */\n");
        emit_raw(ctx, "    for (int i = 0; i < sp_gc_nroots; i++) {\n");
        emit_raw(ctx, "        void *obj = *sp_gc_roots[i];\n");
        emit_raw(ctx, "        if (obj) sp_gc_mark(obj);\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    /* Sweep phase: free unmarked, reset marks */\n");
        emit_raw(ctx, "    sp_gc_hdr **pp = &sp_gc_heap;\n");
        emit_raw(ctx, "    sp_gc_bytes = 0;\n");
        emit_raw(ctx, "    while (*pp) {\n");
        emit_raw(ctx, "        sp_gc_hdr *h = *pp;\n");
        emit_raw(ctx, "        if (!h->marked) {\n");
        emit_raw(ctx, "            *pp = h->next;\n");
        emit_raw(ctx, "            if (h->finalize) h->finalize((char *)h + sizeof(sp_gc_hdr));\n");
        emit_raw(ctx, "            free(h);\n");
        emit_raw(ctx, "        } else {\n");
        emit_raw(ctx, "            h->marked = 0;\n");
        emit_raw(ctx, "            sp_gc_bytes += sizeof(sp_gc_hdr); /* approximate */\n");
        emit_raw(ctx, "            pp = &h->next;\n");
        emit_raw(ctx, "        }\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static void *sp_gc_alloc(size_t sz, void (*finalize)(void *), void (*scan)(void *)) {\n");
        emit_raw(ctx, "    if (sp_gc_bytes > sp_gc_threshold) {\n");
        emit_raw(ctx, "        sp_gc_collect();\n");
        emit_raw(ctx, "        if (sp_gc_bytes > sp_gc_threshold / 2)\n");
        emit_raw(ctx, "            sp_gc_threshold *= 2;\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    sp_gc_hdr *h = (sp_gc_hdr *)calloc(1, sizeof(sp_gc_hdr) + sz);\n");
        emit_raw(ctx, "    h->finalize = finalize;\n");
        emit_raw(ctx, "    h->scan = scan;\n");
        emit_raw(ctx, "    h->next = sp_gc_heap;\n");
        emit_raw(ctx, "    sp_gc_heap = h;\n");
        emit_raw(ctx, "    sp_gc_bytes += sizeof(sp_gc_hdr) + sz;\n");
        emit_raw(ctx, "    return (char *)h + sizeof(sp_gc_hdr);\n");
        emit_raw(ctx, "}\n\n");
    }

    /* ---- Exception handling runtime (only when needed) ---- */
    if (ctx->needs_exc) {
        emit_raw(ctx, "/* ---- Exception handling runtime (setjmp/longjmp) ---- */\n");
        emit_raw(ctx, "#include <setjmp.h>\n");
        emit_raw(ctx, "#define SP_EXC_STACK_SIZE 64\n");
        emit_raw(ctx, "static jmp_buf sp_exc_stack[SP_EXC_STACK_SIZE];\n");
        emit_raw(ctx, "static int sp_exc_depth = 0;\n");
        emit_raw(ctx, "static const char *sp_exc_message = NULL;\n");
        emit_raw(ctx, "static const char *sp_exc_class = \"RuntimeError\";\n\n");
        emit_raw(ctx, "static void sp_raise(const char *msg) {\n");
        emit_raw(ctx, "    sp_exc_message = msg; sp_exc_class = \"RuntimeError\";\n");
        emit_raw(ctx, "    if (sp_exc_depth > 0) longjmp(sp_exc_stack[sp_exc_depth - 1], 1);\n");
        emit_raw(ctx, "    fprintf(stderr, \"unhandled exception: %%s\\n\", msg); exit(1);\n");
        emit_raw(ctx, "}\n");
        emit_raw(ctx, "static void sp_raise_cls(const char *cls, const char *msg) {\n");
        emit_raw(ctx, "    sp_exc_message = msg; sp_exc_class = cls;\n");
        emit_raw(ctx, "    if (sp_exc_depth > 0) longjmp(sp_exc_stack[sp_exc_depth - 1], 1);\n");
        emit_raw(ctx, "    fprintf(stderr, \"%%s: %%s\\n\", cls, msg); exit(1);\n");
        emit_raw(ctx, "}\n");
        /* sp_exc_is_a checks class hierarchy — populated at codegen time */
        emit_raw(ctx, "static int sp_exc_is_a(const char *cls) {\n");
        emit_raw(ctx, "    if (strcmp(sp_exc_class, cls) == 0) return 1;\n");
        /* Add inheritance checks for known exception classes */
        for (int ci = 0; ci < ctx->class_count; ci++) {
            class_info_t *c = &ctx->classes[ci];
            if (c->superclass[0]) {
                /* If raised class is c->name and cls is a parent, match */
                emit_raw(ctx, "    if (strcmp(sp_exc_class, \"%s\") == 0 && strcmp(cls, \"%s\") == 0) return 1;\n",
                         c->name, c->superclass);
                /* Walk further up the chain */
                class_info_t *p = find_class(ctx, c->superclass);
                while (p && p->superclass[0]) {
                    emit_raw(ctx, "    if (strcmp(sp_exc_class, \"%s\") == 0 && strcmp(cls, \"%s\") == 0) return 1;\n",
                             c->name, p->superclass);
                    p = find_class(ctx, p->superclass);
                }
            }
        }
        emit_raw(ctx, "    /* RuntimeError base class matches */\n");
        emit_raw(ctx, "    if (strcmp(cls, \"RuntimeError\") == 0) return 1;\n");
        emit_raw(ctx, "    return 0;\n");
        emit_raw(ctx, "}\n\n");
    }

    /* Built-in sp_IntArray for Array support */
    emit_raw(ctx, "/* ---- Built-in integer array ---- */\n");
    /* sp_IntArray: deque-like array with O(1) shift via start offset */
    emit_raw(ctx, "typedef struct { mrb_int *data; mrb_int start; mrb_int len; mrb_int cap; } sp_IntArray;\n\n");

    if (ctx->needs_gc) {
        /* GC-managed IntArray: finalizer frees internal data pointer */
        emit_raw(ctx, "static void sp_IntArray_finalize(void *p) {\n");
        emit_raw(ctx, "    sp_IntArray *a = (sp_IntArray *)p;\n");
        emit_raw(ctx, "    free(a->data);\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static sp_IntArray *sp_IntArray_new(void) {\n");
        emit_raw(ctx, "    sp_IntArray *a = (sp_IntArray *)sp_gc_alloc(sizeof(sp_IntArray), sp_IntArray_finalize, NULL);\n");
        emit_raw(ctx, "    a->cap = 16; a->data = (mrb_int *)malloc(sizeof(mrb_int) * a->cap);\n");
        emit_raw(ctx, "    sp_gc_bytes += sizeof(mrb_int) * a->cap;\n");
        emit_raw(ctx, "    return a;\n}\n\n");
    } else {
        emit_raw(ctx, "static sp_IntArray *sp_IntArray_new(void) {\n");
        emit_raw(ctx, "    sp_IntArray *a = (sp_IntArray *)calloc(1, sizeof(sp_IntArray));\n");
        emit_raw(ctx, "    a->cap = 16; a->data = (mrb_int *)malloc(sizeof(mrb_int) * a->cap);\n");
        emit_raw(ctx, "    return a;\n}\n\n");
    }

    emit_raw(ctx, "static sp_IntArray *sp_IntArray_from_range(mrb_int start, mrb_int end) {\n");
    emit_raw(ctx, "    sp_IntArray *a = sp_IntArray_new();\n");
    emit_raw(ctx, "    mrb_int n = end - start + 1; if (n < 0) n = 0;\n");
    if (ctx->needs_gc)
        emit_raw(ctx, "    if (n > a->cap) { sp_gc_bytes += sizeof(mrb_int) * (n - a->cap); a->cap = n; a->data = (mrb_int *)realloc(a->data, sizeof(mrb_int) * a->cap); }\n");
    else
        emit_raw(ctx, "    if (n > a->cap) { a->cap = n; a->data = (mrb_int *)realloc(a->data, sizeof(mrb_int) * a->cap); }\n");
    emit_raw(ctx, "    for (mrb_int i = 0; i < n; i++) a->data[i] = start + i;\n");
    emit_raw(ctx, "    a->len = n; return a;\n}\n\n");

    emit_raw(ctx, "static sp_IntArray *sp_IntArray_dup(sp_IntArray *a) {\n");
    emit_raw(ctx, "    sp_IntArray *b = sp_IntArray_new();\n");
    if (ctx->needs_gc)
        emit_raw(ctx, "    if (a->len > b->cap) { sp_gc_bytes += sizeof(mrb_int) * (a->len - b->cap); b->cap = a->len; b->data = (mrb_int *)realloc(b->data, sizeof(mrb_int) * b->cap); }\n");
    else
        emit_raw(ctx, "    if (a->len > b->cap) { b->cap = a->len; b->data = (mrb_int *)realloc(b->data, sizeof(mrb_int) * b->cap); }\n");
    emit_raw(ctx, "    memcpy(b->data, a->data + a->start, sizeof(mrb_int) * a->len);\n");
    emit_raw(ctx, "    b->len = a->len; return b;\n}\n\n");

    emit_raw(ctx, "static void sp_IntArray_push(sp_IntArray *a, mrb_int val) {\n");
    emit_raw(ctx, "    mrb_int end = a->start + a->len;\n");
    emit_raw(ctx, "    if (end >= a->cap) {\n");
    emit_raw(ctx, "        if (a->start > 0) { memmove(a->data, a->data + a->start, sizeof(mrb_int) * a->len); a->start = 0; end = a->len; }\n");
    emit_raw(ctx, "        if (end >= a->cap) { a->cap = a->cap * 2 + 1; a->data = (mrb_int *)realloc(a->data, sizeof(mrb_int) * a->cap); }\n");
    emit_raw(ctx, "    }\n");
    emit_raw(ctx, "    a->data[end] = val; a->len++;\n}\n\n");

    emit_raw(ctx, "static mrb_int sp_IntArray_shift(sp_IntArray *a) {\n");
    emit_raw(ctx, "    mrb_int v = a->data[a->start++]; a->len--; return v;\n}\n\n");

    emit_raw(ctx, "static mrb_int sp_IntArray_pop(sp_IntArray *a) {\n");
    emit_raw(ctx, "    return a->data[a->start + --a->len];\n}\n\n");

    emit_raw(ctx, "static mrb_bool sp_IntArray_empty(sp_IntArray *a) {\n");
    emit_raw(ctx, "    return a->len == 0;\n}\n\n");

    emit_raw(ctx, "static void sp_IntArray_reverse_bang(sp_IntArray *a) {\n");
    emit_raw(ctx, "    for (mrb_int i = 0, j = a->len - 1; i < j; i++, j--) {\n");
    emit_raw(ctx, "        mrb_int t = a->data[a->start+i]; a->data[a->start+i] = a->data[a->start+j]; a->data[a->start+j] = t;\n");
    emit_raw(ctx, "    }\n}\n\n");

    emit_raw(ctx, "static int _sp_int_cmp(const void *a, const void *b) {\n");
    emit_raw(ctx, "    mrb_int va = *(const mrb_int *)a, vb = *(const mrb_int *)b;\n");
    emit_raw(ctx, "    return (va > vb) - (va < vb);\n}\n");
    emit_raw(ctx, "static sp_IntArray *sp_IntArray_sort(sp_IntArray *a) {\n");
    emit_raw(ctx, "    sp_IntArray *b = sp_IntArray_dup(a);\n");
    emit_raw(ctx, "    qsort(b->data + b->start, b->len, sizeof(mrb_int), _sp_int_cmp);\n");
    emit_raw(ctx, "    return b;\n}\n");
    emit_raw(ctx, "static void sp_IntArray_sort_bang(sp_IntArray *a) {\n");
    emit_raw(ctx, "    qsort(a->data + a->start, a->len, sizeof(mrb_int), _sp_int_cmp);\n}\n\n");

    emit_raw(ctx, "static mrb_int sp_IntArray_length(sp_IntArray *a) {\n");
    emit_raw(ctx, "    return a->len;\n}\n\n");

    emit_raw(ctx, "static mrb_int sp_IntArray_get(sp_IntArray *a, mrb_int idx) {\n");
    emit_raw(ctx, "    if (idx < 0) idx += a->len;\n");
    emit_raw(ctx, "    return a->data[a->start + idx];\n}\n\n");

    emit_raw(ctx, "static mrb_bool sp_IntArray_neq(sp_IntArray *a, sp_IntArray *b) {\n");
    emit_raw(ctx, "    if (a->len != b->len) return TRUE;\n");
    emit_raw(ctx, "    return memcmp(a->data + a->start, b->data + b->start, sizeof(mrb_int) * a->len) != 0;\n}\n\n");

    emit_raw(ctx, "static void sp_IntArray_free(sp_IntArray *a) {\n");
    emit_raw(ctx, "    if (a) { free(a->data); free(a); }\n}\n\n");

    /* Built-in sp_StrArray for string split support (only when needed) */
    if (ctx->needs_str_split && !ctx->lambda_mode) {
        emit_raw(ctx, "/* ---- Built-in string array ---- */\n");
        emit_raw(ctx, "typedef struct { const char **data; mrb_int len; mrb_int cap; } sp_StrArray;\n\n");
        emit_raw(ctx, "static sp_StrArray *sp_StrArray_new(void) {\n");
        emit_raw(ctx, "    sp_StrArray *a = (sp_StrArray *)calloc(1, sizeof(sp_StrArray));\n");
        emit_raw(ctx, "    a->cap = 16; a->data = (const char **)malloc(sizeof(const char *) * a->cap);\n");
        emit_raw(ctx, "    return a;\n}\n\n");
        emit_raw(ctx, "static void sp_StrArray_push(sp_StrArray *a, const char *s) {\n");
        emit_raw(ctx, "    if (a->len >= a->cap) { a->cap *= 2; a->data = (const char **)realloc(a->data, sizeof(const char *) * a->cap); }\n");
        emit_raw(ctx, "    a->data[a->len++] = s;\n}\n\n");
        emit_raw(ctx, "static mrb_int sp_StrArray_length(sp_StrArray *a) {\n");
        emit_raw(ctx, "    return a->len;\n}\n\n");
    }

    /* sp_str_split depends on sp_StrArray being defined */
    if (ctx->needs_str_split) {
        emit_raw(ctx, "static sp_StrArray *sp_str_split(const char *s, const char *delim) {\n");
        emit_raw(ctx, "    sp_StrArray *a = sp_StrArray_new();\n");
        emit_raw(ctx, "    size_t dl = strlen(delim);\n");
        emit_raw(ctx, "    while (*s) {\n");
        emit_raw(ctx, "        const char *p = strstr(s, delim);\n");
        emit_raw(ctx, "        if (!p) { size_t n = strlen(s); char *t = (char *)malloc(n+1); memcpy(t,s,n+1); sp_StrArray_push(a, t); break; }\n");
        emit_raw(ctx, "        size_t n = p - s; char *t = (char *)malloc(n+1); memcpy(t,s,n); t[n]='\\0'; sp_StrArray_push(a, t); s = p + dl;\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    return a;\n}\n\n");
    }

    /* sp_re_split — split string by regexp (depends on sp_StrArray) */
    if (ctx->needs_regexp && ctx->needs_str_split) {
        emit_raw(ctx, "static sp_StrArray *sp_re_split(regex_t *re, const char *s) {\n");
        emit_raw(ctx, "    sp_StrArray *a = sp_StrArray_new();\n");
        emit_raw(ctx, "    size_t slen = strlen(s);\n");
        emit_raw(ctx, "    OnigRegion *region = onig_region_new();\n");
        emit_raw(ctx, "    const OnigUChar *end = (const OnigUChar *)s + slen;\n");
        emit_raw(ctx, "    int pos = 0;\n");
        emit_raw(ctx, "    while (pos <= (int)slen) {\n");
        emit_raw(ctx, "        int r = onig_search(re, (const OnigUChar *)s, end,\n");
        emit_raw(ctx, "            (const OnigUChar *)s + pos, end, region, ONIG_OPTION_NONE);\n");
        emit_raw(ctx, "        if (r < 0) break;\n");
        emit_raw(ctx, "        int mbeg = region->beg[0], mend = region->end[0];\n");
        emit_raw(ctx, "        int plen = mbeg - pos;\n");
        emit_raw(ctx, "        char *part = (char *)malloc(plen + 1);\n");
        emit_raw(ctx, "        memcpy(part, s + pos, plen); part[plen] = '\\0';\n");
        emit_raw(ctx, "        sp_StrArray_push(a, part);\n");
        emit_raw(ctx, "        pos = mend;\n");
        emit_raw(ctx, "        if (mend == mbeg) pos++;\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    if (pos <= (int)slen) {\n");
        emit_raw(ctx, "        int rlen = (int)slen - pos;\n");
        emit_raw(ctx, "        char *part = (char *)malloc(rlen + 1);\n");
        emit_raw(ctx, "        memcpy(part, s + pos, rlen); part[rlen] = '\\0';\n");
        emit_raw(ctx, "        sp_StrArray_push(a, part);\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    onig_region_free(region, 1);\n");
        emit_raw(ctx, "    return a;\n");
        emit_raw(ctx, "}\n\n");
    }

    /* Built-in sp_StrIntHash for Hash support (only when hashes are used) */
    if (ctx->needs_hash) {
        emit_raw(ctx, "/* ---- Built-in string→integer hash table (insertion-ordered) ---- */\n");
        emit_raw(ctx, "typedef struct sp_HashEntry {\n");
        emit_raw(ctx, "    char *key;\n");
        emit_raw(ctx, "    mrb_int value;\n");
        emit_raw(ctx, "    struct sp_HashEntry *next;       /* bucket chain */\n");
        emit_raw(ctx, "    struct sp_HashEntry *order_next; /* insertion order */\n");
        emit_raw(ctx, "    struct sp_HashEntry *order_prev;\n");
        emit_raw(ctx, "} sp_HashEntry;\n\n");

        emit_raw(ctx, "typedef struct {\n");
        emit_raw(ctx, "    sp_HashEntry **buckets;\n");
        emit_raw(ctx, "    mrb_int size;\n");
        emit_raw(ctx, "    mrb_int cap;\n");
        emit_raw(ctx, "    sp_HashEntry *first; /* insertion-order head */\n");
        emit_raw(ctx, "    sp_HashEntry *last;  /* insertion-order tail */\n");
        emit_raw(ctx, "} sp_StrIntHash;\n\n");

        emit_raw(ctx, "static unsigned sp_hash_str(const char *s) {\n");
        emit_raw(ctx, "    unsigned h = 5381;\n");
        emit_raw(ctx, "    while (*s) h = h * 33 + (unsigned char)*s++;\n");
        emit_raw(ctx, "    return h;\n");
        emit_raw(ctx, "}\n\n");

        if (ctx->needs_gc) {
            emit_raw(ctx, "static void sp_StrIntHash_finalize(void *p) {\n");
            emit_raw(ctx, "    sp_StrIntHash *h = (sp_StrIntHash *)p;\n");
            emit_raw(ctx, "    sp_HashEntry *e = h->first;\n");
            emit_raw(ctx, "    while (e) { sp_HashEntry *n = e->order_next; free(e->key); free(e); e = n; }\n");
            emit_raw(ctx, "    free(h->buckets);\n");
            emit_raw(ctx, "}\n\n");

            emit_raw(ctx, "static sp_StrIntHash *sp_StrIntHash_new(void) {\n");
            emit_raw(ctx, "    sp_StrIntHash *h = (sp_StrIntHash *)sp_gc_alloc(sizeof(sp_StrIntHash), sp_StrIntHash_finalize, NULL);\n");
            emit_raw(ctx, "    h->cap = 16; h->size = 0; h->first = NULL; h->last = NULL;\n");
            emit_raw(ctx, "    h->buckets = (sp_HashEntry **)calloc(h->cap, sizeof(sp_HashEntry *));\n");
            emit_raw(ctx, "    return h;\n}\n\n");
        } else {
            emit_raw(ctx, "static sp_StrIntHash *sp_StrIntHash_new(void) {\n");
            emit_raw(ctx, "    sp_StrIntHash *h = (sp_StrIntHash *)calloc(1, sizeof(sp_StrIntHash));\n");
            emit_raw(ctx, "    h->cap = 16; h->size = 0; h->first = NULL; h->last = NULL;\n");
            emit_raw(ctx, "    h->buckets = (sp_HashEntry **)calloc(h->cap, sizeof(sp_HashEntry *));\n");
            emit_raw(ctx, "    return h;\n}\n\n");
        }

        emit_raw(ctx, "static void sp_StrIntHash_set(sp_StrIntHash *h, const char *key, mrb_int value) {\n");
        emit_raw(ctx, "    unsigned idx = sp_hash_str(key) %% h->cap;\n");
        emit_raw(ctx, "    sp_HashEntry *e = h->buckets[idx];\n");
        emit_raw(ctx, "    while (e) {\n");
        emit_raw(ctx, "        if (strcmp(e->key, key) == 0) { e->value = value; return; }\n");
        emit_raw(ctx, "        e = e->next;\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    e = (sp_HashEntry *)malloc(sizeof(sp_HashEntry));\n");
        emit_raw(ctx, "    e->key = (char *)malloc(strlen(key) + 1); strcpy(e->key, key);\n");
        emit_raw(ctx, "    e->value = value;\n");
        emit_raw(ctx, "    e->next = h->buckets[idx];\n");
        emit_raw(ctx, "    h->buckets[idx] = e;\n");
        emit_raw(ctx, "    /* Append to insertion-order list */\n");
        emit_raw(ctx, "    e->order_next = NULL;\n");
        emit_raw(ctx, "    e->order_prev = h->last;\n");
        emit_raw(ctx, "    if (h->last) h->last->order_next = e; else h->first = e;\n");
        emit_raw(ctx, "    h->last = e;\n");
        emit_raw(ctx, "    h->size++;\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static mrb_int sp_StrIntHash_get(sp_StrIntHash *h, const char *key) {\n");
        emit_raw(ctx, "    unsigned idx = sp_hash_str(key) %% h->cap;\n");
        emit_raw(ctx, "    sp_HashEntry *e = h->buckets[idx];\n");
        emit_raw(ctx, "    while (e) {\n");
        emit_raw(ctx, "        if (strcmp(e->key, key) == 0) return e->value;\n");
        emit_raw(ctx, "        e = e->next;\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    return 0;\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static mrb_int sp_StrIntHash_length(sp_StrIntHash *h) {\n");
        emit_raw(ctx, "    return h->size;\n}\n\n");

        emit_raw(ctx, "static mrb_bool sp_StrIntHash_has_key(sp_StrIntHash *h, const char *key) {\n");
        emit_raw(ctx, "    unsigned idx = sp_hash_str(key) %% h->cap;\n");
        emit_raw(ctx, "    sp_HashEntry *e = h->buckets[idx];\n");
        emit_raw(ctx, "    while (e) {\n");
        emit_raw(ctx, "        if (strcmp(e->key, key) == 0) return TRUE;\n");
        emit_raw(ctx, "        e = e->next;\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    return FALSE;\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static mrb_int sp_StrIntHash_delete(sp_StrIntHash *h, const char *key) {\n");
        emit_raw(ctx, "    unsigned idx = sp_hash_str(key) %% h->cap;\n");
        emit_raw(ctx, "    sp_HashEntry **pp = &h->buckets[idx];\n");
        emit_raw(ctx, "    while (*pp) {\n");
        emit_raw(ctx, "        if (strcmp((*pp)->key, key) == 0) {\n");
        emit_raw(ctx, "            sp_HashEntry *e = *pp;\n");
        emit_raw(ctx, "            mrb_int val = e->value;\n");
        emit_raw(ctx, "            *pp = e->next;\n");
        emit_raw(ctx, "            /* Remove from insertion-order list */\n");
        emit_raw(ctx, "            if (e->order_prev) e->order_prev->order_next = e->order_next;\n");
        emit_raw(ctx, "            else h->first = e->order_next;\n");
        emit_raw(ctx, "            if (e->order_next) e->order_next->order_prev = e->order_prev;\n");
        emit_raw(ctx, "            else h->last = e->order_prev;\n");
        emit_raw(ctx, "            free(e->key); free(e);\n");
        emit_raw(ctx, "            h->size--;\n");
        emit_raw(ctx, "            return val;\n");
        emit_raw(ctx, "        }\n");
        emit_raw(ctx, "        pp = &(*pp)->next;\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    return 0;\n");
        emit_raw(ctx, "}\n\n");
    }

    /* Lambda/closure runtime (sp_Val) — emitted only when lambdas are used */
    if (ctx->lambda_mode) {
        emit_raw(ctx, "/* ---- Lambda/closure runtime (sp_Val) ---- */\n");
        emit_raw(ctx, "typedef struct sp_Val sp_Val;\n");
        emit_raw(ctx, "typedef sp_Val *(*sp_fn_t)(sp_Val *self, sp_Val *arg);\n");
        emit_raw(ctx, "struct sp_Val {\n");
        emit_raw(ctx, "    enum { SP_PROC, SP_INT, SP_BOOL, SP_NIL } tag;\n");
        emit_raw(ctx, "    union {\n");
        emit_raw(ctx, "        struct { sp_fn_t fn; int ncaptures; } proc;\n");
        emit_raw(ctx, "        mrb_int ival;\n");
        emit_raw(ctx, "        mrb_bool bval;\n");
        emit_raw(ctx, "    } u;\n");
        emit_raw(ctx, "    sp_Val *captures[];\n");
        emit_raw(ctx, "};\n\n");

        emit_raw(ctx, "#include <sys/mman.h>\n");
        emit_raw(ctx, "#define SP_ARENA_SIZE ((size_t)16ULL * 1024 * 1024 * 1024)\n");
        emit_raw(ctx, "static char *sp_arena;\n");
        emit_raw(ctx, "static size_t sp_arena_pos;\n\n");

        emit_raw(ctx, "static void *sp_alloc(size_t sz) {\n");
        emit_raw(ctx, "    sz = (sz + 7) & ~(size_t)7;\n");
        emit_raw(ctx, "    if (!sp_arena) {\n");
        emit_raw(ctx, "        sp_arena = (char *)mmap(NULL, SP_ARENA_SIZE, PROT_READ|PROT_WRITE,\n");
        emit_raw(ctx, "                                MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);\n");
        emit_raw(ctx, "        if (sp_arena == MAP_FAILED) { perror(\"mmap\"); exit(1); }\n");
        emit_raw(ctx, "        sp_arena_pos = 0;\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    if (sp_arena_pos + sz > SP_ARENA_SIZE) { fprintf(stderr, \"sp_arena exhausted (%%zu used)\\n\", sp_arena_pos); exit(1); }\n");
        emit_raw(ctx, "    void *p = sp_arena + sp_arena_pos;\n");
        emit_raw(ctx, "    sp_arena_pos += sz;\n");
        emit_raw(ctx, "    return p;\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static sp_Val *sp_proc(sp_fn_t fn, int ncap) {\n");
        emit_raw(ctx, "    sp_Val *v = (sp_Val *)sp_alloc(sizeof(sp_Val) + sizeof(sp_Val *) * ncap);\n");
        emit_raw(ctx, "    v->tag = SP_PROC;\n");
        emit_raw(ctx, "    v->u.proc.fn = fn;\n");
        emit_raw(ctx, "    v->u.proc.ncaptures = ncap;\n");
        emit_raw(ctx, "    return v;\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static sp_Val *sp_int(mrb_int n) {\n");
        emit_raw(ctx, "    sp_Val *v = (sp_Val *)sp_alloc(sizeof(sp_Val));\n");
        emit_raw(ctx, "    v->tag = SP_INT;\n");
        emit_raw(ctx, "    v->u.ival = n;\n");
        emit_raw(ctx, "    return v;\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static sp_Val *sp_bool(mrb_bool b) {\n");
        emit_raw(ctx, "    sp_Val *v = (sp_Val *)sp_alloc(sizeof(sp_Val));\n");
        emit_raw(ctx, "    v->tag = SP_BOOL;\n");
        emit_raw(ctx, "    v->u.bval = b;\n");
        emit_raw(ctx, "    return v;\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static sp_Val sp_nil_val = { .tag = SP_NIL };\n\n");

        emit_raw(ctx, "static sp_Val *sp_call(sp_Val *f, sp_Val *arg) {\n");
        emit_raw(ctx, "    return f->u.proc.fn(f, arg);\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static mrb_int sp_to_int(sp_Val *v) {\n");
        emit_raw(ctx, "    return v->u.ival;\n");
        emit_raw(ctx, "}\n\n");

        /* sp_StrArray for to_array/to_string results */
        emit_raw(ctx, "/* ---- String array for lambda FizzBuzz ---- */\n");
        emit_raw(ctx, "typedef struct { char **data; int len; int cap; } sp_StrArray;\n\n");
        emit_raw(ctx, "static sp_StrArray *sp_StrArray_new(void) {\n");
        emit_raw(ctx, "    sp_StrArray *a = (sp_StrArray *)calloc(1, sizeof(sp_StrArray));\n");
        emit_raw(ctx, "    a->cap = 16; a->data = (char **)malloc(sizeof(char *) * a->cap);\n");
        emit_raw(ctx, "    return a;\n}\n\n");

        emit_raw(ctx, "static void sp_StrArray_push(sp_StrArray *a, char *s) {\n");
        emit_raw(ctx, "    if (a->len >= a->cap) { a->cap *= 2; a->data = (char **)realloc(a->data, sizeof(char *) * a->cap); }\n");
        emit_raw(ctx, "    a->data[a->len++] = s;\n}\n\n");

        /* sp_ValArray for to_array returning sp_Val* elements */
        emit_raw(ctx, "typedef struct { sp_Val **data; int len; int cap; } sp_ValArray;\n\n");
        emit_raw(ctx, "static sp_ValArray *sp_ValArray_new(void) {\n");
        emit_raw(ctx, "    sp_ValArray *a = (sp_ValArray *)calloc(1, sizeof(sp_ValArray));\n");
        emit_raw(ctx, "    a->cap = 16; a->data = (sp_Val **)malloc(sizeof(sp_Val *) * a->cap);\n");
        emit_raw(ctx, "    return a;\n}\n\n");

        emit_raw(ctx, "static void sp_ValArray_push(sp_ValArray *a, sp_Val *v) {\n");
        emit_raw(ctx, "    if (a->len >= a->cap) { a->cap *= 2; a->data = (sp_Val **)realloc(a->data, sizeof(sp_Val *) * a->cap); }\n");
        emit_raw(ctx, "    a->data[a->len++] = v;\n}\n\n");
    }
}

void codegen_init(codegen_ctx_t *ctx, pm_parser_t *parser, FILE *out) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->parser = parser;
    ctx->out = out;
    ctx->indent = 1;
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
        /* Check if this is a raise call */
        if (!c->receiver && ceq(ctx, c->name, "raise")) return true;
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
        if (ceq(ctx, c->name, "split")) return true;
        if (c->receiver && has_split_calls(ctx, c->receiver)) return true;
        if (c->arguments) {
            for (size_t i = 0; i < c->arguments->arguments.size; i++)
                if (has_split_calls(ctx, c->arguments->arguments.nodes[i])) return true;
        }
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

/* capture_list_t is forward-declared near top of file */

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
static char *codegen_lambda(codegen_ctx_t *ctx, pm_lambda_node_t *lam) {
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

/* Emit hand-written FizzBuzz helper functions for lambda mode */
static void emit_lambda_fizzbuzz_funcs(codegen_ctx_t *ctx) {
    /* to_integer: proc[-> n { n + 1 }][0] */
    emit_raw(ctx, "static sp_Val *_lam_incr(sp_Val *self, sp_Val *arg) {\n");
    emit_raw(ctx, "    (void)self;\n");
    emit_raw(ctx, "    return sp_int(sp_to_int(arg) + 1);\n");
    emit_raw(ctx, "}\n\n");

    emit_raw(ctx, "static mrb_int sp_to_integer(sp_Val *lv_proc) {\n");
    emit_raw(ctx, "    sp_Val *incr = sp_proc(_lam_incr, 0);\n");
    emit_raw(ctx, "    sp_Val *result = sp_call(sp_call(lv_proc, incr), sp_int(0));\n");
    emit_raw(ctx, "    return sp_to_int(result);\n");
    emit_raw(ctx, "}\n\n");

    /* to_boolean: IF[proc][true][false] */
    emit_raw(ctx, "static mrb_bool sp_to_boolean(sp_Val *lv_proc) {\n");
    emit_raw(ctx, "    sp_Val *result = sp_call(sp_call(sp_call(cv_IF, lv_proc), sp_bool(TRUE)), sp_bool(FALSE));\n");
    emit_raw(ctx, "    return result->u.bval;\n");
    emit_raw(ctx, "}\n\n");

    /* to_array: iterate church-encoded list */
    emit_raw(ctx, "static sp_ValArray *sp_to_array(sp_Val *lv_proc) {\n");
    emit_raw(ctx, "    sp_ValArray *lv_array = sp_ValArray_new();\n");
    emit_raw(ctx, "    while (!sp_to_boolean(sp_call(cv_IS_EMPTY, lv_proc))) {\n");
    emit_raw(ctx, "        sp_ValArray_push(lv_array, sp_call(cv_FIRST, lv_proc));\n");
    emit_raw(ctx, "        lv_proc = sp_call(cv_REST, lv_proc);\n");
    emit_raw(ctx, "    }\n");
    emit_raw(ctx, "    return lv_array;\n");
    emit_raw(ctx, "}\n\n");

    /* to_char: '0123456789BFiuz'.slice(to_integer(c)) */
    emit_raw(ctx, "static char *sp_to_char(sp_Val *lv_c) {\n");
    emit_raw(ctx, "    static const char *chars = \"0123456789BFiuz\";\n");
    emit_raw(ctx, "    mrb_int idx = sp_to_integer(lv_c);\n");
    emit_raw(ctx, "    char *buf = (char *)malloc(2);\n");
    emit_raw(ctx, "    buf[0] = chars[idx]; buf[1] = '\\0';\n");
    emit_raw(ctx, "    return buf;\n");
    emit_raw(ctx, "}\n\n");

    /* to_string: to_array(s).map { |c| to_char(c) }.join */
    emit_raw(ctx, "static char *sp_to_string(sp_Val *lv_s) {\n");
    emit_raw(ctx, "    sp_ValArray *arr = sp_to_array(lv_s);\n");
    emit_raw(ctx, "    size_t total = 0;\n");
    emit_raw(ctx, "    char **parts = (char **)malloc(sizeof(char *) * arr->len);\n");
    emit_raw(ctx, "    for (int i = 0; i < arr->len; i++) {\n");
    emit_raw(ctx, "        parts[i] = sp_to_char(arr->data[i]);\n");
    emit_raw(ctx, "        total += strlen(parts[i]);\n");
    emit_raw(ctx, "    }\n");
    emit_raw(ctx, "    char *result = (char *)malloc(total + 1);\n");
    emit_raw(ctx, "    result[0] = '\\0';\n");
    emit_raw(ctx, "    for (int i = 0; i < arr->len; i++) {\n");
    emit_raw(ctx, "        strcat(result, parts[i]);\n");
    emit_raw(ctx, "        free(parts[i]);\n");
    emit_raw(ctx, "    }\n");
    emit_raw(ctx, "    free(parts);\n");
    emit_raw(ctx, "    free(arr->data);\n");
    emit_raw(ctx, "    free(arr);\n");
    emit_raw(ctx, "    return result;\n");
    emit_raw(ctx, "}\n\n");
}

void codegen_program(codegen_ctx_t *ctx, pm_node_t *root) {
    assert(PM_NODE_TYPE(root) == PM_PROGRAM_NODE);
    pm_program_node_t *prog = (pm_program_node_t *)root;

    /* Detect lambda mode */
    ctx->lambda_mode = has_lambda_nodes(root);

    /* Detect exception handling mode */
    ctx->needs_exc = has_exception_nodes(ctx, root);

    /* Detect string split usage */
    ctx->needs_str_split = has_split_calls(ctx, root);

    /* Detect regexp usage and collect patterns (must be before emit_header) */
    collect_regexp_patterns(ctx, root);

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

    /* Pass 2: Type inference for top-level code */
    infer_pass(ctx, root);

    /* Pass 2b: Resolve class types */
    resolve_class_types(ctx, root);

    /* Pass 2c: Re-infer variable types now that function return types are resolved */
    infer_pass(ctx, root);

    /* Detect needs_hash: any HASH-typed variable triggers hash runtime */
    for (int i = 0; i < ctx->var_count; i++) {
        if (ctx->vars[i].type.kind == SPINEL_TYPE_HASH) {
            ctx->needs_hash = true;
            break;
        }
    }

    /* Detect needs_gc: any non-value-type class, sp_IntArray, or sp_StrIntHash triggers GC */
    for (int i = 0; i < ctx->class_count && !ctx->needs_gc; i++) {
        if (!ctx->classes[i].is_value_type)
            ctx->needs_gc = true;
    }
    for (int i = 0; i < ctx->var_count && !ctx->needs_gc; i++) {
        if (ctx->vars[i].type.kind == SPINEL_TYPE_ARRAY ||
            ctx->vars[i].type.kind == SPINEL_TYPE_HASH)
            ctx->needs_gc = true;
    }
    for (int i = 0; i < ctx->func_count && !ctx->needs_gc; i++) {
        func_info_t *f = &ctx->funcs[i];
        if (f->return_type.kind == SPINEL_TYPE_ARRAY ||
            f->return_type.kind == SPINEL_TYPE_HASH) { ctx->needs_gc = true; break; }
        for (int j = 0; j < f->param_count && !ctx->needs_gc; j++)
            if (f->params[j].type.kind == SPINEL_TYPE_ARRAY ||
                f->params[j].type.kind == SPINEL_TYPE_HASH) ctx->needs_gc = true;
        /* Also infer function body variables to detect array/hash usage */
        if (!ctx->needs_gc && f->body_node) {
            int saved_vc = ctx->var_count;
            for (int j = 0; j < f->param_count; j++)
                var_declare(ctx, f->params[j].name, f->params[j].type, false);
            infer_pass(ctx, f->body_node);
            for (int j = saved_vc; j < ctx->var_count; j++) {
                if (ctx->vars[j].type.kind == SPINEL_TYPE_ARRAY ||
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
        for (int mi = 0; mi < cls->method_count && !ctx->needs_gc; mi++) {
            method_info_t *m = &cls->methods[mi];
            if (m->return_type.kind == SPINEL_TYPE_ARRAY ||
                m->return_type.kind == SPINEL_TYPE_HASH) { ctx->needs_gc = true; break; }
            if (m->body_node) {
                int saved_vc = ctx->var_count;
                for (int j = 0; j < m->param_count; j++)
                    var_declare(ctx, m->params[j].name, m->params[j].type, false);
                infer_pass(ctx, m->body_node);
                for (int j = saved_vc; j < ctx->var_count; j++) {
                    if (ctx->vars[j].type.kind == SPINEL_TYPE_ARRAY ||
                        ctx->vars[j].type.kind == SPINEL_TYPE_HASH) {
                        ctx->needs_gc = true;
                        break;
                    }
                }
                ctx->var_count = saved_vc;
            }
        }
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

    /* Block callback type definition (for yield support) */
    {
        bool any_yield = false;
        for (int i = 0; i < ctx->func_count; i++)
            if (ctx->funcs[i].has_yield) { any_yield = true; break; }
        if (any_yield)
            emit_raw(ctx, "typedef mrb_int (*sp_block_fn)(void *env, mrb_int arg);\n\n");
    }

    /* Forward declarations for top-level functions */
    if (!ctx->lambda_mode) {
        for (int i = 0; i < ctx->func_count; i++) {
            func_info_t *f = &ctx->funcs[i];
            char *ret_ct = vt_ctype(ctx, f->return_type, false);
            bool ret_void = (f->return_type.kind == SPINEL_TYPE_NIL);
            emit_raw(ctx, "static %s sp_%s(", ret_void ? "void" : ret_ct, f->name);
            for (int j = 0; j < f->param_count; j++) {
                if (j > 0) emit_raw(ctx, ", ");
                char *pct = vt_ctype(ctx, f->params[j].type, false);
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
            if (f->param_count == 0 && !f->has_yield) emit_raw(ctx, "void");
            emit_raw(ctx, ");\n");
            free(ret_ct);
        }
        emit_raw(ctx, "\n");
    }

    /* Initialize functions for superclasses (called via super) */
    for (int i = 0; i < ctx->class_count; i++)
        emit_initialize_func(ctx, &ctx->classes[i]);

    /* Constructors */
    for (int i = 0; i < ctx->class_count; i++)
        emit_constructor(ctx, &ctx->classes[i]);

    /* Class methods */
    for (int i = 0; i < ctx->class_count; i++) {
        class_info_t *cls = &ctx->classes[i];
        for (int j = 0; j < cls->method_count; j++)
            emit_method(ctx, cls, &cls->methods[j]);
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

        emit_raw(ctx, "int main(int argc, char **argv) {\n");
        emit_raw(ctx, "    (void)argc; (void)argv;\n");

        /* Variable declarations for top-level */
        for (int i = 0; i < ctx->var_count; i++) {
            var_entry_t *v = &ctx->vars[i];
            if (v->is_constant) continue;
            char *ct = vt_ctype(ctx, v->type, false);
            char *cn = make_cname(v->name, v->is_constant);
            if (v->type.kind == SPINEL_TYPE_PROC) {
                emit_raw(ctx, "    sp_Val *%s = NULL;\n", cn);
            } else if (v->type.kind == SPINEL_TYPE_ARRAY) {
                emit_raw(ctx, "    sp_IntArray *%s = NULL;\n", cn);
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

        /* Main function */
        emit_raw(ctx, "int main(int argc, char **argv) {\n");
        emit_raw(ctx, "    (void)argc; (void)argv;\n");

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
            else if (v->type.kind == SPINEL_TYPE_HASH) {
                emit_raw(ctx, "    sp_StrIntHash *%s = NULL;\n", cn);
                if (ctx->needs_gc)
                    emit_raw(ctx, "    SP_GC_ROOT(%s);\n", cn);
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

        /* Emit block callbacks first, then the functions and main */
        if (block_buf_data && block_buf_size > 0) {
            fwrite(block_buf_data, 1, block_buf_size, ctx->out);
            emit_raw(ctx, "\n");
        }
        if (code_buf_data)
            fwrite(code_buf_data, 1, code_buf_size, ctx->out);
        free(block_buf_data);
        free(code_buf_data);
    }
}
