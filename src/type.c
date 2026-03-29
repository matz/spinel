/* type.c - Spinel AOT: type system, inference, and resolution */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <assert.h>
#include "codegen.h"

vtype_t vt_prim(spinel_type_t k) {
  vtype_t t = {k, ""};
  return t;
}

vtype_t vt_obj(const char *klass) {
  vtype_t t;
  t.kind = SPINEL_TYPE_OBJECT;
  snprintf(t.klass, sizeof(t.klass), "%s", klass);
  return t;
}

bool vt_is_numeric(vtype_t t) {
  return t.kind == SPINEL_TYPE_INTEGER || t.kind == SPINEL_TYPE_FLOAT;
}

/* Check if type is a simple scalar that can participate in POLY widening */
bool vt_is_poly_eligible(vtype_t t) {
  return t.kind == SPINEL_TYPE_INTEGER || t.kind == SPINEL_TYPE_FLOAT ||
      t.kind == SPINEL_TYPE_STRING || t.kind == SPINEL_TYPE_BOOLEAN ||
      t.kind == SPINEL_TYPE_NIL || t.kind == SPINEL_TYPE_OBJECT;
}


/* Wrap an expression in a boxing call when assigning/passing to a POLY slot.
 * Returns a newly-allocated string like "sp_box_int(42)".
 * If the source type is already POLY, returns a copy of expr unchanged.
 * The ctx-aware version handles OBJECT types with per-class tags. */
char *poly_box_expr_vt(codegen_ctx_t *ctx, vtype_t src, const char *expr) {
  switch (src.kind) {
  case SPINEL_TYPE_INTEGER: return sfmt("sp_box_int(%s)", expr);
  case SPINEL_TYPE_FLOAT:   return sfmt("sp_box_float(%s)", expr);
  case SPINEL_TYPE_STRING:  return sfmt("sp_box_str(%s)", expr);
  case SPINEL_TYPE_BOOLEAN: return sfmt("sp_box_bool(%s)", expr);
  case SPINEL_TYPE_NIL:     return xstrdup("sp_box_nil()");
  case SPINEL_TYPE_POLY:    return xstrdup(expr);
  case SPINEL_TYPE_OBJECT: {
    if (ctx) {
      class_info_t *cls = find_class(ctx, src.klass);
      if (cls)
        return sfmt("sp_box_obj(SP_TAG_%s, %s)", cls->name, expr);
    }
    return sfmt("sp_box_obj(SP_T_OBJECT, %s)", expr);
  }
  default:                  return sfmt("sp_box_int((int64_t)%s)", expr); /* best-effort */
  }
}

/* Convenience wrapper for callers that only have a kind (non-OBJECT) */
char *poly_box_expr(spinel_type_t src_kind, const char *expr) {
  vtype_t vt; vt.kind = src_kind; vt.klass[0] = '\0';
  return poly_box_expr_vt(NULL, vt, expr);
}

const char *spinel_type_cname(spinel_type_t k) {
  switch (k) {
  case SPINEL_TYPE_INTEGER: return "mrb_int";
  case SPINEL_TYPE_FLOAT:   return "mrb_float";
  case SPINEL_TYPE_BOOLEAN: return "mrb_bool";
  case SPINEL_TYPE_STRING:  return "const char *";
  case SPINEL_TYPE_ARRAY:   return "sp_IntArray *";
  case SPINEL_TYPE_FLOAT_ARRAY: return "sp_FloatArray *";
  case SPINEL_TYPE_HASH:    return "sp_StrIntHash *";
  case SPINEL_TYPE_PROC:    return "sp_Val *";
  case SPINEL_TYPE_POLY:    return "sp_RbValue";
  case SPINEL_TYPE_STR_ARRAY: return "sp_StrArray *";
  case SPINEL_TYPE_RANGE:   return "sp_Range";
  case SPINEL_TYPE_TIME:    return "sp_Time";
  case SPINEL_TYPE_RB_ARRAY: return "sp_RbArray *";
  case SPINEL_TYPE_RB_HASH:  return "sp_RbHash *";
  case SPINEL_TYPE_SP_STRING: return "sp_String *";
  case SPINEL_TYPE_FILE:     return "sp_File *";
  case SPINEL_TYPE_STRINGIO: return "sp_StringIO *";
  default:                  return "mrb_int"; /* fallback for standalone mode */
  }
}

/* ------------------------------------------------------------------ */
/* vt_ctype implementation                                            */
/* ------------------------------------------------------------------ */

char *vt_ctype(codegen_ctx_t *ctx, vtype_t t, bool as_ptr) {
  if (t.kind == SPINEL_TYPE_OBJECT) {
    class_info_t *cls = find_class(ctx, t.klass);
    if (cls && cls->is_value_type)
      return sfmt("sp_%s", t.klass);
    if (as_ptr)
      return sfmt("sp_%s *", t.klass);
    return sfmt("sp_%s", t.klass);
  }
  /* In non-lambda mode, PROC maps to sp_Proc * instead of sp_Val * */
  if (t.kind == SPINEL_TYPE_PROC && !ctx->lambda_mode)
    return xstrdup("sp_Proc *");
  return xstrdup(spinel_type_cname(t.kind));
}

/* ------------------------------------------------------------------ */
/* GC helpers                                                         */
/* ------------------------------------------------------------------ */

/* Return true if a variable of this type needs GC rooting */
bool is_gc_type(codegen_ctx_t *ctx, vtype_t t) {
  if (t.kind == SPINEL_TYPE_ARRAY) return true;
  if (t.kind == SPINEL_TYPE_FLOAT_ARRAY) return true;
  if (t.kind == SPINEL_TYPE_HASH) return true;
  if (t.kind == SPINEL_TYPE_STRINGIO) return true;
  if (t.kind == SPINEL_TYPE_OBJECT) {
    class_info_t *cls = find_class(ctx, t.klass);
    return cls && !cls->is_value_type;
  }
  return false;
}

/* ------------------------------------------------------------------ */
/* Pass 2: Type inference                                             */
/* ------------------------------------------------------------------ */

vtype_t binop_result(vtype_t l, vtype_t r, const char *op) {
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

vtype_t infer_type(codegen_ctx_t *ctx, pm_node_t *node) {
  if (!node) return vt_prim(SPINEL_TYPE_NIL);

  switch (PM_NODE_TYPE(node)) {
  case PM_INTEGER_NODE:  return vt_prim(SPINEL_TYPE_INTEGER);
  case PM_FLOAT_NODE:    return vt_prim(SPINEL_TYPE_FLOAT);
  case PM_STRING_NODE:
  case PM_INTERPOLATED_STRING_NODE:
  case PM_SYMBOL_NODE:
  case PM_X_STRING_NODE:
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

  case PM_IT_LOCAL_VARIABLE_READ_NODE: {
    /* 'it' keyword → same as _1 */
    var_entry_t *v = var_lookup(ctx, "_1");
    return v ? v->type : vt_prim(SPINEL_TYPE_VALUE);
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

  case PM_CONSTANT_PATH_NODE: {
    /* Module::CONST — look up the constant in the module */
    pm_constant_path_node_t *cp = (pm_constant_path_node_t *)node;
    if (cp->parent && PM_NODE_TYPE(cp->parent) == PM_CONSTANT_READ_NODE) {
      pm_constant_read_node_t *p = (pm_constant_read_node_t *)cp->parent;
      char *mod_name = cstr(ctx, p->name);
      char *child_name = cstr(ctx, cp->name);
      /* Float::INFINITY, Float::NAN */
      if (strcmp(mod_name, "Float") == 0 &&
        (strcmp(child_name, "INFINITY") == 0 || strcmp(child_name, "NAN") == 0)) {
        free(mod_name); free(child_name);
        return vt_prim(SPINEL_TYPE_FLOAT);
      }
      module_info_t *mod = find_module(ctx, mod_name);
      if (mod) {
        for (int i = 0; i < mod->const_count; i++) {
          if (strcmp(mod->consts[i].name, child_name) == 0) {
            vtype_t t = mod->consts[i].type;
            free(mod_name); free(child_name);
            return t;
          }
        }
      }
      free(mod_name); free(child_name);
    }
    return vt_prim(SPINEL_TYPE_VALUE);
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

  case PM_INSTANCE_VARIABLE_OR_WRITE_NODE: {
    /* @x ||= expr — type is the type of the value expression */
    pm_instance_variable_or_write_node_t *n = (pm_instance_variable_or_write_node_t *)node;
    return infer_type(ctx, n->value);
  }

  case PM_CALL_NODE: {
    pm_call_node_t *call = (pm_call_node_t *)node;
    char *method = cstr(ctx, call->name);
    vtype_t result = vt_prim(SPINEL_TYPE_VALUE);

    /* N.times.map { block } → IntArray */
    if (strcmp(method, "map") == 0 && call->receiver &&
      PM_NODE_TYPE(call->receiver) == PM_CALL_NODE) {
      pm_call_node_t *inner = (pm_call_node_t *)call->receiver;
      if (ceq(ctx, inner->name, "times")) {
        free(method); return vt_prim(SPINEL_TYPE_ARRAY);
      }
    }

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

    /* ENV['KEY'] → STRING (getenv) */
    if (strcmp(method, "[]") == 0 && call->receiver &&
      PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
      pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
      if (ceq(ctx, cr->name, "ENV")) {
        free(method);
        return vt_prim(SPINEL_TYPE_STRING);
      }
    }

    /* Dir.home → STRING, Dir.glob → STR_ARRAY */
    if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
      pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
      if (ceq(ctx, cr->name, "Dir")) {
        if (strcmp(method, "home") == 0) { free(method); return vt_prim(SPINEL_TYPE_STRING); }
        if (strcmp(method, "glob") == 0) { free(method); return vt_prim(SPINEL_TYPE_STR_ARRAY); }
      }
    }

    /* Time.now / Time.at → SPINEL_TYPE_TIME */
    if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
      pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
      if (ceq(ctx, cr->name, "Time") &&
        (strcmp(method, "now") == 0 || strcmp(method, "at") == 0)) {
        free(method);
        return vt_prim(SPINEL_TYPE_TIME);
      }
    }

    /* File.open → SPINEL_TYPE_FILE */
    if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
      pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
      if (ceq(ctx, cr->name, "File") && strcmp(method, "open") == 0) {
        free(method);
        return vt_prim(SPINEL_TYPE_FILE);
      }
      /* StringIO.new → SPINEL_TYPE_STRINGIO */
      if (ceq(ctx, cr->name, "StringIO") && strcmp(method, "new") == 0) {
        free(method);
        return vt_prim(SPINEL_TYPE_STRINGIO);
      }
    }

    /* Constructor: ClassName.new(...) — check early before binary ops */
    if (strcmp(method, "new") == 0 && call->receiver &&
      PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
      pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
      char *cls_name = cstr(ctx, cr->name);
      if (strcmp(cls_name, "Array") == 0) {
        /* Array.new(n, val): if val is Float, use FloatArray */
        if (call->arguments && call->arguments->arguments.size == 2) {
          vtype_t vt = infer_type(ctx, call->arguments->arguments.nodes[1]);
          if (vt.kind == SPINEL_TYPE_FLOAT) {
            free(cls_name); free(method);
            return vt_prim(SPINEL_TYPE_FLOAT_ARRAY);
          }
        }
        /* Array.new(n) { block } — type depends on block body */
        if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
          pm_block_node_t *blk = (pm_block_node_t *)call->block;
          if (blk->body) {
            pm_node_t *body = (pm_node_t *)blk->body;
            pm_node_t *last = body;
            if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
              pm_statements_node_t *stmts = (pm_statements_node_t *)body;
              if (stmts->body.size > 0)
                last = stmts->body.nodes[stmts->body.size - 1];
            }
            vtype_t bt = infer_type(ctx, last);
            if (bt.kind == SPINEL_TYPE_FLOAT) {
              free(cls_name); free(method);
              return vt_prim(SPINEL_TYPE_FLOAT_ARRAY);
            }
          }
          free(cls_name); free(method);
          return vt_prim(SPINEL_TYPE_ARRAY);
        }
        free(cls_name); free(method);
        return vt_prim(SPINEL_TYPE_ARRAY);
      }
      if (find_class(ctx, cls_name)) {
        result = vt_obj(cls_name);
        free(cls_name); free(method);
        return result;
      }
      free(cls_name);
    }

    /* Binary operators — only for recognized operator method names */
    if (call->receiver && call->arguments &&
      call->arguments->arguments.size == 1 &&
      strcmp(method, "[]") != 0 &&
      (strcmp(method, "+") == 0 || strcmp(method, "-") == 0 ||
       strcmp(method, "*") == 0 || strcmp(method, "/") == 0 ||
       strcmp(method, "%") == 0 || strcmp(method, "**") == 0 ||
       strcmp(method, "<") == 0 || strcmp(method, ">") == 0 ||
       strcmp(method, "<=") == 0 || strcmp(method, ">=") == 0 ||
       strcmp(method, "==") == 0 || strcmp(method, "!=") == 0 ||
       strcmp(method, "<=>") == 0 ||
       strcmp(method, "<<") == 0 || strcmp(method, ">>") == 0 ||
       strcmp(method, "|") == 0 || strcmp(method, "&") == 0 ||
       strcmp(method, "^") == 0 || strcmp(method, "=~") == 0)) {
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
      /* POLY binary ops: comparisons → BOOLEAN, arithmetic → POLY */
      if (lt.kind == SPINEL_TYPE_POLY || rt.kind == SPINEL_TYPE_POLY) {
        if (strcmp(method, ">") == 0 || strcmp(method, "<") == 0 ||
          strcmp(method, ">=") == 0 || strcmp(method, "<=") == 0 ||
          strcmp(method, "==") == 0 || strcmp(method, "!=") == 0) {
          free(method);
          return vt_prim(SPINEL_TYPE_BOOLEAN);
        }
        free(method);
        return vt_prim(SPINEL_TYPE_POLY);
      }
      if (vt_is_numeric(lt) || vt_is_numeric(rt) ||
        lt.kind == SPINEL_TYPE_BOOLEAN || rt.kind == SPINEL_TYPE_BOOLEAN ||
        (lt.kind == SPINEL_TYPE_VALUE && rt.kind == SPINEL_TYPE_VALUE)) {
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

    /* Built-in type methods: table-driven lookup with special-case side effects */
    if (call->receiver) {
      vtype_t recv_t = infer_type(ctx, call->receiver);

      /* Side effects: some methods need to set needs_* flags */
      if (recv_t.kind == SPINEL_TYPE_STRING) {
        if (strcmp(method, "dup") == 0) ctx->needs_sp_string = true;
        if (strcmp(method, "bytes") == 0) { ctx->needs_intarray = ctx->needs_gc = true; }
      }

      /* Table-driven return type lookup */
      spinel_type_t ret = builtin_return_type(recv_t.kind, method);
      if (ret != SPINEL_TYPE_UNKNOWN) {
        free(method);
        return vt_prim(ret);
      }
      /* Extension methods on built-in types (open class support) */
      {
        method_info_t *em = find_ext_method(ctx, recv_t.kind, method);
        if (em) {
          vtype_t rt = em->return_type;
          free(method);
          return rt;
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
          /* Array.new(n, val): if val is Float, use FloatArray */
          if (call->arguments && call->arguments->arguments.size == 2) {
            vtype_t vt = infer_type(ctx, call->arguments->arguments.nodes[1]);
            if (vt.kind == SPINEL_TYPE_FLOAT) {
              free(cls_name); free(method);
              return vt_prim(SPINEL_TYPE_FLOAT_ARRAY);
            }
          }
          /* Array.new(n) { block } — type depends on block body */
          if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
            pm_block_node_t *blk = (pm_block_node_t *)call->block;
            if (blk->body) {
              pm_node_t *body = (pm_node_t *)blk->body;
              pm_node_t *last = body;
              if (PM_NODE_TYPE(body) == PM_STATEMENTS_NODE) {
                pm_statements_node_t *stmts = (pm_statements_node_t *)body;
                if (stmts->body.size > 0)
                  last = stmts->body.nodes[stmts->body.size - 1];
              }
              vtype_t bt = infer_type(ctx, last);
              if (bt.kind == SPINEL_TYPE_FLOAT) {
                free(cls_name); free(method);
                return vt_prim(SPINEL_TYPE_FLOAT_ARRAY);
              }
            }
            free(cls_name); free(method);
            return vt_prim(SPINEL_TYPE_ARRAY);
          }
          free(cls_name); free(method);
          return vt_prim(SPINEL_TYPE_ARRAY);
        }
        if (strcmp(cls_name, "Hash") == 0) {
          free(cls_name); free(method);
          return vt_prim(SPINEL_TYPE_HASH);
        }
        if (find_class(ctx, cls_name)) {
          result = vt_obj(cls_name);
          free(cls_name); free(method);
          return result;
        }
        free(cls_name);
      }
      /* Module::ClassName.new(...) — extract last component */
      if (PM_NODE_TYPE(call->receiver) == PM_CONSTANT_PATH_NODE) {
        pm_constant_path_node_t *cp = (pm_constant_path_node_t *)call->receiver;
        char *cls_name = cstr(ctx, cp->name);
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
      if (strcmp(mod_name, "File") == 0) {
        ctx->needs_file_io = true;
        if (strcmp(method, "read") == 0 || strcmp(method, "join") == 0 ||
          strcmp(method, "expand_path") == 0 || strcmp(method, "basename") == 0 ||
          strcmp(method, "dirname") == 0 || strcmp(method, "readlink") == 0) {
          free(mod_name); free(method); return vt_prim(SPINEL_TYPE_STRING);
        }
        if (strcmp(method, "exist?") == 0 || strcmp(method, "exists?") == 0) { free(mod_name); free(method); return vt_prim(SPINEL_TYPE_BOOLEAN); }
        if (strcmp(method, "write") == 0 || strcmp(method, "delete") == 0 ||
          strcmp(method, "rename") == 0 || strcmp(method, "size") == 0 ||
          strcmp(method, "mtime") == 0 || strcmp(method, "ctime") == 0 ||
          strcmp(method, "stat") == 0) {
          free(mod_name); free(method); return vt_prim(SPINEL_TYPE_INTEGER);
        }
      }
      if (strcmp(mod_name, "Dir") == 0) {
        if (strcmp(method, "glob") == 0) { free(mod_name); free(method); return vt_prim(SPINEL_TYPE_STR_ARRAY); }
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
      /* POLY receiver: check known class set for return type */
      if (recv_t.kind == SPINEL_TYPE_POLY &&
        PM_NODE_TYPE(call->receiver) == PM_LOCAL_VARIABLE_READ_NODE) {
        pm_local_variable_read_node_t *lv = (pm_local_variable_read_node_t *)call->receiver;
        char *vname = cstr(ctx, lv->name);
        func_info_t *cur_func = find_func(ctx, ctx->current_func_name);
        if (cur_func) {
          for (int pi = 0; pi < cur_func->param_count; pi++) {
            if (strcmp(cur_func->params[pi].name, vname) != 0) continue;
            char classes[MAX_POLY_CLASSES][64];
            int nclasses = poly_class_get(ctx, cur_func->name, pi, classes);
            if (nclasses >= 1) {
              class_info_t *cls0 = find_class(ctx, classes[0]);
              if (cls0) {
                method_info_t *m0 = find_method_inherited(ctx, cls0, method, NULL);
                if (m0) { free(vname); free(method); return m0->return_type; }
              }
            }
          }
        }
        free(vname);
      }
    }

    /* $stdin.getc → INTEGER (char code) */
    if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_GLOBAL_VARIABLE_READ_NODE) {
      pm_global_variable_read_node_t *gv = (pm_global_variable_read_node_t *)call->receiver;
      char *gname = cstr(ctx, gv->name);
      if (strcmp(gname, "$stdin") == 0 && strcmp(method, "getc") == 0) {
        free(gname); free(method); return vt_prim(SPINEL_TYPE_INTEGER);
      }
      /* $?.success? → BOOLEAN */
      if (strcmp(gname, "$?") == 0 && strcmp(method, "success?") == 0) {
        free(gname); free(method); return vt_prim(SPINEL_TYPE_BOOLEAN);
      }
      free(gname);
    }

    /* Receiver-less call in class context → implicit self method (with inheritance) */
    if (!call->receiver && ctx->current_class) {
      method_info_t *cm = find_method_inherited(ctx, ctx->current_class, method, NULL);
      if (cm) { free(method); return cm->return_type; }
    }

    /* Receiver-less call inside extension method body → implicit self dispatch */
    if (!call->receiver && ctx->ext_method_recv_type != SPINEL_TYPE_UNKNOWN) {
      spinel_type_t ert = ctx->ext_method_recv_type;
      if (ert == SPINEL_TYPE_STRING) {
        if (strcmp(method, "upcase") == 0 || strcmp(method, "downcase") == 0 ||
          strcmp(method, "reverse") == 0 || strcmp(method, "strip") == 0 ||
          strcmp(method, "chomp") == 0 || strcmp(method, "capitalize") == 0 ||
          strcmp(method, "to_s") == 0) {
          free(method); return vt_prim(SPINEL_TYPE_STRING);
        }
        if (strcmp(method, "length") == 0 || strcmp(method, "size") == 0 ||
          strcmp(method, "to_i") == 0) {
          free(method); return vt_prim(SPINEL_TYPE_INTEGER);
        }
        if (strcmp(method, "to_f") == 0) {
          free(method); return vt_prim(SPINEL_TYPE_FLOAT);
        }
        if (strcmp(method, "empty?") == 0) {
          free(method); return vt_prim(SPINEL_TYPE_BOOLEAN);
        }
      }
      if (ert == SPINEL_TYPE_INTEGER) {
        if (strcmp(method, "abs") == 0 || strcmp(method, "succ") == 0) {
          free(method); return vt_prim(SPINEL_TYPE_INTEGER);
        }
        if (strcmp(method, "even?") == 0 || strcmp(method, "odd?") == 0 ||
          strcmp(method, "zero?") == 0) {
          free(method); return vt_prim(SPINEL_TYPE_BOOLEAN);
        }
        if (strcmp(method, "to_s") == 0) {
          free(method); return vt_prim(SPINEL_TYPE_STRING);
        }
        if (strcmp(method, "to_f") == 0) {
          free(method); return vt_prim(SPINEL_TYPE_FLOAT);
        }
      }
      if (ert == SPINEL_TYPE_FLOAT) {
        if (strcmp(method, "abs") == 0) {
          free(method); return vt_prim(SPINEL_TYPE_FLOAT);
        }
        if (strcmp(method, "to_i") == 0) {
          free(method); return vt_prim(SPINEL_TYPE_INTEGER);
        }
        if (strcmp(method, "to_s") == 0) {
          free(method); return vt_prim(SPINEL_TYPE_STRING);
        }
      }
      /* Also check registered extension methods on this type */
      {
        method_info_t *em = find_ext_method(ctx, ert, method);
        if (em) { free(method); return em->return_type; }
      }
    }

    /* Receiver-less call to top-level function */
    if (!call->receiver) {
      func_info_t *fn = find_func(ctx, method);
      if (fn && fn->return_type.kind != SPINEL_TYPE_VALUE) {
        free(method); return fn->return_type;
      }
    }

    /* method(:name) → SPINEL_TYPE_PROC */
    if (!call->receiver && strcmp(method, "method") == 0) {
      free(method); return vt_prim(SPINEL_TYPE_PROC);
    }

    /* proc {} or Proc.new {} → SPINEL_TYPE_PROC */
    if (!call->receiver && strcmp(method, "proc") == 0 &&
      call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
      free(method);
      return vt_prim(SPINEL_TYPE_PROC);
    }
    if (call->receiver && PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE &&
      strcmp(method, "new") == 0) {
      pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
      if (ceq(ctx, cr->name, "Proc") &&
        call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
        free(method);
        return vt_prim(SPINEL_TYPE_PROC);
      }
    }

    /* Receiver-less method(:name) → PROC */
    if (!call->receiver && strcmp(method, "method") == 0) {
      free(method); return vt_prim(SPINEL_TYPE_PROC);
    }
    /* Receiver-less rand(n) → INTEGER */
    if (!call->receiver && strcmp(method, "rand") == 0) {
      free(method); return vt_prim(SPINEL_TYPE_INTEGER);
    }

    /* system("cmd") → BOOLEAN (true if exit 0) */
    if (!call->receiver && strcmp(method, "system") == 0) {
      free(method); return vt_prim(SPINEL_TYPE_BOOLEAN);
    }

    /* format("fmt", args...) → STRING */
    if (!call->receiver && (strcmp(method, "format") == 0 || strcmp(method, "sprintf") == 0)) {
      free(method); return vt_prim(SPINEL_TYPE_STRING);
    }

    /* trap → NIL */
    if (!call->receiver && strcmp(method, "trap") == 0) {
      free(method); return vt_prim(SPINEL_TYPE_NIL);
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
    else if (strcmp(method, "block_given?") == 0 || strcmp(method, "frozen?") == 0)
      result = vt_prim(SPINEL_TYPE_BOOLEAN);
    else if (strcmp(method, "__method__") == 0)
      result = vt_prim(SPINEL_TYPE_STRING);

    /* Fallback: if result is still VALUE and a receiver-less call has a block,
     * try inferring from block body's last expression.
     * This handles methods like report_duration { ... @grammar }
     * and other block-passing methods that return the block's value.
     * Only for non-lambda mode and receiver-less calls to avoid
     * interfering with lambda call inference. */
    if (result.kind == SPINEL_TYPE_VALUE && !ctx->lambda_mode &&
      !call->receiver && call->block &&
      PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
      pm_block_node_t *blk = (pm_block_node_t *)call->block;
      if (blk->body) {
        vtype_t bt = infer_type(ctx, blk->body);
        if (bt.kind != SPINEL_TYPE_VALUE && bt.kind != SPINEL_TYPE_UNKNOWN)
          result = bt;
      }
    }

    free(method);
    return result;
  }

  case PM_IF_NODE: {
    pm_if_node_t *n = (pm_if_node_t *)node;
    vtype_t then_t = n->statements ? infer_type(ctx, (pm_node_t *)n->statements) : vt_prim(SPINEL_TYPE_NIL);
    vtype_t else_t = n->subsequent ? infer_type(ctx, (pm_node_t *)n->subsequent) : vt_prim(SPINEL_TYPE_NIL);
    if (then_t.kind == else_t.kind) return then_t;
    if (vt_is_numeric(then_t) && vt_is_numeric(else_t)) return vt_prim(SPINEL_TYPE_FLOAT);
    /* Only widen to POLY if both types are poly-eligible scalars */
    if (vt_is_poly_eligible(then_t) && vt_is_poly_eligible(else_t))
      return vt_prim(SPINEL_TYPE_POLY);
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
    if (vt_is_numeric(then_t) && vt_is_numeric(else_t)) return vt_prim(SPINEL_TYPE_FLOAT);
    if (vt_is_poly_eligible(then_t) && vt_is_poly_eligible(else_t))
      return vt_prim(SPINEL_TYPE_POLY);
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
          else if (result.kind != t.kind) {
            if (vt_is_numeric(result) && vt_is_numeric(t))
              result = vt_prim(SPINEL_TYPE_FLOAT);
            else if (vt_is_poly_eligible(result) && vt_is_poly_eligible(t))
              result = vt_prim(SPINEL_TYPE_POLY);
            else
              result = vt_prim(SPINEL_TYPE_VALUE);
          }
        }
      }
    }
    return result;
  }

  case PM_CASE_MATCH_NODE: {
    pm_case_match_node_t *n = (pm_case_match_node_t *)node;
    vtype_t result = vt_prim(SPINEL_TYPE_NIL);
    for (size_t i = 0; i < n->conditions.size; i++) {
      pm_node_t *cond = n->conditions.nodes[i];
      if (PM_NODE_TYPE(cond) == PM_IN_NODE) {
        pm_in_node_t *in = (pm_in_node_t *)cond;
        if (in->statements) {
          vtype_t t = infer_type(ctx, (pm_node_t *)in->statements);
          if (i == 0) result = t;
          else if (result.kind != t.kind) {
            if (vt_is_numeric(result) && vt_is_numeric(t))
              result = vt_prim(SPINEL_TYPE_FLOAT);
            else if (vt_is_poly_eligible(result) && vt_is_poly_eligible(t))
              result = vt_prim(SPINEL_TYPE_POLY);
            else
              result = vt_prim(SPINEL_TYPE_VALUE);
          }
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

  case PM_ARRAY_NODE: {
    /* Array literals: if all elements are integer, use IntArray; otherwise RbArray */
    if (ctx->lambda_mode)
      return vt_prim(SPINEL_TYPE_ARRAY);
    pm_array_node_t *ary_node = (pm_array_node_t *)node;
    if (ary_node->elements.size == 0)
      return vt_prim(SPINEL_TYPE_ARRAY); /* empty [] → IntArray */
    bool all_int = true, all_str = true, all_float = true;
    for (size_t i = 0; i < ary_node->elements.size; i++) {
      vtype_t et = infer_type(ctx, ary_node->elements.nodes[i]);
      if (et.kind != SPINEL_TYPE_INTEGER) all_int = false;
      if (et.kind != SPINEL_TYPE_STRING) all_str = false;
      if (et.kind != SPINEL_TYPE_FLOAT) all_float = false;
    }
    if (all_int) return vt_prim(SPINEL_TYPE_ARRAY);
    if (all_float) return vt_prim(SPINEL_TYPE_FLOAT_ARRAY);
    if (all_str) return vt_prim(SPINEL_TYPE_STR_ARRAY);
    return vt_prim(SPINEL_TYPE_RB_ARRAY);
  }

  case PM_HASH_NODE: {
    /* Detect heterogeneous hash: if values have different types, use sp_RbHash.
     * Also use sp_RbHash when values are not integers (sp_StrIntHash is string→int only). */
    pm_hash_node_t *hn = (pm_hash_node_t *)node;
    if (hn->elements.size > 0) {
      spinel_type_t first_val_kind = SPINEL_TYPE_UNKNOWN;
      bool heterogeneous = false;
      for (size_t i = 0; i < hn->elements.size; i++) {
        if (PM_NODE_TYPE(hn->elements.nodes[i]) != PM_ASSOC_NODE) continue;
        pm_assoc_node_t *assoc = (pm_assoc_node_t *)hn->elements.nodes[i];
        vtype_t vt = infer_type(ctx, assoc->value);
        if (first_val_kind == SPINEL_TYPE_UNKNOWN)
          first_val_kind = vt.kind;
        else if (vt.kind != first_val_kind)
          heterogeneous = true;
      }
      if (heterogeneous)
        return vt_prim(SPINEL_TYPE_RB_HASH);
      /* sp_StrIntHash only supports integer values; use sp_RbHash for other types */
      if (first_val_kind != SPINEL_TYPE_UNKNOWN && first_val_kind != SPINEL_TYPE_INTEGER)
        return vt_prim(SPINEL_TYPE_RB_HASH);
    }
    return vt_prim(SPINEL_TYPE_HASH);
  }

  case PM_BEGIN_NODE: {
    pm_begin_node_t *bn = (pm_begin_node_t *)node;
    if (bn->statements)
      return infer_type(ctx, (pm_node_t *)bn->statements);
    return vt_prim(SPINEL_TYPE_NIL);
  }

  case PM_REGULAR_EXPRESSION_NODE:
  case PM_INTERPOLATED_REGULAR_EXPRESSION_NODE:
    return vt_prim(SPINEL_TYPE_REGEXP);

  case PM_NUMBERED_REFERENCE_READ_NODE:
    return vt_prim(SPINEL_TYPE_STRING);

  case PM_MATCH_WRITE_NODE: {
    pm_match_write_node_t *mw = (pm_match_write_node_t *)node;
    return infer_type(ctx, (pm_node_t *)mw->call);
  }

  case PM_SELF_NODE:
    if (ctx->current_class)
      return vt_obj(ctx->current_class->name);
    if (ctx->ext_method_recv_type != SPINEL_TYPE_UNKNOWN)
      return vt_prim(ctx->ext_method_recv_type);
    return vt_prim(SPINEL_TYPE_VALUE);

  case PM_RANGE_NODE:
    return vt_prim(SPINEL_TYPE_RANGE);

  case PM_GLOBAL_VARIABLE_READ_NODE: {
    pm_global_variable_read_node_t *n = (pm_global_variable_read_node_t *)node;
    char *gname = cstr(ctx, n->name);
    spinel_type_t t = SPINEL_TYPE_VALUE;
    if (strcmp(gname, "$0") == 0 || strcmp(gname, "$PROGRAM_NAME") == 0)
      t = SPINEL_TYPE_STRING;
    else if (strcmp(gname, "$stderr") == 0 || strcmp(gname, "$stdout") == 0 || strcmp(gname, "$stdin") == 0)
      t = SPINEL_TYPE_VALUE;
    else if (strcmp(gname, "$?") == 0)
      t = SPINEL_TYPE_INTEGER;
    free(gname);
    return vt_prim(t);
  }

  case PM_RESCUE_MODIFIER_NODE: {
    pm_rescue_modifier_node_t *rm = (pm_rescue_modifier_node_t *)node;
    return infer_type(ctx, rm->expression);
  }

  case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_local_variable_operator_write_node_t *n =
      (pm_local_variable_operator_write_node_t *)node;
    char *name = cstr(ctx, n->name);
    var_entry_t *v = var_lookup(ctx, name);
    vtype_t t = v ? v->type : infer_type(ctx, n->value);
    free(name);
    return t;
  }

  case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
    pm_local_variable_or_write_node_t *n =
      (pm_local_variable_or_write_node_t *)node;
    char *name = cstr(ctx, n->name);
    var_entry_t *v = var_lookup(ctx, name);
    vtype_t t = v ? v->type : infer_type(ctx, n->value);
    free(name);
    return t;
  }

  case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
    pm_local_variable_and_write_node_t *n =
      (pm_local_variable_and_write_node_t *)node;
    char *name = cstr(ctx, n->name);
    var_entry_t *v = var_lookup(ctx, name);
    vtype_t t = v ? v->type : infer_type(ctx, n->value);
    free(name);
    return t;
  }

  case PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_instance_variable_operator_write_node_t *n =
      (pm_instance_variable_operator_write_node_t *)node;
    char *ivname = cstr(ctx, n->name);
    if (ctx->current_class) {
      ivar_info_t *iv = find_ivar(ctx->current_class, ivname + 1);
      if (iv) { free(ivname); return iv->type; }
    }
    free(ivname);
    return infer_type(ctx, n->value);
  }

  default:
    return vt_prim(SPINEL_TYPE_VALUE);
  }
}

/* Walk AST to register all variables and infer their types */
void infer_pass(codegen_ctx_t *ctx, pm_node_t *node) {
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
          /* Array.new(n) { block } — dynamic array, skip fixed-size */
          if (vc->block && PM_NODE_TYPE(vc->block) == PM_BLOCK_NODE) {
            /* type already inferred correctly (ARRAY or FLOAT_ARRAY) */
            var_declare(ctx, name, type, false);
            free(mname); free(name);
            break;
          }
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
          }
          else {
            /* Dynamic array (IntArray or FloatArray based on infer_type) */
            if (type.kind != SPINEL_TYPE_FLOAT_ARRAY)
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
    }
    else {
      var_declare(ctx, name, rhs_type, false);
    }
    free(name);
    break;
  }
  case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
    pm_local_variable_or_write_node_t *n =
      (pm_local_variable_or_write_node_t *)node;
    infer_pass(ctx, n->value);
    char *name = cstr(ctx, n->name);
    vtype_t type = infer_type(ctx, n->value);
    var_entry_t *v = var_lookup(ctx, name);
    if (!v) var_declare(ctx, name, type, false);
    free(name);
    break;
  }
  case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
    pm_local_variable_and_write_node_t *n =
      (pm_local_variable_and_write_node_t *)node;
    infer_pass(ctx, n->value);
    char *name = cstr(ctx, n->name);
    vtype_t type = infer_type(ctx, n->value);
    var_entry_t *v = var_lookup(ctx, name);
    if (!v) var_declare(ctx, name, type, false);
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
  case PM_CASE_MATCH_NODE: {
    pm_case_match_node_t *n = (pm_case_match_node_t *)node;
    if (n->predicate) infer_pass(ctx, n->predicate);
    for (size_t i = 0; i < n->conditions.size; i++) {
      pm_node_t *cn = n->conditions.nodes[i];
      if (PM_NODE_TYPE(cn) == PM_IN_NODE) {
        pm_in_node_t *in = (pm_in_node_t *)cn;
        if (in->statements) infer_pass(ctx, (pm_node_t *)in->statements);
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
    /* Detect String#dup: ensure sp_String runtime is emitted */
    if (call->receiver && ceq(ctx, call->name, "dup")) {
      vtype_t recv_t = infer_type(ctx, call->receiver);
      if (recv_t.kind == SPINEL_TYPE_STRING || recv_t.kind == SPINEL_TYPE_SP_STRING)
        ctx->needs_sp_string = true;
    }
    /* Detect String << / replace / clear : widen STRING variable to SP_STRING (mutable) */
    if (call->receiver && (ceq(ctx, call->name, "<<") || ceq(ctx, call->name, "replace") || ceq(ctx, call->name, "clear")) &&
      PM_NODE_TYPE(call->receiver) == PM_LOCAL_VARIABLE_READ_NODE) {
      vtype_t recv_t = infer_type(ctx, call->receiver);
      if (recv_t.kind == SPINEL_TYPE_STRING || recv_t.kind == SPINEL_TYPE_SP_STRING) {
        pm_local_variable_read_node_t *lv = (pm_local_variable_read_node_t *)call->receiver;
        char *vn = cstr(ctx, lv->name);
        var_entry_t *v = var_lookup(ctx, vn);
        if (v) v->type = vt_prim(SPINEL_TYPE_SP_STRING);
        free(vn);
      }
    }
    /* Recurse into blocks (for .times do ... end) */
    if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
      pm_block_node_t *blk = (pm_block_node_t *)call->block;
      /* Determine block parameter types based on receiver type */
      bool is_hash_each = false;
      bool is_rb_hash_each = false;
      if (call->receiver) {
        vtype_t recv_t = infer_type(ctx, call->receiver);
        char *meth = cstr(ctx, call->name);
        if (recv_t.kind == SPINEL_TYPE_HASH && strcmp(meth, "each") == 0)
          is_hash_each = true;
        if (recv_t.kind == SPINEL_TYPE_RB_HASH && strcmp(meth, "each") == 0)
          is_rb_hash_each = true;
        free(meth);
      }
      /* Handle numbered parameters (_1) and it-block */
      if (blk->parameters && (PM_NODE_TYPE(blk->parameters) == PM_NUMBERED_PARAMETERS_NODE ||
                  PM_NODE_TYPE(blk->parameters) == PM_IT_PARAMETERS_NODE)) {
        spinel_type_t bp_type = SPINEL_TYPE_INTEGER;
        if (call->receiver) {
          vtype_t recv_t = infer_type(ctx, call->receiver);
          if (recv_t.kind == SPINEL_TYPE_STR_ARRAY) bp_type = SPINEL_TYPE_STRING;
        }
        var_declare(ctx, "_1", vt_prim(bp_type), false);
      }
      if (blk->parameters && PM_NODE_TYPE(blk->parameters) == PM_BLOCK_PARAMETERS_NODE) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)blk->parameters;
        if (bp->parameters) {
          if (is_rb_hash_each) {
            /* RbHash#each: |k, v| where k is STRING, v is POLY */
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
                var_declare(ctx, pname, vt_prim(SPINEL_TYPE_POLY), false);
                free(pname);
              }
            }
          }
          else if (is_hash_each) {
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
          }
          else if (bp->parameters->requireds.size > 0) {
            /* Determine block parameter type based on method */
            spinel_type_t bp_type = SPINEL_TYPE_INTEGER;
            {
              char *meth = cstr(ctx, call->name);
              if (strcmp(meth, "scan") == 0 || strcmp(meth, "each_char") == 0)
                bp_type = SPINEL_TYPE_STRING;
              /* File.open block param → FILE type */
              if (strcmp(meth, "open") == 0 && call->receiver &&
                PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
                pm_constant_read_node_t *cr = (pm_constant_read_node_t *)call->receiver;
                if (ceq(ctx, cr->name, "File"))
                  bp_type = SPINEL_TYPE_FILE;
              }
              /* each_line on FILE-typed receiver → STRING block param */
              if (strcmp(meth, "each_line") == 0 && call->receiver) {
                vtype_t recv_t = infer_type(ctx, call->receiver);
                if (recv_t.kind == SPINEL_TYPE_FILE ||
                  recv_t.kind == SPINEL_TYPE_STRING)
                  bp_type = SPINEL_TYPE_STRING;
              }
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
  case PM_MULTI_WRITE_NODE: {
    pm_multi_write_node_t *mw = (pm_multi_write_node_t *)node;
    /* Infer element type from RHS */
    vtype_t rhs_type = vt_prim(SPINEL_TYPE_UNKNOWN);
    if (PM_NODE_TYPE(mw->value) != PM_ARRAY_NODE)
      rhs_type = infer_type(ctx, mw->value);
    /* Register left-hand-side variables */
    for (size_t i = 0; i < mw->lefts.size; i++) {
      pm_node_t *l = mw->lefts.nodes[i];
      if (PM_NODE_TYPE(l) == PM_LOCAL_VARIABLE_TARGET_NODE) {
        pm_local_variable_target_node_t *t = (pm_local_variable_target_node_t *)l;
        char *vname = cstr(ctx, t->name);
        vtype_t et = vt_prim(SPINEL_TYPE_INTEGER);
        if (PM_NODE_TYPE(mw->value) == PM_ARRAY_NODE) {
          /* If RHS is array literal, infer element type */
          pm_array_node_t *ary = (pm_array_node_t *)mw->value;
          if (i < ary->elements.size)
            et = infer_type(ctx, ary->elements.nodes[i]);
        }
        else if (rhs_type.kind == SPINEL_TYPE_FLOAT_ARRAY) {
          et = vt_prim(SPINEL_TYPE_FLOAT);
        }
        else if (rhs_type.kind == SPINEL_TYPE_ARRAY) {
          et = vt_prim(SPINEL_TYPE_INTEGER);
        }
        var_declare(ctx, vname, et, false);
        free(vname);
      }
    }
    /* Also handle rest (*rest) target */
    if (mw->rest && PM_NODE_TYPE(mw->rest) == PM_SPLAT_NODE) {
      pm_splat_node_t *spl = (pm_splat_node_t *)mw->rest;
      if (spl->expression && PM_NODE_TYPE(spl->expression) == PM_LOCAL_VARIABLE_TARGET_NODE) {
        pm_local_variable_target_node_t *t = (pm_local_variable_target_node_t *)spl->expression;
        char *vname = cstr(ctx, t->name);
        var_declare(ctx, vname, vt_prim(SPINEL_TYPE_ARRAY), false);
        free(vname);
      }
    }
    break;
  }
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
  case PM_PARENTHESES_NODE: {
    pm_parentheses_node_t *pn = (pm_parentheses_node_t *)node;
    if (pn->body) infer_pass(ctx, pn->body);
    break;
  }
  case PM_AND_NODE: {
    pm_and_node_t *an = (pm_and_node_t *)node;
    infer_pass(ctx, an->left);
    infer_pass(ctx, an->right);
    break;
  }
  case PM_OR_NODE: {
    pm_or_node_t *on = (pm_or_node_t *)node;
    infer_pass(ctx, on->left);
    infer_pass(ctx, on->right);
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

void resolve_class_types(codegen_ctx_t *ctx, pm_node_t *prog_root) {
  /* For each class, determine ivar types from initialize body.
   * We need to resolve bottom-up: Vec first (has literal args),
   * then Sphere/Plane/Ray/Isect (have Vec args), then Scene. */
  for (int pass = 0; pass < 3; pass++) {
    for (int ci = 0; ci < ctx->class_count; ci++) {
      class_info_t *cls = &ctx->classes[ci];
      /* Swap to the parser that owns this class's AST */
      pm_parser_t *saved_parser = ctx->parser;
      if (cls->origin_parser)
        ctx->parser = cls->origin_parser;
      method_info_t *init = find_method(cls, "initialize");
      if (!init || !init->body_node) { ctx->parser = saved_parser; continue; }

      /* Walk init body looking for @ivar = param patterns */
      pm_node_t *body = init->body_node;
      if (PM_NODE_TYPE(body) != PM_STATEMENTS_NODE) { ctx->parser = saved_parser; continue; }
      pm_statements_node_t *stmts = (pm_statements_node_t *)body;

      for (size_t i = 0; i < stmts->body.size; i++) {
        pm_node_t *s = stmts->body.nodes[i];
        if (PM_NODE_TYPE(s) != PM_INSTANCE_VARIABLE_WRITE_NODE) continue;
        pm_instance_variable_write_node_t *iw =
          (pm_instance_variable_write_node_t *)s;
        char *ivname_full = cstr(ctx, iw->name);
        const char *ivname = ivname_full + 1; /* skip @ */
        ivar_info_t *iv = find_ivar(cls, ivname);
        if (iv && (iv->type.kind == SPINEL_TYPE_VALUE || iv->type.kind == SPINEL_TYPE_NIL)) {
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

      /* Heuristic: resolve untyped ivars whose name matches a known class.
       * Common Ruby convention: @grammar → Grammar, @context → Context, etc.
       * Also handles @rule_counter → Counter (try stripping underscored prefix). */
      for (int j = 0; j < cls->ivar_count; j++) {
        if (cls->ivars[j].type.kind != SPINEL_TYPE_VALUE && cls->ivars[j].type.kind != SPINEL_TYPE_NIL) continue;
        const char *ivn = cls->ivars[j].name;
        /* Try capitalizing the ivar name: grammar → Grammar */
        char capitalized[64];
        snprintf(capitalized, sizeof(capitalized), "%s", ivn);
        if (capitalized[0] >= 'a' && capitalized[0] <= 'z')
          capitalized[0] -= ('a' - 'A');
        /* Convert snake_case to CamelCase */
        char camel[64];
        int ci2 = 0;
        bool next_upper = true;
        for (int k = 0; ivn[k] && ci2 < 62; k++) {
          if (ivn[k] == '_') {
            next_upper = true;
          }
          else {
            if (next_upper && ivn[k] >= 'a' && ivn[k] <= 'z')
              camel[ci2++] = ivn[k] - ('a' - 'A');
            else
              camel[ci2++] = ivn[k];
            next_upper = false;
          }
        }
        camel[ci2] = '\0';
        /* Try CamelCase first, then simple capitalized */
        class_info_t *matched = find_class(ctx, camel);
        if (!matched) matched = find_class(ctx, capitalized);
        if (matched && matched != cls) {
          cls->ivars[j].type = vt_obj(matched->name);
        }
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
      ctx->parser = saved_parser;
    }

    /* Resolve method return types for ALL classes (separate loop) */
    for (int ci = 0; ci < ctx->class_count; ci++) {
      class_info_t *cls = &ctx->classes[ci];
      pm_parser_t *saved_p2 = ctx->parser;
      if (cls->origin_parser) ctx->parser = cls->origin_parser;
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
          int sf = ctx->var_scope_floor;
          ctx->var_scope_floor = sv;
          class_info_t *saved_cls = ctx->current_class;
          ctx->current_class = cls;
          for (int pi = 0; pi < m->param_count; pi++)
            var_declare(ctx, m->params[pi].name, m->params[pi].type, false);
          vtype_t rt = infer_type(ctx, m->body_node);
          ctx->var_count = sv;
          ctx->var_scope_floor = sf;
          for (int ci = sv; ci < MAX_VARS && ctx->vars[ci].name[0]; ci++)
            ctx->vars[ci].name[0] = '\0';
          ctx->current_class = saved_cls;
          if (rt.kind != SPINEL_TYPE_VALUE && rt.kind != SPINEL_TYPE_UNKNOWN)
            m->return_type = rt;
        }
      }
      ctx->parser = saved_p2;
    }

    /* Resolve extension method return types from body inference */
    for (int ei = 0; ei < ctx->ext_method_count; ei++) {
      method_info_t *m = &ctx->ext_methods[ei].method;
      if (m->return_type.kind != SPINEL_TYPE_VALUE || !m->body_node) continue;
      pm_parser_t *saved_ep = ctx->parser;
      if (m->origin_parser) ctx->parser = m->origin_parser;
      int sv = ctx->var_count;
      int sf = ctx->var_scope_floor;
      ctx->var_scope_floor = sv;
      spinel_type_t saved_ext = ctx->ext_method_recv_type;
      ctx->ext_method_recv_type = ctx->ext_methods[ei].recv_type;
      for (int pi = 0; pi < m->param_count; pi++)
        var_declare(ctx, m->params[pi].name, m->params[pi].type, false);
      vtype_t rt = infer_type(ctx, m->body_node);
      ctx->var_count = sv;
      ctx->var_scope_floor = sf;
      ctx->ext_method_recv_type = saved_ext;
      for (int vi = sv; vi < MAX_VARS && ctx->vars[vi].name[0]; vi++)
        ctx->vars[vi].name[0] = '\0';
      ctx->parser = saved_ep;
      if (rt.kind != SPINEL_TYPE_VALUE && rt.kind != SPINEL_TYPE_UNKNOWN)
        m->return_type = rt;
    }

    /* Infer method param types from body: if param.foo() where foo is a method
     * of the current class, infer param type as the current class.
     * Uses a stack-based recursive scan of all call nodes in the body. */
    for (int ci = 0; ci < ctx->class_count; ci++) {
      class_info_t *cls = &ctx->classes[ci];
      pm_parser_t *saved_p3 = ctx->parser;
      if (cls->origin_parser) ctx->parser = cls->origin_parser;
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
                  /* Check current class first */
                  if (find_method(cls, called)) {
                    m->params[pi].type = vt_obj(cls->name);
                  }
                  else {
                    /* Check all other classes for the called method */
                    for (int oci = 0; oci < ctx->class_count; oci++) {
                      if (oci == ci) continue;
                      if (find_method(&ctx->classes[oci], called)) {
                        m->params[pi].type = vt_obj(ctx->classes[oci].name);
                        break;
                      }
                    }
                  }
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
            else if (PM_NODE_TYPE(n) == PM_CASE_NODE) {
              pm_case_node_t *cn = (pm_case_node_t *)n;
              if (cn->predicate && scan_sp < 127) scan_stack[scan_sp++] = cn->predicate;
              for (size_t wi = 0; wi < cn->conditions.size && scan_sp < 127; wi++) {
                pm_node_t *wn = cn->conditions.nodes[wi];
                if (PM_NODE_TYPE(wn) == PM_WHEN_NODE) {
                  pm_when_node_t *when = (pm_when_node_t *)wn;
                  if (when->statements && scan_sp < 127)
                    scan_stack[scan_sp++] = (pm_node_t *)when->statements;
                }
              }
              if (cn->else_clause && scan_sp < 127)
                scan_stack[scan_sp++] = (pm_node_t *)cn->else_clause;
            }
            else if (PM_NODE_TYPE(n) == PM_IF_NODE) {
              pm_if_node_t *ifn = (pm_if_node_t *)n;
              if (ifn->predicate && scan_sp < 127) scan_stack[scan_sp++] = ifn->predicate;
              if (ifn->statements && scan_sp < 127) scan_stack[scan_sp++] = (pm_node_t *)ifn->statements;
              if (ifn->subsequent && scan_sp < 127) scan_stack[scan_sp++] = ifn->subsequent;
            }
            else if (PM_NODE_TYPE(n) == PM_ELSE_NODE) {
              pm_else_node_t *en = (pm_else_node_t *)n;
              if (en->statements && scan_sp < 127) scan_stack[scan_sp++] = (pm_node_t *)en->statements;
            }
            else if (PM_NODE_TYPE(n) == PM_WHILE_NODE) {
              pm_while_node_t *wn = (pm_while_node_t *)n;
              if (wn->predicate && scan_sp < 127) scan_stack[scan_sp++] = wn->predicate;
              if (wn->statements && scan_sp < 127) scan_stack[scan_sp++] = (pm_node_t *)wn->statements;
            }
            else if (PM_NODE_TYPE(n) == PM_RETURN_NODE) {
              pm_return_node_t *rn = (pm_return_node_t *)n;
              if (rn->arguments && scan_sp < 127)
                scan_stack[scan_sp++] = (pm_node_t *)rn->arguments;
            }
            else if (PM_NODE_TYPE(n) == PM_ARGUMENTS_NODE) {
              pm_arguments_node_t *an = (pm_arguments_node_t *)n;
              for (size_t ai = 0; ai < an->arguments.size && scan_sp < 127; ai++)
                scan_stack[scan_sp++] = an->arguments.nodes[ai];
            }
            else if (PM_NODE_TYPE(n) == PM_UNLESS_NODE) {
              pm_unless_node_t *un = (pm_unless_node_t *)n;
              if (un->predicate && scan_sp < 127) scan_stack[scan_sp++] = un->predicate;
              if (un->statements && scan_sp < 127) scan_stack[scan_sp++] = (pm_node_t *)un->statements;
            }
          }
        }
      }
      ctx->parser = saved_p3;
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
                    /* Setter calls: recv.ivar=(val) → update ivar type */
                    if (target->is_setter && target->accessor_ivar[0] &&
                      call->arguments->arguments.size == 1) {
                      ivar_info_t *iv = find_ivar(rcls, target->accessor_ivar);
                      if (iv && (iv->type.kind == SPINEL_TYPE_VALUE || iv->type.kind == SPINEL_TYPE_NIL)) {
                        vtype_t at = infer_type(ctx, call->arguments->arguments.nodes[0]);
                        if (at.kind != SPINEL_TYPE_VALUE && at.kind != SPINEL_TYPE_NIL && at.kind != SPINEL_TYPE_UNKNOWN)
                          iv->type = at;
                      }
                    }
                  }
                  free(mname);
                }
              }
            }
          }
          /* Recurse into statement types */
          if (PM_NODE_TYPE(s) == PM_CALL_NODE) {
            pm_call_node_t *cc = (pm_call_node_t *)s;
            if (cc->receiver && ms_sp < 255) ms_stack[ms_sp++] = cc->receiver;
            if (cc->arguments) {
              for (size_t ai = 0; ai < cc->arguments->arguments.size && ms_sp < 255; ai++)
                ms_stack[ms_sp++] = cc->arguments->arguments.nodes[ai];
            }
          }
          if (PM_NODE_TYPE(s) == PM_LOCAL_VARIABLE_WRITE_NODE) {
            pm_local_variable_write_node_t *lw = (pm_local_variable_write_node_t *)s;
            if (ms_sp < 255) ms_stack[ms_sp++] = lw->value;
          }
          if (PM_NODE_TYPE(s) == PM_MULTI_WRITE_NODE) {
            pm_multi_write_node_t *mw = (pm_multi_write_node_t *)s;
            if (ms_sp < 255) ms_stack[ms_sp++] = mw->value;
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

    /* Infer top-level function param/return types from call sites.
     * Run 2 iterations: first resolves params, second resolves return types
     * that depend on those params. */
    for (int func_pass = 0; func_pass < 4; func_pass++) {
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
                    vtype_t at = infer_type(ctx, call->arguments->arguments.nodes[pi]);
                    if (target->params[pi].type.kind == SPINEL_TYPE_VALUE) {
                      if (at.kind != SPINEL_TYPE_VALUE)
                        target->params[pi].type = at;
                    }
                    else if (at.kind == SPINEL_TYPE_OBJECT &&
                          target->params[pi].type.kind == SPINEL_TYPE_OBJECT &&
                          strcmp(target->params[pi].type.klass, at.klass) != 0) {
                      /* Different OBJECT classes → POLY with class tracking */
                      poly_class_add(ctx, target->name, pi, target->params[pi].type.klass);
                      poly_class_add(ctx, target->name, pi, at.klass);
                      target->params[pi].type = vt_prim(SPINEL_TYPE_POLY);
                    }
                    else if (at.kind == SPINEL_TYPE_OBJECT &&
                          target->params[pi].type.kind == SPINEL_TYPE_POLY) {
                      /* Already POLY, just track this class */
                      poly_class_add(ctx, target->name, pi, at.klass);
                    }
                    else if (at.kind != SPINEL_TYPE_VALUE &&
                          at.kind != target->params[pi].type.kind &&
                          target->params[pi].type.kind != SPINEL_TYPE_POLY) {
                      /* Widen: called with incompatible types → POLY */
                      if (vt_is_poly_eligible(target->params[pi].type) && vt_is_poly_eligible(at))
                        target->params[pi].type = vt_prim(SPINEL_TYPE_POLY);
                      /* else: leave as-is (complex types like ARRAY shouldn't become POLY) */
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
                      /* Check for keyword hash in nested call arguments */
                      bool found_kw2 = false;
                      for (size_t ai2 = 0; ai2 < inner->arguments->arguments.size; ai2++) {
                        pm_node_t *iarg = inner->arguments->arguments.nodes[ai2];
                        if (PM_NODE_TYPE(iarg) == PM_KEYWORD_HASH_NODE) {
                          pm_keyword_hash_node_t *kwh = (pm_keyword_hash_node_t *)iarg;
                          for (size_t ki = 0; ki < kwh->elements.size; ki++) {
                            if (PM_NODE_TYPE(kwh->elements.nodes[ki]) != PM_ASSOC_NODE) continue;
                            pm_assoc_node_t *assoc = (pm_assoc_node_t *)kwh->elements.nodes[ki];
                            if (PM_NODE_TYPE(assoc->key) != PM_SYMBOL_NODE) continue;
                            pm_symbol_node_t *sym = (pm_symbol_node_t *)assoc->key;
                            const uint8_t *ksrc = pm_string_source(&sym->unescaped);
                            size_t klen = pm_string_length(&sym->unescaped);
                            char kn[64]; size_t cl = klen < 63 ? klen : 63;
                            memcpy(kn, ksrc, cl); kn[cl] = '\0';
                            for (int pi = 0; pi < target2->param_count; pi++) {
                              if (target2->params[pi].is_keyword &&
                                strcmp(target2->params[pi].name, kn) == 0 &&
                                target2->params[pi].type.kind == SPINEL_TYPE_VALUE) {
                                vtype_t at = infer_type(ctx, assoc->value);
                                if (at.kind != SPINEL_TYPE_VALUE)
                                  target2->params[pi].type = at;
                              }
                            }
                          }
                          found_kw2 = true;
                        }
                      }
                      if (!found_kw2) {
                      for (int pi = 0; pi < target2->param_count &&
                         pi < (int)inner->arguments->arguments.size; pi++) {
                        vtype_t at = infer_type(ctx, inner->arguments->arguments.nodes[pi]);
                        if (target2->params[pi].type.kind == SPINEL_TYPE_VALUE) {
                          if (at.kind != SPINEL_TYPE_VALUE)
                            target2->params[pi].type = at;
                        }
                        else if (at.kind == SPINEL_TYPE_OBJECT &&
                              target2->params[pi].type.kind == SPINEL_TYPE_OBJECT &&
                              strcmp(target2->params[pi].type.klass, at.klass) != 0) {
                          poly_class_add(ctx, target2->name, pi, target2->params[pi].type.klass);
                          poly_class_add(ctx, target2->name, pi, at.klass);
                          target2->params[pi].type = vt_prim(SPINEL_TYPE_POLY);
                        }
                        else if (at.kind == SPINEL_TYPE_OBJECT &&
                              target2->params[pi].type.kind == SPINEL_TYPE_POLY) {
                          poly_class_add(ctx, target2->name, pi, at.klass);
                        }
                        else if (at.kind != SPINEL_TYPE_VALUE &&
                              at.kind != target2->params[pi].type.kind &&
                              target2->params[pi].type.kind != SPINEL_TYPE_POLY) {
                          if (vt_is_poly_eligible(target2->params[pi].type) && vt_is_poly_eligible(at))
                            target2->params[pi].type = vt_prim(SPINEL_TYPE_POLY);
                        }
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
          if (PM_NODE_TYPE(s) == PM_MULTI_WRITE_NODE) {
            pm_multi_write_node_t *mw = (pm_multi_write_node_t *)s;
            if (tl_sp < 255) tl_stack[tl_sp++] = mw->value;
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
      }
      else if (PM_NODE_TYPE(body) == PM_BEGIN_NODE) {
        stack[sp++] = body;
      }
      else {
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
              /* Register caller params AND body locals for type inference */
              int sv = ctx->var_count;
              int sf = ctx->var_scope_floor;
              ctx->var_scope_floor = sv;
              for (int cp = 0; cp < caller->param_count; cp++)
                var_declare(ctx, caller->params[cp].name, caller->params[cp].type, false);
              infer_pass(ctx, caller->body_node);
              /* Check for keyword hash in arguments */
              bool found_kw_inner = false;
              for (size_t ai2 = 0; ai2 < cc->arguments->arguments.size; ai2++) {
                pm_node_t *iarg = cc->arguments->arguments.nodes[ai2];
                if (PM_NODE_TYPE(iarg) == PM_KEYWORD_HASH_NODE) {
                  pm_keyword_hash_node_t *kwh = (pm_keyword_hash_node_t *)iarg;
                  for (size_t ki = 0; ki < kwh->elements.size; ki++) {
                    if (PM_NODE_TYPE(kwh->elements.nodes[ki]) != PM_ASSOC_NODE) continue;
                    pm_assoc_node_t *assoc = (pm_assoc_node_t *)kwh->elements.nodes[ki];
                    if (PM_NODE_TYPE(assoc->key) != PM_SYMBOL_NODE) continue;
                    pm_symbol_node_t *sym = (pm_symbol_node_t *)assoc->key;
                    const uint8_t *ksrc = pm_string_source(&sym->unescaped);
                    size_t klen = pm_string_length(&sym->unescaped);
                    char kn[64]; size_t cl2 = klen < 63 ? klen : 63;
                    memcpy(kn, ksrc, cl2); kn[cl2] = '\0';
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
                  found_kw_inner = true;
                }
              }
              if (!found_kw_inner) {
              for (int pi = 0; pi < target->param_count &&
                 pi < (int)cc->arguments->arguments.size; pi++) {
                vtype_t at = infer_type(ctx, cc->arguments->arguments.nodes[pi]);
                if (target->params[pi].type.kind == SPINEL_TYPE_VALUE) {
                  if (at.kind != SPINEL_TYPE_VALUE)
                    target->params[pi].type = at;
                }
                else if (at.kind == SPINEL_TYPE_OBJECT &&
                      target->params[pi].type.kind == SPINEL_TYPE_OBJECT &&
                      strcmp(target->params[pi].type.klass, at.klass) != 0) {
                  poly_class_add(ctx, target->name, pi, target->params[pi].type.klass);
                  poly_class_add(ctx, target->name, pi, at.klass);
                  target->params[pi].type = vt_prim(SPINEL_TYPE_POLY);
                }
                else if (at.kind == SPINEL_TYPE_OBJECT &&
                      target->params[pi].type.kind == SPINEL_TYPE_POLY) {
                  poly_class_add(ctx, target->name, pi, at.klass);
                }
                else if (at.kind != SPINEL_TYPE_VALUE &&
                      at.kind != target->params[pi].type.kind &&
                      target->params[pi].type.kind != SPINEL_TYPE_POLY) {
                  if (vt_is_poly_eligible(target->params[pi].type) && vt_is_poly_eligible(at))
                    target->params[pi].type = vt_prim(SPINEL_TYPE_POLY);
                }
              }
              }
              ctx->var_count = sv;
              ctx->var_scope_floor = sf;
              for (int ci = sv; ci < MAX_VARS && ctx->vars[ci].name[0]; ci++)
                ctx->vars[ci].name[0] = '\0';
            }
            free(cn2);
          }
          /* Setter calls on typed receivers: recv.ivar=(val) → update ivar type */
          if (cc->receiver && cc->arguments && cc->arguments->arguments.size == 1) {
            int sv2 = ctx->var_count;
            int sf2 = ctx->var_scope_floor;
            ctx->var_scope_floor = sv2;
            for (int cp = 0; cp < caller->param_count; cp++)
              var_declare(ctx, caller->params[cp].name, caller->params[cp].type, false);
            infer_pass(ctx, caller->body_node);
            vtype_t recv_t = infer_type(ctx, cc->receiver);
            ctx->var_count = sv2;
            ctx->var_scope_floor = sf2;
            for (int ci = sv2; ci < MAX_VARS && ctx->vars[ci].name[0]; ci++)
              ctx->vars[ci].name[0] = '\0';
            if (recv_t.kind == SPINEL_TYPE_OBJECT) {
              class_info_t *rcls = find_class(ctx, recv_t.klass);
              if (rcls) {
                char *smn = cstr(ctx, cc->name);
                method_info_t *sm = find_method(rcls, smn);
                if (sm && sm->is_setter && sm->accessor_ivar[0]) {
                  ivar_info_t *iv = find_ivar(rcls, sm->accessor_ivar);
                  if (iv && (iv->type.kind == SPINEL_TYPE_VALUE || iv->type.kind == SPINEL_TYPE_NIL)) {
                    int sv3 = ctx->var_count;
                    int sf3 = ctx->var_scope_floor;
                    ctx->var_scope_floor = sv3;
                    for (int cp = 0; cp < caller->param_count; cp++)
                      var_declare(ctx, caller->params[cp].name, caller->params[cp].type, false);
                    infer_pass(ctx, caller->body_node);
                    vtype_t at = infer_type(ctx, cc->arguments->arguments.nodes[0]);
                    ctx->var_count = sv3;
                    ctx->var_scope_floor = sf3;
                    for (int ci = sv3; ci < MAX_VARS && ctx->vars[ci].name[0]; ci++)
                      ctx->vars[ci].name[0] = '\0';
                    if (at.kind != SPINEL_TYPE_VALUE && at.kind != SPINEL_TYPE_NIL && at.kind != SPINEL_TYPE_UNKNOWN)
                      iv->type = at;
                  }
                }
                free(smn);
              }
            }
          }
          /* Push receiver and arguments for further scanning */
          if (cc->receiver && sp < 255) stack[sp++] = cc->receiver;
          if (cc->arguments) {
            for (size_t ai = 0; ai < cc->arguments->arguments.size && sp < 255; ai++)
              stack[sp++] = cc->arguments->arguments.nodes[ai];
          }
          /* Push block body for nested function calls */
          if (cc->block && PM_NODE_TYPE(cc->block) == PM_BLOCK_NODE) {
            pm_block_node_t *blk = (pm_block_node_t *)cc->block;
            if (blk->body && sp < 255) stack[sp++] = (pm_node_t *)blk->body;
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
        if (PM_NODE_TYPE(cur) == PM_MULTI_WRITE_NODE) {
          pm_multi_write_node_t *mw = (pm_multi_write_node_t *)cur;
          if (sp < 255) stack[sp++] = mw->value;
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
      if (f->body_node) {
        int sv = ctx->var_count;
        int sf = ctx->var_scope_floor;
        ctx->var_scope_floor = sv;
        for (int pi = 0; pi < f->param_count; pi++)
          var_declare(ctx, f->params[pi].name, f->params[pi].type, false);
        /* Register &block parameter if present */
        if (f->has_block_param)
          var_declare(ctx, f->block_param_name, vt_prim(SPINEL_TYPE_PROC), false);
        /* Run infer_pass to register local variables */
        infer_pass(ctx, f->body_node);
        vtype_t rt = infer_type(ctx, f->body_node);
        ctx->var_count = sv;
        ctx->var_scope_floor = sf;
        for (int ci = sv; ci < MAX_VARS && ctx->vars[ci].name[0]; ci++)
          ctx->vars[ci].name[0] = '\0';
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
        f->params[0].type.kind != SPINEL_TYPE_FLOAT_ARRAY &&
        f->params[0].type.kind != SPINEL_TYPE_OBJECT &&
        f->params[0].type.kind != SPINEL_TYPE_POLY) {
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
                }
                else if (pn->body) { inner = pn->body; }
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

    } /* end func_pass loop */

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
                bool kw_init = init->param_count > 0 && init->params[0].is_keyword;
                if (kw_init && call->arguments->arguments.size == 1 &&
                  PM_NODE_TYPE(call->arguments->arguments.nodes[0]) == PM_KEYWORD_HASH_NODE) {
                  /* keyword_init Struct: match keyword args to params by name */
                  pm_keyword_hash_node_t *kwh = (pm_keyword_hash_node_t *)call->arguments->arguments.nodes[0];
                  for (int pi = 0; pi < init->param_count; pi++) {
                    for (size_t ki = 0; ki < kwh->elements.size; ki++) {
                      if (PM_NODE_TYPE(kwh->elements.nodes[ki]) != PM_ASSOC_NODE) continue;
                      pm_assoc_node_t *assoc = (pm_assoc_node_t *)kwh->elements.nodes[ki];
                      if (PM_NODE_TYPE(assoc->key) == PM_SYMBOL_NODE) {
                        pm_symbol_node_t *ksym = (pm_symbol_node_t *)assoc->key;
                        const uint8_t *ksrc = pm_string_source(&ksym->unescaped);
                        size_t klen = pm_string_length(&ksym->unescaped);
                        char kname[64];
                        snprintf(kname, sizeof(kname), "%.*s", (int)klen, ksrc);
                        if (strcmp(kname, init->params[pi].name) == 0) {
                          vtype_t at = infer_type(ctx, assoc->value);
                          if (at.kind != SPINEL_TYPE_VALUE && at.kind != SPINEL_TYPE_UNKNOWN) {
                            init->params[pi].type = at;
                            /* Also update ivar type */
                            ivar_info_t *iv = find_ivar(cls, init->params[pi].name);
                            if (iv) iv->type = at;
                          }
                          break;
                        }
                      }
                    }
                  }
                }
                else {
                  for (int ai = 0; ai < (int)call->arguments->arguments.size &&
                     ai < init->param_count; ai++) {
                    if (init->params[ai].type.kind == SPINEL_TYPE_VALUE) {
                      vtype_t at = infer_type(ctx, call->arguments->arguments.nodes[ai]);
                      if (at.kind != SPINEL_TYPE_VALUE && at.kind != SPINEL_TYPE_UNKNOWN)
                        init->params[ai].type = at;
                    }
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

    /* Infer constructor arg types and method param types from call sites
     * within class method bodies.  This handles patterns like:
     *   class Command
     *     def run(argv)
     *       grammar = Grammar.new(text)   # → propagate arg types to Grammar#initialize params
     *       grammar.rules.each { ... }     # → needs Grammar ivars resolved
     *     end
     *   end
     */
    for (int ci2 = 0; ci2 < ctx->class_count; ci2++) {
      class_info_t *cls2 = &ctx->classes[ci2];
      pm_parser_t *saved_cls_p = ctx->parser;
      if (cls2->origin_parser) ctx->parser = cls2->origin_parser;
      for (int mi2 = 0; mi2 < cls2->method_count; mi2++) {
        method_info_t *m2 = &cls2->methods[mi2];
        if (!m2->body_node || m2->is_getter || m2->is_setter) continue;
        if (strcmp(m2->name, "initialize") == 0) continue;

        /* Set up method params in var table for type inference */
        int sv = ctx->var_count;
        int sf = ctx->var_scope_floor;
        ctx->var_scope_floor = sv;
        class_info_t *saved_cls2 = ctx->current_class;
        ctx->current_class = cls2;
        for (int pi = 0; pi < m2->param_count; pi++)
          var_declare(ctx, m2->params[pi].name, m2->params[pi].type, false);
        /* Register self ivars so method body inference can see them */
        for (int ii = 0; ii < cls2->ivar_count; ii++) {
          char self_name[128];
          snprintf(self_name, sizeof(self_name), "@%s", cls2->ivars[ii].name);
          (void)self_name; /* ivars handled via current_class */
        }
        infer_pass(ctx, m2->body_node);

        /* Stack-based scan of method body for constructor and method calls */
        pm_node_t *mstack[512];
        int msp = 0;
        mstack[msp++] = m2->body_node;
        while (msp > 0) {
          pm_node_t *cur = mstack[--msp];
          if (!cur) continue;
          if (PM_NODE_TYPE(cur) == PM_CALL_NODE) {
            pm_call_node_t *call = (pm_call_node_t *)cur;
            char *mname = cstr(ctx, call->name);

            /* Constructor calls: ClassName.new(args) */
            if (strcmp(mname, "new") == 0 && call->receiver && call->arguments) {
              pm_constant_read_node_t *cr = NULL;
              char *cname = NULL;
              if (PM_NODE_TYPE(call->receiver) == PM_CONSTANT_READ_NODE) {
                cr = (pm_constant_read_node_t *)call->receiver;
                cname = cstr(ctx, cr->name);
              }
              else if (PM_NODE_TYPE(call->receiver) == PM_CONSTANT_PATH_NODE) {
                pm_constant_path_node_t *cp = (pm_constant_path_node_t *)call->receiver;
                cname = cstr(ctx, cp->name);
              }
              if (cname) {
                class_info_t *target_cls = find_class(ctx, cname);
                method_info_t *init = target_cls ? find_method(target_cls, "initialize") : NULL;
                if (target_cls && !init && target_cls->superclass[0]) {
                  class_info_t *parent = find_class(ctx, target_cls->superclass);
                  init = parent ? find_method(parent, "initialize") : NULL;
                }
                if (init) {
                  bool kw_init = init->param_count > 0 && init->params[0].is_keyword;
                  if (kw_init && call->arguments->arguments.size == 1 &&
                    PM_NODE_TYPE(call->arguments->arguments.nodes[0]) == PM_KEYWORD_HASH_NODE) {
                    pm_keyword_hash_node_t *kwh = (pm_keyword_hash_node_t *)call->arguments->arguments.nodes[0];
                    for (int pi = 0; pi < init->param_count; pi++) {
                      for (size_t ki = 0; ki < kwh->elements.size; ki++) {
                        if (PM_NODE_TYPE(kwh->elements.nodes[ki]) != PM_ASSOC_NODE) continue;
                        pm_assoc_node_t *assoc = (pm_assoc_node_t *)kwh->elements.nodes[ki];
                        if (PM_NODE_TYPE(assoc->key) == PM_SYMBOL_NODE) {
                          pm_symbol_node_t *ksym = (pm_symbol_node_t *)assoc->key;
                          const uint8_t *ksrc = pm_string_source(&ksym->unescaped);
                          size_t klen = pm_string_length(&ksym->unescaped);
                          char kname[64];
                          snprintf(kname, sizeof(kname), "%.*s", (int)klen, ksrc);
                          if (strcmp(kname, init->params[pi].name) == 0) {
                            vtype_t at = infer_type(ctx, assoc->value);
                            if (at.kind != SPINEL_TYPE_VALUE && at.kind != SPINEL_TYPE_UNKNOWN) {
                              if (init->params[pi].type.kind == SPINEL_TYPE_VALUE)
                                init->params[pi].type = at;
                              ivar_info_t *iv = find_ivar(target_cls, init->params[pi].name);
                              if (iv && (iv->type.kind == SPINEL_TYPE_VALUE || iv->type.kind == SPINEL_TYPE_NIL)) iv->type = at;
                            }
                            break;
                          }
                        }
                      }
                    }
                  }
                  else {
                    for (int ai = 0; ai < (int)call->arguments->arguments.size &&
                       ai < init->param_count; ai++) {
                      if (init->params[ai].type.kind == SPINEL_TYPE_VALUE) {
                        vtype_t at = infer_type(ctx, call->arguments->arguments.nodes[ai]);
                        if (at.kind != SPINEL_TYPE_VALUE && at.kind != SPINEL_TYPE_UNKNOWN)
                          init->params[ai].type = at;
                      }
                    }
                  }
                }
                free(cname);
              }
            }

            /* Method calls on typed receivers: recv.method(args) → propagate arg types */
            if (call->receiver && call->arguments) {
              vtype_t recv_t = infer_type(ctx, call->receiver);
              if (recv_t.kind == SPINEL_TYPE_OBJECT) {
                class_info_t *rcls = find_class(ctx, recv_t.klass);
                if (rcls) {
                  method_info_t *target = find_method_inherited(ctx, rcls, mname, NULL);
                  if (target) {
                    for (int pi = 0; pi < target->param_count &&
                       pi < (int)call->arguments->arguments.size; pi++) {
                      if (target->params[pi].type.kind == SPINEL_TYPE_VALUE) {
                        vtype_t at = infer_type(ctx, call->arguments->arguments.nodes[pi]);
                        if (at.kind != SPINEL_TYPE_VALUE && at.kind != SPINEL_TYPE_UNKNOWN)
                          target->params[pi].type = at;
                      }
                    }
                    /* Setter calls: recv.ivar=(val) → update ivar type from arg */
                    if (target->is_setter && target->accessor_ivar[0] &&
                      call->arguments->arguments.size == 1) {
                      ivar_info_t *iv = find_ivar(rcls, target->accessor_ivar);
                      if (iv && (iv->type.kind == SPINEL_TYPE_VALUE || iv->type.kind == SPINEL_TYPE_NIL)) {
                        vtype_t at = infer_type(ctx, call->arguments->arguments.nodes[0]);
                        if (at.kind != SPINEL_TYPE_VALUE && at.kind != SPINEL_TYPE_NIL && at.kind != SPINEL_TYPE_UNKNOWN)
                          iv->type = at;
                      }
                    }
                  }
                }
              }
            }

            free(mname);
            /* Push children for scanning */
            if (call->receiver && msp < 510) mstack[msp++] = call->receiver;
            if (call->arguments) {
              for (size_t ai = 0; ai < call->arguments->arguments.size && msp < 510; ai++)
                mstack[msp++] = call->arguments->arguments.nodes[ai];
            }
            if (call->block && PM_NODE_TYPE(call->block) == PM_BLOCK_NODE) {
              pm_block_node_t *blk = (pm_block_node_t *)call->block;
              if (blk->body && msp < 510) mstack[msp++] = (pm_node_t *)blk->body;
            }
          }
          /* Recurse into common statement types */
          if (PM_NODE_TYPE(cur) == PM_STATEMENTS_NODE) {
            pm_statements_node_t *ss = (pm_statements_node_t *)cur;
            for (size_t si = 0; si < ss->body.size && msp < 510; si++)
              mstack[msp++] = ss->body.nodes[si];
          }
          if (PM_NODE_TYPE(cur) == PM_LOCAL_VARIABLE_WRITE_NODE) {
            pm_local_variable_write_node_t *lw = (pm_local_variable_write_node_t *)cur;
            if (msp < 510) mstack[msp++] = lw->value;
          }
          if (PM_NODE_TYPE(cur) == PM_IF_NODE) {
            pm_if_node_t *ifn = (pm_if_node_t *)cur;
            if (ifn->predicate && msp < 510) mstack[msp++] = ifn->predicate;
            if (ifn->statements && msp < 510) mstack[msp++] = (pm_node_t *)ifn->statements;
            if (ifn->subsequent && msp < 510) mstack[msp++] = (pm_node_t *)ifn->subsequent;
          }
          if (PM_NODE_TYPE(cur) == PM_ELSE_NODE) {
            pm_else_node_t *en = (pm_else_node_t *)cur;
            if (en->statements && msp < 510) mstack[msp++] = (pm_node_t *)en->statements;
          }
          if (PM_NODE_TYPE(cur) == PM_WHILE_NODE) {
            pm_while_node_t *wn = (pm_while_node_t *)cur;
            if (wn->predicate && msp < 510) mstack[msp++] = wn->predicate;
            if (wn->statements && msp < 510) mstack[msp++] = (pm_node_t *)wn->statements;
          }
          if (PM_NODE_TYPE(cur) == PM_UNLESS_NODE) {
            pm_unless_node_t *un = (pm_unless_node_t *)cur;
            if (un->predicate && msp < 510) mstack[msp++] = un->predicate;
            if (un->statements && msp < 510) mstack[msp++] = (pm_node_t *)un->statements;
          }
          if (PM_NODE_TYPE(cur) == PM_BEGIN_NODE) {
            pm_begin_node_t *bn = (pm_begin_node_t *)cur;
            if (bn->statements && msp < 510) mstack[msp++] = (pm_node_t *)bn->statements;
            if (bn->rescue_clause && bn->rescue_clause->statements && msp < 510)
              mstack[msp++] = (pm_node_t *)bn->rescue_clause->statements;
            if (bn->ensure_clause && bn->ensure_clause->statements && msp < 510)
              mstack[msp++] = (pm_node_t *)bn->ensure_clause->statements;
          }
          if (PM_NODE_TYPE(cur) == PM_RESCUE_NODE) {
            pm_rescue_node_t *rn = (pm_rescue_node_t *)cur;
            if (rn->statements && msp < 510) mstack[msp++] = (pm_node_t *)rn->statements;
            if (rn->subsequent && msp < 510) mstack[msp++] = (pm_node_t *)rn->subsequent;
          }
          if (PM_NODE_TYPE(cur) == PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE) {
            pm_local_variable_operator_write_node_t *ow = (pm_local_variable_operator_write_node_t *)cur;
            if (msp < 510) mstack[msp++] = ow->value;
          }
          /* Scan for @ivar = expr in method bodies to resolve NIL-initialized ivars */
          if (PM_NODE_TYPE(cur) == PM_INSTANCE_VARIABLE_WRITE_NODE) {
            pm_instance_variable_write_node_t *iw = (pm_instance_variable_write_node_t *)cur;
            char *ivname_full = cstr(ctx, iw->name);
            const char *ivname = ivname_full + 1; /* skip @ */
            ivar_info_t *iv = find_ivar(cls2, ivname);
            if (iv && (iv->type.kind == SPINEL_TYPE_VALUE || iv->type.kind == SPINEL_TYPE_NIL)) {
              vtype_t vt = infer_type(ctx, iw->value);
              if (vt.kind != SPINEL_TYPE_VALUE && vt.kind != SPINEL_TYPE_NIL && vt.kind != SPINEL_TYPE_UNKNOWN)
                iv->type = vt;
            }
            free(ivname_full);
            if (iw->value && msp < 510) mstack[msp++] = iw->value;
          }
        }

        ctx->var_count = sv;
        ctx->var_scope_floor = sf;
        for (int ci = sv; ci < MAX_VARS && ctx->vars[ci].name[0]; ci++)
          ctx->vars[ci].name[0] = '\0';
        ctx->current_class = saved_cls2;
      }
      ctx->parser = saved_cls_p;
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

  /* Resolve module method return types from body */
  for (int mi = 0; mi < ctx->module_count; mi++) {
    module_info_t *mod = &ctx->modules[mi];
    pm_parser_t *saved_p = ctx->parser;
    if (mod->origin_parser) ctx->parser = mod->origin_parser;
    for (int mj = 0; mj < mod->method_count; mj++) {
      method_info_t *m = &mod->methods[mj];
      if (m->return_type.kind != SPINEL_TYPE_VALUE) continue;
      if (!m->body_node) continue;
      int sv = ctx->var_count;
      int sf = ctx->var_scope_floor;
      ctx->var_scope_floor = sv;
      /* Register module ivars as local vars for inference */
      for (int vi = 0; vi < mod->var_count; vi++)
        var_declare(ctx, mod->vars[vi].name, mod->vars[vi].type, false);
      for (int pi = 0; pi < m->param_count; pi++)
        var_declare(ctx, m->params[pi].name, m->params[pi].type, false);
      infer_pass(ctx, m->body_node);
      vtype_t rt = infer_type(ctx, m->body_node);
      ctx->var_count = sv;
      ctx->var_scope_floor = sf;
      for (int ci = sv; ci < MAX_VARS && ctx->vars[ci].name[0]; ci++)
        ctx->vars[ci].name[0] = '\0';
      if (rt.kind != SPINEL_TYPE_VALUE && rt.kind != SPINEL_TYPE_UNKNOWN)
        m->return_type = rt;
    }
    ctx->parser = saved_p;
  }
}
