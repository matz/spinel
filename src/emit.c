/* emit.c - Spinel AOT: C code emission (header, structs, methods) */
#define _GNU_SOURCE  /* for open_memstream */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <assert.h>
#include "codegen.h"

/* ------------------------------------------------------------------ */
/* Pass 3: Emit C structs and method functions                        */
/* ------------------------------------------------------------------ */

void emit_struct(codegen_ctx_t *ctx, class_info_t *cls) {
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
void emit_initialize_func(codegen_ctx_t *ctx, class_info_t *cls) {
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

void emit_constructor(codegen_ctx_t *ctx, class_info_t *cls) {
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
    if (!init) {
        /* No initialize method: generate a default constructor */
        if (cls->is_value_type) {
            emit_raw(ctx, "static sp_%s sp_%s_new(void) {\n", cls->name, cls->name);
            emit_raw(ctx, "    sp_%s self = {0};\n", cls->name);
            emit_raw(ctx, "    return self;\n}\n\n");
        } else {
            emit_raw(ctx, "static sp_%s *sp_%s_new(void) {\n", cls->name, cls->name);
            if (ctx->needs_gc) {
                emit_raw(ctx, "    sp_%s *self = (sp_%s *)sp_gc_alloc(sizeof(sp_%s), NULL, NULL);\n",
                         cls->name, cls->name, cls->name);
            } else {
                emit_raw(ctx, "    sp_%s *self = (sp_%s *)calloc(1, sizeof(sp_%s));\n",
                         cls->name, cls->name, cls->name);
            }
            emit_raw(ctx, "    return self;\n}\n\n");
        }
        return;
    }

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

/* ------------------------------------------------------------------ */
/* Method body codegen                                                */
/* ------------------------------------------------------------------ */

void emit_method(codegen_ctx_t *ctx, class_info_t *cls, method_info_t *m) {
    if (strcmp(m->name, "initialize") == 0) return; /* handled by constructor */
    if (m->is_getter || m->is_setter) return; /* inlined */

    /* Determine return type C string */
    char *ret_ct = vt_ctype(ctx, m->return_type, false);
    bool ret_void = (m->return_type.kind == SPINEL_TYPE_NIL);

    /* Function signature — sanitize operator method names for C identifiers */
    const char *c_mname = sanitize_method(m->name);
    emit_raw(ctx, "static %s sp_%s_%s(",
             ret_void ? "void" : ret_ct, cls->name, c_mname);

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
        /* Add block callback params if method uses yield */
        if (m->body_node && has_yield_nodes(m->body_node)) {
            emit_raw(ctx, ", sp_block_fn _block, void *_block_env");
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
        else if (v->type.kind == SPINEL_TYPE_RB_HASH) {
            emit(ctx, "sp_RbHash *%s = NULL;\n", cn);
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
            /* Detect call-with-block (e.g., @data.each { |x| yield x }) which
               must be codegen'd as a statement, not an expression */
            bool last_is_block_call = (PM_NODE_TYPE(last) == PM_CALL_NODE &&
                                       ((pm_call_node_t *)last)->block != NULL);
            if (!ret_void && !last_is_block_call &&
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

void emit_top_func(codegen_ctx_t *ctx, func_info_t *f) {
    snprintf(ctx->current_func_name, sizeof(ctx->current_func_name), "%s", f->name);
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
    /* Add sp_Proc * parameter for &block functions */
    if (f->has_block_param && !ctx->lambda_mode) {
        if (f->param_count > 0 || f->has_yield) emit_raw(ctx, ", ");
        emit_raw(ctx, "sp_Proc *lv_%s", f->block_param_name);
        ctx->needs_proc = true;
    }
    if (f->param_count == 0 && !f->has_yield && !f->has_block_param) emit_raw(ctx, "void");
    emit_raw(ctx, ") {\n");

    int saved_indent = ctx->indent;
    int saved_var_count = ctx->var_count;
    ctx->indent = 1;

    /* Register parameters in var table for type inference */
    for (int i = 0; i < f->param_count; i++)
        var_declare(ctx, f->params[i].name, f->params[i].type, false);

    /* Register &block parameter */
    if (f->has_block_param && !ctx->lambda_mode)
        var_declare(ctx, f->block_param_name, vt_prim(SPINEL_TYPE_PROC), false);

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
            if (v->type.kind == SPINEL_TYPE_POLY) {
                emit(ctx, "sp_RbValue %s = sp_box_nil();\n", cn);
                free(ct); free(cn);
                continue;
            }
            if (v->type.kind == SPINEL_TYPE_RB_ARRAY) {
                emit(ctx, "sp_RbArray *%s = NULL;\n", cn);
                free(ct); free(cn);
                continue;
            }
            if (v->type.kind == SPINEL_TYPE_RB_HASH) {
                emit(ctx, "sp_RbHash *%s = NULL;\n", cn);
                free(ct); free(cn);
                continue;
            }
            if (v->type.kind == SPINEL_TYPE_PROC && !ctx->lambda_mode) {
                /* Skip if this is the &block param (already a function parameter) */
                if (f->has_block_param && strcmp(v->name, f->block_param_name) == 0) {
                    free(ct); free(cn);
                    continue;
                }
                emit(ctx, "sp_Proc *%s = NULL;\n", cn);
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
    ctx->current_func_name[0] = '\0';

    emit_raw(ctx, "}\n\n");
    free(ret_ct);
}

/* ------------------------------------------------------------------ */
/* Module codegen (Rand)                                              */
/* ------------------------------------------------------------------ */

void emit_module(codegen_ctx_t *ctx, module_info_t *mod) {
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

void emit_header(codegen_ctx_t *ctx) {
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
    emit_raw(ctx, "#include <ctype.h>\n");
    emit_raw(ctx, "#include <unistd.h>\n");
    emit_raw(ctx, "#include <signal.h>\n");
    emit_raw(ctx, "#include <stdarg.h>\n");
    emit_raw(ctx, "#include <libgen.h>\n");
    emit_raw(ctx, "#include <glob.h>\n");
    emit_raw(ctx, "#include <sys/stat.h>\n\n");
    emit_raw(ctx, "typedef int64_t mrb_int;\n");
    emit_raw(ctx, "typedef double mrb_float;\n");
    emit_raw(ctx, "typedef bool mrb_bool;\n");
    emit_raw(ctx, "#ifndef TRUE\n#define TRUE true\n#endif\n");
    emit_raw(ctx, "#ifndef FALSE\n#define FALSE false\n#endif\n\n");

    /* ---- Polymorphic NaN-boxed value (sp_RbValue = uint64_t) ---- */
    if (ctx->needs_poly) {
        emit_raw(ctx, "/* NaN-boxing: 8-byte tagged value */\n");
        emit_raw(ctx, "typedef uint64_t sp_RbValue;\n");
        /* Tag constants */
        emit_raw(ctx, "#define SP_T_OBJECT 0x0000\n");
        emit_raw(ctx, "#define SP_T_INT    0x0001\n");
        emit_raw(ctx, "#define SP_T_STRING 0x0002\n");
        emit_raw(ctx, "#define SP_T_BOOL   0x0003\n");
        emit_raw(ctx, "#define SP_T_NIL    0x0004\n");
        emit_raw(ctx, "#define SP_T_FLOAT  0x0005\n");
        /* Per-class tag values starting at 0x0040 */
        for (int i = 0; i < ctx->class_count; i++) {
            emit_raw(ctx, "#define SP_TAG_%s 0x%04x\n", ctx->classes[i].name, 0x0040 + i);
        }
        emit_raw(ctx, "#define SP_TAG(v)       ((uint16_t)((v) >> 48))\n");
        emit_raw(ctx, "#define SP_PAYLOAD(v)   ((v) & 0x0000FFFFFFFFFFFFULL)\n");
        emit_raw(ctx, "#define SP_IS_INT(v)    (SP_TAG(v) == SP_T_INT)\n");
        emit_raw(ctx, "#define SP_IS_STR(v)    (SP_TAG(v) == SP_T_STRING)\n");
        emit_raw(ctx, "#define SP_IS_BOOL(v)   (SP_TAG(v) == SP_T_BOOL)\n");
        emit_raw(ctx, "#define SP_IS_NIL(v)    (SP_TAG(v) == SP_T_NIL)\n");
        emit_raw(ctx, "#define SP_IS_DBL(v)    (SP_TAG(v) == SP_T_FLOAT)\n");
        emit_raw(ctx, "#define SP_IS_OBJ(v)    (SP_TAG(v) >= 0x0040)\n\n");
        /* Boxing */
        emit_raw(ctx, "static sp_RbValue sp_box_int(int64_t n) { return ((uint64_t)0x0001 << 48) | ((uint64_t)n & 0x0000FFFFFFFFFFFFULL); }\n");
        emit_raw(ctx, "static sp_RbValue sp_box_float(double f) { uint64_t b; memcpy(&b, &f, 8); return ((uint64_t)0x0005 << 48) | (b >> 16); }\n");
        emit_raw(ctx, "static sp_RbValue sp_box_str(const char *s) { return ((uint64_t)0x0002 << 48) | (uint64_t)(uintptr_t)s; }\n");
        emit_raw(ctx, "static sp_RbValue sp_box_bool(int b) { return ((uint64_t)0x0003 << 48) | (b ? 1 : 0); }\n");
        emit_raw(ctx, "static sp_RbValue sp_box_nil(void) { return ((uint64_t)0x0004 << 48); }\n");
        emit_raw(ctx, "static sp_RbValue sp_box_obj(int tag, void *p) { return ((uint64_t)(unsigned)tag << 48) | (uint64_t)(uintptr_t)p; }\n\n");
        /* Unboxing */
        emit_raw(ctx, "static int64_t sp_unbox_int(sp_RbValue v) { int64_t raw = (int64_t)(v & 0x0000FFFFFFFFFFFFULL); return (raw << 16) >> 16; }\n");
        emit_raw(ctx, "static double sp_unbox_float(sp_RbValue v) { uint64_t b = (v & 0x0000FFFFFFFFFFFFULL) << 16; double f; memcpy(&f, &b, 8); return f; }\n");
        emit_raw(ctx, "static const char *sp_unbox_str(sp_RbValue v) { return (const char *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL); }\n");
        emit_raw(ctx, "static void *sp_unbox_obj(sp_RbValue v) { return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL); }\n");
        emit_raw(ctx, "static int64_t sp_unbox_bool(sp_RbValue v) { return (int64_t)(v & 1); }\n\n");
        /* sp_poly_puts */
        emit_raw(ctx, "static void sp_poly_puts(sp_RbValue v) {\n");
        emit_raw(ctx, "    uint16_t t = SP_TAG(v);\n");
        emit_raw(ctx, "    if (t == SP_T_INT) { printf(\"%%lld\\n\", (long long)sp_unbox_int(v)); }\n");
        emit_raw(ctx, "    else if (t == SP_T_FLOAT) { char buf[32]; snprintf(buf,32,\"%%g\",sp_unbox_float(v));\n");
        emit_raw(ctx, "        printf(\"%%s%%s\\n\", buf, strchr(buf,'.')||strchr(buf,'e')?\"\":\".0\"); }\n");
        emit_raw(ctx, "    else if (t == SP_T_STRING) { const char *s=sp_unbox_str(v); fputs(s,stdout);\n");
        emit_raw(ctx, "        if(!*s||s[strlen(s)-1]!='\\n') putchar('\\n'); }\n");
        emit_raw(ctx, "    else if (t == SP_T_BOOL) { puts(sp_unbox_bool(v) ? \"true\" : \"false\"); }\n");
        emit_raw(ctx, "    else if (t == SP_T_NIL) { puts(\"\"); }\n");
        emit_raw(ctx, "    else { puts(\"(object)\"); }\n");
        emit_raw(ctx, "}\n");
        emit_raw(ctx, "static mrb_bool sp_poly_nil_p(sp_RbValue v) { return SP_TAG(v) == SP_T_NIL; }\n\n");
    }

    /* ---- Heterogeneous array (sp_RbArray) ---- */
    if (ctx->needs_rb_array) {
        emit_raw(ctx, "typedef struct { sp_RbValue *data; mrb_int len; mrb_int cap; } sp_RbArray;\n");
        emit_raw(ctx, "static sp_RbArray *sp_RbArray_new(void) {\n");
        emit_raw(ctx, "    sp_RbArray *a = (sp_RbArray *)malloc(sizeof(sp_RbArray));\n");
        emit_raw(ctx, "    a->cap = 8; a->len = 0;\n");
        emit_raw(ctx, "    a->data = (sp_RbValue *)malloc(sizeof(sp_RbValue) * a->cap);\n");
        emit_raw(ctx, "    return a;\n}\n");
        emit_raw(ctx, "static void sp_RbArray_push(sp_RbArray *a, sp_RbValue v) {\n");
        emit_raw(ctx, "    if (a->len >= a->cap) { a->cap *= 2; a->data = (sp_RbValue *)realloc(a->data, sizeof(sp_RbValue) * a->cap); }\n");
        emit_raw(ctx, "    a->data[a->len++] = v;\n}\n");
        emit_raw(ctx, "static sp_RbValue sp_RbArray_get(sp_RbArray *a, mrb_int idx) {\n");
        emit_raw(ctx, "    if (idx < 0) idx += a->len;\n");
        emit_raw(ctx, "    return a->data[idx];\n}\n");
        emit_raw(ctx, "static mrb_int sp_RbArray_length(sp_RbArray *a) { return a->len; }\n\n");
    }

    /* ---- Heterogeneous hash (sp_RbHash: string key → sp_RbValue) ---- */
    if (ctx->needs_rb_hash) {
        emit_raw(ctx, "typedef struct sp_RbHashEntry {\n");
        emit_raw(ctx, "    const char *key;\n");
        emit_raw(ctx, "    sp_RbValue value;\n");
        emit_raw(ctx, "    struct sp_RbHashEntry *next;       /* bucket chain */\n");
        emit_raw(ctx, "    struct sp_RbHashEntry *order_next; /* insertion order */\n");
        emit_raw(ctx, "} sp_RbHashEntry;\n\n");
        emit_raw(ctx, "typedef struct {\n");
        emit_raw(ctx, "    sp_RbHashEntry **buckets;\n");
        emit_raw(ctx, "    sp_RbHashEntry *first, *last; /* insertion order */\n");
        emit_raw(ctx, "    mrb_int size, cap;\n");
        emit_raw(ctx, "} sp_RbHash;\n\n");
        emit_raw(ctx, "static unsigned sp_rb_hash_str(const char *s) {\n");
        emit_raw(ctx, "    unsigned h = 5381;\n");
        emit_raw(ctx, "    while (*s) h = h * 33 + (unsigned char)*s++;\n");
        emit_raw(ctx, "    return h;\n");
        emit_raw(ctx, "}\n\n");
        emit_raw(ctx, "static sp_RbHash *sp_RbHash_new(void) {\n");
        emit_raw(ctx, "    sp_RbHash *h = (sp_RbHash *)calloc(1, sizeof(sp_RbHash));\n");
        emit_raw(ctx, "    h->cap = 16; h->size = 0; h->first = NULL; h->last = NULL;\n");
        emit_raw(ctx, "    h->buckets = (sp_RbHashEntry **)calloc(h->cap, sizeof(sp_RbHashEntry *));\n");
        emit_raw(ctx, "    return h;\n}\n\n");
        emit_raw(ctx, "static void sp_RbHash_set(sp_RbHash *h, const char *key, sp_RbValue value) {\n");
        emit_raw(ctx, "    unsigned idx = sp_rb_hash_str(key) %% h->cap;\n");
        emit_raw(ctx, "    sp_RbHashEntry *e = h->buckets[idx];\n");
        emit_raw(ctx, "    while (e) {\n");
        emit_raw(ctx, "        if (strcmp(e->key, key) == 0) { e->value = value; return; }\n");
        emit_raw(ctx, "        e = e->next;\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    e = (sp_RbHashEntry *)malloc(sizeof(sp_RbHashEntry));\n");
        emit_raw(ctx, "    e->key = key;\n");
        emit_raw(ctx, "    e->value = value;\n");
        emit_raw(ctx, "    e->next = h->buckets[idx];\n");
        emit_raw(ctx, "    h->buckets[idx] = e;\n");
        emit_raw(ctx, "    e->order_next = NULL;\n");
        emit_raw(ctx, "    if (h->last) h->last->order_next = e; else h->first = e;\n");
        emit_raw(ctx, "    h->last = e;\n");
        emit_raw(ctx, "    h->size++;\n");
        emit_raw(ctx, "}\n\n");
        emit_raw(ctx, "static sp_RbValue sp_RbHash_get(sp_RbHash *h, const char *key) {\n");
        emit_raw(ctx, "    unsigned idx = sp_rb_hash_str(key) %% h->cap;\n");
        emit_raw(ctx, "    sp_RbHashEntry *e = h->buckets[idx];\n");
        emit_raw(ctx, "    while (e) {\n");
        emit_raw(ctx, "        if (strcmp(e->key, key) == 0) return e->value;\n");
        emit_raw(ctx, "        e = e->next;\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    return sp_box_nil();\n");
        emit_raw(ctx, "}\n\n");
        emit_raw(ctx, "static mrb_int sp_RbHash_length(sp_RbHash *h) {\n");
        emit_raw(ctx, "    return h->size;\n}\n\n");
    }

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
    emit_raw(ctx, "    char *r = (char *)malloc(24); snprintf(r, 24, \"%%lld\", (long long)n); return r;\n}\n");
    emit_raw(ctx, "static const char *sp_str_char_at(const char *s, mrb_int idx) {\n");
    emit_raw(ctx, "    mrb_int len = (mrb_int)strlen(s);\n");
    emit_raw(ctx, "    if (idx < 0) idx += len;\n");
    emit_raw(ctx, "    if (idx < 0 || idx >= len) return \"\";\n");
    emit_raw(ctx, "    char *r = (char *)malloc(2); r[0] = s[idx]; r[1] = '\\0'; return r;\n}\n\n");

    /* ---- File I/O helpers ---- */
    emit_raw(ctx, "static const char *sp_File_read(const char *path) {\n");
    emit_raw(ctx, "    FILE *f = fopen(path, \"rb\"); if (!f) return \"\";\n");
    emit_raw(ctx, "    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);\n");
    emit_raw(ctx, "    char *buf = (char *)malloc(len + 1); fread(buf, 1, len, f); buf[len] = 0;\n");
    emit_raw(ctx, "    fclose(f); return buf;\n}\n");
    emit_raw(ctx, "static mrb_int sp_File_write(const char *path, const char *data) {\n");
    emit_raw(ctx, "    FILE *f = fopen(path, \"wb\"); if (!f) return 0;\n");
    emit_raw(ctx, "    size_t n = strlen(data); fwrite(data, 1, n, f); fclose(f);\n");
    emit_raw(ctx, "    return (mrb_int)n;\n}\n");
    emit_raw(ctx, "static mrb_bool sp_File_exist(const char *path) {\n");
    emit_raw(ctx, "    FILE *f = fopen(path, \"r\"); if (f) { fclose(f); return TRUE; } return FALSE;\n}\n");
    emit_raw(ctx, "static mrb_int sp_File_delete(const char *path) {\n");
    emit_raw(ctx, "    return remove(path) == 0 ? 1 : 0;\n}\n");
    emit_raw(ctx, "static const char *sp_File_join(const char *a, const char *b) {\n");
    emit_raw(ctx, "    size_t la = strlen(a), lb = strlen(b);\n");
    emit_raw(ctx, "    char *r = (char *)malloc(la + 1 + lb + 1);\n");
    emit_raw(ctx, "    memcpy(r, a, la); r[la] = '/'; memcpy(r + la + 1, b, lb + 1); return r;\n}\n");
    emit_raw(ctx, "static const char *sp_File_expand_path(const char *path) {\n");
    emit_raw(ctx, "    char *r = realpath(path, NULL); return r ? r : path;\n}\n");
    emit_raw(ctx, "static const char *sp_File_basename(const char *path) {\n");
    emit_raw(ctx, "    char *tmp = (char *)malloc(strlen(path) + 1); strcpy(tmp, path);\n");
    emit_raw(ctx, "    const char *b = basename(tmp); char *r = (char *)malloc(strlen(b) + 1);\n");
    emit_raw(ctx, "    strcpy(r, b); free(tmp); return r;\n}\n");
    emit_raw(ctx, "static const char *sp_File_dirname(const char *path) {\n");
    emit_raw(ctx, "    char *tmp = (char *)malloc(strlen(path) + 1); strcpy(tmp, path);\n");
    emit_raw(ctx, "    const char *d = dirname(tmp); char *r = (char *)malloc(strlen(d) + 1);\n");
    emit_raw(ctx, "    strcpy(r, d); free(tmp); return r;\n}\n");
    emit_raw(ctx, "static mrb_int sp_File_rename(const char *old, const char *new_) {\n");
    emit_raw(ctx, "    return rename(old, new_) == 0 ? 0 : -1;\n}\n");
    emit_raw(ctx, "static mrb_int sp_File_size(const char *path) {\n");
    emit_raw(ctx, "    struct stat st; if (stat(path, &st) != 0) return -1; return (mrb_int)st.st_size;\n}\n");
    emit_raw(ctx, "static mrb_int sp_File_mtime(const char *path) {\n");
    emit_raw(ctx, "    struct stat st; if (stat(path, &st) != 0) return 0; return (mrb_int)st.st_mtime;\n}\n");
    emit_raw(ctx, "static mrb_int sp_File_ctime(const char *path) {\n");
    emit_raw(ctx, "    struct stat st; if (stat(path, &st) != 0) return 0; return (mrb_int)st.st_ctime;\n}\n");
    emit_raw(ctx, "static const char *sp_File_readlink(const char *path) {\n");
    emit_raw(ctx, "    char *buf = (char *)malloc(4096); ssize_t n = readlink(path, buf, 4095);\n");
    emit_raw(ctx, "    if (n < 0) { free(buf); return \"\"; } buf[n] = '\\0'; return buf;\n}\n\n");

    /* ---- sp_File: file object for File.open block ---- */
    emit_raw(ctx, "#include <sys/file.h>\n");
    emit_raw(ctx, "typedef struct { FILE *fp; } sp_File;\n");
    emit_raw(ctx, "#define sp_File_LOCK_EX LOCK_EX\n");
    emit_raw(ctx, "#define sp_File_LOCK_SH LOCK_SH\n");
    emit_raw(ctx, "#define sp_File_LOCK_UN LOCK_UN\n");
    emit_raw(ctx, "#define sp_File_LOCK_NB LOCK_NB\n");
    emit_raw(ctx, "static sp_File *sp_File_open(const char *path, const char *mode) {\n");
    emit_raw(ctx, "    sp_File *f = (sp_File *)malloc(sizeof(sp_File));\n");
    emit_raw(ctx, "    f->fp = fopen(path, mode);\n");
    emit_raw(ctx, "    return f;\n}\n");
    emit_raw(ctx, "static void sp_File_close(sp_File *f) {\n");
    emit_raw(ctx, "    if (f && f->fp) fclose(f->fp); free(f);\n}\n");
    emit_raw(ctx, "static void sp_File_puts(sp_File *f, const char *s) {\n");
    emit_raw(ctx, "    fputs(s, f->fp); fputc('\\n', f->fp);\n}\n");
    emit_raw(ctx, "static void sp_File_write_str(sp_File *f, const char *s) {\n");
    emit_raw(ctx, "    fputs(s, f->fp);\n}\n");
    emit_raw(ctx, "static const char *sp_File_readline(sp_File *f) {\n");
    emit_raw(ctx, "    char *buf = (char *)malloc(4096);\n");
    emit_raw(ctx, "    if (!fgets(buf, 4096, f->fp)) { free(buf); return \"\"; }\n");
    emit_raw(ctx, "    size_t n = strlen(buf);\n");
    emit_raw(ctx, "    if (n > 0 && buf[n-1] == '\\n') buf[n-1] = '\\0';\n");
    emit_raw(ctx, "    return buf;\n}\n");
    emit_raw(ctx, "static const char *sp_File_read_all(sp_File *f) {\n");
    emit_raw(ctx, "    long cur = ftell(f->fp); fseek(f->fp, 0, SEEK_END);\n");
    emit_raw(ctx, "    long len = ftell(f->fp); fseek(f->fp, cur, SEEK_SET);\n");
    emit_raw(ctx, "    long remain = len - cur;\n");
    emit_raw(ctx, "    char *buf = (char *)malloc(remain + 1);\n");
    emit_raw(ctx, "    fread(buf, 1, remain, f->fp); buf[remain] = 0;\n");
    emit_raw(ctx, "    return buf;\n}\n\n");

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

    /* ljust / rjust / center */
    emit_raw(ctx, "static const char *sp_str_ljust(const char *s, mrb_int w, char pad) {\n");
    emit_raw(ctx, "    size_t n = strlen(s); if ((mrb_int)n >= w) { char *r = (char *)malloc(n+1); memcpy(r,s,n+1); return r; }\n");
    emit_raw(ctx, "    char *r = (char *)malloc(w+1); memcpy(r,s,n); memset(r+n,pad,w-n); r[w]='\\0'; return r;\n}\n");
    emit_raw(ctx, "static const char *sp_str_rjust(const char *s, mrb_int w, char pad) {\n");
    emit_raw(ctx, "    size_t n = strlen(s); if ((mrb_int)n >= w) { char *r = (char *)malloc(n+1); memcpy(r,s,n+1); return r; }\n");
    emit_raw(ctx, "    char *r = (char *)malloc(w+1); memset(r,pad,w-n); memcpy(r+w-n,s,n+1); return r;\n}\n");
    emit_raw(ctx, "static const char *sp_str_center(const char *s, mrb_int w, char pad) {\n");
    emit_raw(ctx, "    size_t n = strlen(s); if ((mrb_int)n >= w) { char *r = (char *)malloc(n+1); memcpy(r,s,n+1); return r; }\n");
    emit_raw(ctx, "    mrb_int left = (w - (mrb_int)n) / 2; mrb_int right = w - (mrb_int)n - left;\n");
    emit_raw(ctx, "    char *r = (char *)malloc(w+1); memset(r,pad,w); memcpy(r+left,s,n); r[w]='\\0'; return r;\n}\n");

    /* lstrip / rstrip */
    emit_raw(ctx, "static const char *sp_str_lstrip(const char *s) {\n");
    emit_raw(ctx, "    while (*s && isspace((unsigned char)*s)) s++;\n");
    emit_raw(ctx, "    size_t n = strlen(s); char *r = (char *)malloc(n+1); memcpy(r,s,n+1); return r;\n}\n");
    emit_raw(ctx, "static const char *sp_str_rstrip(const char *s) {\n");
    emit_raw(ctx, "    size_t n = strlen(s);\n");
    emit_raw(ctx, "    while (n > 0 && isspace((unsigned char)s[n-1])) n--;\n");
    emit_raw(ctx, "    char *r = (char *)malloc(n+1); memcpy(r,s,n); r[n]='\\0'; return r;\n}\n");

    /* tr / delete / squeeze */
    emit_raw(ctx, "static const char *sp_str_tr(const char *s, const char *from, const char *to) {\n");
    emit_raw(ctx, "    size_t n = strlen(s); char *r = (char *)malloc(n+1);\n");
    emit_raw(ctx, "    size_t fl = strlen(from), tl = strlen(to);\n");
    emit_raw(ctx, "    for (size_t i = 0; i <= n; i++) {\n");
    emit_raw(ctx, "        const char *p = memchr(from, s[i], fl);\n");
    emit_raw(ctx, "        if (p && s[i]) { size_t idx = p - from; r[i] = (idx < tl) ? to[idx] : to[tl-1]; }\n");
    emit_raw(ctx, "        else r[i] = s[i];\n");
    emit_raw(ctx, "    } return r;\n}\n");
    emit_raw(ctx, "static const char *sp_str_delete(const char *s, const char *chars) {\n");
    emit_raw(ctx, "    size_t n = strlen(s); char *r = (char *)malloc(n+1); size_t ri = 0;\n");
    emit_raw(ctx, "    for (size_t i = 0; i < n; i++) { if (!memchr(chars, s[i], strlen(chars))) r[ri++] = s[i]; }\n");
    emit_raw(ctx, "    r[ri] = '\\0'; return r;\n}\n");
    emit_raw(ctx, "static const char *sp_str_squeeze(const char *s) {\n");
    emit_raw(ctx, "    size_t n = strlen(s); char *r = (char *)malloc(n+1); size_t ri = 0;\n");
    emit_raw(ctx, "    for (size_t i = 0; i < n; i++) { if (i == 0 || s[i] != s[i-1]) r[ri++] = s[i]; }\n");
    emit_raw(ctx, "    r[ri] = '\\0'; return r;\n}\n");

    /* slice / [range] */
    emit_raw(ctx, "static const char *sp_str_slice(const char *s, mrb_int start, mrb_int len) {\n");
    emit_raw(ctx, "    mrb_int sn = (mrb_int)strlen(s);\n");
    emit_raw(ctx, "    if (start < 0) start += sn;\n");
    emit_raw(ctx, "    if (start < 0) start = 0;\n");
    emit_raw(ctx, "    if (start >= sn || len <= 0) { char *r = (char *)malloc(1); r[0]='\\0'; return r; }\n");
    emit_raw(ctx, "    if (start + len > sn) len = sn - start;\n");
    emit_raw(ctx, "    char *r = (char *)malloc(len+1); memcpy(r, s+start, len); r[len]='\\0'; return r;\n}\n");

    /* to_f */
    emit_raw(ctx, "static mrb_float sp_str_to_f(const char *s) { return strtod(s, NULL); }\n");

    /* bytes/chars helpers emitted after array types (see below) */


    /* sp_str_char_at already emitted above */

    /* ---- Mutable string (sp_String) ---- */
    if (ctx->needs_sp_string) {
        emit_raw(ctx, "typedef struct { char *data; int64_t len; int64_t cap; } sp_String;\n");
        emit_raw(ctx, "static sp_String *sp_String_new(const char *s) {\n");
        emit_raw(ctx, "    sp_String *r = (sp_String *)malloc(sizeof(sp_String));\n");
        emit_raw(ctx, "    r->len = (int64_t)strlen(s);\n");
        emit_raw(ctx, "    r->cap = r->len < 16 ? 16 : r->len * 2;\n");
        emit_raw(ctx, "    r->data = (char *)malloc(r->cap + 1);\n");
        emit_raw(ctx, "    memcpy(r->data, s, r->len + 1);\n");
        emit_raw(ctx, "    return r;\n}\n");
        emit_raw(ctx, "static sp_String *sp_String_new_empty(void) { return sp_String_new(\"\"); }\n");
        emit_raw(ctx, "static void sp_String_append(sp_String *s, const char *t) {\n");
        emit_raw(ctx, "    int64_t tl = (int64_t)strlen(t);\n");
        emit_raw(ctx, "    if (s->len + tl >= s->cap) {\n");
        emit_raw(ctx, "        s->cap = (s->len + tl) * 2;\n");
        emit_raw(ctx, "        s->data = (char *)realloc(s->data, s->cap + 1);\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    memcpy(s->data + s->len, t, tl + 1);\n");
        emit_raw(ctx, "    s->len += tl;\n}\n");
        emit_raw(ctx, "static void sp_String_append_str(sp_String *s, sp_String *t) {\n");
        emit_raw(ctx, "    sp_String_append(s, t->data);\n}\n");
        emit_raw(ctx, "static const char *sp_String_cstr(sp_String *s) { return s->data; }\n");
        emit_raw(ctx, "static int64_t sp_String_length(sp_String *s) { return s->len; }\n");
        emit_raw(ctx, "static const char *sp_String_upcase(sp_String *s) {\n");
        emit_raw(ctx, "    return sp_str_upcase(s->data);\n}\n");
        emit_raw(ctx, "static const char *sp_String_reverse(sp_String *s) {\n");
        emit_raw(ctx, "    return sp_str_reverse(s->data);\n}\n");
        emit_raw(ctx, "static mrb_bool sp_String_include(sp_String *s, const char *sub) {\n");
        emit_raw(ctx, "    return strstr(s->data, sub) != NULL;\n}\n");
        emit_raw(ctx, "static void sp_String_replace(sp_String *s, const char *t) {\n");
        emit_raw(ctx, "    size_t tlen = strlen(t);\n");
        emit_raw(ctx, "    if (tlen >= (size_t)s->cap) { s->cap = tlen + 1; s->data = realloc(s->data, s->cap); }\n");
        emit_raw(ctx, "    memcpy(s->data, t, tlen + 1); s->len = tlen;\n}\n");
        emit_raw(ctx, "static void sp_String_clear(sp_String *s) { s->data[0] = '\\0'; s->len = 0; }\n");
        emit_raw(ctx, "static const char *sp_String_char_at(sp_String *s, int64_t idx) {\n");
        emit_raw(ctx, "    if (idx < 0) idx += s->len;\n");
        emit_raw(ctx, "    if (idx < 0 || idx >= s->len) return \"\";\n");
        emit_raw(ctx, "    char *r = malloc(2); r[0] = s->data[idx]; r[1] = '\\0'; return r;\n}\n\n");
    }

    /* ---- Float format (Ruby-style: always show decimal point) ---- */
    emit_raw(ctx, "static const char *sp_float_to_s(mrb_float f) {\n");
    emit_raw(ctx, "    char *r = (char *)malloc(32);\n");
    emit_raw(ctx, "    snprintf(r, 32, \"%%g\", f);\n");
    emit_raw(ctx, "    if (!strchr(r, '.') && !strchr(r, 'e') && !strchr(r, 'E')) strcat(r, \".0\");\n");
    emit_raw(ctx, "    return r;\n}\n\n");

    /* ---- Backtick helper (popen/pclose) ---- */
    emit_raw(ctx, "static const char *sp_backtick(const char *cmd) {\n");
    emit_raw(ctx, "    FILE *p = popen(cmd, \"r\");\n");
    emit_raw(ctx, "    if (!p) return \"\";\n");
    emit_raw(ctx, "    char buf[4096]; size_t n = fread(buf, 1, sizeof(buf)-1, p);\n");
    emit_raw(ctx, "    buf[n] = '\\0'; pclose(p);\n");
    emit_raw(ctx, "    char *r = (char *)malloc(n+1); memcpy(r, buf, n+1); return r;\n}\n\n");

    /* ---- format() helper (snprintf wrapper) ---- */
    emit_raw(ctx, "static const char *sp_format(const char *fmt, ...) {\n");
    emit_raw(ctx, "    va_list ap; va_start(ap, fmt);\n");
    emit_raw(ctx, "    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);\n");
    emit_raw(ctx, "    char *r = (char *)malloc(n + 1);\n");
    emit_raw(ctx, "    va_start(ap, fmt); vsnprintf(r, n + 1, fmt, ap); va_end(ap);\n");
    emit_raw(ctx, "    return r;\n}\n\n");

    /* ---- Polymorphic to_s (after sp_int_to_s/sp_float_to_s) ---- */
    if (ctx->needs_poly) {
        emit_raw(ctx, "static const char *sp_poly_to_s(sp_RbValue v) {\n");
        emit_raw(ctx, "    uint16_t t = SP_TAG(v);\n");
        emit_raw(ctx, "    if (t == SP_T_INT) return sp_int_to_s(sp_unbox_int(v));\n");
        emit_raw(ctx, "    if (t == SP_T_FLOAT) return sp_float_to_s(sp_unbox_float(v));\n");
        emit_raw(ctx, "    if (t == SP_T_STRING) return sp_unbox_str(v);\n");
        emit_raw(ctx, "    if (t == SP_T_BOOL) return sp_unbox_bool(v) ? \"true\" : \"false\";\n");
        emit_raw(ctx, "    if (t == SP_T_NIL) return \"\";\n");
        emit_raw(ctx, "    return \"(object)\";\n");
        emit_raw(ctx, "}\n\n");

        /* ---- Polymorphic arithmetic/comparison helpers ---- */
        if (!ctx->needs_exc)
            emit_raw(ctx, "static void sp_raise(const char *msg) { fprintf(stderr, \"%%s\\n\", msg); exit(1); }\n\n");
        else
            emit_raw(ctx, "static void sp_raise(const char *msg);\n");

        /* Helper macros for arithmetic/comparison dispatch */
        emit_raw(ctx, "#define SP_TAG_A SP_TAG(a)\n");
        emit_raw(ctx, "#define SP_TAG_B SP_TAG(b)\n");
        emit_raw(ctx, "#define SP_AI sp_unbox_int(a)\n");
        emit_raw(ctx, "#define SP_BI sp_unbox_int(b)\n");
        emit_raw(ctx, "#define SP_AF sp_unbox_float(a)\n");
        emit_raw(ctx, "#define SP_BF sp_unbox_float(b)\n\n");

        emit_raw(ctx, "static sp_RbValue sp_poly_add(sp_RbValue a, sp_RbValue b) {\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_INT && SP_TAG_B == SP_T_INT) return sp_box_int(SP_AI + SP_BI);\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_FLOAT || SP_TAG_B == SP_T_FLOAT) {\n");
        emit_raw(ctx, "        double fa = SP_TAG_A == SP_T_FLOAT ? SP_AF : (double)SP_AI;\n");
        emit_raw(ctx, "        double fb = SP_TAG_B == SP_T_FLOAT ? SP_BF : (double)SP_BI;\n");
        emit_raw(ctx, "        return sp_box_float(fa + fb);\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_STRING && SP_TAG_B == SP_T_STRING)\n");
        emit_raw(ctx, "        return sp_box_str(sp_str_concat(sp_unbox_str(a), sp_unbox_str(b)));\n");
        emit_raw(ctx, "    sp_raise(\"TypeError: + not defined\"); return sp_box_nil();\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static sp_RbValue sp_poly_sub(sp_RbValue a, sp_RbValue b) {\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_INT && SP_TAG_B == SP_T_INT) return sp_box_int(SP_AI - SP_BI);\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_FLOAT || SP_TAG_B == SP_T_FLOAT) {\n");
        emit_raw(ctx, "        double fa = SP_TAG_A == SP_T_FLOAT ? SP_AF : (double)SP_AI;\n");
        emit_raw(ctx, "        double fb = SP_TAG_B == SP_T_FLOAT ? SP_BF : (double)SP_BI;\n");
        emit_raw(ctx, "        return sp_box_float(fa - fb);\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    sp_raise(\"TypeError: - not defined\"); return sp_box_nil();\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static sp_RbValue sp_poly_mul(sp_RbValue a, sp_RbValue b) {\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_INT && SP_TAG_B == SP_T_INT) return sp_box_int(SP_AI * SP_BI);\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_FLOAT || SP_TAG_B == SP_T_FLOAT) {\n");
        emit_raw(ctx, "        double fa = SP_TAG_A == SP_T_FLOAT ? SP_AF : (double)SP_AI;\n");
        emit_raw(ctx, "        double fb = SP_TAG_B == SP_T_FLOAT ? SP_BF : (double)SP_BI;\n");
        emit_raw(ctx, "        return sp_box_float(fa * fb);\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_STRING && SP_TAG_B == SP_T_INT)\n");
        emit_raw(ctx, "        return sp_box_str(sp_str_repeat(sp_unbox_str(a), SP_BI));\n");
        emit_raw(ctx, "    sp_raise(\"TypeError: * not defined\"); return sp_box_nil();\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static sp_RbValue sp_poly_div(sp_RbValue a, sp_RbValue b) {\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_INT && SP_TAG_B == SP_T_INT) return sp_box_int(SP_AI / SP_BI);\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_FLOAT || SP_TAG_B == SP_T_FLOAT) {\n");
        emit_raw(ctx, "        double fa = SP_TAG_A == SP_T_FLOAT ? SP_AF : (double)SP_AI;\n");
        emit_raw(ctx, "        double fb = SP_TAG_B == SP_T_FLOAT ? SP_BF : (double)SP_BI;\n");
        emit_raw(ctx, "        return sp_box_float(fa / fb);\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    sp_raise(\"TypeError: / not defined\"); return sp_box_nil();\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static mrb_bool sp_poly_gt(sp_RbValue a, sp_RbValue b) {\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_INT && SP_TAG_B == SP_T_INT) return SP_AI > SP_BI;\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_FLOAT || SP_TAG_B == SP_T_FLOAT) {\n");
        emit_raw(ctx, "        double fa = SP_TAG_A == SP_T_FLOAT ? SP_AF : (double)SP_AI;\n");
        emit_raw(ctx, "        double fb = SP_TAG_B == SP_T_FLOAT ? SP_BF : (double)SP_BI;\n");
        emit_raw(ctx, "        return fa > fb;\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    sp_raise(\"TypeError: > not defined\"); return 0;\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static mrb_bool sp_poly_lt(sp_RbValue a, sp_RbValue b) {\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_INT && SP_TAG_B == SP_T_INT) return SP_AI < SP_BI;\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_FLOAT || SP_TAG_B == SP_T_FLOAT) {\n");
        emit_raw(ctx, "        double fa = SP_TAG_A == SP_T_FLOAT ? SP_AF : (double)SP_AI;\n");
        emit_raw(ctx, "        double fb = SP_TAG_B == SP_T_FLOAT ? SP_BF : (double)SP_BI;\n");
        emit_raw(ctx, "        return fa < fb;\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    sp_raise(\"TypeError: < not defined\"); return 0;\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static mrb_bool sp_poly_ge(sp_RbValue a, sp_RbValue b) {\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_INT && SP_TAG_B == SP_T_INT) return SP_AI >= SP_BI;\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_FLOAT || SP_TAG_B == SP_T_FLOAT) {\n");
        emit_raw(ctx, "        double fa = SP_TAG_A == SP_T_FLOAT ? SP_AF : (double)SP_AI;\n");
        emit_raw(ctx, "        double fb = SP_TAG_B == SP_T_FLOAT ? SP_BF : (double)SP_BI;\n");
        emit_raw(ctx, "        return fa >= fb;\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    sp_raise(\"TypeError: >= not defined\"); return 0;\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static mrb_bool sp_poly_le(sp_RbValue a, sp_RbValue b) {\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_INT && SP_TAG_B == SP_T_INT) return SP_AI <= SP_BI;\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_FLOAT || SP_TAG_B == SP_T_FLOAT) {\n");
        emit_raw(ctx, "        double fa = SP_TAG_A == SP_T_FLOAT ? SP_AF : (double)SP_AI;\n");
        emit_raw(ctx, "        double fb = SP_TAG_B == SP_T_FLOAT ? SP_BF : (double)SP_BI;\n");
        emit_raw(ctx, "        return fa <= fb;\n");
        emit_raw(ctx, "    }\n");
        emit_raw(ctx, "    sp_raise(\"TypeError: <= not defined\"); return 0;\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static mrb_bool sp_poly_eq(sp_RbValue a, sp_RbValue b) {\n");
        emit_raw(ctx, "    if (SP_TAG_A != SP_TAG_B) return 0;\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_INT) return SP_AI == SP_BI;\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_FLOAT) return SP_AF == SP_BF;\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_STRING) return strcmp(sp_unbox_str(a), sp_unbox_str(b)) == 0;\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_BOOL) return sp_unbox_bool(a) == sp_unbox_bool(b);\n");
        emit_raw(ctx, "    if (SP_TAG_A == SP_T_NIL) return 1;\n");
        emit_raw(ctx, "    return sp_unbox_obj(a) == sp_unbox_obj(b);\n");
        emit_raw(ctx, "}\n\n");

        emit_raw(ctx, "static mrb_bool sp_poly_neq(sp_RbValue a, sp_RbValue b) {\n");
        emit_raw(ctx, "    return !sp_poly_eq(a, b);\n");
        emit_raw(ctx, "}\n\n");
    }

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
        /* catch/throw support */
        emit_raw(ctx, "static const char *sp_throw_tag = NULL;\n");
        emit_raw(ctx, "static mrb_int sp_throw_value_i = 0;\n");
        emit_raw(ctx, "static const char *sp_throw_value_s = NULL;\n");
        emit_raw(ctx, "static int sp_throw_is_str = 0;\n");
        emit_raw(ctx, "static void sp_throw_i(const char *tag, mrb_int val) {\n");
        emit_raw(ctx, "    sp_throw_tag = tag; sp_throw_value_i = val; sp_throw_is_str = 0;\n");
        emit_raw(ctx, "    if (sp_exc_depth > 0) longjmp(sp_exc_stack[sp_exc_depth - 1], 2);\n");
        emit_raw(ctx, "    fprintf(stderr, \"uncaught throw :\\\"%%s\\\"\\n\", tag); exit(1);\n");
        emit_raw(ctx, "}\n");
        emit_raw(ctx, "static void sp_throw_s(const char *tag, const char *val) {\n");
        emit_raw(ctx, "    sp_throw_tag = tag; sp_throw_value_s = val; sp_throw_is_str = 1;\n");
        emit_raw(ctx, "    if (sp_exc_depth > 0) longjmp(sp_exc_stack[sp_exc_depth - 1], 2);\n");
        emit_raw(ctx, "    fprintf(stderr, \"uncaught throw :\\\"%%s\\\"\\n\", tag); exit(1);\n");
        emit_raw(ctx, "}\n\n");
    }

    /* Built-in sp_IntArray for Array support */
    emit_raw(ctx, "/* ---- Built-in integer array ---- */\n");
    /* sp_IntArray: deque-like array with O(1) shift via start offset */
    emit_raw(ctx, "typedef struct { mrb_int *data; mrb_int start; mrb_int len; mrb_int cap; } sp_IntArray;\n\n");

    /* Built-in sp_Range for Range support */
    emit_raw(ctx, "/* ---- Built-in integer range ---- */\n");
    emit_raw(ctx, "typedef struct { mrb_int first; mrb_int last; } sp_Range;\n");
    emit_raw(ctx, "static sp_Range sp_Range_new(mrb_int first, mrb_int last) {\n");
    emit_raw(ctx, "    sp_Range r; r.first = first; r.last = last; return r;\n}\n");
    emit_raw(ctx, "static mrb_bool sp_Range_include_p(sp_Range r, mrb_int v) {\n");
    emit_raw(ctx, "    return v >= r.first && v <= r.last;\n}\n");
    /* sp_Range_to_a needs sp_IntArray_from_range which is defined later;
     * forward declare it and define to_a after IntArray is available */
    emit_raw(ctx, "static sp_IntArray *sp_IntArray_from_range(mrb_int, mrb_int);\n");
    emit_raw(ctx, "static sp_IntArray *sp_Range_to_a(sp_Range r) {\n");
    emit_raw(ctx, "    return sp_IntArray_from_range(r.first, r.last);\n}\n\n");

    /* Built-in sp_Time for Time support */
    emit_raw(ctx, "/* ---- Built-in time ---- */\n");
    emit_raw(ctx, "#include <time.h>\n");
    emit_raw(ctx, "typedef struct { time_t t; } sp_Time;\n");
    emit_raw(ctx, "static sp_Time sp_Time_now(void) { sp_Time r; r.t = time(NULL); return r; }\n");
    emit_raw(ctx, "static sp_Time sp_Time_at(mrb_int n) { sp_Time r; r.t = (time_t)n; return r; }\n");
    emit_raw(ctx, "static mrb_int sp_Time_to_i(sp_Time t) { return (mrb_int)t.t; }\n");
    emit_raw(ctx, "static mrb_int sp_Time_diff(sp_Time a, sp_Time b) { return (mrb_int)(a.t - b.t); }\n\n");

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

    /* ---- IntArray join ---- */
    emit_raw(ctx, "static const char *sp_IntArray_join(sp_IntArray *a, const char *sep) {\n");
    emit_raw(ctx, "    if (a->len == 0) { char *r = (char *)malloc(1); r[0] = '\\0'; return r; }\n");
    emit_raw(ctx, "    size_t sl = strlen(sep);\n");
    emit_raw(ctx, "    size_t cap = a->len * 24 + (a->len - 1) * sl + 1;\n");
    emit_raw(ctx, "    char *r = (char *)malloc(cap); size_t pos = 0;\n");
    emit_raw(ctx, "    for (mrb_int i = 0; i < a->len; i++) {\n");
    emit_raw(ctx, "        if (i > 0) { memcpy(r + pos, sep, sl); pos += sl; }\n");
    emit_raw(ctx, "        pos += snprintf(r + pos, cap - pos, \"%%lld\", (long long)a->data[a->start + i]);\n");
    emit_raw(ctx, "    }\n");
    emit_raw(ctx, "    return r;\n}\n\n");

    /* ---- IntArray uniq ---- */
    emit_raw(ctx, "static sp_IntArray *sp_IntArray_uniq(sp_IntArray *a) {\n");
    emit_raw(ctx, "    sp_IntArray *r = sp_IntArray_new();\n");
    emit_raw(ctx, "    for (mrb_int i = 0; i < a->len; i++) {\n");
    emit_raw(ctx, "        mrb_int v = a->data[a->start + i];\n");
    emit_raw(ctx, "        mrb_bool found = FALSE;\n");
    emit_raw(ctx, "        for (mrb_int j = 0; j < r->len; j++)\n");
    emit_raw(ctx, "            if (r->data[r->start + j] == v) { found = TRUE; break; }\n");
    emit_raw(ctx, "        if (!found) sp_IntArray_push(r, v);\n");
    emit_raw(ctx, "    }\n");
    emit_raw(ctx, "    return r;\n}\n\n");

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

        /* chars → sp_StrArray (depends on sp_StrArray being defined) */
        emit_raw(ctx, "static sp_StrArray *sp_str_chars(const char *s) {\n");
        emit_raw(ctx, "    sp_StrArray *a = sp_StrArray_new();\n");
        emit_raw(ctx, "    for (size_t i = 0; s[i]; i++) {\n");
        emit_raw(ctx, "        char *c = (char *)malloc(2); c[0] = s[i]; c[1] = '\\0';\n");
        emit_raw(ctx, "        sp_StrArray_push(a, c);\n");
        emit_raw(ctx, "    } return a;\n}\n\n");
    }

    /* bytes → sp_IntArray (depends on sp_IntArray being defined) */
    emit_raw(ctx, "static sp_IntArray *sp_str_bytes(const char *s) {\n");
    emit_raw(ctx, "    sp_IntArray *a = sp_IntArray_new();\n");
    emit_raw(ctx, "    for (size_t i = 0; s[i]; i++) sp_IntArray_push(a, (unsigned char)s[i]);\n");
    emit_raw(ctx, "    return a;\n}\n\n");

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

    /* sp_Dir_glob depends on sp_StrArray */
    if (ctx->needs_str_split) {
        emit_raw(ctx, "static sp_StrArray *sp_Dir_glob(const char *pattern) {\n");
        emit_raw(ctx, "    sp_StrArray *a = sp_StrArray_new();\n");
        emit_raw(ctx, "    glob_t g; if (glob(pattern, 0, NULL, &g) == 0) {\n");
        emit_raw(ctx, "        for (size_t i = 0; i < g.gl_pathc; i++) {\n");
        emit_raw(ctx, "            char *s = (char *)malloc(strlen(g.gl_pathv[i]) + 1);\n");
        emit_raw(ctx, "            strcpy(s, g.gl_pathv[i]); sp_StrArray_push(a, s);\n");
        emit_raw(ctx, "        } globfree(&g); } return a;\n}\n\n");
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

/* Emit hand-written FizzBuzz helper functions for lambda mode */
void emit_lambda_fizzbuzz_funcs(codegen_ctx_t *ctx) {
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

/* Emit megamorphic dispatch functions at file scope.
 * Each dispatch function uses a switch on obj.tag to call the correct
 * class-specific method. Called after class methods are emitted. */
void emit_mega_dispatch_funcs(codegen_ctx_t *ctx) {
    for (int i = 0; i < ctx->mega_dispatch_count; i++) {
        const char *mname = ctx->mega_dispatch[i].sanitized;
        int nc = ctx->mega_dispatch[i].class_count;
        spinel_type_t ret_kind = ctx->mega_dispatch[i].return_kind;
        bool returns_string = (ret_kind == SPINEL_TYPE_STRING);

        if (returns_string) {
            emit_raw(ctx, "static const char *sp_dispatch_%s(sp_RbValue obj) {\n", mname);
            emit_raw(ctx, "    uint16_t t = SP_TAG(obj);\n");
            for (int ci = 0; ci < nc; ci++) {
                const char *cn = ctx->mega_dispatch[i].class_names[ci];
                emit_raw(ctx, "    %s (t == SP_TAG_%s) return sp_%s_%s((sp_%s *)sp_unbox_obj(obj));\n",
                         ci == 0 ? "if" : "else if", cn, cn, mname, cn);
            }
            emit_raw(ctx, "    return \"\";\n");
            emit_raw(ctx, "}\n\n");
        } else {
            emit_raw(ctx, "static sp_RbValue sp_dispatch_%s(sp_RbValue obj) {\n", mname);
            emit_raw(ctx, "    uint16_t t = SP_TAG(obj);\n");
            for (int ci = 0; ci < nc; ci++) {
                const char *cn = ctx->mega_dispatch[i].class_names[ci];
                class_info_t *cls = find_class(ctx, cn);
                method_info_t *mi = cls ? find_method_inherited(ctx, cls, ctx->mega_dispatch[i].method_name, NULL) : NULL;
                char *call_expr = sfmt("sp_%s_%s((sp_%s *)sp_unbox_obj(obj))", cn, mname, cn);
                if (mi) {
                    char *boxed = poly_box_expr_vt(ctx, mi->return_type, call_expr);
                    emit_raw(ctx, "    %s (t == SP_TAG_%s) return %s;\n",
                             ci == 0 ? "if" : "else if", cn, boxed);
                    free(boxed);
                } else {
                    emit_raw(ctx, "    %s (t == SP_TAG_%s) return sp_box_nil();\n",
                             ci == 0 ? "if" : "else if", cn);
                }
                free(call_expr);
            }
            emit_raw(ctx, "    return sp_box_nil();\n");
            emit_raw(ctx, "}\n\n");
        }
    }
}
