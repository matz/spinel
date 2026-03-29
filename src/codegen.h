/*
 * codegen.h - Spinel AOT compiler: C code generation from Prism AST
 *
 * Walks the Prism AST and generates C source code. For programs with
 * class definitions, generates C structs and direct-call functions.
 * For proven types, generates unboxed C operations.
 */

#ifndef SPINEL_CODEGEN_H
#define SPINEL_CODEGEN_H

#include <stdio.h>
#include <stdbool.h>
#include <limits.h>
#include <prism.h>

/* Inferred type for an expression or variable */
typedef enum {
  SPINEL_TYPE_UNKNOWN = 0,
  SPINEL_TYPE_INTEGER,
  SPINEL_TYPE_FLOAT,
  SPINEL_TYPE_BOOLEAN,
  SPINEL_TYPE_STRING,
  SPINEL_TYPE_NIL,
  SPINEL_TYPE_OBJECT,  /* user-defined class instance */
  SPINEL_TYPE_ARRAY,   /* sp_IntArray * (built-in integer array) */
  SPINEL_TYPE_FLOAT_ARRAY, /* sp_FloatArray * (built-in float array) */
  SPINEL_TYPE_HASH,    /* sp_StrIntHash * (string→integer hash table) */
  SPINEL_TYPE_PROC,    /* sp_Val * (lambda/closure) */
  SPINEL_TYPE_POLY,    /* sp_RbValue (polymorphic: tagged union) */
  SPINEL_TYPE_VALUE,   /* boxed mrb_value (fallback) */
  SPINEL_TYPE_STR_ARRAY, /* sp_StrArray * (string array from split) */
  SPINEL_TYPE_REGEXP,    /* compiled regex pattern (regex_t *) */
  SPINEL_TYPE_RANGE,     /* sp_Range (integer range: first..last) */
  SPINEL_TYPE_TIME,      /* sp_Time (wraps time_t) */
  SPINEL_TYPE_RB_ARRAY,  /* sp_RbArray * (heterogeneous array of sp_RbValue) */
  SPINEL_TYPE_RB_HASH,   /* sp_RbHash * (heterogeneous hash: string key → sp_RbValue) */
  SPINEL_TYPE_SP_STRING, /* sp_String * (mutable, GC-managed string) */
  SPINEL_TYPE_FILE,      /* sp_File * (file object wrapping FILE *) */
  SPINEL_TYPE_STRINGIO,  /* sp_StringIO * (in-memory IO) */
} spinel_type_t;

/* Extended type: kind + optional class name for OBJECT types */
typedef struct {
  spinel_type_t kind;
  char klass[64];      /* class name when kind == SPINEL_TYPE_OBJECT */
} vtype_t;

/* Instance variable info */
typedef struct {
  char name[64];
  vtype_t type;
} ivar_info_t;

/* Method parameter info */
typedef struct {
  char name[64];
  vtype_t type;
  bool is_array;
  bool is_optional;
  bool is_keyword;     /* true for keyword parameters (name:) */
  void *default_node;  /* pm_node_t * for optional param default value */
} param_info_t;

/* Method info */
#define MAX_PARAMS 16
typedef struct {
  char name[64];
  pm_node_t *body_node;  /* AST of the method body */
  pm_node_t *params_node;
  param_info_t params[MAX_PARAMS];
  int param_count;
  vtype_t return_type;
  bool is_getter;        /* simple ivar getter: def x; @x; end */
  bool is_setter;        /* simple ivar setter: def x=(v); @x = v; end */
  char accessor_ivar[64]; /* ivar name for getter/setter */
  bool is_class_method;  /* true for def self.foo (class method) */
  bool has_rest;         /* true if method has *rest parameter */
  char rest_name[64];    /* name of the rest parameter */
  bool has_yield;        /* true if method body contains yield */
  pm_parser_t *origin_parser; /* parser that owns this method's AST */
} method_info_t;

/* Class info */
#define MAX_IVARS 96
#define MAX_METHODS 96
#define MAX_INCLUDES 16
typedef struct {
  char name[64];
  char superclass[64];   /* superclass name ("" if none) */
  ivar_info_t ivars[MAX_IVARS];
  int ivar_count;
  int own_ivar_start;    /* index where this class's own ivars begin (after inherited) */
  method_info_t *methods;
  int method_count;
  int methods_cap;
  bool is_value_type;    /* pass by value (small: e.g., Vec) */
  pm_node_t *class_node; /* AST node of the class definition */
  char includes[MAX_INCLUDES][64]; /* included module names */
  int include_count;
  int class_tag;             /* unique tag for POLY dispatch (SP_T_CLASS_BASE + N) */
  pm_parser_t *origin_parser; /* parser that owns this class's AST (for require_relative) */
} class_info_t;

/* Module constant info */
typedef struct {
  char name[64];
  vtype_t type;
  pm_node_t *value_node;  /* AST node for the value expression */
} module_const_t;

/* Module info (for module Rand etc.) */
typedef struct {
  char name[64];
  method_info_t *methods;
  int method_count;
  int methods_cap;
  ivar_info_t vars[MAX_IVARS];  /* module-level instance vars */
  int var_count;
  module_const_t consts[MAX_IVARS]; /* module-level constants */
  int const_count;
  pm_node_t *module_node;
  pm_parser_t *origin_parser;
} module_info_t;

/* Block callback function type (for yield support) */
typedef int64_t (*sp_block_fn)(void *env, int64_t arg);

/* Top-level function info */
typedef struct {
  char name[64];
  pm_node_t *body_node;
  pm_node_t *params_node;
  param_info_t params[MAX_PARAMS];
  int param_count;
  vtype_t return_type;
  bool has_yield;           /* true if function body contains yield */
  bool has_rest;            /* true if function has *rest parameter */
  char rest_name[64];       /* name of the rest parameter */
  int rest_param_index;     /* index of the rest param in params[] */
  bool has_block_param;     /* true if function has &block parameter */
  char block_param_name[64]; /* name of the block parameter (without &) */
} func_info_t;

/* Variable entry in the variable table */
typedef struct {
  char name[64];
  vtype_t type;
  bool declared;
  bool is_constant;
  bool is_array;
  int array_size;
} var_entry_t;

#define MAX_VARS 1024
#define MAX_CLASSES 160
#define MAX_MODULES 64
#define MAX_FUNCS 128

/* Code generation context */
typedef struct {
  pm_parser_t *parser;
  FILE *out;
  int indent;
  var_entry_t vars[MAX_VARS];
  int var_count;
  int var_scope_floor;  /* var_lookup searches [var_scope_floor..var_count) first */
  int temp_counter;
  int for_depth;

  /* Class/module/function registry */
  class_info_t classes[MAX_CLASSES];
  int class_count;
  module_info_t modules[MAX_MODULES];
  int module_count;
  func_info_t funcs[MAX_FUNCS];
  int func_count;

  /* Current method context (NULL when in top-level) */
  class_info_t *current_class;
  method_info_t *current_method;
  module_info_t *current_module;
  char current_func_name[64]; /* for __method__ in top-level functions */

  /* When true, the last expression in a block should be emitted as a return */
  bool implicit_return;

  /* Extension method emit context: receiver type when inside an ext method body */
  spinel_type_t ext_method_recv_type;

  /* Lambda/closure codegen state */
  int lambda_counter;            /* unique ID for each lambda function */
  bool lambda_mode;              /* true when fizzbuzz-style lambda code detected */
  FILE *lambda_out;              /* secondary output for lambda function bodies */

  /* Block/yield codegen state */
  int block_counter;             /* unique ID for each block callback */
  FILE *block_out;               /* secondary output for block function bodies */
  bool in_yield_func;            /* true when inside a function that uses yield */

  /* Non-local return from blocks: when a block contains 'return', it should
   * return from the enclosing method, not just from the block callback. */
  bool in_block_nonlocal;        /* true when inside a block with non-local return */
  int block_return_id;           /* block ID for non-local return env access */

  /* Exception handling: true when raise/rescue is used */
  bool needs_exc;
  int exc_counter;   /* unique ID for retry labels */

  /* Hash: true when any sp_StrIntHash is used */
  bool needs_hash;

  /* String split: true when sp_StrArray is needed outside lambda mode */
  bool needs_str_split;

  /* GC: true when any non-value-type class or sp_IntArray is used */
  bool needs_gc;
  int gc_type_count;  /* number of GC-managed types (for type_id assignment) */
  bool gc_scope_active; /* true when inside a function with GC roots */

  /* Poly: true when sp_RbValue (polymorphic tagged union) is used */
  bool needs_poly;

  /* RbArray: true when sp_RbArray (heterogeneous array) is used */
  bool needs_rb_array;

  /* RbHash: true when sp_RbHash (heterogeneous hash) is used */
  bool needs_rb_hash;

  /* Poly class set: track which classes a POLY func param can hold */
  #define MAX_POLY_CLASSES 8
  struct {
    char func_name[64];
    int param_idx;
    char class_names[MAX_POLY_CLASSES][64];
    int class_count;
  } poly_class_sets[MAX_FUNCS];
  int poly_class_set_count;

  /* IntArray: true when sp_IntArray is used */
  bool needs_intarray;

  /* FloatArray: true when sp_FloatArray is used */
  bool needs_floatarray;

  /* Range: true when sp_Range is used */
  bool needs_range;

  /* Time: true when sp_Time is used */
  bool needs_time;

  /* StringIO: true when sp_StringIO is used */
  bool needs_stringio;

  /* Proc: true when sp_Proc (block param / proc {} / Proc.new) is used */
  bool needs_proc;

  /* sp_String: true when mutable strings (<<) are used */
  bool needs_sp_string;

  /* File I/O: true when sp_File (file object with block) is used */
  bool needs_file_io;

  /* Regexp: true when any regex literal is used */
  bool needs_regexp;
  int regexp_counter;   /* unique ID for each compiled regex pattern */

  /* Regexp pattern storage (source strings for initialization) */
  #define MAX_REGEXPS 64
  struct {
    char pattern[256];  /* regex source pattern */
    int id;             /* unique ID (_re_N) */
  } regexps[MAX_REGEXPS];

  /* Megamorphic dispatch: collect per-method dispatch functions for 3+ types */
  #define MAX_MEGA_DISPATCH 32
  struct {
    char method_name[64];                        /* Ruby method name */
    char sanitized[64];                          /* C-safe method name */
    char class_names[MAX_POLY_CLASSES][64];      /* classes in the dispatch set */
    int class_count;
    spinel_type_t return_kind;                   /* return type kind */
  } mega_dispatch[MAX_MEGA_DISPATCH];
  int mega_dispatch_count;

  /* Lambda scope stack for capture analysis */
  #define MAX_LAMBDA_DEPTH 64
  #define MAX_SCOPE_VARS 32
  struct {
    char param[64];            /* parameter name for this lambda */
    char captures[MAX_SCOPE_VARS][64]; /* captured variable names */
    int capture_count;
    int depth;
  } lambda_scope[MAX_LAMBDA_DEPTH];
  int lambda_scope_depth;

  /* Extension methods on built-in types (open class support) */
  #define MAX_EXT_METHODS 32
  struct {
    char type_name[64];    /* "String", "Integer", "Array", etc. */
    method_info_t method;
    spinel_type_t recv_type; /* SPINEL_TYPE_STRING, etc. */
  } ext_methods[MAX_EXT_METHODS];
  int ext_method_count;

  /* Source file path (for resolving require_relative) */
  const char *source_path;

  /* Library search paths (for resolving require "name") */
  #define MAX_LIB_PATHS 16
  const char *lib_paths[MAX_LIB_PATHS];
  int lib_path_count;

  /* Required files (kept alive for AST references in method_info_t) */
  #define MAX_REQUIRED_FILES 80
  struct {
    pm_parser_t parser;
    pm_node_t *root;
    char *source;
    char *path; /* resolved path for deduplication (heap-allocated) */
  } required_files[MAX_REQUIRED_FILES];
  int required_file_count;
} codegen_ctx_t;

void codegen_init(codegen_ctx_t *ctx, pm_parser_t *parser, FILE *out,
         const char *source_path);
void codegen_program(codegen_ctx_t *ctx, pm_node_t *root);

/* --- Capture list (used by expr.c and codegen.c) --- */
typedef struct {
  char names[256][64];
  int count;
} capture_list_t;

/* --- C keyword escaping --- */
static inline const char *escape_c_keyword(const char *name) {
  /* Only escape the most problematic C keywords that might be ivar names */
  if (name[0] == 'u' && strcmp(name, "union") == 0) return "union_";
  if (name[0] == 's' && strcmp(name, "struct") == 0) return "struct_";
  if (name[0] == 'e' && strcmp(name, "enum") == 0) return "enum_";
  if (name[0] == 'd' && strcmp(name, "default") == 0) return "default_";
  if (name[0] == 'r' && strcmp(name, "register") == 0) return "register_";
  return name;
}

/* --- Default initializer for a type kind (e.g., " = 0" for INTEGER) --- */
static inline const char *default_init_for_type(spinel_type_t kind) {
  switch (kind) {
  case SPINEL_TYPE_INTEGER: return " = 0";
  case SPINEL_TYPE_FLOAT:   return " = 0.0";
  case SPINEL_TYPE_BOOLEAN: return " = FALSE";
  case SPINEL_TYPE_STRING:
  case SPINEL_TYPE_PROC:    return " = NULL";
  default:                  return "";
  }
}

/* --- Shared utility functions (codegen.c) --- */
char *xstrdup(const char *s);
char *sfmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
char *c_safe_name(const char *name);
char *cstr(codegen_ctx_t *ctx, pm_constant_id_t id);
bool ceq(codegen_ctx_t *ctx, pm_constant_id_t id, const char *s);
void emit(codegen_ctx_t *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void emit_raw(codegen_ctx_t *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* --- Type helpers (type.c) --- */
vtype_t vt_prim(spinel_type_t k);
vtype_t vt_obj(const char *klass);
bool vt_is_numeric(vtype_t t);
bool vt_is_poly_eligible(vtype_t t);
char *poly_box_expr_vt(codegen_ctx_t *ctx, vtype_t src, const char *expr);
char *poly_box_expr(spinel_type_t src_kind, const char *expr);
const char *spinel_type_cname(spinel_type_t type);
char *vt_ctype(codegen_ctx_t *ctx, vtype_t t, bool as_ptr);
bool is_gc_type(codegen_ctx_t *ctx, vtype_t t);
vtype_t binop_result(vtype_t l, vtype_t r, const char *op);

/* --- Lookup functions (codegen.c) --- */
class_info_t *find_class(codegen_ctx_t *ctx, const char *name);
method_info_t *find_method(class_info_t *cls, const char *name);
method_info_t *find_method_inherited(codegen_ctx_t *ctx, class_info_t *cls, const char *name, class_info_t **owner);
ivar_info_t *find_ivar(class_info_t *cls, const char *name);
module_info_t *find_module(codegen_ctx_t *ctx, const char *name);
func_info_t *find_func(codegen_ctx_t *ctx, const char *name);

/* --- Built-in method table (methods.c) --- */
#define MAX_METHOD_ARGS 4
typedef struct {
  const char *name;
  spinel_type_t return_type;
  int min_argc;           /* minimum required args (-1 = don't care) */
  int max_argc;           /* maximum args (-1 = variadic) */
  bool needs_block;       /* true if method requires a block */
  spinel_type_t arg_types[MAX_METHOD_ARGS]; /* expected arg types (0=any) */
} builtin_method_def_t;

const builtin_method_def_t *builtin_find_method(spinel_type_t kind, const char *name);
bool builtin_has_method(spinel_type_t kind, const char *name);
spinel_type_t builtin_return_type(spinel_type_t recv_kind, const char *name);
spinel_type_t builtin_arg_type(spinel_type_t recv_kind, const char *name, int arg_idx);

/* --- Extension method lookup (codegen.c) --- */
method_info_t *find_ext_method(codegen_ctx_t *ctx, spinel_type_t recv_type, const char *name);
spinel_type_t builtin_type_for_name(const char *name);

/* --- Variable management (codegen.c) --- */
var_entry_t *var_lookup(codegen_ctx_t *ctx, const char *name);
var_entry_t *var_declare(codegen_ctx_t *ctx, const char *name, vtype_t type, bool is_constant);
char *make_cname(const char *name, bool is_constant);
const char *sanitize_method(const char *name);

/* --- Polymorphism helpers (codegen.c) --- */
void poly_class_add(codegen_ctx_t *ctx, const char *func_name, int param_idx, const char *class_name);
int poly_class_get(codegen_ctx_t *ctx, const char *func_name, int param_idx, char classes[][64]);
int mega_dispatch_register(codegen_ctx_t *ctx, const char *method, const char *sanitized, char classes[][64], int nclasses, spinel_type_t return_kind);

/* --- Capture management (codegen.c) --- */
void capture_list_add(capture_list_t *cl, const char *name);
bool capture_list_has(capture_list_t *cl, const char *name);
void scan_captures(codegen_ctx_t *ctx, pm_node_t *node, const char *param_name, capture_list_t *local_defs, capture_list_t *result);

/* --- Type inference (type.c) --- */
vtype_t infer_type(codegen_ctx_t *ctx, pm_node_t *node);
void infer_pass(codegen_ctx_t *ctx, pm_node_t *node);
void resolve_class_types(codegen_ctx_t *ctx, pm_node_t *prog_root);

/* --- Class analysis (codegen.c) --- */
void class_analysis_pass(codegen_ctx_t *ctx, pm_node_t *root);
bool has_yield_nodes(pm_node_t *node);
bool block_has_return(pm_node_t *node);

/* --- Expression codegen (expr.c) --- */
char *codegen_expr(codegen_ctx_t *ctx, pm_node_t *node);

/* --- Statement codegen (stmt.c) --- */
void codegen_stmt(codegen_ctx_t *ctx, pm_node_t *node);
void codegen_stmts(codegen_ctx_t *ctx, pm_node_t *node);
void codegen_pattern_cond(codegen_ctx_t *ctx, pm_node_t *pattern, int case_id);

/* --- Emission functions (emit.c) --- */
void emit_header(codegen_ctx_t *ctx);
void emit_struct(codegen_ctx_t *ctx, class_info_t *cls);
void emit_initialize_func(codegen_ctx_t *ctx, class_info_t *cls);
void emit_constructor(codegen_ctx_t *ctx, class_info_t *cls);
void emit_method(codegen_ctx_t *ctx, class_info_t *cls, method_info_t *m);
void emit_top_func(codegen_ctx_t *ctx, func_info_t *f);
void emit_module(codegen_ctx_t *ctx, module_info_t *mod);
void emit_lambda_fizzbuzz_funcs(codegen_ctx_t *ctx);
void emit_mega_dispatch_funcs(codegen_ctx_t *ctx);

/* --- Lambda codegen (codegen.c) --- */
char *codegen_lambda(codegen_ctx_t *ctx, pm_lambda_node_t *lam);

/* --- Block/parameter helpers (codegen.c) --- */
char *extract_block_param(codegen_ctx_t *ctx, pm_block_node_t *blk);
void extract_params(codegen_ctx_t *ctx, pm_parameters_node_t *params,
          param_info_t *out, int *count, bool *has_rest,
          char *rest_name, size_t rest_name_size);

/* --- Require handling (codegen.c) --- */
bool is_require_relative(codegen_ctx_t *ctx, pm_node_t *node);
bool is_require(codegen_ctx_t *ctx, pm_node_t *node);

#endif /* SPINEL_CODEGEN_H */
