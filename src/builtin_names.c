/* builtin_names.c -- the families of builtin method names (builtin_names.h).
   Each family is spelled once here; the order is the order its compares run
   in, the one most of the replaced chains used. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "builtin_names.h"

int is_zip_name(const char *n) {
  return sp_streq(n, "zip");
}

int is_dig_name(const char *n) {
  return sp_streq(n, "dig");
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

/* The calls whose arguments name a method by a value (a Symbol, a String,
   or one computed at run time): the send family, the Method and
   UnboundMethod readers, define/alias/respond_to?/method_defined?. A
   computed name among them can call or answer for any method the
   receiver has; the analysis bounds it by the literals that reach it
   (cf_collect_dyn), and the load graph counts a program's literals as
   names where one is. */
int is_method_name_reflection(const char *n) {
  return is_send_family(n) || sp_streq(n, "method") || sp_streq(n, "public_method") ||
         sp_streq(n, "instance_method") || sp_streq(n, "define_method") ||
         sp_streq(n, "define_singleton_method") || sp_streq(n, "alias_method") ||
         sp_streq(n, "singleton_method") || sp_streq(n, "respond_to?") || sp_streq(n, "method_defined?");
}

/* The calls given their code as a String, which the load graph reads as the
   code it is */
int is_code_eval_name(const char *n) {
  return sp_streq(n, "eval") || sp_streq(n, "class_eval") || sp_streq(n, "module_eval") ||
         sp_streq(n, "instance_eval");
}

/* A Symbol made of a value: the name it makes is known only at run time */
int is_symbol_conversion(const char *n) {
  return sp_streq(n, "to_sym") || sp_streq(n, "intern");
}

/* The name a builtins/ file gives a helper of its own (the enumerator's
   `__enumw_map`, enumerable.rb's `__no_false`, ThreadGroup's `__remove`):
   a leading `__`, without the trailing one of CRuby's own `__send__`,
   `__id__` and `__method__`. A definition of that name in a builtins/ file
   is the compiler's, not a Ruby method (comp_scope_is_private_helper). */
int is_builtin_helper_name(const char *n) {
  size_t l = n ? strlen(n) : 0;
  return l > 2 && n[0] == '_' && n[1] == '_' && !(n[l - 1] == '_' && n[l - 2] == '_');
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

/* A call whose block the program keeps to run later, not before the call
   answers: a Proc or lambda made of it (`proc`, `lambda`, `Proc.new`), a
   method body (`define_method`, `define_singleton_method`), code run on its
   own (a thread, a fiber, a signal handler: is_async_code_entry), at exit
   (`at_exit`) or at collection (`ObjectSpace.define_finalizer`), and an
   Enumerator's generator (`Enumerator.new`). */
int is_deferred_block_call(const char *recv, const char *n) {
  if (!n) return 0;
  if (is_proc_constructor(n) || is_define_method_name(n) || is_define_singleton_method_name(n) ||
      sp_streq(n, "at_exit") || is_async_code_entry(recv, n)) return 1;
  if (!recv) return 0;
  return ((sp_streq(recv, "Proc") || sp_streq(recv, "Enumerator")) && sp_streq(n, "new")) ||
         (sp_streq(recv, "ObjectSpace") && sp_streq(n, "define_finalizer"));
}

int is_name_reader(const char *n) {
  return sp_streq(n, "name") || sp_streq(n, "to_s") || sp_streq(n, "inspect");
}

/* A name every builtin class that has it (Module, Symbol, Method, Thread,
   Encoding, NameError) answers by reading a value, running no Ruby code */
int is_plain_builtin_reader(const char *n) {
  return sp_streq(n, "name");
}

/* `tap` alone: it answers its receiver, where then/yield_self answer the
   block's value (is_tap_alias takes all three) */
int is_tap_name(const char *n) {
  return sp_streq(n, "tap");
}

int is_itself_name(const char *n) {
  return sp_streq(n, "itself");
}

int is_each_char_name(const char *n) {
  return sp_streq(n, "each_char");
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

/* The visibility a bare `module_function` / `public` / `private` /
   `protected` sets: BVIS_* as named, 0 for any other name. */
int visibility_of_name(const char *n) {
  if (!n) return 0;
  if (sp_streq(n, "public")) return BVIS_PUBLIC;
  if (sp_streq(n, "private")) return BVIS_PRIVATE;
  if (sp_streq(n, "protected")) return BVIS_PROTECTED;
  return sp_streq(n, "module_function") ? BVIS_MODULE_FUNCTION : 0;
}

int is_select_bang(const char *n) {
  return sp_streq(n, "select!") || sp_streq(n, "filter!") || sp_streq(n, "keep_if") ||
         sp_streq(n, "reject!") || sp_streq(n, "delete_if");
}




int is_stop_walk(const char *n) {
  return is_find_or_take_while(n) || sp_streq(n, "find_index") || sp_streq(n, "each_with_index");
}

int is_range_member_walk(const char *n) {
  return sp_streq(n, "each") || sp_streq(n, "each_entry") || sp_streq(n, "step");
}

int is_find_index(const char *n) {
  return sp_streq(n, "find_index");
}

int is_each_entry(const char *n) {
  return sp_streq(n, "each_entry");
}

int is_each_name(const char *n) {
  return sp_streq(n, "each");
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

int is_step_call(const char *n) { return sp_streq(n, "step"); }

int is_int_step(const char *n) {
  return sp_streq(n, "times") || sp_streq(n, "upto") || sp_streq(n, "downto");
}

/* The builtin protocol's next method when the receiver has no override. */
const char *builtin_operator_delegate(const char *name) {
  if (sp_streq(name, "!=") || sp_streq(name, "===")) return "==";
  if (sp_streq(name, "==") || sp_streq(name, "<") || sp_streq(name, "<=") ||
      sp_streq(name, ">") || sp_streq(name, ">=") || sp_streq(name, "between?") ||
      sp_streq(name, "clamp")) return "<=>";
  return NULL;
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

/* The calls that run a String in the class or module they are called on, as
   its body's code: class_eval and module_eval (their exec kin take only a
   block) */
int is_class_eval_text_name(const char *n) {
  return sp_streq(n, "class_eval") || sp_streq(n, "module_eval");
}

/* The calls that read the frame a String of code runs in: one that runs
   code itself, the frame's binding, and its file's directory, which an
   eval's text answers its own */
int is_eval_frame_reader(const char *n) {
  return is_code_eval_name(n) || is_eval_exec_family(n) || sp_streq(n, "binding") || sp_streq(n, "__dir__");
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

/* The conversions that turn a nil receiver into a String ("" for to_s) and
   answer a String receiver itself: to_s to_str +@. */
int is_string_conversion(const char *n) {
  return sp_streq(n, "to_s") || sp_streq(n, "to_str") || sp_streq(n, "+@");
}

/* The names a String answers itself to: to_s to_str itself +@. */
int is_string_self_answer(const char *n) {
  return is_receiver_conversion(n) || sp_streq(n, "+@");
}

/* A call that can run code of the program's own the walk does not see, and
   so rebind a variable through it: a proc's call, a send, a reflective
   write or eval, a yield. */
int is_rebinding_reach_call(const char *n) {
  return sp_streq(n, "call") || sp_streq(n, "()") || is_send_family(n) ||
         sp_streq(n, "instance_variable_set") || is_instance_eval_family(n) || sp_streq(n, "yield");
}

int is_message_name(const char *n) { return n && sp_streq(n, "message"); }

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

/* The named spelling of Array#-: its functions raise under - (#9347). */
int is_difference_spelling(const char *n) {
  return sp_streq(n, "difference");
}

/* The spellings of an Array's or Hash's length whose function names length
   (sp_*Array_length_m): a nil receiver raises under the name as written, which
   is length's own only for length itself (#9347). */
int is_length_spelling(const char *n) {
  return sp_streq(n, "size") || sp_streq(n, "count") || sp_streq(n, "empty?");
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

int is_freeze_name(const char *n) { return sp_streq(n, "freeze"); }

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

int is_minmax_pair(const char *n) { return n && sp_streq(n, "minmax"); }

int is_endpoint_query(const char *n) {
  return sp_streq(n, "first") || sp_streq(n, "last");
}

int is_map_alias(const char *n) {
  return sp_streq(n, "map") || sp_streq(n, "collect");
}

int is_instance_eval_family(const char *n) {
  return sp_streq(n, "instance_eval") || sp_streq(n, "instance_exec");
}
/* The reflective reads and writes of a variable by the name it is given: the
   ivar, class variable and constant accessors and removers */
int is_reflective_access(const char *n) {
  return sp_streq(n, "instance_variable_get") || sp_streq(n, "instance_variable_set") ||
         sp_streq(n, "class_variable_get") || sp_streq(n, "class_variable_set") ||
         sp_streq(n, "const_get") || sp_streq(n, "const_set") || sp_streq(n, "remove_instance_variable") ||
         sp_streq(n, "remove_class_variable") || sp_streq(n, "remove_const");
}
/* The reflective listings of a kind of variable */
int is_reflective_listing(const char *n) {
  return sp_streq(n, "instance_variables") || sp_streq(n, "class_variables") || sp_streq(n, "constants");
}
/* The calls that reach a local by a name or a string of code */
int is_local_reflection(const char *n) {
  return sp_streq(n, "eval") || sp_streq(n, "binding") || sp_streq(n, "local_variable_get") ||
         sp_streq(n, "local_variable_set") || sp_streq(n, "local_variables");
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

int is_lambda_constructor(const char *n) {
  return sp_streq(n, "lambda");
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
/* delete: removal by key */
int is_delete_name(const char *n) { return n && sp_streq(n, "delete"); }

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
int is_seq_index_call(const char *n) {
  return is_slice_alias(n) || sp_streq(n, "[]=") || sp_streq(n, "slice!") || sp_streq(n, "byteslice");
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

/* the output calls a singleton-class body runs where they stand */
int is_stmt_output_name(const char *n) {
  return n && (sp_streq(n, "puts") || sp_streq(n, "print") || sp_streq(n, "p"));
}

/* the Kernel calls that act (output, an exception, exit) and so run where
   they stand in a class body, as CRuby runs it top to bottom */
int is_class_body_acting_name(const char *n) {
  static const char *const names[] = { "puts", "print", "p", "pp", "printf",
    "putc", "warn", "raise", "fail", "exit", "exit!", "abort", "sleep", "at_exit",
    "srand", "loop", "catch", NULL };
  for (int i = 0; n && names[i]; i++) if (sp_streq(n, names[i])) return 1;
  return 0;
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

int is_mixin_call(const char *n) {
  return sp_streq(n, "include") || sp_streq(n, "extend") || sp_streq(n, "prepend");
}

int is_class_plain_query(const char *n) {
  static const char *const names[] = {
    "new", "allocate", "name", "to_s", "inspect", "===", "==", "!=", "<", "<=", ">", ">=", "<=>",
    "hash", "eql?", "equal?", "respond_to?", "is_a?", "kind_of?", "instance_of?", "const_defined?",
    "method_defined?", "instance_methods", "attr_accessor", "attr_reader", "attr_writer",
    "include?", NULL };
  for (int i = 0; n && names[i]; i++) if (sp_streq(n, names[i])) return 1;
  return visibility_of_name(n) != 0;
}

int is_class_self_answer(const char *n) {
  return n && (is_mixin_call(n) || sp_streq(n, "private_class_method") || sp_streq(n, "public_class_method") ||
               sp_streq(n, "private_constant") || sp_streq(n, "public_constant") ||
               sp_streq(n, "remove_method") || sp_streq(n, "undef_method"));
}

int is_self_rebinding_call(const char *n) {
  return is_eval_exec_family(n) || sp_streq(n, "define_singleton_method");
}

int is_class_relatives_query(const char *n) {
  return sp_streq(n, "ancestors") || sp_streq(n, "superclass") || sp_streq(n, "subclasses") ||
         sp_streq(n, "descendants") || sp_streq(n, "included_modules");
}

int is_class_query(const char *n) {
  return sp_streq(n, "class");
}

int is_method_owner_query(const char *n) {
  return sp_streq(n, "owner");
}

int is_instance_method_name(const char *n) { return n && (sp_streq(n, "instance_method") || sp_streq(n, "public_instance_method")); }
int is_method_object_maker(const char *n) {
  return n && (sp_streq(n, "method") || sp_streq(n, "public_method") || sp_streq(n, "singleton_method") ||
               sp_streq(n, "instance_method") || sp_streq(n, "public_instance_method"));
}

int is_constant_or_object_reflection(const char *n) {
  return sp_streq(n, "const_get") || sp_streq(n, "const_source_location") || sp_streq(n, "nesting") ||
         sp_streq(n, "attached_object") || sp_streq(n, "each_object") || sp_streq(n, "eval") ||
         sp_streq(n, "binding");
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

int is_autoload_query_name(const char *n) {
  return sp_streq(n, "autoload?");
}

/* the methods a definition runs when the program defines them: inherited
   (class), included, extended, prepended, method_added and its singleton
   kin, const_added */
int is_definition_hook_name(const char *n) {
  return sp_streq(n, "inherited") || sp_streq(n, "included") || sp_streq(n, "extended") ||
         sp_streq(n, "prepended") || sp_streq(n, "method_added") ||
         sp_streq(n, "singleton_method_added") || sp_streq(n, "const_added");
}

/* The hooks definition form `form` (given its arguments) fires, on the
   class that receives it (sg: as its singleton's), into out[0..1]: a
   mixin's on the module (included and append_features, extended and
   extend_object, prepended and prepend_features); a form that adds a
   method or changes one's visibility, method_added (singleton_method_added
   for a singleton's; module_function both); remove_method method_removed,
   undef_method method_undefined. Answers how many. */
int definition_form_hooks(const char *form, int sg, const char **out) {
  if (!form) return 0;
  if (sp_streq(form, "include")) { out[0] = "included"; out[1] = "append_features"; return 2; }
  if (sp_streq(form, "extend")) { out[0] = "extended"; out[1] = "extend_object"; return 2; }
  if (sp_streq(form, "prepend")) { out[0] = "prepended"; out[1] = "prepend_features"; return 2; }
  if (sp_streq(form, "remove_method")) { out[0] = sg ? "singleton_method_removed" : "method_removed"; return 1; }
  if (sp_streq(form, "undef_method")) { out[0] = sg ? "singleton_method_undefined" : "method_undefined"; return 1; }
  if (sp_streq(form, "module_function")) { out[0] = "singleton_method_added"; out[1] = "method_added"; return 2; }
  if (sp_streq(form, "define_singleton_method")) { out[0] = "singleton_method_added"; return 1; }
  if (is_method_naming_call(form) || visibility_of_name(form)) { out[0] = sg ? "singleton_method_added" : "method_added"; return 1; }
  return 0;
}

int is_method_missing_name(const char *n) {
  return n && (sp_streq(n, "method_missing") || sp_streq(n, "respond_to_missing?"));
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

/* A core call that makes objects of whatever classes its data names (or
   finds any object): Marshal.load and restore, YAML's and Psych's load
   family, JSON.load, JSON.parse given options (create_additions),
   ObjectSpace._id2ref; argc its argument count */
int is_object_loader(const char *recv, const char *meth, int argc) {
  if (!recv || !meth) return 0;
  if (sp_streq(recv, "Marshal")) return sp_streq(meth, "load") || sp_streq(meth, "restore");
  if (sp_streq(recv, "YAML") || sp_streq(recv, "Psych"))
    return sp_streq(meth, "load") || sp_streq(meth, "unsafe_load") || sp_streq(meth, "load_file") || sp_streq(meth, "unsafe_load_file");
  if (sp_streq(recv, "ObjectSpace")) return sp_streq(meth, "_id2ref");
  return sp_streq(recv, "JSON") && (sp_streq(meth, "load") || sp_streq(meth, "load_file") || sp_streq(meth, "unsafe_load") ||
                                    ((sp_streq(meth, "parse") || sp_streq(meth, "parse!")) && argc > 1));
}

/* UnboundMethod#bind and bind_call */
int is_method_bind_name(const char *n) {
  return n && (sp_streq(n, "bind") || is_bind_call(n));
}

/* Timeout.timeout(sec, klass): raises klass.new when the time runs out */
int is_timeout_call(const char *recv, const char *meth) {
  return recv && meth && sp_streq(recv, "Timeout") && sp_streq(meth, "timeout");
}

/* exception: Exception's class-level constructor (raise calls it) */
int is_exception_ctor_name(const char *n) {
  return n && sp_streq(n, "exception");
}

/* to_enum and enum_for: an Enumerator that calls the method they name */
int is_enumerator_maker(const char *n) {
  return n && (sp_streq(n, "to_enum") || sp_streq(n, "enum_for"));
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
  /* (a String's table is keyed by its shared handle: the share walk gives
     a String an instance variable is kept on that identity) */
  return sp_streq(n, "Array") || sp_streq(n, "Hash") || sp_streq(n, "Random") || sp_streq(n, "String");
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
/* the reflection that reads or writes an object's instance variables by name */
int is_ivar_reflection_name(const char *n) {
  return sp_streq(n, "instance_variable_get") || sp_streq(n, "instance_variable_set") ||
         sp_streq(n, "instance_variable_defined?") || sp_streq(n, "instance_variables") ||
         sp_streq(n, "remove_instance_variable");
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

int is_substitution_bang(const char *n) {
  return sp_streq(n, "gsub!") || sp_streq(n, "sub!");
}

int is_encoding_mutator(const char *n) {
  return sp_streq(n, "force_encoding") || sp_streq(n, "encode!");
}

int encoding_mutator_max_argc(const char *n) {
  return sp_streq(n, "encode!") ? 3 : 2;
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

int str_int_promotes(const char *n, int argc) {
  if (sp_streq(n, "to_i")) return argc <= 1;
  return argc == 0 && (sp_streq(n, "hex") || sp_streq(n, "oct"));
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

/* The Socket constants sp_sock_const (lib/sp_io.c) answers on every
   platform -- the entries of its table outside any #ifdef. The others are
   the system's to define, so only the run time knows them. */
int sock_const_always(const char *n) {
  static const char *const names[] = {
    "AF_INET", "AF_INET6", "AF_UNIX", "PF_INET", "PF_INET6", "PF_UNIX",
    "SOCK_STREAM", "SOCK_DGRAM", "SHUT_RD", "SHUT_WR", "SHUT_RDWR", NULL
  };
  for (int i = 0; n && names[i]; i++) if (sp_streq(names[i], n)) return 1;
  return 0;
}
int is_iso8601_alias(const char *n) {
  return sp_streq(n, "iso8601") || sp_streq(n, "xmlschema");
}

int is_rfc2822_alias(const char *n) {
  return sp_streq(n, "rfc2822") || sp_streq(n, "rfc822");
}

/* The formats Spinel's runtime writes and reads itself once "time" is
   required, which CRuby's time.rb (packages/time) defines in Ruby: Time#x
   and Time.x for each. time.rb's own definitions of them are not what a
   program reaches here; its parse and strptime are (#9946). */
int is_time_native_format(const char *n) {
  return is_iso8601_alias(n) || sp_streq(n, "httpdate") || is_rfc2822_alias(n);
}

/* Time's class-method parsers: parse, strptime and the native formats */
int is_time_parser_name(const char *n) {
  return sp_streq(n, "parse") || sp_streq(n, "strptime") || is_time_native_format(n);
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

/* The names CRuby's core asks a receiver through rb_check_funcall, which
   calls method_missing when respond_to_missing? answers true */
int is_check_funcall_name(const char *n) {
  return n && (sp_streq(n, "respond_to?") || sp_streq(n, "respond_to_missing?") || sp_streq(n, "method_missing"));
}

/* The methods CRuby makes private wherever they are defined:
   initialize, its copy forms, and respond_to_missing? */
int is_implicitly_private_name(const char *n) {
  return n && (is_initialize_family(n) || sp_streq(n, "initialize_clone") || sp_streq(n, "initialize_dup") ||
               sp_streq(n, "respond_to_missing?"));
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

/* The first row of each name, through a table hashed on the first ask: the
   lookup compared the name with every row, and is asked of class names per
   call (is_builtin_reopen, of each class a poly block dispatch weighs). */
#define BUILTIN_ROW_CAP 512   /* a power of two above 4 * the rows */
static short builtin_row_slot[BUILTIN_ROW_CAP];
static int builtin_row_built;
int builtin_core_class_slot(const char *n) {
  if (!n) return -1;
  const BuiltinClass *r;
  if (!builtin_row_built) {
    for (int i = 0; i < BUILTIN_ROW_CAP; i++) builtin_row_slot[i] = -1;
    for (int i = 0; (r = builtin_class_at(i)) != NULL; i++) {
      unsigned h = sp_strhash(r->name) & (BUILTIN_ROW_CAP - 1);
      while (builtin_row_slot[h] >= 0 && !sp_streq(builtin_class_at(builtin_row_slot[h])->name, r->name))
        h = (h + 1) & (BUILTIN_ROW_CAP - 1);
      if (builtin_row_slot[h] < 0) builtin_row_slot[h] = (short)i;   /* a later row of the name: the first answers */
    }
    builtin_row_built = 1;
  }
  unsigned h = sp_strhash(n) & (BUILTIN_ROW_CAP - 1);
  while (builtin_row_slot[h] >= 0 && !sp_streq(builtin_class_at(builtin_row_slot[h])->name, n))
    h = (h + 1) & (BUILTIN_ROW_CAP - 1);
  if (builtin_row_slot[h] >= 0) return builtin_row_slot[h];
  if (sp_streq(n, "StringIO")) return CORE_STRINGIO;
  if (sp_streq(n, "Set")) return CORE_SET;
  if (sp_streq(n, CLASS_REOPEN_NAME)) return CORE_CLASS_REOPEN;
  if (sp_streq(n, "FileTest")) return CORE_FILETEST;
  if (sp_streq(n, "Errno")) return CORE_ERRNO;
  if (sp_streq(n, "Warning")) return CORE_WARNING;
  return -1;
}

/* A class or module besides the builtin classes (is_builtin_reopen) whose
   methods every builtin value may run: Object, BasicObject, Kernel,
   Comparable, Enumerable, Module. */
int is_builtin_method_host_name(const char *n) {
  return is_object_root(n) || sp_streq(n, "Comparable") || sp_streq(n, "Enumerable") || sp_streq(n, "Module");
}

int is_queue_class_name(const char *n) {
  return sp_streq(n, "Queue") || sp_streq(n, "SizedQueue");
}

int is_standard_output_global(const char *n) {
  return sp_streq(n, "$stdout") || sp_streq(n, "$stderr");
}



int is_io_class_name(const char *n) {
  return sp_streq(n, "IO") || sp_streq(n, "File");
}

int is_program_name_global(const char *n) {
  return sp_streq(n, "$PROGRAM_NAME") || sp_streq(n, "$0");
}
int is_loaded_features_global(const char *n) {
  return n && (sp_streq(n, "$\"") || sp_streq(n, "$LOADED_FEATURES"));
}
int is_load_path_global(const char *n) {
  return n && (sp_streq(n, "$:") || sp_streq(n, "$LOAD_PATH") || sp_streq(n, "$-I"));
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

/* A builtin module the ancestors tables list by its (negative) builtin id
   (Comparable, Enumerable, Kernel, Math): that id, 0 for any other name */
int builtin_mixin_module_id(const char *n) {
  int id = n ? builtin_class_id(n) : 0;
  return id == -114 || id == -115 || id == -119 || id == -130 ? id : 0;
}
int is_include_name(const char *n) { return sp_streq(n, "include"); }
int is_prepend_name(const char *n) { return sp_streq(n, "prepend"); }
int is_load_name(const char *n) { return sp_streq(n, "load"); }
int is_require_name(const char *n) { return sp_streq(n, "require") || sp_streq(n, "require_relative"); }
int is_require_relative_name(const char *n) { return sp_streq(n, "require_relative"); }
int is_extend_name(const char *n) { return sp_streq(n, "extend"); }
int is_autoload_name(const char *n) { return sp_streq(n, "autoload"); }
int is_file_load_name(const char *n) {
  return n && (is_require_name(n) || is_load_name(n) || is_autoload_name(n));
}
/* A LoadNode's `how` (spinel_parse.c, sp_load_resolve) that names no file
   at compile time: one not there (LoadError when it runs, for the feature
   the node carries), or a path the require computes when it runs
   (is_load_how_set). */
int is_load_how_missing(const char *how) { return how && sp_streq(how, "none"); }
/* ...a path computed when it runs: it names one of a set of files (the
   node's candidates, prog_load_cand), or any file of the program (open), or a
   feature the runtime provides, or none of them */
int is_load_how_set(const char *how) { return how && sp_streq(how, "set"); }
/* ...and the two the runtime provides without a file: a core capability
   (Thread, Enumerator, Rational: CRuby has it loaded already) and a native
   library (io/console, time, socket, ostruct, monitor). */
int is_load_how_core(const char *how) { return how && sp_streq(how, "core"); }
int is_load_how_native(const char *how) { return how && sp_streq(how, "native"); }
/* ...and a native library CRuby has loaded before the program runs
   (`monitor`, which rubygems requires), whose require answers false the
   first time as a core capability's does; the others answer true once. */
int is_native_lib_at_boot(const char *path) { return path && sp_streq(path, "monitor"); }

int is_enumerable_module_name(const char *n) {
  return n && sp_streq(n, "Enumerable");
}

int is_comparable_module_name(const char *n) {
  return n && sp_streq(n, "Comparable");
}

int is_comparable_builtin_name(const char *n) {
  return sp_streq(n, "String") || sp_streq(n, "Numeric") ||
         is_numeric_class_name(n) || sp_streq(n, "Rational") ||
         sp_streq(n, "Complex") || sp_streq(n, "Time") || sp_streq(n, "Symbol");
}

/* Numeric's default <=> tests identity, ignoring even a program ==.
   Object's default can call a program == ahead of Comparable; builtin ==
   below Comparable is shadowed by its recursive, false equality answer.
   String and Time instances use their builtin representation and comparator. */
BuiltinDefaultCmp builtin_default_cmp(int family) {
  if (family == builtin_class_id("String") || family == builtin_class_id("Time")) return BDC_BUILTIN;
  if (family == builtin_class_id("Numeric")) return BDC_IDENTITY;
  return BDC_EQUAL;
}

/* The modules a builtin class includes ahead of Object, with their own
   public methods as CRuby 4.0 lists them: Integer and Float are Numeric and
   Comparable, String and Symbol Comparable, Array, Hash and Range
   Enumerable. The builtin table holds each class's own methods only. */
/* Comparable's own methods on a builtin class that includes it: Integer,
   Float, String and Symbol. */
int builtin_comparable_owns(const char *cls, const char *m) {
  static const char *const cmp[] = { "<", "<=", "==", ">", ">=", "between?", "clamp", NULL };
  return is_comparable_builtin_name(cls) &&
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
    "value", "status", "success?", "signo", "signm", "name", "errno", "error_code", "result", NULL };
  if (!n) return 0;
  for (int i = 0; names[i]; i++) if (sp_streq(n, names[i])) return 1;
  return 0;
}

int is_symbol_exception_accessor(const char *n) {
  return sp_streq(n, "reason") || sp_streq(n, "tag") || sp_streq(n, "key") || sp_streq(n, "name");
}

int is_boxed_self_class_name(const char *name) {
  return name && (sp_streq(name, "Object") || sp_streq(name, "Numeric") ||
                  sp_streq(name, "Array") || sp_streq(name, "Hash"));
}

/* Classes whose reopenings keep the runtime value instead of a user struct.
   The names are a hash set, built at the first ask: the question is asked
   of every class of the program at each poly block dispatch the share facts
   weigh, where a chain of twenty-seven compares cost a Rails application
   ten seconds of its storage phase. */
static const char *const builtin_reopen_names[] = {
  "Toplevel", "String", "Integer", "Float", "Symbol", "TrueClass", "FalseClass",
  "NilClass", "Array", "Object", "Numeric", "Dir",
  /* runtime value types with a typedef of their own (sp_Range, sp_Time,
     sp_File, sp_Class): a user struct under that name was a C-level
     typedef collision before any call was reached (activesupport's
     blank.rb reopens Range and Time) */
  "Range", "Time", "Rational", "Proc", "Enumerator", "MatchData", "Complex",
  "Regexp", "Struct", "File", "Class", "Hash",
  /* a thread and a fiber are runtime handles too (activesupport's
     IsolatedExecutionState gives both an accessor) */
  "Thread", "Fiber", "Random",
  NULL };
#define BRN_CAP 128   /* a power of two above 4 * the names */
static signed char brn_slot[BRN_CAP];
static int brn_built;
int is_builtin_reopen_name(const char *name) {
  if (!brn_built) {
    for (int i = 0; i < BRN_CAP; i++) brn_slot[i] = -1;
    for (int i = 0; builtin_reopen_names[i]; i++) {
      unsigned h = sp_strhash(builtin_reopen_names[i]) & (BRN_CAP - 1);
      while (brn_slot[h] >= 0) h = (h + 1) & (BRN_CAP - 1);
      brn_slot[h] = (signed char)i;
    }
    brn_built = 1;
  }
  if (!name) return 0;
  unsigned h = sp_strhash(name) & (BRN_CAP - 1);
  while (brn_slot[h] >= 0) {
    if (sp_streq(builtin_reopen_names[brn_slot[h]], name)) return 1;
    h = (h + 1) & (BRN_CAP - 1);
  }
  return 0;
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
int is_insert_name(const char *n) { return sp_streq(n, "insert"); }
int is_clear_name(const char *n) { return sp_streq(n, "clear"); }

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
/* a Method (or an UnboundMethod) of the name a call is handed: calling it
   runs that method, a writer included */
int is_method_object_name(const char *n) {
  return n && (sp_streq(n, "method") || sp_streq(n, "public_method") || sp_streq(n, "instance_method"));
}
/* Marshal, whose load makes objects with any ivars set */
int is_marshal_const(const char *n) { return n && sp_streq(n, "Marshal"); }
/* ENV, which answers a new String each read */
int is_env_const(const char *n) { return n && sp_streq(n, "ENV"); }
/* RUBY_DESCRIPTION and its siblings: a builtin constant that is a frozen
   String (one object, read after read) */
int is_engine_string_const(const char *n) {
  return n && (sp_streq(n, "RUBY_DESCRIPTION") || sp_streq(n, "RUBY_VERSION") || sp_streq(n, "RUBY_PLATFORM") ||
               sp_streq(n, "RUBY_ENGINE") || sp_streq(n, "RUBY_ENGINE_VERSION") ||
               sp_streq(n, "RUBY_RELEASE_DATE") || sp_streq(n, "RUBY_REVISION") || sp_streq(n, "RUBY_COPYRIGHT"));
}
/* File's String constants, which codegen emits as static literals (the
   File arm of the constant path emitter): frozen, one String each */
int is_file_string_const(const char *n) {
  return n && (sp_streq(n, "SEPARATOR") || sp_streq(n, "PATH_SEPARATOR") || sp_streq(n, "NULL"));
}
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
/* Dir.foreach and Dir.each_child: without a block each answers an Enumerator
   over the entries (see sp_dir_foreach_enum). */
int is_dir_blockless_iter(const char *n) {
  return dir_surface_alias(n, 1) != NULL;
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
    "select",
    "Integer", "Float", "String", "Array", "Hash", "Rational", "Complex",
    NULL
  };
  return m && builtin_name_in(m, K);
}

/* Is m one of Object's own methods every object answers? */
int builtin_object_method_known(const char *m) {
  static const char *const OBJM2[] = {
    "class", "clone", "dup", "display", "enum_for", "eql?", "equal?",
    "extend", "freeze", "frozen?", "hash", "inspect", "instance_of?",
    "instance_variable_get", "instance_variable_set", "instance_variables",
    "is_a?", "itself", "kind_of?", "method", "methods", "nil?",
    "object_id", "public_send", "respond_to?", "send", "__send__",
    "tap", "then", "to_s", "yield_self", "==", "!=", "!", "===", NULL };
  return m && builtin_name_in(m, OBJM2);
}

/* The names CRuby's own facts give Object, BasicObject, Kernel, Module and
   Class (builtin_arity.inc), sorted once per self kind for
   builtin_self_method_known */
#define BAI(c,m,...) {c,m},
#define BAM(c,m,a) {c,m},
#define BAS(c,m,...) {c,m},
#define BAC(...)
#define BAK(...)
static const struct { const char *cls, *m; } self_fact_rows[] = {
#include "builtin_arity.inc"
};
#undef BAI
#undef BAM
#undef BAS
#undef BAC
#undef BAK
static int self_fact_cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
/* main's own methods, beside Object's: its singleton's (`to_s`, `inspect`
   answer "main") and the private ones a top-level body calls as a class
   body would (`include`, `private`, `define_method`, ...) */
static int main_self_method(const char *m) {
  static const char *const MAINM[] = { "to_s", "inspect", "include", "public", "private", "define_method",
                                       "using", "ruby2_keywords", NULL };
  return builtin_name_in(m, MAINM);
}
/* See builtin_names.h. Kernel's module functions and Object's surface
   answer on every self; the facts' Object, BasicObject and Kernel methods
   too, Module's on a module or class, Class's on a class. A binary search
   over each kind's names, sorted at the first ask. */
int builtin_self_method_known(const char *name, int kind) {
  enum { N = sizeof self_fact_rows / sizeof self_fact_rows[0] };
  static const char *names[BSELF_N][N];
  static int n[BSELF_N] = { -1, -1, -1 };
  if (!name || kind < 0 || kind >= BSELF_N) return 0;
  if (is_kernel_module_function(name) || builtin_object_method_known(name) ||
      (kind == BSELF_MAIN && main_self_method(name))) return 1;
  if (n[kind] < 0) {
    static const char *const owners[BSELF_N][6] = {
      { "Object", "BasicObject", "Kernel", NULL },
      { "Object", "BasicObject", "Kernel", "Module", NULL },
      { "Object", "BasicObject", "Kernel", "Module", "Class", NULL } };
    n[kind] = 0;
    for (int i = 0; i < N; i++)
      if (builtin_name_in(self_fact_rows[i].cls, owners[kind])) names[kind][n[kind]++] = self_fact_rows[i].m;
    qsort(names[kind], (size_t)n[kind], sizeof names[kind][0], self_fact_cmp);
  }
  return bsearch(&name, names[kind], (size_t)n[kind], sizeof names[kind][0], self_fact_cmp) != NULL;
}

/* the builtins whose subclass instance IS the builtin (#7449) */
int is_embedding_builtin(const char *nm) {
  return nm && (sp_streq(nm, "Array") || sp_streq(nm, "Hash") || sp_streq(nm, "String"));
}

int is_class_name_name(const char *n) { return n && sp_streq(n, "name"); }

int is_alias_method_name(const char *n) { return n && sp_streq(n, "alias_method"); }

int is_define_method_name(const char *n) {
  return n && sp_streq(n, "define_method");
}
int is_define_singleton_method_name(const char *n) { return n && sp_streq(n, "define_singleton_method"); }
int is_undef_method_name(const char *n) { return n && sp_streq(n, "undef_method"); }
int is_remove_method_name(const char *n) { return n && sp_streq(n, "remove_method"); }

/* node types with no NodeKind of their own, as nt_type spells them */
int is_arguments_node_type(const char *t) { return t && sp_streq(t, "ArgumentsNode"); }

int is_when_node_type(const char *t) { return t && sp_streq(t, "WhenNode"); }


/* The encoding `Encoding::<nm>` names, or NULL. CRuby defines a constant
   for every encoding name and alias with `-` and `.` as `_`, as written and
   upcased (Encoding::Shift_JIS, Encoding::ISO_8859_1,
   Encoding::ANSI_X3_4_1968, Encoding::CP65001); the answer is the
   encoding's own name (CP65001 is UTF-8). */
static const char *const enc_const_names[][2] = {
#include "../lib/sp_encoding_names.h"
  { NULL, NULL } };
static int enc_const_spells(const char *name, const char *nm, int up) {
  for (; *name && *nm; name++, nm++) {
    char ch = *name == '-' || *name == '.' ? '_' : up && *name >= 'a' && *name <= 'z' ? (char)(*name - 'a' + 'A') : *name;
    if (ch != *nm) return 0;
  }
  return !*name && !*nm;
}
const char *encoding_const_name(const char *nm) {
  if (!nm || !*nm) return NULL;
  for (int k = 0; enc_const_names[k][0]; k++)
    if (enc_const_spells(enc_const_names[k][0], nm, 1) || enc_const_spells(enc_const_names[k][0], nm, 0))
      return enc_const_names[k][1];
  return NULL;
}

int strh_op(const char *n, int argc, int block) {
  if (!n) return STRH_NONE;
  if (block == 1) {
    if (argc == 0 && sp_streq(n, "each")) return STRH_EACH;
    return STRH_NONE;
  }
  if (block) return STRH_NONE;
  if (argc == 1 && is_element_at_alias(n)) return STRH_ELEM;
  if (argc == 0 && is_endpoint_query(n)) return STRH_ELEM;
  if (argc == 2 && sp_streq(n, "[]=")) return STRH_STORE;
  if (argc >= 1 && is_push_alias(n) && (argc == 1 || !sp_streq(n, "<<"))) return STRH_PUSH;
  if (argc == 0 && (sp_streq(n, "pop") || sp_streq(n, "shift"))) return STRH_TAKE;
  if (argc == 0 && is_len_alias(n)) return STRH_LEN;
  if (argc == 0 && sp_streq(n, "empty?")) return STRH_EMPTY;
  if (argc == 0 && sp_streq(n, "dup")) return STRH_DUP;
  if (argc == 0 && sp_streq(n, "to_a")) return STRH_SELF;
  if (argc <= 1 && sp_streq(n, "join")) return STRH_JOIN;
  if (argc == 1 && sp_streq(n, "include?")) return STRH_INCLUDE;
  if (argc == 1 && sp_streq(n, "==")) return STRH_EQ;
  if (argc == 0 && (sp_streq(n, "inspect") || sp_streq(n, "to_s"))) return STRH_INSPECT;
  return STRH_NONE;
}






int is_pop_name(const char *n) { return n && sp_streq(n, "pop"); }

int is_first_name(const char *n) { return n && sp_streq(n, "first"); }

int is_mul_op(const char *n) { return n && sp_streq(n, "*"); }

int is_pow_op(const char *n) { return n && sp_streq(n, "**"); }

int is_minus_op(const char *n) { return n && sp_streq(n, "-"); }

int is_clone_name(const char *n) { return n && sp_streq(n, "clone"); }

int is_cover_name(const char *n) { return n && sp_streq(n, "cover?"); }

int is_flatten_name(const char *n) { return n && sp_streq(n, "flatten"); }

int is_hash_name(const char *n) { return n && sp_streq(n, "hash"); }

int is_puts_name(const char *n) { return n && sp_streq(n, "puts"); }

int is_to_f_name(const char *n) { return n && sp_streq(n, "to_f"); }

int is_to_i_name(const char *n) { return n && sp_streq(n, "to_i"); }

int is_intersect_name(const char *n) { return n && sp_streq(n, "intersect?"); }

int is_compare_by_identity_name(const char *n) { return n && sp_streq(n, "compare_by_identity"); }


/* File::Stat's predicates, which the TY_IO arms answer for the handle a stat
   is carried in: the mode predicates by their slot in sp_stat_pred (zero?,
   slot 1, has its own poly arm), the type predicates by theirs in
   sp_stat_type_pred plus 100. -1 for any other name. */
int boxed_stat_pred(const char *name) {
  static const char *const spred[] = { "pipe?", "", "readable?", "writable?",
                                       "executable?", "blockdev?", "chardev?",
                                       "size?", "executable_real?", "readable_real?",
                                       "writable_real?", NULL };
  static const char *const tpred[] = { "file?", "directory?", "symlink?",
                                       "owned?", "grpowned?", "setuid?",
                                       "setgid?", "sticky?", "socket?", NULL };
  for (int k = 0; spred[k]; k++) if (spred[k][0] && sp_streq(name, spred[k])) return k;
  for (int k = 0; tpred[k]; k++) if (sp_streq(name, tpred[k])) return 100 + k;
  return -1;
}

/* File::Stat's numeric fields, which the TY_IO arms answer for the handle a
   stat is carried in; a field's index is its slot in sp_stat_field. The
   world_*? pair answer the permission bits or nil, as a field may. */
static const char *const boxed_stat_sfield[] = { "uid", "gid", "nlink", "dev", "ino",
                                                 "blksize", "blocks", "rdev", "dev_major", "dev_minor",
                                                 "world_readable?", "world_writable?", NULL };
/* a field's slot in sp_stat_field, or -1 */
int stat_field_slot(const char *name) {
  for (int k = 0; boxed_stat_sfield[k]; k++)
    if (sp_streq(name, boxed_stat_sfield[k])) return k;
  return -1;
}
/* mode or one of those fields */
int boxed_stat_name(const char *name) {
  return sp_streq(name, "mode") || stat_field_slot(name) >= 0;
}
/* File::Stat's readers that a boxed receiver always dispatches on, where a
   class of the program's own keeps its arm: mtime, atime and ftype (their
   TY_IO rows say what each answers). ctime is left out: on a boxed
   receiver it may be Time#ctime's String as well. */
int boxed_stat_reader(const char *name) {
  return sp_streq(name, "mtime") || sp_streq(name, "atime") || sp_streq(name, "ftype");
}

/* The lowerings onto a library's method (builtin_names.h, LowerRow): one
   row per shape, in the order each family's desugar asked its names. */
static const char *const lr_walk_sources[] = { "each", "cycle", "new", NULL };
static const LowerRow lower_rows[] = {
  /* system(cmd, ..., out: File::NULL): CRuby's spawn-and-wait, with options */
  { LR_SYSTEM_OPTIONS, "system", LR_RECV_NONE, NULL, NULL, NULL, 2, -1, LR_NO_BLOCK | LR_KWHASH, 0, 0, 0,
    "__sp_system_spawn", "builtins/system_spawn" },
  /* IO.popen(cmd) { |io| ... }: read through a pipe, the child reaped */
  { LR_POPEN_BLOCK, "popen", LR_RECV_CONST, "IO", NULL, NULL, 0, -1, LR_BLOCK, 0, 0, 0,
    "__sp_popen", "builtins/io_popen" },
  /* File.foreach(path).first(n): the first n lines, read one at a time */
  { LR_FOREACH_FIRST, "first", LR_RECV_CALL, "foreach", "File", NULL, 1, 1, LR_NO_BLOCK | LR_PLAIN_ARGS,
    1, 1, LR_NO_BLOCK | LR_PLAIN_ARGS, "__sp_foreach_first", "builtins/file_foreach_first" },
  { LR_FOREACH_FIRST, "first", LR_RECV_CALL, "foreach", "IO", NULL, 1, 1, LR_NO_BLOCK | LR_PLAIN_ARGS,
    1, 1, LR_NO_BLOCK | LR_PLAIN_ARGS, "__sp_foreach_first", "builtins/file_foreach_first" },
  /* an Enumerator's walk whose block breaks: the helper drives `each` */
  { LR_ENUM_WALK, "map", LR_RECV_SOME, NULL, NULL, NULL, 0, 0, LR_BLOCK_LITERAL | LR_BREAKS, 0, 0, 0,
    "__enumw_map", "builtins/enumerator" },
  { LR_ENUM_WALK, "collect", LR_RECV_SOME, NULL, NULL, NULL, 0, 0, LR_BLOCK_LITERAL | LR_BREAKS, 0, 0, 0,
    "__enumw_map", "builtins/enumerator" },
  { LR_ENUM_WALK, "select", LR_RECV_SOME, NULL, NULL, NULL, 0, 0, LR_BLOCK_LITERAL | LR_BREAKS, 0, 0, 0,
    "__enumw_select", "builtins/enumerator" },
  { LR_ENUM_WALK, "filter", LR_RECV_SOME, NULL, NULL, NULL, 0, 0, LR_BLOCK_LITERAL | LR_BREAKS, 0, 0, 0,
    "__enumw_select", "builtins/enumerator" },
  { LR_ENUM_WALK, "reject", LR_RECV_SOME, NULL, NULL, NULL, 0, 0, LR_BLOCK_LITERAL | LR_BREAKS, 0, 0, 0,
    "__enumw_reject", "builtins/enumerator" },
  { LR_ENUM_WALK, "filter_map", LR_RECV_SOME, NULL, NULL, NULL, 0, 0, LR_BLOCK_LITERAL | LR_BREAKS, 0, 0, 0,
    "__enumw_filter_map", "builtins/enumerator" },
  { LR_ENUM_WALK, "each_with_object", LR_RECV_SOME, NULL, NULL, NULL, 1, 1, LR_BLOCK_LITERAL | LR_BREAKS, 0, 0, 0,
    "__enumw_each_with_object", "builtins/enumerator" },
  { LR_ENUM_WALK, "with_object", LR_RECV_SOME, NULL, NULL, NULL, 1, 1, LR_BLOCK_LITERAL | LR_BREAKS, 0, 0, 0,
    "__enumw_each_with_object", "builtins/enumerator" },
  { LR_ENUM_WALK, "inject", LR_RECV_SOME, NULL, NULL, NULL, 0, 0, LR_BLOCK_LITERAL | LR_BREAKS, 0, 0, 0,
    "__enumw_inject0", "builtins/enumerator" },
  { LR_ENUM_WALK, "inject", LR_RECV_SOME, NULL, NULL, NULL, 1, 1, LR_BLOCK_LITERAL | LR_BREAKS, 0, 0, 0,
    "__enumw_inject1", "builtins/enumerator" },
  { LR_ENUM_WALK, "reduce", LR_RECV_SOME, NULL, NULL, NULL, 0, 0, LR_BLOCK_LITERAL | LR_BREAKS, 0, 0, 0,
    "__enumw_inject0", "builtins/enumerator" },
  { LR_ENUM_WALK, "reduce", LR_RECV_SOME, NULL, NULL, NULL, 1, 1, LR_BLOCK_LITERAL | LR_BREAKS, 0, 0, 0,
    "__enumw_inject1", "builtins/enumerator" },
  { LR_ENUM_WALK, "each_slice", LR_RECV_SOME, NULL, NULL, NULL, 1, 1, LR_BLOCK_LITERAL | LR_BREAKS, 0, 0, 0,
    "__enumw_each_slice", "builtins/enumerator" },
  { LR_ENUM_WALK, "each_cons", LR_RECV_SOME, NULL, NULL, NULL, 1, 1, LR_BLOCK_LITERAL | LR_BREAKS, 0, 0, 0,
    "__enumw_each_cons", "builtins/enumerator" },
  { LR_ENUM_WALK, "each_entry", LR_RECV_SOME, NULL, NULL, NULL, 0, 0, LR_BLOCK_LITERAL | LR_BREAKS, 0, 0, 0,
    "__enumw_each_entry", "builtins/enumerator" },
  /* `arr.map.with_index { }` is map's: only an Enumerator that walks its
     source (each, cycle, a generator) is a plain walk with a counter */
  { LR_ENUM_WALK, "with_index", LR_RECV_SOME, NULL, NULL, lr_walk_sources, 0, 1, LR_BLOCK_LITERAL | LR_BREAKS,
    0, 0, 0, "__enumw_with_index", "builtins/enumerator" },
  /* a String's piece walk with an index, the String no variable or the block breaking */
  { LR_STR_WALK, "with_index", LR_RECV_CALL, "each_char", NULL, NULL, 0, 1, LR_BLOCK_LITERAL | LR_NO_SPLAT,
    0, 0, LR_NO_BLOCK | LR_HAS_RECV | LR_NO_ARGS, "__enumw_str_chars_with_index", "builtins/enumerator" },
  { LR_STR_WALK, "with_index", LR_RECV_CALL, "each_line", NULL, NULL, 0, 1, LR_BLOCK_LITERAL | LR_NO_SPLAT,
    0, 0, LR_NO_BLOCK | LR_HAS_RECV | LR_NO_ARGS, "__enumw_str_lines_with_index", "builtins/enumerator" },
  { LR_STR_WALK, "with_index", LR_RECV_CALL, "each_byte", NULL, NULL, 0, 1, LR_BLOCK_LITERAL | LR_NO_SPLAT,
    0, 0, LR_NO_BLOCK | LR_HAS_RECV | LR_NO_ARGS, "__enumw_str_bytes_with_index", "builtins/enumerator" },
  { LR_STR_WALK, "with_index", LR_RECV_CALL, "each_codepoint", NULL, NULL, 0, 1, LR_BLOCK_LITERAL | LR_NO_SPLAT,
    0, 0, LR_NO_BLOCK | LR_HAS_RECV | LR_NO_ARGS, "__enumw_str_codepoints_with_index", "builtins/enumerator" },
  /* Enumerable's other names for a definition builtins/enumerable.rb has */
  { LR_ENUM_NAME, "collect_concat", LR_RECV_ANY, NULL, NULL, NULL, 0, -1, 0, 0, 0, 0, "flat_map", "builtins/enumerable" },
  { LR_ENUM_NAME, "detect", LR_RECV_ANY, NULL, NULL, NULL, 0, -1, 0, 0, 0, 0, "find", "builtins/enumerable" },
  /* Array#index with a block and no argument is find_index */
  { LR_INDEX_BLOCK, "index", LR_RECV_SOME, NULL, NULL, NULL, 0, 0, LR_BLOCK | LR_NO_ARGS, 0, 0, 0,
    "find_index", "builtins/enumerable" },
  /* a stored Enumerator's with_object(memo) { } is each_with_object */
  { LR_WITH_OBJECT, "with_object", LR_RECV_SOME, NULL, NULL, NULL, 1, 1, LR_BLOCK, 0, 0, 0,
    "each_with_object", "builtins/enumerable" },
  /* to_set is Set.new(recv.to_a) */
  { LR_TO_SET, "to_set", LR_RECV_SOME, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, "Set", "set" },
};
#define LOWER_ROW_N ((int)(sizeof lower_rows / sizeof lower_rows[0]))

const LowerRow *lower_row_at(int i) {
  return i >= 0 && i < LOWER_ROW_N ? &lower_rows[i] : NULL;
}
const LowerRow *lower_row_of(int family) {
  for (int i = 0; i < LOWER_ROW_N; i++) if (lower_rows[i].family == family) return &lower_rows[i];
  return NULL;
}
const LowerRow *lower_row_named(int family, const char *name) {
  if (!name) return NULL;
  for (int i = 0; i < LOWER_ROW_N; i++)
    if (lower_rows[i].family == family && sp_streq(lower_rows[i].name, name)) return &lower_rows[i];
  return NULL;
}

/* The pre-installed packages' facts, from their manifests (package_facts.inc) */
#include "package_facts.inc"
#define SP_PACKAGE_FACT_ROW(pkg, feature, boot, rides) { pkg, feature, boot, rides },
static const PackageFact package_facts[] = { SP_PACKAGE_FACTS(SP_PACKAGE_FACT_ROW) };
#undef SP_PACKAGE_FACT_ROW
const PackageFact *package_fact_at(int i) {
  return i >= 0 && i < (int)(sizeof package_facts / sizeof package_facts[0]) ? &package_facts[i] : NULL;
}

/* One table for the builtin class/module/exception names. These four
   predicates were four independent lists that had to be kept in step by hand,
   and the code has a comment noting where they already disagree. A row now
   carries every fact about one name: the flags say which predicate accepts it,
   and `id` is its negative cls_id (0 when codegen has none). Adding a name is
   one row instead of four edits.

   The rows are exactly the union of the old lists, flags included, so every
   predicate answers what it answered before -- what the table changes is that
   the answers can no longer drift apart. It also makes the existing gaps
   visible rather than implicit: six names are classes with no cls_id
   (Encoding, GC, Method, ObjectSpace, Process, UnboundMethod), seven have a
   cls_id but are not bare constants (the Thread:: / Process:: / Math::
   qualified forms), and fourteen exception names have no id. Each is a
   deliberate answer today; closing any of them is a behavior change and does
   not belong in the same commit as the table.

   The `CRUBY_KNOWN` list in codegen_call.c is a fifth list, for a different
   question (names CRuby defines that spinel does not implement), and is left
   alone here. */
#define X(k, n, id, flags) { n, id, flags },
static const BuiltinClass BUILTIN_CLASSES[] = { SP_BUILTIN_CLASSES(X) };
#undef X
#define BUILTIN_CLASS_N ((int)(sizeof BUILTIN_CLASSES / sizeof BUILTIN_CLASSES[0]))

/* The i-th row, NULL past the last. The socket rows are a constant only
   after `require "socket"`: the analysis's predicates ask that themselves. */
const BuiltinClass *builtin_class_at(int i) {
  return i >= 0 && i < BUILTIN_CLASS_N ? &BUILTIN_CLASSES[i] : NULL;
}
/* The core class of a receiver kind; each reader keeps its own kind filter. */
int core_of_type(TyKind t) {
  if (t == TY_INT || t == TY_BIGINT) return CORE_INTEGER;
  if (t == TY_FLOAT) return CORE_FLOAT;
  if (t == TY_STRING || t == TY_STRBUF) return CORE_STRING;
  if (t == TY_SYMBOL) return CORE_SYMBOL;
  if (t == TY_RANGE || t == TY_FLOAT_RANGE || t == TY_STR_RANGE) return CORE_RANGE;
  if (t == TY_TIME) return CORE_TIME;
  if (t == TY_IO) return CORE_FILE;
  if (t == TY_CLASS) return CORE_CLASS;
  if (t == TY_NIL) return CORE_NILCLASS;
  if (t == TY_MATCHDATA) return CORE_MATCHDATA;
  if (t == TY_REGEX) return CORE_REGEXP;
  if (t == TY_THREAD) return CORE_THREAD;
  if (t == TY_FIBER) return CORE_FIBER;
  if (t == TY_RANDOM) return CORE_RANDOM;
  if (ty_is_array(t) || ty_is_obj_array(t)) return CORE_ARRAY;
  if (ty_is_hash(t)) return CORE_HASH;
  if (t == TY_ENUMERATOR) return CORE_ENUMERATOR;
  if (t == TY_RATIONAL) return CORE_RATIONAL;
  if (t == TY_COMPLEX) return CORE_COMPLEX;
  if (t == TY_PROC) return CORE_PROC;
  if (t == TY_QUEUE) return CORE_QUEUE;
  if (t == TY_EXCEPTION) return CORE_EXCEPTION;
  return -1;
}
/* The spelling held by a core row, including the library and synthetic slots. */
const char *core_class_name(int k) {
  const BuiltinClass *r = builtin_class_at(k);
  if (r) return r->name;
  switch (k) {
  case CORE_STRINGIO: return "StringIO";
  case CORE_SET: return "Set";
  case CORE_CLASS_REOPEN: return CLASS_REOPEN_NAME;
  case CORE_FILETEST: return "FileTest";
  case CORE_ERRNO: return "Errno";
  case CORE_WARNING: return "Warning";
  default: return NULL;
  }
}
/* Is `n` a class or module Spinel has natively (any row, the socket ones
   whether or not "socket" is required)? The load graph asks it of a
   library's `class X`: a native X is reopened, and the methods the library
   adds to it are what a program's names reach. */
int is_native_class_name(const char *n) {
  if (!n) return 0;
  for (int i = 0; i < BUILTIN_CLASS_N; i++) if (sp_streq(n, BUILTIN_CLASSES[i].name)) return 1;
  return 0;
}
/* Is the builtin class of cls_id `id` (class_builtin_superclass's answer) one
   whose subclass in a program has no method of an Enumerable walk from it:
   Object, BasicObject, Data, or an exception? Such a class answers the walk
   only through its own chain. */
int is_plain_builtin_superclass(int id) {
  for (int i = 0; id != 0 && i < BUILTIN_CLASS_N; i++)
    if (BUILTIN_CLASSES[i].id == id) return (BUILTIN_CLASSES[i].flags & (BC_PLAIN | BC_EXCEPTION)) != 0;
  return 0;
}

/* ---- the compile-time evaluation and its questions ---- */
int is_class_const_name(const char *n) { return n && sp_streq(n, "Class"); }
int is_hash_read_only(const char *n) {
  static const char *const R[] = { "each", "each_pair", "[]", "fetch", "key?", "has_key?", "include?",
    "member?", "key", "keys", "values", "size", "length", "empty?", "dig", "to_a", "frozen?", "freeze",
    "each_key", "each_value", "values_at", "has_value?", "value?", "count", "any?", "all?", "none?",
    "find", "detect", "map", "collect", "select", "filter", "reject", "min_by", "max_by", "sort_by",
    "group_by", "sum", "inject", "reduce", "to_h", "invert", "sort", "first", "inspect", "to_s", "hash", "==", NULL };
  for (int i = 0; n && R[i]; i++) if (sp_streq(n, R[i])) return 1;
  return 0;
}
int is_to_a_name(const char *n) { return n && sp_streq(n, "to_a"); }
int is_hash_self_answer(const char *n) {
  return n && (sp_streq(n, "each") || sp_streq(n, "each_pair") || sp_streq(n, "each_key") ||
               sp_streq(n, "each_value") || sp_streq(n, "freeze") || sp_streq(n, "to_h"));
}
int is_core_change_call(const char *n) {
  return n && (is_class_eval_family(n) || sp_streq(n, "prepend") || sp_streq(n, "include") ||
               sp_streq(n, "define_method") || is_send_family(n) || sp_streq(n, "alias_method") ||
               sp_streq(n, "remove_method") || sp_streq(n, "undef_method"));
}
int is_singleton_reach_call(const char *n) {
  return n && (sp_streq(n, "define_singleton_method") || sp_streq(n, "extend") ||
               sp_streq(n, "singleton_class") || is_instance_eval_family(n));
}
int is_method_naming_call(const char *n) {
  return n && (sp_streq(n, "define_method") || sp_streq(n, "define_singleton_method") ||
               sp_streq(n, "alias_method") || sp_streq(n, "remove_method") || sp_streq(n, "undef_method") ||
               sp_streq(n, "attr") || is_attr_reader_family(n) || is_attr_writer_family(n));
}
/* The attribute declarations (`attribute`/`attributes` are Spinel's own
   compile-time class-body declarations, accessors as attr_accessor's) */
int is_attr_decl_reader(const char *n) {
  return n && (sp_streq(n, "attr") || is_attr_reader_family(n) || sp_streq(n, "attribute") || sp_streq(n, "attributes"));
}
int is_attr_decl_writer(const char *n) {
  return n && (is_attr_writer_family(n) || sp_streq(n, "attribute") || sp_streq(n, "attributes"));
}
int class_method_vis_of_name(const char *n) {
  return !n ? 0 : sp_streq(n, "private_class_method") ? BVIS_PRIVATE : sp_streq(n, "public_class_method") ? BVIS_PUBLIC : 0;
}
int is_refine_name(const char *n) { return n && sp_streq(n, "refine"); }
/* The compiler's own enumerable helpers, which a receiverless call names the way
   a Kernel function is named */
int is_enum_helper_name(const char *n) {
  return n && (sp_streq(n, "__enum_pairs") || sp_streq(n, "__enum_count_int"));
}
/* The calls by which a method body changes what the load record holds:
   ones that define, undefine or set methods, constants or variables, mix
   a module in or refine, load a file, or run code it cannot follow from
   the call (a send, an eval, a block or proc called) */
int is_load_effect_call(const char *n) {
  return n && (visibility_of_name(n) || is_method_naming_call(n) || is_mixin_call(n) || is_refine_name(n) ||
               is_using_name(n) || is_code_eval_name(n) || is_eval_exec_family(n) || is_rebinding_reach_call(n) ||
               is_const_set_name(n) || is_remove_const_name(n) || is_const_visibility_name(n) || is_cvar_write_name(n) ||
               is_ivar_remove_name(n) || is_file_load_name(n));
}
int is_using_name(const char *n) { return n && sp_streq(n, "using"); }
int is_remove_const_name(const char *n) { return n && sp_streq(n, "remove_const"); }
/* private_constant public_constant deprecate_constant */
int is_const_visibility_name(const char *n) {
  static const char *const names[] = { "private_constant", "public_constant", "deprecate_constant", NULL };
  return n && builtin_name_in(n, names);
}
/* class_variable_set remove_class_variable */
int is_cvar_write_name(const char *n) { return n && (sp_streq(n, "class_variable_set") || sp_streq(n, "remove_class_variable")); }
int is_hook_protocol_name(const char *n) {
  return n && (sp_streq(n, "append_features") || sp_streq(n, "extend_object") || sp_streq(n, "prepend_features"));
}

int is_port_gvar_name(const char *n) {
  return n && (sp_streq(n, "$stdout") || sp_streq(n, "$stderr") || sp_streq(n, "$>"));
}

int synth_stands_for(const char *name, int i, const char **proto) {
  const char *stands = NULL;
  if (!name || i < 0) return 0;
  if (sp_streq(name, "__noarg to_s")) stands = i == 0 ? "to_s" : NULL;
  else if (sp_streq(name, "__noarg inspect")) stands = i == 0 ? "inspect" : NULL;
  else if (sp_streq(name, "__enum_to_a")) stands = i == 0 ? "each" : i == 1 ? "to_a" : i == 2 ? "deconstruct" : NULL;
  if (stands && proto) *proto = stands;
  return stands != NULL;
}
int is_method_list_call(const char *n) {
  return n && (sp_streq(n, "attr") || is_attr_reader_family(n) || is_attr_writer_family(n) ||
               sp_streq(n, "remove_method") || sp_streq(n, "undef_method"));
}

/* The standard library's top-level names: a program that requires one
   reaches it from anywhere */
int is_stdlib_const_name(const char *n) {
  static const char *const names[] = {
    "Abbrev", "Addrinfo", "Base64", "BasicSocket", "Benchmark", "BigDecimal", "CGI", "CSV", "Coverage",
    "DRb", "Date", "DateTime", "DelegateClass", "Delegator", "Digest", "ERB", "English", "Etc", "FFI",
    "Fcntl", "Fiddle", "FileUtils", "Find", "Forwardable", "Gem", "GetoptLong", "IPAddr", "IPSocket",
    "JSON", "Logger", "Matrix", "Minitest", "Monitor", "MonitorMixin", "Net", "Observable", "Open3",
    "OpenSSL", "OpenStruct", "OptionParser", "PP", "PStore", "Prime", "Psych", "RbConfig", "Readline",
    "Resolv", "Ripper", "SecureRandom", "Shellwords", "SimpleDelegator", "SingleForwardable", "Singleton",
    "Socket", "StringIO", "StringScanner", "TCPServer", "TCPSocket", "TSort", "Tempfile", "Test", "Timeout",
    "UDPSocket", "UNIXServer", "UNIXSocket", "URI", "Vector", "WeakRef", "YAML", "Zlib",
    NULL
  };
  for (int i = 0; names[i]; i++) if (sp_streq(names[i], n)) return 1;
  return 0;
}

/* A name CRuby (or a library spinel provides) answers at the top level. */
int is_toplevel_const_name(const char *n) {
  static const char *const names[] = {
    "ARGF", "ARGV", "ArgumentError", "Array", "BasicObject", "Binding", "Class", "ClosedQueueError",
    "Comparable", "Complex", "ConditionVariable", "Data", "Dir", "ENV", "EOFError", "Encoding",
    "EncodingError", "Enumerable", "Enumerator", "Errno", "Exception", "FalseClass", "Fiber", "FiberError",
    "File", "FileTest", "Float", "FloatDomainError", "FrozenError", "GC", "Hash", "IO",
    "IOError", "IndexError", "Integer", "Interrupt", "Kernel", "KeyError", "LoadError", "LocalJumpError",
    "Marshal", "MatchData", "Math", "Method", "Module", "Mutex", "NameError", "NilClass",
    "NoMatchingPatternError", "NoMatchingPatternKeyError", "NoMemoryError", "NoMethodError", "NotImplementedError", "Numeric", "Object", "ObjectSpace",
    "Pathname", "Proc", "Process", "Queue", "RUBY_COPYRIGHT", "RUBY_DESCRIPTION", "RUBY_ENGINE", "RUBY_ENGINE_VERSION",
    "RUBY_PATCHLEVEL", "RUBY_PLATFORM", "RUBY_RELEASE_DATE", "RUBY_REVISION", "RUBY_VERSION", "Ractor", "Random", "Range",
    "RangeError", "Rational", "Refinement", "Regexp", "RegexpError", "Ruby", "RubyVM", "RuntimeError",
    "STDERR", "STDIN", "STDOUT", "ScriptError", "SecurityError", "Set", "Signal", "SignalException",
    "SizedQueue", "StandardError", "StopIteration", "String", "Struct", "Symbol", "SyntaxError", "SystemCallError",
    "SystemExit", "SystemStackError", "TOPLEVEL_BINDING", "Thread", "ThreadError", "ThreadGroup", "Time", "TracePoint",
    "TrueClass", "TypeError", "UnboundMethod", "UncaughtThrowError", "UnicodeNormalize", "Warning", "ZeroDivisionError",
    NULL
  };
  if (!n) return 1;
  for (int i = 0; names[i]; i++) if (sp_streq(names[i], n)) return 1;
  if (is_stdlib_const_name(n)) return 1;
  return builtin_class_id(n) != 0 || is_builtin_class_name(n) || is_builtin_module_name(n) ||
         is_builtin_exception_name(n);
}

/* A builtin ancestor with no constants of its own (CRuby 4.0's
   `K.constants - Object.constants` is empty): the user's reopenings are all
   it can contribute. Any other builtin ancestor may answer the name itself. */
int is_constless_builtin_name(const char *n) {
  static const char *const names[] = {
    "Object", "BasicObject", "Kernel", "Comparable", "Enumerable", "Array", "Hash", "String",
    "Integer", "Numeric", "Struct", "Data", "Symbol", "Proc", "Range", "Module", "Class", "Time",
    "Rational", "Dir", "Fiber", "Mutex", "Queue", "SizedQueue", "ConditionVariable", "MatchData",
    "Method", "UnboundMethod", "NilClass", "TrueClass", "FalseClass", "Signal", "Warning", NULL
  };
  for (int i = 0; names[i]; i++) if (sp_streq(names[i], n)) return 1;
  return is_builtin_exception_name(n) && !strchr(n, ':');
}
