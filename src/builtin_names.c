/* builtin_names.c -- the families of builtin method names (builtin_names.h).
   Each family is spelled once here; the order is the order its compares run
   in, the one most of the replaced chains used. */
#include "types.h"
#include "builtin_names.h"

int is_call_alias(const char *n) {
  return sp_streq(n, "call") || sp_streq(n, "()") || sp_streq(n, "[]");
}

int is_kind_query(const char *n) {
  return sp_streq(n, "is_a?") || sp_streq(n, "kind_of?") || sp_streq(n, "instance_of?");
}

int is_round_family(const char *n) {
  return sp_streq(n, "round") || sp_streq(n, "ceil") || sp_streq(n, "floor") || sp_streq(n, "truncate");
}

int is_push_alias(const char *n) {
  return sp_streq(n, "push") || sp_streq(n, "<<") || sp_streq(n, "append");
}

int is_bit_op(const char *n) {
  return sp_streq(n, "&") || sp_streq(n, "|") || sp_streq(n, "^");
}

int is_basic_arith(const char *n) {
  return sp_streq(n, "+") || sp_streq(n, "-") || sp_streq(n, "*") || sp_streq(n, "/");
}

int is_object_root(const char *n) {
  return sp_streq(n, "Object") || sp_streq(n, "BasicObject") || sp_streq(n, "Kernel");
}

int is_send_family(const char *n) {
  return sp_streq(n, "send") || sp_streq(n, "__send__") || sp_streq(n, "public_send");
}

int is_name_reader(const char *n) {
  return sp_streq(n, "name") || sp_streq(n, "to_s") || sp_streq(n, "inspect");
}

int is_tap_alias(const char *n) {
  return sp_streq(n, "tap") || sp_streq(n, "then") || sp_streq(n, "yield_self");
}

int is_quantifier(const char *n) {
  return sp_streq(n, "all?") || sp_streq(n, "any?") || sp_streq(n, "none?") || sp_streq(n, "one?");
}

int is_set_op(const char *n) {
  return sp_streq(n, "&") || sp_streq(n, "intersection") || sp_streq(n, "|") || sp_streq(n, "union") ||
         sp_streq(n, "-") || sp_streq(n, "difference");
}

int is_combination_family(const char *n) {
  return sp_streq(n, "combination") || sp_streq(n, "permutation") ||
         sp_streq(n, "repeated_combination") || sp_streq(n, "repeated_permutation");
}

int is_visibility_name(const char *n) {
  return sp_streq(n, "private") || sp_streq(n, "protected") || sp_streq(n, "public");
}

int is_select_bang(const char *n) {
  return sp_streq(n, "select!") || sp_streq(n, "filter!") || sp_streq(n, "keep_if") ||
         sp_streq(n, "reject!") || sp_streq(n, "delete_if");
}

int is_each_walk(const char *n) {
  return sp_streq(n, "each") || sp_streq(n, "each_entry") || sp_streq(n, "reverse_each");
}

int is_index_query(const char *n) {
  return sp_streq(n, "find_index") || sp_streq(n, "index") || sp_streq(n, "rindex");
}

int is_self_copy(const char *n) {
  return sp_streq(n, "freeze") || sp_streq(n, "dup") || sp_streq(n, "clone") ||
         sp_streq(n, "itself");
}

int is_int_step(const char *n) {
  return sp_streq(n, "times") || sp_streq(n, "upto") || sp_streq(n, "downto");
}

int is_equality_name(const char *n) {
  return sp_streq(n, "equal?") || sp_streq(n, "eql?") || sp_streq(n, "==");
}

int is_bits_query(const char *n) {
  return sp_streq(n, "allbits?") || sp_streq(n, "anybits?") || sp_streq(n, "nobits?");
}

int is_count_alias(const char *n) {
  return sp_streq(n, "length") || sp_streq(n, "size") || sp_streq(n, "count");
}

int is_class_eval_family(const char *n) {
  return sp_streq(n, "class_eval") || sp_streq(n, "module_eval") || sp_streq(n, "class_exec") ||
         sp_streq(n, "module_exec");
}

int is_call_or_yield(const char *n) {
  return sp_streq(n, "call") || sp_streq(n, "()") || sp_streq(n, "[]") || sp_streq(n, "yield");
}

int is_quantifier_or_count(const char *n) {
  return sp_streq(n, "all?") || sp_streq(n, "any?") || sp_streq(n, "none?") ||
         sp_streq(n, "one?") || sp_streq(n, "count");
}

int is_push_unshift(const char *n) {
  return sp_streq(n, "<<") || sp_streq(n, "push") || sp_streq(n, "append") ||
         sp_streq(n, "unshift");
}

int is_len_alias(const char *n) {
  return sp_streq(n, "length") || sp_streq(n, "size");
}

int is_add_sub_mul(const char *n) {
  return sp_streq(n, "+") || sp_streq(n, "-") || sp_streq(n, "*");
}

int is_int_bit_op(const char *n) {
  return sp_streq(n, "&") || sp_streq(n, "|") || sp_streq(n, "^") || sp_streq(n, "<<") || sp_streq(n, ">>");
}

/* Classes whose reopenings keep the runtime value instead of a user struct. */
int is_builtin_reopen_name(const char *name) {
  return sp_streq(name, "Toplevel") ||
         sp_streq(name, "String")    || sp_streq(name, "Integer") ||
         sp_streq(name, "Float")     || sp_streq(name, "Symbol")  ||
         sp_streq(name, "TrueClass") || sp_streq(name, "FalseClass") ||
         sp_streq(name, "NilClass")  || sp_streq(name, "Array")   ||
         sp_streq(name, "Object")    || sp_streq(name, "Numeric") ||
         sp_streq(name, "Dir")       ||
         /* runtime value types with a typedef of their own (sp_Range, sp_Time,
            sp_File, sp_Class): a user struct under that name was a C-level
            typedef collision before any call was reached (activesupport's
            blank.rb reopens Range and Time) */
         sp_streq(name, "Range")     || sp_streq(name, "Time") ||
         sp_streq(name, "File")      || sp_streq(name, "Class") ||
         sp_streq(name, "Hash")      ||
         /* a thread and a fiber are runtime handles too (activesupport's
            IsolatedExecutionState gives both an accessor) */
         sp_streq(name, "Thread")    || sp_streq(name, "Fiber") ||
         sp_streq(name, "Random");
}
