/* builtin_names.c -- the families of builtin method names (builtin_names.h).
   Each family is spelled once here; the order is the order its compares run
   in, the one most of the replaced chains used. */
#include <stddef.h>
#include <string.h>
#include "types.h"
#include "builtin_names.h"

int is_zip_name(const char *n) {
  return sp_streq(n, "zip");
}

/* the match name that takes a start position (match?(pattern, pos)) */
int is_match_p_name(const char *n) {
  return sp_streq(n, "match?");
}

/* `Struct.new(...)` / `Data.define(...)`: the call that builds a record
   class, by its receiver's constant name and the method */
int is_record_class_builder(const char *recv, const char *meth) {
  return recv && meth && ((sp_streq(recv, "Struct") && sp_streq(meth, "new")) ||
                          (sp_streq(recv, "Data") && sp_streq(meth, "define")));
}

/* Struct.new, Data.define, Class.new, Module.new: the builders of a class or a
   module, whose block runs as its body */
int is_class_builder(const char *recv, const char *meth) {
  return is_record_class_builder(recv, meth) ||
         (recv && meth && sp_streq(meth, "new") && (sp_streq(recv, "Class") || sp_streq(recv, "Module")));
}

/* Hash.new, Array.new, String.new, Object.new: a builtin's own constructor */
int is_value_constructor(const char *recv, const char *meth) {
  return recv && meth && sp_streq(meth, "new") &&
         (sp_streq(recv, "Hash") || sp_streq(recv, "Array") || sp_streq(recv, "String") || sp_streq(recv, "Object"));
}

/* the Array class's own name, exactly */
int is_array_class_name(const char *n) {
  return n && sp_streq(n, "Array");
}
/* `Array.new(...)`, by its receiver's constant name and the method */
int is_array_new(const char *recv, const char *meth) {
  return is_array_class_name(recv) && meth && sp_streq(meth, "new");
}

int is_call_alias(const char *n) {
  return sp_streq(n, "call") || sp_streq(n, "()") || sp_streq(n, "[]");
}

int is_method_invoke(const char *n) {
  return is_call_alias(n) || sp_streq(n, "===");
}

int is_bind_call(const char *n) { return n && sp_streq(n, "bind_call"); }

int is_kind_query(const char *n) {
  return sp_streq(n, "is_a?") || sp_streq(n, "kind_of?") || sp_streq(n, "instance_of?");
}

/* A call that answers about the receiver itself without reading any of
   its instance variables: its class, its identity, nil?, frozen?. */
int is_member_blind_query(const char *n) {
  return is_kind_query(n) || sp_streq(n, "class") || sp_streq(n, "object_id") ||
         sp_streq(n, "__id__") || sp_streq(n, "nil?") || sp_streq(n, "frozen?") ||
         sp_streq(n, "equal?") || sp_streq(n, "respond_to?");
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

int is_int_arith_op(const char *n) {
  return is_basic_arith(n) || sp_streq(n, "%");
}

int is_object_root(const char *n) {
  return sp_streq(n, "Object") || sp_streq(n, "BasicObject") || sp_streq(n, "Kernel");
}

int is_send_family(const char *n) {
  return sp_streq(n, "send") || sp_streq(n, "__send__") || sp_streq(n, "public_send");
}

/* A call that can run code the analysis does not see, or capture a String:
   send and its siblings, call, new, lambda/proc, freeze, eval, and the
   instance_* / class_* / module_* / *method* reflection families. */
int is_opaque_reaching_call(const char *n) {
  return is_send_family(n) || sp_streq(n, "call") || sp_streq(n, "new") ||
         sp_streq(n, "lambda") || sp_streq(n, "proc") || sp_streq(n, "freeze") ||
         sp_streq(n, "eval") || strncmp(n, "instance_", 9) == 0 ||
         strncmp(n, "class_", 6) == 0 || strncmp(n, "module_", 7) == 0 ||
         strstr(n, "method") != NULL;
}

/* A builtin call that hands Ruby code to the runtime to run later, at a
   point where no call in the running method names it: a thread's or a
   fiber's body, and a signal handler (run inside the C handler, at any
   instruction). `recv` is the receiver's constant name, NULL for a bare
   call. */
int is_async_code_entry(const char *recv, const char *n) {
  if (!n) return 0;
  if (sp_streq(n, "trap")) return !recv || sp_streq(recv, "Signal");
  if (!recv) return 0;
  if (sp_streq(recv, "Thread"))
    return sp_streq(n, "new") || sp_streq(n, "start") || sp_streq(n, "fork");
  return sp_streq(recv, "Fiber") && sp_streq(n, "new");
}

int is_name_reader(const char *n) {
  return sp_streq(n, "name") || sp_streq(n, "to_s") || sp_streq(n, "inspect");
}

/* `tap` alone: it answers its receiver, where then/yield_self answer the
   block's value (is_tap_alias takes all three) */
int is_tap_name(const char *n) {
  return sp_streq(n, "tap");
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

int is_retaining_filter(const char *n) {
  return is_select_bang(n) || sp_streq(n, "select") || sp_streq(n, "filter") ||
         sp_streq(n, "find_all") || sp_streq(n, "reject");
}

int is_enum_partition_def(const char *n) {
  return strncmp(n, "__enum_partition__", 18) == 0;
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

int is_eval_exec_family(const char *n) {
  return is_class_eval_family(n) || sp_streq(n, "instance_eval") || sp_streq(n, "instance_exec");
}

int is_key_query(const char *n) {
  return sp_streq(n, "key?") || sp_streq(n, "has_key?") ||
         sp_streq(n, "include?") || sp_streq(n, "member?");
}

int is_hash_key_lookup(const char *n) {
  return sp_streq(n, "[]") || is_key_query(n) || sp_streq(n, "fetch") || sp_streq(n, "delete");
}

int is_receiver_conversion(const char *n) {
  return sp_streq(n, "to_s") || sp_streq(n, "to_str") || sp_streq(n, "itself");
}
int is_to_s_name(const char *n) { return n && sp_streq(n, "to_s"); }

int is_range_membership(const char *n) {
  return sp_streq(n, "cover?") || sp_streq(n, "include?") ||
         sp_streq(n, "member?") || sp_streq(n, "===");
}

int is_each_walk_or_with_index(const char *n) {
  return sp_streq(n, "each") || sp_streq(n, "each_with_index") ||
         sp_streq(n, "reverse_each") || sp_streq(n, "each_entry");
}

int is_call_or_yield(const char *n) {
  return sp_streq(n, "call") || sp_streq(n, "()") || sp_streq(n, "[]") || sp_streq(n, "yield");
}

int is_proc_invoke(const char *n) {
  return is_call_or_yield(n) || sp_streq(n, "===");
}

int is_quantifier_or_count(const char *n) {
  return sp_streq(n, "all?") || sp_streq(n, "any?") || sp_streq(n, "none?") ||
         sp_streq(n, "one?") || sp_streq(n, "count");
}

int is_push_unshift(const char *n) {
  return sp_streq(n, "<<") || sp_streq(n, "push") || sp_streq(n, "append") ||
         sp_streq(n, "unshift");
}

/* A call whose answer tells an object from a copy of it: its identity, or
   whether it is frozen (a copy of a String later frozen is not). */
int is_identity_query(const char *n) {
  return sp_streq(n, "equal?") || sp_streq(n, "object_id") || sp_streq(n, "__id__") || sp_streq(n, "frozen?");
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

int is_str_each_iter(const char *n) {
  return sp_streq(n, "each_char") || sp_streq(n, "each_line") ||
         sp_streq(n, "each_byte") || sp_streq(n, "each_codepoint");
}

int is_str_string_yield(const char *n) {
  return sp_streq(n, "each_char") || sp_streq(n, "each_line") || sp_streq(n, "upto") ||
         sp_streq(n, "chars") || sp_streq(n, "lines") || sp_streq(n, "split") || sp_streq(n, "scrub");
}

int is_unpack_name(const char *n) {
  return sp_streq(n, "unpack");
}

int is_catch_name(const char *n) { return sp_streq(n, "catch"); }
int is_throw_name(const char *n) { return sp_streq(n, "throw"); }

int is_diverging_call(const char *n) {
  return sp_streq(n, "raise") || sp_streq(n, "fail") || sp_streq(n, "throw") ||
         sp_streq(n, "exit") || sp_streq(n, "exit!") || sp_streq(n, "abort");
}

int is_block_loop_method(const char *n) {
  return sp_streq(n, "times") || sp_streq(n, "each") ||
         sp_streq(n, "upto") || sp_streq(n, "downto") ||
         sp_streq(n, "step") || sp_streq(n, "loop") ||
         sp_streq(n, "each_with_index");
}

int is_each_window(const char *n) {
  return sp_streq(n, "each_cons") || sp_streq(n, "each_slice");
}

int is_sum_name(const char *n) { return n && sp_streq(n, "sum"); }

int is_reduce_alias(const char *n) {
  return sp_streq(n, "inject") || sp_streq(n, "reduce");
}

int is_minmax_query(const char *n) {
  return sp_streq(n, "min") || sp_streq(n, "max");
}

int is_endpoint_query(const char *n) {
  return sp_streq(n, "first") || sp_streq(n, "last");
}

int is_map_alias(const char *n) {
  return sp_streq(n, "map") || sp_streq(n, "collect");
}

int is_instance_eval_family(const char *n) {
  return sp_streq(n, "instance_eval") || sp_streq(n, "instance_exec");
}

int is_find_alias(const char *n) {
  return sp_streq(n, "find") || sp_streq(n, "detect");
}

int is_mul_or_pow(const char *n) {
  return sp_streq(n, "*") || sp_streq(n, "**");
}

int is_proc_constructor(const char *n) {
  return sp_streq(n, "proc") || sp_streq(n, "lambda");
}

int is_lazy_force(const char *n) {
  return sp_streq(n, "to_a") || sp_streq(n, "force");
}

int is_with_object_alias(const char *n) {
  return sp_streq(n, "each_with_object") || sp_streq(n, "with_object");
}

int is_indexed_each(const char *n) {
  return sp_streq(n, "each_index") || sp_streq(n, "each_with_index");
}

int is_range_bound_reader(const char *n) {
  return sp_streq(n, "begin") || sp_streq(n, "end");
}

int is_each_or_index(const char *n) {
  return sp_streq(n, "each") || sp_streq(n, "each_with_index");
}

int is_find_or_take_while(const char *n) {
  return sp_streq(n, "find") || sp_streq(n, "detect") || sp_streq(n, "take_while");
}

int is_membership_alias(const char *n) {
  return sp_streq(n, "include?") || sp_streq(n, "member?");
}

int is_element_at_alias(const char *n) {
  return sp_streq(n, "[]") || sp_streq(n, "at");
}

int is_hash_transform(const char *n) {
  return sp_streq(n, "transform_values") || sp_streq(n, "transform_keys");
}

/* the calls whose block is the fallback for a missing key or index: they
   hand it the one value they could not find */
int is_fallback_block_call(const char *n) {
  return sp_streq(n, "fetch") || sp_streq(n, "delete") || sp_streq(n, "fetch_values");
}

int is_sort_family(const char *n) {
  return sp_streq(n, "sort") || sp_streq(n, "sort!");
}

int is_store_alias(const char *n) {
  return sp_streq(n, "[]=") || sp_streq(n, "store");
}

int is_hash_default_setter(const char *n) {
  return sp_streq(n, "default=");
}

int is_hash_merge_bang(const char *n) {
  return sp_streq(n, "merge!") || sp_streq(n, "update");
}

int is_append_concat(const char *n) {
  return sp_streq(n, "<<") || sp_streq(n, "concat");
}

int is_each_or_pair(const char *n) {
  return sp_streq(n, "each") || sp_streq(n, "each_pair");
}

int is_slice_alias(const char *n) {
  return sp_streq(n, "[]") || sp_streq(n, "slice");
}

int is_element_pick(const char *n) {
  return sp_streq(n, "first") || sp_streq(n, "last") || sp_streq(n, "sample");
}

int is_equal_name(const char *n) {
  return sp_streq(n, "equal?");
}
int is_eql_or_equal(const char *n) {
  return sp_streq(n, "equal?") || sp_streq(n, "eql?");
}

int is_with_index_alias(const char *n) {
  return sp_streq(n, "with_index") || sp_streq(n, "each_with_index");
}

int is_text_print(const char *n) {
  return sp_streq(n, "puts") || sp_streq(n, "print");
}

int is_printf_name(const char *n) {
  return sp_streq(n, "printf");
}

int is_match_operator(const char *n) {
  return sp_streq(n, "=~") || sp_streq(n, "!~");
}

/* Object's methods that hand their receiver on to code over its members or
   bind it into a wrapper: to_enum / enum_for walk each, instance_eval /
   instance_exec run a block with it as self, method / public_method bind a
   Method to it. */
int is_object_receiver_handoff(const char *n) {
  return sp_streq(n, "to_enum") || sp_streq(n, "enum_for") || is_instance_eval_family(n) ||
         sp_streq(n, "method") || sp_streq(n, "public_method");
}

int is_eq_or_ne(const char *n) {
  return sp_streq(n, "==") || sp_streq(n, "!=");
}

int is_string_index(const char *n) {
  return sp_streq(n, "index") || sp_streq(n, "rindex");
}

int is_current_method(const char *n) {
  return sp_streq(n, "__method__") || sp_streq(n, "__callee__");
}

int is_freeze_family(const char *n) {
  return sp_streq(n, "freeze") || sp_streq(n, "frozen?");
}

int is_attr_writer_family(const char *n) {
  return sp_streq(n, "attr_writer") || sp_streq(n, "attr_accessor");
}

int is_visibility_or_module_function(const char *n) {
  return sp_streq(n, "private") || sp_streq(n, "protected") || sp_streq(n, "public") || sp_streq(n, "module_function");
}

int is_mixin_call(const char *n) {
  return sp_streq(n, "include") || sp_streq(n, "extend") || sp_streq(n, "prepend");
}

int is_const_set_name(const char *n) {
  return sp_streq(n, "const_set");
}

int is_const_query_name(const char *n) {
  return sp_streq(n, "const_get") || sp_streq(n, "const_defined?") || sp_streq(n, "const_source_location");
}

int is_constants_list_name(const char *n) {
  return sp_streq(n, "constants");
}

/* the methods a definition runs when the program defines them: inherited
   (class), included, extended, prepended, method_added and its singleton
   kin, const_added */
int is_definition_hook_name(const char *n) {
  return sp_streq(n, "inherited") || sp_streq(n, "included") || sp_streq(n, "extended") ||
         sp_streq(n, "prepended") || sp_streq(n, "method_added") ||
         sp_streq(n, "singleton_method_added") || sp_streq(n, "const_added");
}

int is_const_missing_name(const char *n) {
  return sp_streq(n, "const_missing");
}

/* the tables a const_get or const_defined? with a computed name becomes */
int is_const_table_def_name(const char *n) {
  return n && (sp_streq(n, "__const_get__") || sp_streq(n, "__const_defined__"));
}

/* Kernel#dup and #clone, which copy any object with its ivars */
int is_object_copy(const char *n) {
  return sp_streq(n, "dup") || sp_streq(n, "clone");
}

/* The reflective ivar write */
int is_ivar_set_name(const char *n) {
  return sp_streq(n, "instance_variable_set");
}

/* Object#inspect, the text p prints */
int is_inspect_name(const char *n) {
  return n && sp_streq(n, "inspect");
}

/* `Marshal.dump`, by its receiver's constant name and the method */
int is_marshal_dump(const char *recv, const char *meth) {
  return recv && meth && sp_streq(meth, "dump") && sp_streq(recv, "Marshal");
}

/* `Marshal.load`, which builds the objects it reads without their initialize */
int is_marshal_load(const char *recv, const char *meth) {
  return recv && meth && sp_streq(meth, "load") && sp_streq(recv, "Marshal");
}

/* Class#allocate, an object without its initialize run */
int is_allocate_name(const char *n) {
  return sp_streq(n, "allocate");
}

/* The reflective ivar removal */
int is_ivar_remove_name(const char *n) {
  return sp_streq(n, "remove_instance_variable");
}

/* A call that tells an ivar assigned from one never assigned: the
   reflective readers and removal, and the default inspect (inspect, and p
   and pp, which print it) */
int is_ivar_presence_read(const char *n) {
  return sp_streq(n, "instance_variables") || sp_streq(n, "instance_variable_defined?") ||
         is_ivar_remove_name(n) || is_inspect_name(n) || is_inspect_print(n);
}

/* A builtin class whose values keep their ivars in the runtime's map
   (desugar_builtin_ivars), and one whose values are all frozen: an ivar
   of theirs reads nil and a write raises FrozenError */
int is_bivar_keyed_class(const char *n) {
  return sp_streq(n, "Array") || sp_streq(n, "Hash") || sp_streq(n, "Random");
}
int is_string_class_name(const char *n) {
  return sp_streq(n, "String");
}
int is_frozen_value_class(const char *n) {
  return sp_streq(n, "Integer") || sp_streq(n, "Float") || sp_streq(n, "Symbol") ||
         sp_streq(n, "NilClass") || sp_streq(n, "TrueClass") || sp_streq(n, "FalseClass") ||
         sp_streq(n, "Range");
}

/* desugar_builtin_ivars' access to an ivar of a builtin class's self */
int is_bivar_access(const char *n) {
  return sp_streq(n, "__bivar_get") || sp_streq(n, "__bivar_set") || sp_streq(n, "__bivar_defined");
}

int is_attr_reader_family(const char *n) {
  return sp_streq(n, "attr_accessor") || sp_streq(n, "attr_reader");
}

int is_then_alias(const char *n) {
  return sp_streq(n, "then") || sp_streq(n, "yield_self");
}

int is_select_alias(const char *n) {
  return sp_streq(n, "select") || sp_streq(n, "filter");
}

int is_hash_key_value_each(const char *n) {
  return sp_streq(n, "each_value") || sp_streq(n, "each_key");
}

int is_bounded_int_step(const char *n) {
  return sp_streq(n, "upto") || sp_streq(n, "downto");
}

int is_upto_name(const char *n) {
  return sp_streq(n, "upto");
}

int is_add_sub(const char *n) {
  return sp_streq(n, "+") || sp_streq(n, "-");
}

int is_array_push_family(const char *n) {
  return sp_streq(n, "push") || sp_streq(n, "<<") || sp_streq(n, "append") || sp_streq(n, "unshift") || sp_streq(n, "prepend");
}

int is_take_drop(const char *n) {
  return sp_streq(n, "take") || sp_streq(n, "drop");
}

int is_extrema_family(const char *n) {
  return sp_streq(n, "min") || sp_streq(n, "max") || sp_streq(n, "minmax");
}

int is_unary_sign(const char *n) {
  return sp_streq(n, "-@") || sp_streq(n, "+@");
}

int is_unary_minus(const char *n) {
  return sp_streq(n, "-@");
}

int is_text_conversion(const char *n) {
  return sp_streq(n, "to_s") || sp_streq(n, "inspect");
}

int is_substitution(const char *n) {
  return sp_streq(n, "gsub") || sp_streq(n, "sub");
}

int is_encoding_mutator(const char *n) {
  return sp_streq(n, "force_encoding") || sp_streq(n, "encode!");
}

int is_socket_address(const char *n) {
  return sp_streq(n, "addr") || sp_streq(n, "peeraddr");
}

int is_nonblock_io(const char *n) {
  return sp_streq(n, "read_nonblock") || sp_streq(n, "write_nonblock");
}

int is_io_wait(const char *n) {
  return sp_streq(n, "wait_readable") || sp_streq(n, "wait_writable") || sp_streq(n, "wait_priority");
}

int is_io_write(const char *n) {
  return sp_streq(n, "write") || sp_streq(n, "syswrite");
}

int is_path_reader(const char *n) {
  return sp_streq(n, "path") || sp_streq(n, "to_path");
}

int is_hash_constructor(const char *n) {
  return sp_streq(n, "new") || sp_streq(n, "__hash_new_default");
}

/* A Struct or Data class's constructor calls: `S.new(...)` and `S[...]`. */
int is_struct_constructor(const char *n) {
  return sp_streq(n, "new") || sp_streq(n, "[]");
}

int is_exist_alias(const char *n) {
  return sp_streq(n, "exist?") || sp_streq(n, "exists?");
}

int is_open_constructor(const char *n) {
  return sp_streq(n, "open") || sp_streq(n, "new");
}

int is_socket_pair_alias(const char *n) {
  return sp_streq(n, "pair") || sp_streq(n, "socketpair");
}

int is_directory_entries(const char *n) {
  return sp_streq(n, "children") || sp_streq(n, "entries");
}

int is_io_position(const char *n) {
  return sp_streq(n, "tell") || sp_streq(n, "pos");
}

int is_rewind_name(const char *n) {
  return sp_streq(n, "rewind");
}

int is_io_offset_move(const char *n) {
  return sp_streq(n, "pos=") || sp_streq(n, "sysseek");
}

int is_byte_codepoint_each(const char *n) {
  return sp_streq(n, "each_byte") || sp_streq(n, "each_codepoint");
}

int is_to_array_alias(const char *n) {
  return sp_streq(n, "to_a") || sp_streq(n, "entries");
}

int is_select_reject(const char *n) {
  return sp_streq(n, "select") || sp_streq(n, "reject") || sp_streq(n, "filter");
}

int is_casecmp_family(const char *n) {
  return sp_streq(n, "casecmp") || sp_streq(n, "casecmp?");
}

int is_to_integer(const char *n) {
  return sp_streq(n, "to_i") || sp_streq(n, "to_int");
}

int is_add_or_mul(const char *n) {
  return sp_streq(n, "+") || sp_streq(n, "*");
}

int is_remainder_family(const char *n) {
  return sp_streq(n, "modulo") || sp_streq(n, "%") || sp_streq(n, "remainder");
}

int is_to_rational(const char *n) {
  return sp_streq(n, "to_r") || sp_streq(n, "rationalize");
}

int is_format_alias(const char *n) {
  return sp_streq(n, "format") || sp_streq(n, "sprintf");
}

int is_shift_op(const char *n) {
  return sp_streq(n, "<<") || sp_streq(n, ">>");
}

int is_named_set_operator(const char *n) {
  return sp_streq(n, "intersection") || sp_streq(n, "union") || sp_streq(n, "difference");
}

int is_exception_message(const char *n) {
  return sp_streq(n, "message") || sp_streq(n, "to_s");
}

int is_eq_or_eql(const char *n) {
  return sp_streq(n, "==") || sp_streq(n, "eql?");
}

int is_copy_alias(const char *n) {
  return sp_streq(n, "dup") || sp_streq(n, "clone");
}

int is_pop_shift(const char *n) {
  return sp_streq(n, "pop") || sp_streq(n, "shift");
}

int is_element_access(const char *n) {
  return sp_streq(n, "[]") || sp_streq(n, "[]=");
}

int is_index_assign(const char *n) {
  return sp_streq(n, "[]=");
}

int is_fill_name(const char *n) { return sp_streq(n, "fill"); }

int is_first_or_take(const char *n) {
  return sp_streq(n, "first") || sp_streq(n, "take");
}

int is_map_bang_alias(const char *n) {
  return sp_streq(n, "map!") || sp_streq(n, "collect!");
}

int is_exception_full_message(const char *n) {
  return sp_streq(n, "full_message") || sp_streq(n, "detailed_message");
}

int is_inspect_print(const char *n) {
  return sp_streq(n, "p") || sp_streq(n, "pp");
}

int is_select_reject_bang(const char *n) {
  return sp_streq(n, "reject!") || sp_streq(n, "select!") || sp_streq(n, "filter!");
}

int is_raise_alias(const char *n) {
  return sp_streq(n, "raise") || sp_streq(n, "fail");
}
/* the Exception methods that answer the message (the share walk's sh_exc) */
int is_exc_message_name(const char *n) {
  return sp_streq(n, "message") || sp_streq(n, "full_message") || sp_streq(n, "detailed_message");
}
/* +s alone: a route that answers its String operand unless frozen
   (--share-strings, emit_strbuf_route) */
int is_unary_plus(const char *n) {
  return sp_streq(n, "+@");
}
/* Kernel#loop, whose value is what a break gives it */
int is_loop_name(const char *n) {
  return sp_streq(n, "loop");
}

int is_size_or_count(const char *n) {
  return sp_streq(n, "size") || sp_streq(n, "count");
}

int is_modulo_alias(const char *n) {
  return sp_streq(n, "%") || sp_streq(n, "modulo");
}

int is_bit_set_operator(const char *n) {
  return sp_streq(n, "&") || sp_streq(n, "|") || sp_streq(n, "-");
}

int is_rectangular_alias(const char *n) {
  return sp_streq(n, "rect") || sp_streq(n, "rectangular");
}

int is_partition_family(const char *n) {
  return sp_streq(n, "partition") || sp_streq(n, "rpartition");
}

int is_local_time(const char *n) {
  return sp_streq(n, "localtime") || sp_streq(n, "getlocal");
}

int is_iso8601_alias(const char *n) {
  return sp_streq(n, "iso8601") || sp_streq(n, "xmlschema");
}

int is_prepend_alias(const char *n) {
  return sp_streq(n, "unshift") || sp_streq(n, "prepend");
}

int is_push_operator(const char *n) {
  return sp_streq(n, "<<") || sp_streq(n, "push");
}

int is_integer_iteration(const char *n) {
  return sp_streq(n, "times") || sp_streq(n, "upto") || sp_streq(n, "downto") || sp_streq(n, "step");
}

int is_numeric_conversion(const char *n) {
  return sp_streq(n, "to_i") || sp_streq(n, "to_f");
}

int is_succ_alias(const char *n) {
  return sp_streq(n, "succ") || sp_streq(n, "next");
}

int is_div_or_mod(const char *n) {
  return sp_streq(n, "/") || sp_streq(n, "%");
}

int is_div_or_modulo(const char *n) {
  return sp_streq(n, "div") || sp_streq(n, "modulo");
}

int is_div_name(const char *n) {
  return sp_streq(n, "div");
}

int is_divmod_name(const char *n) {
  return sp_streq(n, "divmod");
}

int is_modulo_name(const char *n) {
  return sp_streq(n, "modulo");
}

int is_mod_operator(const char *n) {
  return sp_streq(n, "%");
}

int is_initialize_name(const char *n) {
  return sp_streq(n, "initialize");
}

int is_initialize_family(const char *n) {
  return sp_streq(n, "initialize_copy") || sp_streq(n, "initialize");
}

int is_intersection_alias(const char *n) {
  return sp_streq(n, "&") || sp_streq(n, "intersection");
}

int is_union_alias(const char *n) {
  return sp_streq(n, "|") || sp_streq(n, "union");
}

int is_line_read(const char *n) {
  return sp_streq(n, "gets") || sp_streq(n, "readline");
}

int is_string_position_mutator(const char *n) {
  return sp_streq(n, "slice!") || sp_streq(n, "setbyte") || sp_streq(n, "insert") || sp_streq(n, "clear") || sp_streq(n, "[]=");
}

int is_range_end_reader(const char *n) {
  return sp_streq(n, "end") || sp_streq(n, "last");
}

int is_match_family(const char *n) {
  return sp_streq(n, "=~") || sp_streq(n, "!~") || sp_streq(n, "match?") || sp_streq(n, "match");
}

int is_integer_class_name(const char *n) {
  return sp_streq(n, "Integer") || sp_streq(n, "Fixnum");
}

int is_numeric_class_name(const char *n) {
  return sp_streq(n, "Integer") || sp_streq(n, "Float");
}

int is_range_or_time_class(const char *n) {
  return sp_streq(n, "Time") || sp_streq(n, "Range");
}

int is_object_base_name(const char *n) {
  return sp_streq(n, "Object") || sp_streq(n, "BasicObject");
}

int is_queue_class_name(const char *n) {
  return sp_streq(n, "Queue") || sp_streq(n, "SizedQueue");
}

int is_standard_output_global(const char *n) {
  return sp_streq(n, "$stdout") || sp_streq(n, "$stderr");
}

int is_file_class_name(const char *n) {
  return n && sp_streq(n, "File");
}

int is_filetest_module_name(const char *n) {
  return n && sp_streq(n, "FileTest");
}

int is_io_class_name(const char *n) {
  return sp_streq(n, "IO") || sp_streq(n, "File");
}

int is_program_name_global(const char *n) {
  return sp_streq(n, "$PROGRAM_NAME") || sp_streq(n, "$0");
}

int is_immediate_class_name(const char *n) {
  return sp_streq(n, "TrueClass") || sp_streq(n, "FalseClass") || sp_streq(n, "NilClass");
}

int is_numeric_literal_tag(const char *n) {
  return sp_streq(n, "Int") || sp_streq(n, "Float");
}

int is_boolean_class_name(const char *n) {
  return sp_streq(n, "TrueClass") || sp_streq(n, "FalseClass");
}

int is_array_or_object_class(const char *n) {
  return sp_streq(n, "Object") || sp_streq(n, "Array");
}

int is_array_hash_or_object_class(const char *n) {
  return sp_streq(n, "Array") || sp_streq(n, "Hash") || sp_streq(n, "Object");
}

int is_ivar_access(const char *n) {
  return sp_streq(n, "instance_variable_get") || is_ivar_set(n);
}

int is_ivar_set(const char *n) {
  return sp_streq(n, "instance_variable_set");
}

int is_plus_op(const char *n) {
  return sp_streq(n, "+");
}

int is_string_append_or_prepend(const char *n) {
  return is_append_concat(n) || sp_streq(n, "prepend");
}

int is_string_append(const char *n) {
  return sp_streq(n, "<<") || sp_streq(n, "concat");
}

int is_string_byte_mutator(const char *n) {
  return sp_streq(n, "bytesplice") || sp_streq(n, "append_as_bytes");
}

int is_replace_name(const char *n) {
  return sp_streq(n, "replace");
}

/* The arguments String mutator `n`, called with `argc` of them, takes as
   Strings: the first one's position (argc when none), the rest following
   it, and in *int_ok whether an Integer is taken there too. `<<`, concat
   and append_as_bytes take each one (an Integer as a codepoint), prepend
   each one, replace its one, insert its second and `[]=` its last (the
   value; the index is an Integer, a Range, a String or a Regexp). */
int str_mutator_str_args(const char *n, int argc, int *int_ok) {
  *int_ok = 0;
  if (is_string_append(n) || sp_streq(n, "append_as_bytes")) { *int_ok = 1; return 0; }
  if (sp_streq(n, "prepend") || (is_replace_name(n) && argc == 1)) return 0;
  if (sp_streq(n, "insert") && argc == 2) return 1;
  if (sp_streq(n, "[]=") && (argc == 2 || argc == 3)) return argc - 1;
  return argc;
}

/* String methods that only read the receiver's bytes: they answer a scalar or
   build a new string, and never retain the pointer they were handed. A
   shared-mutable receiver can hand them its live buffer instead of a copy. */
int is_string_read_only_method(const char *name) {
  static const char *const ro[] = {
    "[]", "slice", "byteslice", "getbyte", "ord", "chr",
    "index", "rindex", "include?", "start_with?", "end_with?",
    "count", "length", "size", "bytesize", "empty?",
    "to_i", "to_f", "hex", "oct", "match?", "casecmp", "casecmp?",
    "upcase", "downcase", "capitalize", "swapcase", "reverse",
    "strip", "lstrip", "rstrip", "chomp", "chop", "center", "ljust", "rjust",
    "each_char", "each_byte", "each_line", "chars", "bytes", "lines", "split",
    "sum", "hash", "unpack", "unpack1", "codepoints", "scan", NULL };
  for (int i = 0; ro[i]; i++) if (sp_streq(name, ro[i])) return 1;
  return 0;
}

int is_string_rebind_mutator(const char *n) {
  static const char *const MUT[] = {
    "<<", "concat", "prepend", "insert", "replace", "[]=", "slice!", "setbyte", "bytesplice",
    "sub!", "gsub!", "chomp!", "scrub!", "tr!", "tr_s!", "delete!", "squeeze!", "delete_prefix!", "delete_suffix!",
    "append_as_bytes", "force_encoding", "encode!", "unicode_normalize!", NULL };
  for (int i = 0; MUT[i]; i++)
    if (sp_streq(n, MUT[i])) return 1;
  return 0;
}

static int builtin_name_in(const char *name, const char *const *names) {
  for (int i = 0; names[i]; i++)
    if (sp_streq(name, names[i])) return 1;
  return 0;
}

/* The modules a builtin class includes ahead of Object, with their own
   public methods as CRuby 4.0 lists them: Integer and Float are Numeric and
   Comparable, String and Symbol Comparable, Array, Hash and Range
   Enumerable. The builtin table holds each class's own methods only. */
/* Comparable's own methods on a builtin class that includes it: Integer,
   Float, String and Symbol. */
int builtin_comparable_owns(const char *cls, const char *m) {
  static const char *const cmp[] = { "<", "<=", "==", ">", ">=", "between?", "clamp", NULL };
  return (is_numeric_class_name(cls) || sp_streq(cls, "String") || sp_streq(cls, "Symbol")) &&
         builtin_name_in(m, cmp);
}

/* Method#arity of a Comparable method: between?(min, max), clamp(min, max)
   or clamp(range), and the one-operand comparisons. */
int builtin_comparable_arity(const char *m) {
  return sp_streq(m, "between?") ? 2 : sp_streq(m, "clamp") ? -1 : 1;
}

/* Numeric's own methods on Integer and Float, which inherit them */
int builtin_numeric_owns(const char *cls, const char *m) {
  static const char *const num[] = {
    "%", "+@", "-@", "<=>", "abs", "abs2", "angle", "arg", "ceil", "clone", "coerce", "conj",
    "conjugate", "denominator", "div", "divmod", "dup", "eql?", "fdiv", "finite?", "floor", "i",
    "imag", "imaginary", "infinite?", "integer?", "magnitude", "modulo", "negative?", "nonzero?",
    "numerator", "phase", "polar", "positive?", "quo", "real", "real?", "rect", "rectangular",
    "remainder", "round", "step", "to_c", "to_int", "truncate", "zero?", NULL };
  return is_numeric_class_name(cls) && builtin_name_in(m, num);
}

int builtin_module_owns(const char *cls, const char *m) {
  static const char *const enm[] = {
    "all?", "any?", "chain", "chunk", "chunk_while", "collect", "collect_concat", "compact",
    "count", "cycle", "detect", "drop", "drop_while", "each_cons", "each_entry", "each_slice",
    "each_with_index", "each_with_object", "entries", "filter", "filter_map", "find", "find_all",
    "find_index", "first", "flat_map", "grep", "grep_v", "group_by", "include?", "inject", "lazy",
    "map", "max", "max_by", "member?", "min", "min_by", "minmax", "minmax_by", "none?", "one?",
    "partition", "reduce", "reject", "reverse_each", "select", "slice_after", "slice_before",
    "slice_when", "sort", "sort_by", "sum", "take", "take_while", "tally", "to_a", "to_h", "to_set",
    "uniq", "zip", NULL };
  if (builtin_comparable_owns(cls, m)) return 1;
  if (builtin_numeric_owns(cls, m)) return 1;
  if ((sp_streq(cls, "Array") || sp_streq(cls, "Hash") || sp_streq(cls, "Range")) && builtin_name_in(m, enm)) return 1;
  return 0;
}

int is_gated_exception_accessor(const char *n) {
  static const char *const names[] = {
    "key", "receiver", "args", "private_call?", "reason", "exit_value", "tag",
    "value", "status", "success?", "signo", "signm", "name", "errno", "result", NULL };
  if (!n) return 0;
  for (int i = 0; names[i]; i++) if (sp_streq(n, names[i])) return 1;
  return 0;
}

int is_symbol_exception_accessor(const char *n) {
  return sp_streq(n, "reason") || sp_streq(n, "tag") || sp_streq(n, "key") || sp_streq(n, "name");
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
         sp_streq(name, "Rational")  || sp_streq(name, "Proc") ||
         sp_streq(name, "Enumerator") || sp_streq(name, "MatchData") ||
         sp_streq(name, "Complex")   || sp_streq(name, "Regexp") ||
         sp_streq(name, "Struct")    ||
         sp_streq(name, "File")      || sp_streq(name, "Class") ||
         sp_streq(name, "Hash")      ||
         /* a thread and a fiber are runtime handles too (activesupport's
            IsolatedExecutionState gives both an accessor) */
         sp_streq(name, "Thread")    || sp_streq(name, "Fiber") ||
         sp_streq(name, "Random");
}

/* CRuby's nil.public_methods: NilClass's own (to_a, to_s, inspect, &, ...)
   and the ones every object has from Object and Kernel. A call of any other
   name on nil raises NoMethodError. */
int is_nil_method(const char *n) {
  static const char *const names[] = {
    "!", "!=", "!~", "&", "<=>", "==", "===", "=~", "^", "__id__", "__send__", "class",
    "clone", "define_singleton_method", "display", "dup", "enum_for", "eql?", "equal?",
    "extend", "freeze", "frozen?", "hash", "inspect", "instance_eval", "instance_exec",
    "instance_of?", "instance_variable_defined?", "instance_variable_get",
    "instance_variable_set", "instance_variables", "is_a?", "itself", "kind_of?", "method",
    "methods", "nil?", "object_id", "private_methods", "protected_methods", "public_method",
    "public_methods", "public_send", "rationalize", "remove_instance_variable", "respond_to?",
    "send", "singleton_class", "singleton_method", "singleton_methods", "tap", "then", "to_a",
    "to_c", "to_enum", "to_f", "to_h", "to_i", "to_r", "to_s", "yield_self", "|", NULL };
  for (int i = 0; names[i]; i++) if (sp_streq(n, names[i])) return 1;
  return 0;
}

int is_io_read(const char *n) { return n && sp_streq(n, "read"); }
int is_positional_io(const char *n) {
  return sp_streq(n, "pread") || sp_streq(n, "pwrite");
}

/* An Array subclass instance's questions about the object itself rather
   than its elements (#7449): the class's own answers, through the object
   paths. dup and clone keep the class and copy the elements. */
int is_arysub_object_name(const char *n) {
  static const char *const names[] = {
    "class", "singleton_class", "is_a?", "kind_of?", "instance_of?", "respond_to?",
    "equal?", "object_id", "__id__", "dup", "clone", "itself", "tap", "then",
    "yield_self", "instance_variable_get", "instance_variable_set",
    "instance_variable_defined?", "instance_variables", "remove_instance_variable",
    "send", "public_send", "__send__", "method", "public_method", "methods",
    "public_methods", "singleton_methods", "define_singleton_method", "extend",
    "instance_eval", "instance_exec", "nil?", "!", "display", NULL };
  for (int i = 0; names[i]; i++) if (sp_streq(n, names[i])) return 1;
  return 0;
}

/* The Object methods an Array subclass instance answers as its Array
   (#7449): to_enum and enum_for walk its elements, frozen? reads the
   Array's frozen flag, != negates Array#==. */
int is_arysub_kernel_name(const char *n) {
  return sp_streq(n, "to_enum") || sp_streq(n, "enum_for") || sp_streq(n, "frozen?") || sp_streq(n, "!=");
}

int array_unseen_add_kind(const char *n) {
  if (sp_streq(n, "concat")) return ARRAY_ADD_CONCAT;
  if (sp_streq(n, "insert")) return ARRAY_ADD_INSERT;
  if (sp_streq(n, "prepend")) return ARRAY_ADD_PREPEND;
  return ARRAY_ADD_NONE;
}

int is_scan_name(const char *n) { return sp_streq(n, "scan"); }

/* with_index / with_object: an enumerator link that hands on its source's
   elements first */
int is_enumerator_with(const char *n) {
  return sp_streq(n, "with_index") || sp_streq(n, "with_object");
}
int is_lazy_name(const char *n) { return n && sp_streq(n, "lazy"); }
int is_concat_name(const char *n) { return sp_streq(n, "concat"); }

/* `Array.new(...)`, by its receiver's constant name and the method */
int is_array_constructor(const char *recv, const char *meth) {
  return recv && meth && sp_streq(meth, "new") && sp_streq(recv, "Array");
}
int is_pow_name(const char *n) { return sp_streq(n, "pow"); }
/* `new`: a class's constructor (a native binding's "new" names them) */
int is_new_name(const char *n) { return n && sp_streq(n, "new"); }
/* `native_share`: a package's declaration of what a native binding does
   with the String its object keeps (--share-strings) */
int is_native_share_decl(const char *n) { return n && sp_streq(n, "native_share"); }
/* `Proc.new`, by its receiver's constant name and the method */
int is_proc_new(const char *recv, const char *meth) {
  return recv && meth && sp_streq(meth, "new") && sp_streq(recv, "Proc");
}
/* `method`: Kernel#method, the Method of the name it is handed */
int is_method_ref_name(const char *n) { return n && sp_streq(n, "method"); }
/* ENV, which answers a new String each read */
int is_env_const(const char *n) { return n && sp_streq(n, "ENV"); }
/* ARGV, whose Strings CRuby freezes */
int is_argv_const(const char *n) { return n && sp_streq(n, "ARGV"); }
/* to_proc curry: a proc made of a Symbol, a Method or a proc */
int is_proc_conversion_name(const char *n) { return n && (sp_streq(n, "to_proc") || sp_streq(n, "curry")); }
/* `[]` alone: an element read, not its write */
int is_aref_name(const char *n) { return n && sp_streq(n, "[]"); }
/* `<<` alone: String#<<'s append, a chain's link */
int is_shovel_name(const char *n) { return n && sp_streq(n, "<<"); }

/* Dir's surface rewrites keep the written name distinct from the builtin
   operation it lowers to. The iterator aliases apply only without a block. */
const char *dir_surface_alias(const char *n, int blockless_iter) {
  if (blockless_iter) {
    if (sp_streq(n, "foreach")) return "entries";
    if (sp_streq(n, "each_child")) return "children";
  }
  else {
    if (sp_streq(n, "getwd")) return "pwd";
    if (sp_streq(n, "delete") || sp_streq(n, "unlink")) return "rmdir";
    if (is_aref_name(n)) return "glob";
  }
  return NULL;
}

int is_kernel_module_name(const char *n) { return n && sp_streq(n, "Kernel"); }

/* Kernel's module functions, by name. A whitelist rather than "anything with
   the Kernel receiver": Kernel is also a VALUE, so `Kernel === 5` asks whether
   5 is in the Object hierarchy, and `Kernel.name` / `.to_s` / `.freeze` /
   `.instance_methods` are Module's own methods on it. Dropping the receiver for
   those would change what they mean. Only names that Module does not also
   answer belong here. */
int is_kernel_module_function(const char *m) {
  static const char *const K[] = {
    "puts", "print", "p", "pp", "printf", "sprintf", "format",
    "raise", "fail", "exit", "exit!", "abort", "at_exit",
    "rand", "srand", "sleep", "gets", "loop", "lambda", "proc",
    "block_given?", "catch", "throw", "caller", "binding", "__method__",
    "require", "require_relative", "load", "warn", "system", "exec", "spawn",
    "Integer", "Float", "String", "Array", "Hash", "Rational", "Complex",
    NULL
  };
  return m && builtin_name_in(m, K);
}

/* the builtins whose subclass instance IS the builtin (#7449) */
int is_embedding_builtin(const char *nm) {
  return nm && (sp_streq(nm, "Array") || sp_streq(nm, "Hash"));
}

int is_class_name_name(const char *n) { return n && sp_streq(n, "name"); }

int is_alias_method_name(const char *n) { return n && sp_streq(n, "alias_method"); }

int is_define_method_name(const char *n) {
  return n && sp_streq(n, "define_method");
}
