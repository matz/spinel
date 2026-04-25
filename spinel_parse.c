/*
 * spinel_parse.c - Prism AST Serializer (C version)
 *
 * Equivalent to spinel_parse.rb but links with libprism directly.
 * Parses Ruby source and outputs line-based text AST for spinel_codegen.
 *
 * Build: cc -O2 -I$(PRISM)/include spinel_parse.c -L$(PRISM)/build -lprism -o spinel_parse
 *
 * Output format:
 *   ROOT <id>
 *   N <id> <type>           - node declaration
 *   S <id> <field> <escaped> - string field
 *   I <id> <field> <integer> - integer field
 *   F <id> <field> <float>   - float field
 *   R <id> <field> <ref_id>  - reference (-1 for nil)
 *   A <id> <field> <ids>     - array of references
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <prism.h>

/* ---- Output buffer ---- */
static char **lines;
static int line_count;
static int line_cap;
static int node_counter;

static void out_add(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char buf[4096];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (line_count >= line_cap) {
    line_cap = line_cap * 2 + 256;
    lines = realloc(lines, sizeof(char *) * line_cap);
  }
  lines[line_count++] = strdup(buf);
}

/* ---- Name from constant pool ---- */
static const pm_parser_t *g_parser;

static char *cstr(pm_constant_id_t id) {
  if (id == 0) return strdup("");
  pm_constant_t *c = &g_parser->constant_pool.constants[id - 1];
  char *buf = malloc(c->length + 1);
  memcpy(buf, c->start, c->length);
  buf[c->length] = '\0';
  return buf;
}

/* ---- String escaping ---- */
static char *escape_str(const uint8_t *src, size_t len) {
  /* Worst case: every char becomes %XX = 3x */
  char *out = malloc(len * 3 + 1);
  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    uint8_t c = src[i];
    if (c == '%')       { out[j++]='%'; out[j++]='2'; out[j++]='5'; }
    else if (c == '\n') { out[j++]='%'; out[j++]='0'; out[j++]='A'; }
    else if (c == '\r') { out[j++]='%'; out[j++]='0'; out[j++]='D'; }
    else if (c == '\t') { out[j++]='%'; out[j++]='0'; out[j++]='9'; }
    else if (c == ' ')  { out[j++]='%'; out[j++]='2'; out[j++]='0'; }
    else out[j++] = c;
  }
  out[j] = '\0';
  return out;
}

static char *escape_pm_string(const pm_string_t *s) {
  return escape_str(pm_string_source(s), pm_string_length(s));
}

/* ---- Forward ---- */
static int flatten(pm_node_t *node);

/* ---- Emit helpers ---- */
static void emit_str(int id, const char *field, const char *val) {
  out_add("S %d %s %s", id, field, val);
}

static void emit_int(int id, const char *field, long long val) {
  out_add("I %d %s %lld", id, field, val);
}

static void emit_float(int id, const char *field, double val) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%.17g", val);
  /* Ensure there's a decimal point (Ruby outputs 0.0, not 0) */
  if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E'))
    strcat(buf, ".0");
  out_add("F %d %s %s", id, field, buf);
}

static void emit_ref(int id, const char *field, pm_node_t *child) {
  int cid = child ? flatten(child) : -1;
  out_add("R %d %s %d", id, field, cid);
}

static void emit_node_array(int id, const char *field, pm_node_list_t *list) {
  if (!list || list->size == 0) {
    out_add("A %d %s ", id, field);
    return;
  }
  int *ids = malloc(sizeof(int) * list->size);
  for (size_t i = 0; i < list->size; i++)
    ids[i] = flatten(list->nodes[i]);
  /* Build comma-separated string */
  char buf[65536];
  int pos = 0;
  for (size_t i = 0; i < list->size; i++) {
    if (i > 0) buf[pos++] = ',';
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%d", ids[i]);
  }
  buf[pos] = '\0';
  out_add("A %d %s %s", id, field, buf);
  free(ids);
}

/* ---- Integer value extraction ---- */
static long long pm_int_value(pm_integer_t *integer) {
  long long val = 0;
  if (integer->length == 0) {
    val = (long long)integer->value;
  } else {
    /* Large integer - approximate with first word */
    val = (long long)integer->value;
    /* TODO: proper bignum support */
  }
  if (integer->negative) val = -val;
  return val;
}

/* ---- Main flattening ---- */
static int flatten(pm_node_t *node) {
  if (!node) return -1;

  int id = node_counter++;
  pm_node_type_t t = PM_NODE_TYPE(node);

#define N(type_name) out_add("N %d " type_name, id)
#define S(field, val) do { char *_e = (val); emit_str(id, field, _e); free(_e); } while(0)
#define I(field, val) emit_int(id, field, val)
#define F(field, val) emit_float(id, field, val)
#define R(field, child) emit_ref(id, field, (pm_node_t *)(child))
#define A(field, list) emit_node_array(id, field, list)
#define NAME(field, cid) do { char *_n = cstr(cid); char *_e = escape_str((const uint8_t *)_n, strlen(_n)); emit_str(id, field, _e); free(_e); free(_n); } while(0)

  switch (t) {
  case PM_PROGRAM_NODE: {
    pm_program_node_t *n = (pm_program_node_t *)node;
    N("ProgramNode");
    R("statements", n->statements);
    break;
  }
  case PM_STATEMENTS_NODE: {
    pm_statements_node_t *n = (pm_statements_node_t *)node;
    N("StatementsNode");
    A("body", &n->body);
    break;
  }
  case PM_CLASS_NODE: {
    pm_class_node_t *n = (pm_class_node_t *)node;
    N("ClassNode");
    R("constant_path", n->constant_path);
    R("superclass", n->superclass);
    R("body", n->body);
    break;
  }
  case PM_MODULE_NODE: {
    pm_module_node_t *n = (pm_module_node_t *)node;
    N("ModuleNode");
    R("constant_path", n->constant_path);
    R("body", n->body);
    break;
  }
  case PM_DEF_NODE: {
    pm_def_node_t *n = (pm_def_node_t *)node;
    N("DefNode");
    NAME("name", n->name);
    R("parameters", n->parameters);
    R("body", n->body);
    R("receiver", n->receiver);
    break;
  }
  case PM_CALL_NODE: {
    pm_call_node_t *n = (pm_call_node_t *)node;
    N("CallNode");
    NAME("name", n->name);
    R("receiver", n->receiver);
    R("arguments", n->arguments);
    R("block", n->block);
    if (PM_NODE_FLAG_P(node, PM_CALL_NODE_FLAGS_SAFE_NAVIGATION)) {
      S("call_operator", escape_str((const uint8_t *)"&.", 2));
    }
    break;
  }
  case PM_CONSTANT_WRITE_NODE: {
    pm_constant_write_node_t *n = (pm_constant_write_node_t *)node;
    N("ConstantWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_CONSTANT_PATH_WRITE_NODE: {
    pm_constant_path_write_node_t *n = (pm_constant_path_write_node_t *)node;
    N("ConstantPathWriteNode");
    R("value", n->value);
    R("target", n->target);
    break;
  }
  case PM_CONSTANT_READ_NODE: {
    pm_constant_read_node_t *n = (pm_constant_read_node_t *)node;
    N("ConstantReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_CONSTANT_PATH_NODE: {
    pm_constant_path_node_t *n = (pm_constant_path_node_t *)node;
    N("ConstantPathNode");
    R("parent", n->parent);
    NAME("name", n->name);
    break;
  }
  case PM_LOCAL_VARIABLE_WRITE_NODE: {
    pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
    N("LocalVariableWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_READ_NODE: {
    pm_local_variable_read_node_t *n = (pm_local_variable_read_node_t *)node;
    N("LocalVariableReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_local_variable_operator_write_node_t *n = (pm_local_variable_operator_write_node_t *)node;
    N("LocalVariableOperatorWriteNode");
    NAME("name", n->name);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_TARGET_NODE: {
    pm_local_variable_target_node_t *n = (pm_local_variable_target_node_t *)node;
    N("LocalVariableTargetNode");
    NAME("name", n->name);
    break;
  }
  case PM_INSTANCE_VARIABLE_WRITE_NODE: {
    pm_instance_variable_write_node_t *n = (pm_instance_variable_write_node_t *)node;
    N("InstanceVariableWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_INSTANCE_VARIABLE_READ_NODE: {
    pm_instance_variable_read_node_t *n = (pm_instance_variable_read_node_t *)node;
    N("InstanceVariableReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_INSTANCE_VARIABLE_TARGET_NODE: {
    pm_instance_variable_target_node_t *n = (pm_instance_variable_target_node_t *)node;
    N("InstanceVariableTargetNode");
    NAME("name", n->name);
    break;
  }
  case PM_INSTANCE_VARIABLE_AND_WRITE_NODE: {
    pm_instance_variable_and_write_node_t *n = (pm_instance_variable_and_write_node_t *)node;
    N("InstanceVariableAndWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_INSTANCE_VARIABLE_OR_WRITE_NODE: {
    pm_instance_variable_or_write_node_t *n = (pm_instance_variable_or_write_node_t *)node;
    N("InstanceVariableOrWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_instance_variable_operator_write_node_t *n = (pm_instance_variable_operator_write_node_t *)node;
    N("InstanceVariableOperatorWriteNode");
    NAME("name", n->name);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    break;
  }
  case PM_GLOBAL_VARIABLE_WRITE_NODE: {
    pm_global_variable_write_node_t *n = (pm_global_variable_write_node_t *)node;
    N("GlobalVariableWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_GLOBAL_VARIABLE_READ_NODE: {
    pm_global_variable_read_node_t *n = (pm_global_variable_read_node_t *)node;
    N("GlobalVariableReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_INTEGER_NODE: {
    pm_integer_node_t *n = (pm_integer_node_t *)node;
    N("IntegerNode");
    I("value", pm_int_value(&n->value));
    break;
  }
  case PM_FLOAT_NODE: {
    pm_float_node_t *n = (pm_float_node_t *)node;
    N("FloatNode");
    F("value", n->value);
    break;
  }
  case PM_STRING_NODE: {
    pm_string_node_t *n = (pm_string_node_t *)node;
    N("StringNode");
    S("content", escape_pm_string(&n->unescaped));
    break;
  }
  case PM_INTERPOLATED_STRING_NODE: {
    pm_interpolated_string_node_t *n = (pm_interpolated_string_node_t *)node;
    N("InterpolatedStringNode");
    A("parts", &n->parts);
    break;
  }
  case PM_EMBEDDED_STATEMENTS_NODE: {
    pm_embedded_statements_node_t *n = (pm_embedded_statements_node_t *)node;
    N("EmbeddedStatementsNode");
    R("statements", n->statements);
    break;
  }
  case PM_SYMBOL_NODE: {
    pm_symbol_node_t *n = (pm_symbol_node_t *)node;
    N("SymbolNode");
    S("value", escape_pm_string(&n->unescaped));
    break;
  }
  case PM_TRUE_NODE:
    N("TrueNode");
    break;
  case PM_FALSE_NODE:
    N("FalseNode");
    break;
  case PM_NIL_NODE:
    N("NilNode");
    break;
  case PM_SELF_NODE:
    N("SelfNode");
    break;
  case PM_ARRAY_NODE: {
    pm_array_node_t *n = (pm_array_node_t *)node;
    N("ArrayNode");
    A("elements", &n->elements);
    break;
  }
  case PM_HASH_NODE: {
    pm_hash_node_t *n = (pm_hash_node_t *)node;
    N("HashNode");
    A("elements", &n->elements);
    break;
  }
  case PM_ASSOC_NODE: {
    pm_assoc_node_t *n = (pm_assoc_node_t *)node;
    N("AssocNode");
    R("key", n->key);
    R("value", n->value);
    break;
  }
  case PM_KEYWORD_HASH_NODE: {
    pm_keyword_hash_node_t *n = (pm_keyword_hash_node_t *)node;
    N("KeywordHashNode");
    A("elements", &n->elements);
    break;
  }
  case PM_RANGE_NODE: {
    pm_range_node_t *n = (pm_range_node_t *)node;
    N("RangeNode");
    R("left", n->left);
    R("right", n->right);
    break;
  }
  case PM_IF_NODE: {
    pm_if_node_t *n = (pm_if_node_t *)node;
    N("IfNode");
    R("predicate", n->predicate);
    R("statements", n->statements);
    R("subsequent", n->subsequent);
    break;
  }
  case PM_ELSE_NODE: {
    pm_else_node_t *n = (pm_else_node_t *)node;
    N("ElseNode");
    R("statements", n->statements);
    break;
  }
  case PM_UNLESS_NODE: {
    pm_unless_node_t *n = (pm_unless_node_t *)node;
    N("UnlessNode");
    R("predicate", n->predicate);
    R("statements", n->statements);
    R("else_clause", n->else_clause);
    break;
  }
  case PM_WHILE_NODE: {
    pm_while_node_t *n = (pm_while_node_t *)node;
    N("WhileNode");
    R("predicate", n->predicate);
    R("statements", n->statements);
    break;
  }
  case PM_UNTIL_NODE: {
    pm_until_node_t *n = (pm_until_node_t *)node;
    N("UntilNode");
    R("predicate", n->predicate);
    R("statements", n->statements);
    break;
  }
  case PM_FOR_NODE: {
    pm_for_node_t *n = (pm_for_node_t *)node;
    N("ForNode");
    R("index", n->index);
    R("collection", n->collection);
    R("statements", n->statements);
    break;
  }
  case PM_CASE_NODE: {
    pm_case_node_t *n = (pm_case_node_t *)node;
    N("CaseNode");
    R("predicate", n->predicate);
    A("conditions", &n->conditions);
    R("else_clause", n->else_clause);
    break;
  }
  case PM_CASE_MATCH_NODE: {
    pm_case_match_node_t *n = (pm_case_match_node_t *)node;
    N("CaseMatchNode");
    R("predicate", n->predicate);
    A("conditions", &n->conditions);
    R("else_clause", n->else_clause);
    break;
  }
  case PM_WHEN_NODE: {
    pm_when_node_t *n = (pm_when_node_t *)node;
    N("WhenNode");
    A("conditions", &n->conditions);
    R("statements", n->statements);
    break;
  }
  case PM_IN_NODE: {
    pm_in_node_t *n = (pm_in_node_t *)node;
    N("InNode");
    R("pattern", n->pattern);
    R("statements", n->statements);
    break;
  }
  case PM_BEGIN_NODE: {
    pm_begin_node_t *n = (pm_begin_node_t *)node;
    N("BeginNode");
    R("statements", n->statements);
    R("rescue_clause", n->rescue_clause);
    R("ensure_clause", n->ensure_clause);
    R("else_clause", n->else_clause);
    break;
  }
  case PM_ENSURE_NODE: {
    pm_ensure_node_t *n = (pm_ensure_node_t *)node;
    N("EnsureNode");
    R("statements", n->statements);
    break;
  }
  case PM_RESCUE_NODE: {
    pm_rescue_node_t *n = (pm_rescue_node_t *)node;
    N("RescueNode");
    A("exceptions", &n->exceptions);
    R("reference", n->reference);
    R("statements", n->statements);
    R("subsequent", n->subsequent);
    break;
  }
  case PM_RESCUE_MODIFIER_NODE: {
    pm_rescue_modifier_node_t *n = (pm_rescue_modifier_node_t *)node;
    N("RescueModifierNode");
    R("expression", n->expression);
    R("rescue_expression", n->rescue_expression);
    break;
  }
  case PM_RETURN_NODE: {
    pm_return_node_t *n = (pm_return_node_t *)node;
    N("ReturnNode");
    R("arguments", n->arguments);
    break;
  }
  case PM_BREAK_NODE:
    N("BreakNode");
    break;
  case PM_NEXT_NODE:
    N("NextNode");
    break;
  case PM_RETRY_NODE:
    N("RetryNode");
    break;
  case PM_YIELD_NODE: {
    pm_yield_node_t *n = (pm_yield_node_t *)node;
    N("YieldNode");
    R("arguments", n->arguments);
    break;
  }
  case PM_BLOCK_NODE: {
    pm_block_node_t *n = (pm_block_node_t *)node;
    N("BlockNode");
    /* Serialize block parameters */
    if (n->parameters) {
      if (PM_NODE_TYPE(n->parameters) == PM_BLOCK_PARAMETERS_NODE) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)n->parameters;
        int bpid = node_counter++;
        out_add("N %d BlockParametersNode", bpid);
        if (bp->parameters) {
          emit_ref(bpid, "parameters", (pm_node_t *)bp->parameters);
        }
        out_add("R %d %s %d", id, "parameters", bpid);
      } else if (PM_NODE_TYPE(n->parameters) == PM_NUMBERED_PARAMETERS_NODE) {
        pm_numbered_parameters_node_t *np = (pm_numbered_parameters_node_t *)n->parameters;
        int npid = node_counter++;
        out_add("N %d NumberedParametersNode", npid);
        emit_int(npid, "maximum", np->maximum);
        out_add("R %d %s %d", id, "parameters", npid);
      } else {
        R("parameters", n->parameters);
      }
    }
    R("body", n->body);
    break;
  }
  case PM_PARAMETERS_NODE: {
    pm_parameters_node_t *n = (pm_parameters_node_t *)node;
    N("ParametersNode");
    A("requireds", &n->requireds);
    A("optionals", &n->optionals);
    A("keywords", &n->keywords);
    if (n->rest) R("rest", n->rest);
    if (n->block) R("block", n->block);
    break;
  }
  case PM_REQUIRED_PARAMETER_NODE: {
    pm_required_parameter_node_t *n = (pm_required_parameter_node_t *)node;
    N("RequiredParameterNode");
    NAME("name", n->name);
    break;
  }
  case PM_OPTIONAL_PARAMETER_NODE: {
    pm_optional_parameter_node_t *n = (pm_optional_parameter_node_t *)node;
    N("OptionalParameterNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_REST_PARAMETER_NODE: {
    pm_rest_parameter_node_t *n = (pm_rest_parameter_node_t *)node;
    N("RestParameterNode");
    if (n->name) { NAME("name", n->name); }
    break;
  }
  case PM_BLOCK_PARAMETER_NODE: {
    pm_block_parameter_node_t *n = (pm_block_parameter_node_t *)node;
    N("BlockParameterNode");
    if (n->name) { NAME("name", n->name); }
    break;
  }
  case PM_BLOCK_LOCAL_VARIABLE_NODE: {
    pm_block_local_variable_node_t *n = (pm_block_local_variable_node_t *)node;
    N("BlockLocalVariableNode");
    NAME("name", n->name);
    break;
  }
  case PM_KEYWORD_REST_PARAMETER_NODE: {
    pm_keyword_rest_parameter_node_t *n = (pm_keyword_rest_parameter_node_t *)node;
    N("KeywordRestParameterNode");
    if (n->name) { NAME("name", n->name); }
    break;
  }
  case PM_REQUIRED_KEYWORD_PARAMETER_NODE: {
    pm_required_keyword_parameter_node_t *n = (pm_required_keyword_parameter_node_t *)node;
    N("RequiredKeywordParameterNode");
    NAME("name", n->name);
    break;
  }
  case PM_OPTIONAL_KEYWORD_PARAMETER_NODE: {
    pm_optional_keyword_parameter_node_t *n = (pm_optional_keyword_parameter_node_t *)node;
    N("OptionalKeywordParameterNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_PARENTHESES_NODE: {
    pm_parentheses_node_t *n = (pm_parentheses_node_t *)node;
    N("ParenthesesNode");
    R("body", n->body);
    break;
  }
  case PM_AND_NODE: {
    pm_and_node_t *n = (pm_and_node_t *)node;
    N("AndNode");
    R("left", n->left);
    R("right", n->right);
    break;
  }
  case PM_OR_NODE: {
    pm_or_node_t *n = (pm_or_node_t *)node;
    N("OrNode");
    R("left", n->left);
    R("right", n->right);
    break;
  }
  case PM_DEFINED_NODE: {
    pm_defined_node_t *n = (pm_defined_node_t *)node;
    N("DefinedNode");
    R("value", n->value);
    break;
  }
  case PM_SOURCE_LINE_NODE: {
    N("SourceLineNode");
    int32_t line = pm_newline_list_line(&g_parser->newline_list, node->location.start, g_parser->start_line);
    I("start_line", (long long)line);
    break;
  }
  case PM_SPLAT_NODE: {
    pm_splat_node_t *n = (pm_splat_node_t *)node;
    N("SplatNode");
    R("expression", n->expression);
    break;
  }
  case PM_SUPER_NODE: {
    pm_super_node_t *n = (pm_super_node_t *)node;
    N("SuperNode");
    R("arguments", n->arguments);
    break;
  }
  case PM_FORWARDING_SUPER_NODE:
    N("ForwardingSuperNode");
    break;
  case PM_MULTI_WRITE_NODE: {
    pm_multi_write_node_t *n = (pm_multi_write_node_t *)node;
    N("MultiWriteNode");
    A("lefts", &n->lefts);
    R("value", n->value);
    break;
  }
  case PM_LAMBDA_NODE: {
    pm_lambda_node_t *n = (pm_lambda_node_t *)node;
    N("LambdaNode");
    if (n->parameters) {
      if (PM_NODE_TYPE(n->parameters) == PM_BLOCK_PARAMETERS_NODE) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)n->parameters;
        if (bp->parameters) {
          R("parameters", bp->parameters);
        }
      } else if (PM_NODE_TYPE(n->parameters) != PM_NUMBERED_PARAMETERS_NODE) {
        R("parameters", n->parameters);
      }
    }
    if (n->body) R("body", n->body);
    break;
  }
  case PM_X_STRING_NODE: {
    pm_x_string_node_t *n = (pm_x_string_node_t *)node;
    N("XStringNode");
    S("content", escape_pm_string(&n->unescaped));
    break;
  }
  case PM_INTERPOLATED_X_STRING_NODE: {
    pm_interpolated_x_string_node_t *n = (pm_interpolated_x_string_node_t *)node;
    N("InterpolatedXStringNode");
    A("parts", &n->parts);
    break;
  }
  case PM_REGULAR_EXPRESSION_NODE: {
    pm_regular_expression_node_t *n = (pm_regular_expression_node_t *)node;
    N("RegularExpressionNode");
    S("unescaped", escape_pm_string(&n->unescaped));
    break;
  }
  case PM_NUMBERED_REFERENCE_READ_NODE: {
    pm_numbered_reference_read_node_t *n = (pm_numbered_reference_read_node_t *)node;
    N("NumberedReferenceReadNode");
    I("number", n->number);
    break;
  }
  case PM_MATCH_WRITE_NODE: {
    pm_match_write_node_t *n = (pm_match_write_node_t *)node;
    N("MatchWriteNode");
    R("call", n->call);
    break;
  }
  case PM_ALTERNATION_PATTERN_NODE: {
    pm_alternation_pattern_node_t *n = (pm_alternation_pattern_node_t *)node;
    N("AlternationPatternNode");
    R("left", n->left);
    R("right", n->right);
    break;
  }
  case PM_NUMBERED_PARAMETERS_NODE: {
    pm_numbered_parameters_node_t *n = (pm_numbered_parameters_node_t *)node;
    N("NumberedParametersNode");
    I("maximum", n->maximum);
    break;
  }
  case PM_ARGUMENTS_NODE: {
    pm_arguments_node_t *n = (pm_arguments_node_t *)node;
    N("ArgumentsNode");
    A("arguments", &n->arguments);
    break;
  }
  case PM_BLOCK_PARAMETERS_NODE: {
    pm_block_parameters_node_t *n = (pm_block_parameters_node_t *)node;
    N("BlockParametersNode");
    if (n->parameters) R("parameters", n->parameters);
    break;
  }
  case PM_IT_PARAMETERS_NODE:
    N("ItParametersNode");
    break;
  default: {
    /* Fallback: emit unknown node type */
    char buf[64];
    snprintf(buf, sizeof(buf), "UnknownNode_%d", (int)t);
    out_add("N %d %s", id, buf);
    break;
  }
  }

#undef N
#undef S
#undef I
#undef F
#undef R
#undef A
#undef NAME

  return id;
}

/* ---- require_relative resolution ---- */
static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc(len + 1);
  size_t nread = fread(buf, 1, len, f);
  buf[nread] = '\0';
  fclose(f);
  return buf;
}

/* Simple require_relative resolver: replace lines matching
   require_relative "path" with the file content */
static char *resolve_requires(const char *source, const char *source_path) {
  /* Get base directory */
  char *path_copy = strdup(source_path);
  char *dir = strdup(path_copy);
  /* Find last / */
  char *slash = strrchr(dir, '/');
  if (slash) *slash = '\0';
  else { free(dir); dir = strdup("."); }
  free(path_copy);

  char *result = strdup(source);
  char *pos;
  while ((pos = strstr(result, "require_relative")) != NULL) {
    /* Check it's at start of line */
    if (pos != result && *(pos - 1) != '\n') {
      /* Not at line start, skip */
      break;
    }
    char *line_end = strchr(pos, '\n');
    if (!line_end) line_end = pos + strlen(pos);

    /* Extract quoted path */
    char *q1 = strchr(pos, '"');
    char *q2 = strchr(pos, '\'');
    char quote_char;
    char *start;
    if (q1 && q1 < line_end && (!q2 || q1 < q2)) {
      quote_char = '"';
      start = q1 + 1;
    } else if (q2 && q2 < line_end) {
      quote_char = '\'';
      start = q2 + 1;
    } else break;

    char *end = strchr(start, quote_char);
    if (!end || end > line_end) break;

    size_t path_len = end - start;
    char rel_path[512];
    snprintf(rel_path, sizeof(rel_path), "%.*s", (int)path_len, start);

    /* Build full path */
    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir, rel_path);
    if (!strstr(full_path, ".rb"))
      strcat(full_path, ".rb");

    char *content = read_file(full_path);
    if (!content) {
      content = strdup("# require_relative not found");
    } else {
      /* Recursively resolve */
      char *resolved = resolve_requires(content, full_path);
      free(content);
      content = resolved;
    }

    /* Replace the line */
    size_t line_len = (line_end - pos) + ((*line_end == '\n') ? 1 : 0);
    size_t content_len = strlen(content);
    size_t result_len = strlen(result);
    size_t before_len = pos - result;

    char *new_result = malloc(result_len - line_len + content_len + 2);
    memcpy(new_result, result, before_len);
    memcpy(new_result + before_len, content, content_len);
    if (content_len > 0 && content[content_len - 1] != '\n')
      new_result[before_len + content_len++] = '\n';
    memcpy(new_result + before_len + content_len, pos + line_len, result_len - before_len - line_len + 1);

    free(result);
    result = new_result;
    free(content);
  }
  free(dir);
  return result;
}

/* ---- Plain require resolution ---- */
static char *resolve_plain_requires(char *source, const char *exe_path) {
  /* Find lib/ directory relative to this executable */
  char lib_dir[1024];
  strncpy(lib_dir, exe_path, sizeof(lib_dir) - 1);
  char *slash = strrchr(lib_dir, '/');
  if (slash) *slash = '\0';
  else strcpy(lib_dir, ".");
  strcat(lib_dir, "/lib");

  char *result = source;
  char *pos;
  while ((pos = strstr(result, "\nrequire ")) != NULL ||
         (pos == NULL && result == source && strncmp(result, "require ", 8) == 0 && (pos = result))) {
    if (pos != result) pos++; /* skip \n */
    if (pos != result && *(pos - 1) != '\n') break;
    char *line_end = strchr(pos, '\n');
    if (!line_end) line_end = pos + strlen(pos);

    /* Must be: require "name" or require 'name' */
    char *q1 = strchr(pos + 7, '"');
    char *q2 = strchr(pos + 7, '\'');
    char *start; char quote_char;
    if (q1 && q1 < line_end && (!q2 || q1 < q2)) { quote_char = '"'; start = q1 + 1; }
    else if (q2 && q2 < line_end) { quote_char = '\''; start = q2 + 1; }
    else break;
    char *end = strchr(start, quote_char);
    if (!end || end > line_end) break;

    char lib_name[256];
    snprintf(lib_name, sizeof(lib_name), "%.*s", (int)(end - start), start);
    char lib_path[1024];
    snprintf(lib_path, sizeof(lib_path), "%s/%s", lib_dir, lib_name);
    if (!strstr(lib_path, ".rb")) strcat(lib_path, ".rb");

    char *content = read_file(lib_path);
    if (!content) {
      content = strdup("# require not resolved");
    } else {
      char *resolved = resolve_requires(content, lib_path);
      free(content);
      content = resolved;
    }

    size_t line_len = (line_end - pos) + ((*line_end == '\n') ? 1 : 0);
    size_t content_len = strlen(content);
    size_t result_len = strlen(result);
    size_t before_len = pos - result;
    char *new_result = malloc(result_len - line_len + content_len + 2);
    memcpy(new_result, result, before_len);
    memcpy(new_result + before_len, content, content_len);
    if (content_len > 0 && content[content_len - 1] != '\n')
      new_result[before_len + content_len++] = '\n';
    memcpy(new_result + before_len + content_len, pos + line_len, result_len - before_len - line_len + 1);
    free(result);
    result = new_result;
    free(content);
  }
  return result;
}

/* ---- Syntax sugar rewriting ---- */
static char *rewrite_syntax_sugar(char *source) {
  /* Rewrite .send(:method, args) → .method(args) */
  /* Rewrite &:symbol → { |_spx| _spx.symbol } */
  size_t len = strlen(source);
  size_t cap = len * 2 + 256;
  char *out = malloc(cap);
  size_t oi = 0;
  size_t i = 0;

  #define OUT_CHAR(c) do { if (oi >= cap - 1) { cap *= 2; out = realloc(out, cap); } out[oi++] = (c); } while(0)
  #define OUT_STR(s) do { const char *_s = (s); while (*_s) { OUT_CHAR(*_s); _s++; } } while(0)

  while (i < len) {
    /* .send(:symbol ...) */
    if (i + 7 < len && strncmp(source + i, ".send(:", 7) == 0) {
      i += 7; /* skip .send(: */
      /* Extract method name */
      size_t ns = i;
      while (i < len && (source[i] == '_' || (source[i] >= 'a' && source[i] <= 'z') ||
             (source[i] >= 'A' && source[i] <= 'Z') || (source[i] >= '0' && source[i] <= '9') ||
             source[i] == '?' || source[i] == '!' || source[i] == '+' || source[i] == '-' ||
             source[i] == '*' || source[i] == '/' || source[i] == '<' || source[i] == '>' ||
             source[i] == '=' || source[i] == '&' || source[i] == '|' || source[i] == '^' ||
             source[i] == '~' || source[i] == '%')) i++;
      size_t name_len = i - ns;
      if (name_len > 0) {
        OUT_CHAR('.');
        size_t k; for (k = 0; k < name_len; k++) OUT_CHAR(source[ns + k]);
        /* Skip optional comma + space, then copy remaining args until ) */
        if (i < len && source[i] == ')') {
          i++; /* no args */
        } else if (i < len && source[i] == ',') {
          i++; /* skip comma */
          while (i < len && source[i] == ' ') i++;
          OUT_CHAR('(');
          /* Copy args until matching ) */
          int depth = 1;
          while (i < len && depth > 0) {
            if (source[i] == '(') depth++;
            else if (source[i] == ')') { depth--; if (depth == 0) { i++; break; } }
            OUT_CHAR(source[i]); i++;
          }
          OUT_CHAR(')');
        }
        continue;
      }
      /* Failed to parse, output original */
      OUT_STR(".send(:");
      continue;
    }
    /* (&:symbol) → { |_spx| _spx.symbol } — also remove enclosing parens */
    if (i + 2 < len && source[i] == '&' && source[i + 1] == ':') {
      /* Check if preceded by ( and remove it */
      int had_paren = 0;
      if (oi > 0 && out[oi - 1] == '(') { oi--; had_paren = 1; }
      i += 2;
      size_t ns = i;
      while (i < len && (source[i] == '_' || (source[i] >= 'a' && source[i] <= 'z') ||
             (source[i] >= 'A' && source[i] <= 'Z') || (source[i] >= '0' && source[i] <= '9') ||
             source[i] == '?' || source[i] == '!')) i++;
      size_t name_len = i - ns;
      if (name_len > 0) {
        /* Skip closing paren if we removed opening */
        if (had_paren && i < len && source[i] == ')') i++;
        OUT_STR(" { |_spx| _spx.");
        size_t k; for (k = 0; k < name_len; k++) OUT_CHAR(source[ns + k]);
        OUT_STR(" }");
        continue;
      }
      if (had_paren) OUT_CHAR('('); /* restore if failed */
      OUT_STR("&:");
      continue;
    }
    OUT_CHAR(source[i]);
    i++;
  }
  out[oi] = '\0';
  free(source);
  return out;
  #undef OUT_CHAR
  #undef OUT_STR
}

/* ---- Main ---- */
int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: spinel_parse input.rb [output.ast]\n");
    return 1;
  }

  const char *source_file = argv[1];
  char *source = read_file(source_file);
  if (!source) {
    fprintf(stderr, "spinel_parse: cannot open '%s'\n", source_file);
    return 1;
  }

  /* Resolve require_relative and plain require */
  char *resolved = resolve_requires(source, source_file);
  free(source);
  source = resolve_plain_requires(resolved, argv[0]);
  source = rewrite_syntax_sugar(source);

  size_t source_len = strlen(source);

  /* Parse with Prism */
  pm_parser_t parser;
  pm_parser_init(&parser, (const uint8_t *)source, source_len, NULL);
  pm_node_t *root = pm_parse(&parser);

  if (parser.error_list.size > 0) {
    fprintf(stderr, "Parse errors in '%s':\n", source_file);
    pm_diagnostic_t *diag;
    for (diag = (pm_diagnostic_t *)parser.error_list.head; diag; diag = (pm_diagnostic_t *)diag->node.next) {
      fprintf(stderr, "  %s\n", diag->message);
    }
    pm_node_destroy(&parser, root);
    pm_parser_free(&parser);
    free(source);
    return 1;
  }

  g_parser = &parser;

  /* Flatten AST to text */
  lines = NULL;
  line_count = 0;
  line_cap = 0;
  node_counter = 0;

  int root_id = flatten(root);

  /* Output */
  FILE *out = stdout;
  if (argc >= 3) {
    out = fopen(argv[2], "wb");
    if (!out) {
      fprintf(stderr, "spinel_parse: cannot write '%s'\n", argv[2]);
      return 1;
    }
  }

  fprintf(out, "ROOT %d\n", root_id);
  for (int i = 0; i < line_count; i++) {
    fprintf(out, "%s\n", lines[i]);
    free(lines[i]);
  }
  free(lines);

  if (out != stdout) fclose(out);

  pm_node_destroy(&parser, root);
  pm_parser_free(&parser);
  free(source);
  return 0;
}
