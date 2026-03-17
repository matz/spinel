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

/* Module info (for module Rand etc.) */
typedef struct {
    char name[64];
    method_info_t methods[MAX_METHODS];
    int method_count;
    ivar_info_t vars[MAX_IVARS];  /* module-level instance vars */
    int var_count;
    pm_node_t *module_node;
} module_info_t;

/* Top-level function info */
typedef struct {
    char name[64];
    pm_node_t *body_node;
    pm_node_t *params_node;
    param_info_t params[MAX_PARAMS];
    int param_count;
    vtype_t return_type;
} func_info_t;

/* Variable entry in the variable table */
typedef struct {
    char name[64];
    vtype_t type;
    bool declared;
    bool is_constant;
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
} codegen_ctx_t;

void codegen_init(codegen_ctx_t *ctx, pm_parser_t *parser, FILE *out);
void codegen_program(codegen_ctx_t *ctx, pm_node_t *root);
const char *spinel_type_cname(spinel_type_t type);

#endif /* SPINEL_CODEGEN_H */
