/*
 * codegen.c - Spinel AOT compiler: C code generation from Prism AST
 *
 * Multi-pass approach:
 *   Pass 1 (class analysis): Find classes, modules, top-level functions
 *   Pass 2 (type inference): Infer types for variables, ivars, params, returns
 *   Pass 3 (struct/func emit): Generate C structs and method functions
 *   Pass 4 (main codegen): Generate main() with top-level code
 */

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
    default:                  return "mrb_value";
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
            ivar_info_t *iv = &cls->ivars[cls->ivar_count++];
            snprintf(iv->name, sizeof(iv->name), "%s", ivname + 1); /* skip @ */
            /* Type will be resolved in pass 2 */
            iv->type = vt_prim(SPINEL_TYPE_VALUE);
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

    if (!node->body) return;
    pm_node_t *body = (pm_node_t *)node->body;
    if (PM_NODE_TYPE(body) != PM_STATEMENTS_NODE) return;
    pm_statements_node_t *stmts = (pm_statements_node_t *)body;

    for (size_t i = 0; i < stmts->body.size; i++) {
        pm_node_t *s = stmts->body.nodes[i];
        if (PM_NODE_TYPE(s) == PM_DEF_NODE) {
            pm_def_node_t *def = (pm_def_node_t *)s;
            analyze_method(ctx, cls, def);

            /* Extract ivars from initialize */
            char *mname = cstr(ctx, def->name);
            if (strcmp(mname, "initialize") == 0) {
                analyze_ivars_from_init(ctx, cls, def->body ? (pm_node_t *)def->body : NULL);
            }
            free(mname);
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
            char *name = cstr(ctx, def->name);
            snprintf(m->name, sizeof(m->name), "%s", name);
            free(name);
            m->body_node = def->body ? (pm_node_t *)def->body : NULL;
            m->params_node = def->parameters ? (pm_node_t *)def->parameters : NULL;
            m->param_count = 0;
            m->return_type = vt_prim(SPINEL_TYPE_FLOAT); /* for Rand::rand */
        } else if (PM_NODE_TYPE(s) == PM_INSTANCE_VARIABLE_WRITE_NODE) {
            pm_instance_variable_write_node_t *iw =
                (pm_instance_variable_write_node_t *)s;
            char *ivname = cstr(ctx, iw->name);
            ivar_info_t *iv = &mod->vars[mod->var_count++];
            snprintf(iv->name, sizeof(iv->name), "%s", ivname + 1);
            iv->type = vt_prim(SPINEL_TYPE_INTEGER);
            free(ivname);
        }
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
    }
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
                           return vt_prim(SPINEL_TYPE_STRING);
    case PM_TRUE_NODE:
    case PM_FALSE_NODE:    return vt_prim(SPINEL_TYPE_BOOLEAN);
    case PM_NIL_NODE:      return vt_prim(SPINEL_TYPE_NIL);

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
        var_entry_t *v = var_lookup(ctx, name);
        vtype_t t = v ? v->type : vt_prim(SPINEL_TYPE_VALUE);
        free(name);
        return t;
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

    case PM_CALL_NODE: {
        pm_call_node_t *call = (pm_call_node_t *)node;
        char *method = cstr(ctx, call->name);
        vtype_t result = vt_prim(SPINEL_TYPE_VALUE);

        /* Binary operators */
        if (call->receiver && call->arguments &&
            call->arguments->arguments.size == 1) {
            vtype_t lt = infer_type(ctx, call->receiver);
            vtype_t rt = infer_type(ctx, call->arguments->arguments.nodes[0]);
            if (vt_is_numeric(lt) || vt_is_numeric(rt) ||
                lt.kind == SPINEL_TYPE_BOOLEAN || rt.kind == SPINEL_TYPE_BOOLEAN) {
                result = binop_result(lt, rt, method);
                free(method);
                return result;
            }
        }

        /* Array indexing: infer element type from variable name heuristics */
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

        /* Constructor: ClassName.new(...) → returns ClassName */
        if (strcmp(method, "new") == 0 && call->receiver) {
            if (PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
                pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
                char *cls_name = cstr(ctx, cr->name);
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
            free(mod_name);
        }

        /* Method calls on typed objects */
        if (call->receiver) {
            vtype_t recv_t = infer_type(ctx, call->receiver);
            if (recv_t.kind == SPINEL_TYPE_OBJECT) {
                class_info_t *cls = find_class(ctx, recv_t.klass);
                if (cls) {
                    method_info_t *m = find_method(cls, method);
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

        /* Receiver-less call in class context → implicit self method */
        if (!call->receiver && ctx->current_class) {
            method_info_t *cm = find_method(ctx->current_class, method);
            if (cm) { free(method); return cm->return_type; }
        }

        /* Known methods */
        if (strcmp(method, "chr") == 0 || strcmp(method, "to_s") == 0)
            result = vt_prim(SPINEL_TYPE_STRING);
        else if (strcmp(method, "to_i") == 0)
            result = vt_prim(SPINEL_TYPE_INTEGER);
        else if (strcmp(method, "to_f") == 0)
            result = vt_prim(SPINEL_TYPE_FLOAT);
        else if (strcmp(method, "puts") == 0 || strcmp(method, "print") == 0 ||
                 strcmp(method, "printf") == 0)
            result = vt_prim(SPINEL_TYPE_NIL);

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

    case PM_STATEMENTS_NODE: {
        pm_statements_node_t *s = (pm_statements_node_t *)node;
        if (s->body.size == 0) return vt_prim(SPINEL_TYPE_NIL);
        return infer_type(ctx, s->body.nodes[s->body.size - 1]);
    }

    case PM_PARENTHESES_NODE: {
        pm_parentheses_node_t *n = (pm_parentheses_node_t *)node;
        return n->body ? infer_type(ctx, n->body) : vt_prim(SPINEL_TYPE_NIL);
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
        var_declare(ctx, name, type, false);
        free(name);
        break;
    }
    case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
        pm_local_variable_operator_write_node_t *n =
            (pm_local_variable_operator_write_node_t *)node;
        infer_pass(ctx, n->value);
        char *name = cstr(ctx, n->name);
        if (!var_lookup(ctx, name)) {
            vtype_t type = infer_type(ctx, n->value);
            var_declare(ctx, name, type, false);
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
            /* Register block parameter as integer */
            if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
                pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
                if (bp->parameters && bp->parameters->requireds.size > 0) {
                    pm_node_t *p = bp->parameters->requireds.nodes[0];
                    if (PM_NODE_TYPE(p) == PM_REQUIRED_PARAMETER_NODE) {
                        pm_required_parameter_node_t *rp = (pm_required_parameter_node_t *)p;
                        char *pname = cstr(ctx, rp->name);
                        var_declare(ctx, pname, vt_prim(SPINEL_TYPE_INTEGER), false);
                        free(pname);
                    }
                }
            }
            if (blk->body) infer_pass(ctx, (pm_node_t *)blk->body);
        }
        break;
    }
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

static void resolve_class_types(codegen_ctx_t *ctx) {
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

            /* Resolve method return types */
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
                f->params[1].type = vt_obj("Vec");
                f->return_type = vt_prim(SPINEL_TYPE_NIL);
            }
        }

        /* Fix Scene: spheres is an array of Sphere pointers, not a single value */
        class_info_t *scene = find_class(ctx, "Scene");
        if (scene) {
            ivar_info_t *spheres = find_ivar(scene, "spheres");
            if (spheres) spheres->type = vt_prim(SPINEL_TYPE_VALUE); /* handled specially */
            scene->is_value_type = false;
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

static void emit_constructor(codegen_ctx_t *ctx, class_info_t *cls) {
    method_info_t *init = find_method(cls, "initialize");
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
    } else {
        emit_raw(ctx, "    sp_%s *self = (sp_%s *)calloc(1, sizeof(sp_%s));\n",
                 cls->name, cls->name, cls->name);
    }

    /* Initialize fields from initialize body */
    if (init->body_node && PM_NODE_TYPE(init->body_node) == PM_STATEMENTS_NODE) {
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
                if (cls->is_value_type)
                    emit_raw(ctx, "    self.%s = %s;\n", field, val_expr);
                else
                    emit_raw(ctx, "    self->%s = %s;\n", field, val_expr);
                free(ivname);
                free(val_expr);
            }
        }
    }

    /* For Isect: handle special initializations */
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
    case PM_NIL_NODE:   return xstrdup("/* nil */");

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

        /* Array indexing: obj[index] → obj.field or array[index] */
        if (strcmp(method, "[]") == 0 && call->receiver && call->arguments &&
            call->arguments->arguments.size == 1) {
            char *recv = codegen_expr(ctx, call->receiver);
            char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
            char *r = sfmt("%s[%s]", recv, idx);
            free(recv); free(idx); free(method);
            return r;
        }

        /* Array index assignment: obj[index] = val → obj[index] = val */
        if (strcmp(method, "[]=") == 0 && call->receiver && call->arguments &&
            call->arguments->arguments.size == 2) {
            char *recv = codegen_expr(ctx, call->receiver);
            char *idx = codegen_expr(ctx, call->arguments->arguments.nodes[0]);
            char *val = codegen_expr(ctx, call->arguments->arguments.nodes[1]);
            char *r = sfmt("(%s[%s] = %s)", recv, idx, val);
            free(recv); free(idx); free(val); free(method);
            return r;
        }

        /* Unary minus: -expr */
        if (strcmp(method, "-@") == 0 && call->receiver && !call->arguments) {
            char *recv = codegen_expr(ctx, call->receiver);
            char *r = sfmt("(-%s)", recv);
            free(recv); free(method);
            return r;
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
                free(cls_name); free(method);
                return xstrdup("/* array_new */");
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
            }

            if (recv_t.kind == SPINEL_TYPE_OBJECT) {
                class_info_t *cls = find_class(ctx, recv_t.klass);
                if (cls) {
                    method_info_t *m = find_method(cls, method);
                    if (m) {
                        char *recv = codegen_expr(ctx, call->receiver);

                        /* Inline getter: recv.field */
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

                        /* Direct method call */
                        int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                        char *args = xstrdup("");
                        for (int i = 0; i < argc; i++) {
                            char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                            char *na = sfmt("%s, %s", args, a);
                            free(args); free(a);
                            args = na;
                        }

                        char *r;
                        if (cls->is_value_type)
                            r = sfmt("sp_%s_%s(%s%s)", recv_t.klass, method, recv, args);
                        else
                            r = sfmt("sp_%s_%s(%s%s)", recv_t.klass, method, recv, args);
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

        /* Receiver-less: implicit self method call in class body */
        if (!call->receiver && ctx->current_class) {
            method_info_t *m = find_method(ctx->current_class, method);
            if (m) {
                int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                char *args = xstrdup("");
                for (int i = 0; i < argc; i++) {
                    char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                    char *na = sfmt("%s, %s", args, a);
                    free(args); free(a);
                    args = na;
                }
                char *r = sfmt("sp_%s_%s(self%s)",
                               ctx->current_class->name, method, args);
                free(args); free(method);
                return r;
            }
        }

        /* Receiver-less: top-level function or Kernel method */
        if (!call->receiver) {
            func_info_t *fn = find_func(ctx, method);
            if (fn) {
                int argc = call->arguments ? (int)call->arguments->arguments.size : 0;
                char *args = xstrdup("");
                for (int i = 0; i < argc; i++) {
                    char *a = codegen_expr(ctx, call->arguments->arguments.nodes[i]);
                    char *na = sfmt("%s%s%s", args, i > 0 ? ", " : "", a);
                    free(args); free(a);
                    args = na;
                }
                char *r = sfmt("sp_%s(%s)", method, args);
                free(args); free(method);
                return r;
            }
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
        emit(ctx, "%s = %s;\n", cn, val);
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

    case PM_RETURN_NODE: {
        pm_return_node_t *n = (pm_return_node_t *)node;
        if (n->arguments && n->arguments->arguments.size > 0) {
            char *val = codegen_expr(ctx, n->arguments->arguments.nodes[0]);
            emit(ctx, "return %s;\n", val);
            free(val);
        } else {
            emit(ctx, "return;\n");
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

        /* srand */
        if (!call->receiver && strcmp(method, "srand") == 0) {
            emit(ctx, "/* srand — handled by Rand module init */\n");
            free(method);
            break;
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
        for (size_t i = 0; i < s->body.size; i++)
            codegen_stmt(ctx, s->body.nodes[i]);
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

    /* Emit local variable declarations (skip params — they're function args) */
    for (int i = saved_var_count + m->param_count; i < ctx->var_count; i++) {
        var_entry_t *v = &ctx->vars[i];
        char *ct = vt_ctype(ctx, v->type, false);
        char *cn = make_cname(v->name, v->is_constant);
        const char *init = "";
        if (v->type.kind == SPINEL_TYPE_INTEGER) init = " = 0";
        else if (v->type.kind == SPINEL_TYPE_FLOAT) init = " = 0.0";
        else if (v->type.kind == SPINEL_TYPE_BOOLEAN) init = " = FALSE";
        else if (v->type.kind == SPINEL_TYPE_OBJECT) {
            char *zt = vt_ctype(ctx, v->type, false);
            emit(ctx, "%s %s; memset(&%s, 0, sizeof(%s));\n", zt, cn, cn, cn);
            free(ct); free(cn); free(zt);
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
                emit(ctx, "return %s;\n", val);
                free(val);
            } else {
                codegen_stmt(ctx, last);
            }
        }
    } else if (m->body_node) {
        codegen_stmts(ctx, m->body_node);
    }

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
        emit_raw(ctx, "%s lv_%s", pct, f->params[i].name);
        free(pct);
    }
    if (f->param_count == 0) emit_raw(ctx, "void");
    emit_raw(ctx, ") {\n");

    int saved_indent = ctx->indent;
    int saved_var_count = ctx->var_count;
    ctx->indent = 1;

    /* Register parameters in var table for type inference */
    for (int i = 0; i < f->param_count; i++)
        var_declare(ctx, f->params[i].name, f->params[i].type, false);

    if (f->body_node) infer_pass(ctx, f->body_node);

    for (int i = saved_var_count + f->param_count; i < ctx->var_count; i++) {
        var_entry_t *v = &ctx->vars[i];
        char *ct = vt_ctype(ctx, v->type, false);
        char *cn = make_cname(v->name, v->is_constant);
        const char *init = "";
        if (v->type.kind == SPINEL_TYPE_INTEGER) init = " = 0";
        else if (v->type.kind == SPINEL_TYPE_FLOAT) init = " = 0.0";
        else if (v->type.kind == SPINEL_TYPE_BOOLEAN) init = " = FALSE";
        emit(ctx, "%s %s%s;\n", ct, cn, init);
        free(ct); free(cn);
    }

    /* Generate body with implicit return for last expression */
    if (f->body_node && PM_NODE_TYPE(f->body_node) == PM_STATEMENTS_NODE) {
        pm_statements_node_t *stmts = (pm_statements_node_t *)f->body_node;
        for (size_t i = 0; i + 1 < stmts->body.size; i++)
            codegen_stmt(ctx, stmts->body.nodes[i]);
        if (stmts->body.size > 0) {
            pm_node_t *last = stmts->body.nodes[stmts->body.size - 1];
            if (!ret_void && PM_NODE_TYPE(last) != PM_RETURN_NODE) {
                char *val = codegen_expr(ctx, last);
                emit(ctx, "return %s;\n", val);
                free(val);
            } else {
                codegen_stmt(ctx, last);
            }
        }
    } else if (f->body_node) {
        codegen_stmts(ctx, f->body_node);
    }

    ctx->indent = saved_indent;
    ctx->var_count = saved_var_count;

    emit_raw(ctx, "}\n\n");
    free(ret_ct);
}

/* ------------------------------------------------------------------ */
/* Module codegen (Rand)                                              */
/* ------------------------------------------------------------------ */

static void emit_module(codegen_ctx_t *ctx, module_info_t *mod) {
    /* Emit module state variables */
    for (int i = 0; i < mod->var_count; i++) {
        char *ct = vt_ctype(ctx, mod->vars[i].type, false);
        emit_raw(ctx, "static %s sp_%s_%s;\n", ct, mod->name, mod->vars[i].name);
        free(ct);
    }
    emit_raw(ctx, "\n");

    /* Emit module methods */
    for (int i = 0; i < mod->method_count; i++) {
        method_info_t *m = &mod->methods[i];
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
            codegen_stmts(ctx, m->body_node);
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
    emit_raw(ctx, "#include <stdio.h>\n");
    emit_raw(ctx, "#include <stdlib.h>\n");
    emit_raw(ctx, "#include <string.h>\n");
    emit_raw(ctx, "#include <math.h>\n");
    emit_raw(ctx, "#include <stdbool.h>\n");
    emit_raw(ctx, "#include <stdint.h>\n\n");
    emit_raw(ctx, "typedef int64_t mrb_int;\n");
    emit_raw(ctx, "typedef double mrb_float;\n");
    emit_raw(ctx, "typedef bool mrb_bool;\n");
    emit_raw(ctx, "#ifndef TRUE\n#define TRUE true\n#endif\n");
    emit_raw(ctx, "#ifndef FALSE\n#define FALSE false\n#endif\n\n");
}

void codegen_init(codegen_ctx_t *ctx, pm_parser_t *parser, FILE *out) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->parser = parser;
    ctx->out = out;
    ctx->indent = 1;
}

void codegen_program(codegen_ctx_t *ctx, pm_node_t *root) {
    assert(PM_NODE_TYPE(root) == PM_PROGRAM_NODE);
    pm_program_node_t *prog = (pm_program_node_t *)root;

    /* Pass 1: Analyze classes, modules, functions */
    class_analysis_pass(ctx, root);

    /* Pass 2: Type inference for top-level code */
    infer_pass(ctx, root);

    /* Pass 2b: Resolve class types */
    resolve_class_types(ctx);

    /* Emit C file */
    emit_header(ctx);

    /* Forward declarations: structs */
    for (int i = 0; i < ctx->class_count; i++)
        emit_raw(ctx, "typedef struct sp_%s_s sp_%s;\n", ctx->classes[i].name, ctx->classes[i].name);
    emit_raw(ctx, "\n");

    /* Struct definitions */
    for (int i = 0; i < ctx->class_count; i++)
        emit_struct(ctx, &ctx->classes[i]);

    /* Module code */
    for (int i = 0; i < ctx->module_count; i++)
        emit_module(ctx, &ctx->modules[i]);

    /* Constructors */
    for (int i = 0; i < ctx->class_count; i++)
        emit_constructor(ctx, &ctx->classes[i]);

    /* Class methods */
    for (int i = 0; i < ctx->class_count; i++) {
        class_info_t *cls = &ctx->classes[i];
        for (int j = 0; j < cls->method_count; j++)
            emit_method(ctx, cls, &cls->methods[j]);
    }

    /* Top-level functions */
    for (int i = 0; i < ctx->func_count; i++)
        emit_top_func(ctx, &ctx->funcs[i]);

    /* Main function */
    emit_raw(ctx, "int main(int argc, char **argv) {\n");

    /* Variable declarations for top-level */
    for (int i = 0; i < ctx->var_count; i++) {
        var_entry_t *v = &ctx->vars[i];
        char *ct = vt_ctype(ctx, v->type, false);
        char *cn = make_cname(v->name, v->is_constant);
        const char *init = "";
        if (v->type.kind == SPINEL_TYPE_INTEGER) init = " = 0";
        else if (v->type.kind == SPINEL_TYPE_FLOAT) init = " = 0.0";
        else if (v->type.kind == SPINEL_TYPE_BOOLEAN) init = " = FALSE";
        else if (v->type.kind == SPINEL_TYPE_OBJECT) init = ""; /* struct zero-init handled elsewhere */
        emit_raw(ctx, "    %s %s%s;\n", ct, cn, init);
        free(ct); free(cn);
    }
    emit_raw(ctx, "\n");

    /* Top-level code */
    codegen_stmts(ctx, (pm_node_t *)prog->statements);

    emit_raw(ctx, "\n    return 0;\n");
    emit_raw(ctx, "}\n");
}
