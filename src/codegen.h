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
    SPINEL_TYPE_PROC,    /* sp_Val * (lambda/closure) */
    SPINEL_TYPE_VALUE,   /* boxed mrb_value (fallback) */
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
} param_info_t;

/* Method info */
#define MAX_PARAMS 8
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
} method_info_t;

/* Class info */
#define MAX_IVARS 16
#define MAX_METHODS 32
typedef struct {
    char name[64];
    ivar_info_t ivars[MAX_IVARS];
    int ivar_count;
    method_info_t methods[MAX_METHODS];
    int method_count;
    bool is_value_type;    /* pass by value (small: e.g., Vec) */
    pm_node_t *class_node; /* AST node of the class definition */
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
    method_info_t methods[MAX_METHODS];
    int method_count;
    ivar_info_t vars[MAX_IVARS];  /* module-level instance vars */
    int var_count;
    module_const_t consts[MAX_IVARS]; /* module-level constants */
    int const_count;
    pm_node_t *module_node;
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

#define MAX_VARS 256
#define MAX_CLASSES 16
#define MAX_MODULES 8
#define MAX_FUNCS 16

/* Code generation context */
typedef struct {
    pm_parser_t *parser;
    FILE *out;
    int indent;
    var_entry_t vars[MAX_VARS];
    int var_count;
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

    /* When true, the last expression in a block should be emitted as a return */
    bool implicit_return;

    /* Lambda/closure codegen state */
    int lambda_counter;            /* unique ID for each lambda function */
    bool lambda_mode;              /* true when fizzbuzz-style lambda code detected */
    FILE *lambda_out;              /* secondary output for lambda function bodies */

    /* Block/yield codegen state */
    int block_counter;             /* unique ID for each block callback */
    FILE *block_out;               /* secondary output for block function bodies */
    bool in_yield_func;            /* true when inside a function that uses yield */

    /* GC: true when any non-value-type class or sp_IntArray is used */
    bool needs_gc;
    int gc_type_count;  /* number of GC-managed types (for type_id assignment) */
    bool gc_scope_active; /* true when inside a function with GC roots */

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
} codegen_ctx_t;

void codegen_init(codegen_ctx_t *ctx, pm_parser_t *parser, FILE *out);
void codegen_program(codegen_ctx_t *ctx, pm_node_t *root);
const char *spinel_type_cname(spinel_type_t type);

#endif /* SPINEL_CODEGEN_H */
