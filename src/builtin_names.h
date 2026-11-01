/* builtin_names.h -- families of builtin method, class, and global names,
   plus numeric literal tags, that the compiler asks about in many places:
   `[] () call`, `is_a? kind_of? instance_of?`,
   ... Each predicate answers whether a name is one of its family; the
   family's names are listed once, in builtin_names.c, where every site that
   spelled the chain out now asks. A predicate compares the name with each of
   its family in turn, as the chains did (sp_streq counts the work).
   Near-families (a set one name larger or smaller) are
   different questions and keep their own spelling. */
#ifndef SPINEL_BUILTIN_NAMES_H
#define SPINEL_BUILTIN_NAMES_H

#include "types.h"

int is_zip_name(const char *n);       /* zip: tuple-yielding iteration */
int is_dig_name(const char *n);       /* dig: a path through containers */
int is_call_alias(const char *n);     /* call () []: a Proc/Method's invocation */
int is_method_invoke(const char *n);  /* call () [] ===: Method invocation */
int is_bind_call(const char *n);  /* bind_call: an UnboundMethod bound and called in one */
int is_kind_query(const char *n);     /* is_a? kind_of? instance_of? */
int is_instance_of_query(const char *n);   /* instance_of? */
int is_case_eq(const char *n);        /* === */
int is_round_family(const char *n);   /* round ceil floor truncate */
int is_push_alias(const char *n);     /* push << append */
int is_bit_op(const char *n);         /* & | ^ */
int is_basic_arith(const char *n);    /* + - * / (is_arith_op adds % and **) */
int is_int_arith_op(const char *n);   /* + - * / %: an Integer's arithmetic that answers an Integer */
int is_add_sub_mul(const char *n);    /* + - * */
int is_int_bit_op(const char *n);     /* & | ^ << >>: Integer's bitwise operators */
int is_embedding_builtin(const char *nm); /* Array Hash String: subclass instances embed the builtin */
int is_class_name_name(const char *n);   /* name */
int is_inspect_name(const char *n);       /* inspect */
int is_object_class_name(const char *n);   /* Object alone: the top level's constants */
int is_object_root(const char *n);    /* Object Kernel BasicObject: the classes every object has */
int is_send_family(const char *n);    /* send __send__ public_send */
int is_method_name_reflection(const char *n); /* send family, method public_method instance_method define_method define_singleton_method alias_method singleton_method respond_to? method_defined? */
int is_code_eval_name(const char *n);  /* eval class_eval module_eval instance_eval: given its code as a String */
int is_symbol_conversion(const char *n); /* to_sym intern: a Symbol made of a value */
int is_builtin_helper_name(const char *n); /* __x but not __x__: a builtins/ file's own helper */
int is_opaque_reaching_call(const char *n); /* send family, call, new, lambda/proc, freeze, eval, instance_/class_/module_*, *method* */
int is_async_code_entry(const char *recv, const char *n); /* Thread.new/start/fork, Fiber.new, trap, Signal.trap */
int is_deferred_block_call(const char *recv, const char *n); /* proc, Proc.new, define_method, at_exit, Enumerator.new, ...: the block runs later */
int is_name_reader(const char *n);    /* name to_s inspect: a Class's or Module's name */
int is_plain_builtin_reader(const char *n); /* name: every builtin with it reads a value */
int is_tap_alias(const char *n);      /* tap then yield_self */
int is_tap_name(const char *n);       /* tap */
int is_quantifier(const char *n);     /* all? any? none? one? */
int is_set_op(const char *n);         /* & intersection | union - difference */
int is_combination_family(const char *n);  /* combination permutation repeated_combination repeated_permutation */
/* the visibility word a call names: module_function first, so that a
   method visibility is `visibility_of_name(n) > BVIS_MODULE_FUNCTION` */
enum { BVIS_MODULE_FUNCTION = 1, BVIS_PUBLIC, BVIS_PRIVATE, BVIS_PROTECTED };
int visibility_of_name(const char *n);    /* module_function public private protected: BVIS_*, else 0 */
int is_select_bang(const char *n);    /* select! filter! keep_if reject! delete_if: the in-place filters */
int is_each_name(const char *n); /* each */
int is_each_entry(const char *n); /* each_entry */
int is_find_index(const char *n); /* find_index */
int is_range_member_walk(const char *n); /* each each_entry step: a Range's block walks of its members */
int is_stop_walk(const char *n); /* find detect take_while find_index each_with_index: walks that stop at the block's answer */
int is_each_walk(const char *n);      /* each each_entry reverse_each */
int is_index_query(const char *n);    /* find_index index rindex */
int is_itself_name(const char *n);    /* itself */
int is_each_char_name(const char *n);  /* each_char */
int is_self_copy(const char *n);      /* freeze dup clone itself: the receiver, or a copy of it */
int is_step_call(const char *n);       /* step: Numeric#step and Range#step */
int is_int_step(const char *n);       /* times upto downto: Integer's counting iterators */
int is_equality_name(const char *n);  /* equal? eql? == */
int is_bits_query(const char *n);     /* allbits? anybits? nobits? */
int is_count_alias(const char *n);    /* length size count */
int is_class_eval_family(const char *n);  /* class_eval module_eval class_exec module_exec */
int is_class_eval_text_name(const char *n);  /* class_eval module_eval: run a String as the body's code */
int is_eval_frame_reader(const char *n);  /* eval and exec forms, binding, __dir__: read an eval's frame */
int is_eval_exec_family(const char *n);   /* class/module/instance eval and exec */
int is_key_query(const char *n);      /* key? has_key? include? member?: Hash/ENV membership aliases */
int is_hash_key_lookup(const char *n); /* [] fetch delete and is_key_query: a Hash call that only compares its key */
int is_receiver_conversion(const char *n); /* to_s to_str itself: conversions a String answers with itself */
int is_string_conversion(const char *n);   /* to_s to_str +@: conversions that turn a nil receiver into a String */
int is_string_self_answer(const char *n);   /* to_s to_str itself +@: names a String answers itself to */
int is_rebinding_reach_call(const char *n); /* call () send instance_variable_set instance_eval yield...: code the walk cannot see */
int is_to_s_name(const char *n);
int is_message_name(const char *n);
int is_range_membership(const char *n); /* cover? include? member? ===: Range membership predicates */
int is_each_walk_or_with_index(const char *n); /* each each_entry reverse_each each_with_index */
int is_call_or_yield(const char *n);  /* call () [] yield: is_call_alias's names and yield */
int is_proc_invoke(const char *n);    /* call () [] yield ===: every name that invokes a Proc */
int is_quantifier_or_count(const char *n);  /* all? any? none? one? count: is_quantifier's names and count */
int is_push_unshift(const char *n);   /* << push append unshift: is_push_alias's names and unshift */
int is_identity_query(const char *n); /* equal? object_id __id__ frozen?: tells an object from its copy */
int is_len_alias(const char *n);      /* length size */
int is_length_spelling(const char *n); /* size count empty? */
int is_difference_spelling(const char *n); /* difference */
int is_str_each_iter(const char *n);  /* each_char each_line each_byte each_codepoint: String's element iterators */
int is_freeze_name(const char *n);    /* freeze */
int is_str_string_yield(const char *n); /* each_char each_line upto chars lines split scrub: String methods whose block takes a String */
int is_unpack_name(const char *n); /* unpack: a String decoded into values, which a block takes one by one */
int is_catch_name(const char *n); /* catch: a tagged non-local jump target */
int is_throw_name(const char *n); /* throw: a tagged non-local jump */
int is_diverging_call(const char *n); /* raise fail throw exit exit! abort: a Kernel call that never returns */
int is_block_loop_method(const char *n); /* times each upto downto step loop each_with_index: a block run an unbounded number of times */

int is_each_window(const char *n); /* each_cons each_slice: consecutive or disjoint element windows */
int is_sum_name(const char *n);
int is_reduce_alias(const char *n); /* inject reduce: Enumerable reduction aliases */
int is_minmax_query(const char *n); /* min max: extrema queries */
int is_minmax_pair(const char *n); /* minmax: both extrema in one answer */
int is_endpoint_query(const char *n); /* first last: collection or Range endpoints */
int is_map_alias(const char *n); /* map collect: Enumerable transformation aliases */
int is_instance_eval_family(const char *n); /* instance_eval instance_exec */
int is_reflective_access(const char *n); /* instance_variable_get/set, class_variable_get/set, const_get/set, remove_* */
int is_reflective_listing(const char *n); /* instance_variables class_variables constants */
int is_local_reflection(const char *n); /* eval binding local_variable_get/set local_variables */
int is_block_given_name(const char *n); /* block_given? */
int is_find_alias(const char *n); /* find detect: Enumerable search aliases */

int is_text_conversion(const char *n); /* inspect to_s */
int is_slice_alias(const char *n); /* [] slice */
int is_seq_index_call(const char *n); /* [] []= slice slice! byteslice: an Array or String index */
int is_shift_op(const char *n); /* << >> */
int is_copy_alias(const char *n); /* clone dup */
int is_hash_merge_bang(const char *n); /* merge! update */
int is_membership_alias(const char *n); /* include? member? */
int is_eql_or_equal(const char *n); /* eql? equal? */
int is_equal_name(const char *n);   /* equal? */
int is_substitution(const char *n); /* gsub sub */
int is_substitution_bang(const char *n); /* gsub! sub! */
int is_element_at_alias(const char *n); /* [] at */
int is_map_bang_alias(const char *n); /* collect! map! */
int is_each_or_pair(const char *n); /* each each_pair */
int is_proc_constructor(const char *n); /* lambda proc */
int is_lambda_constructor(const char *n); /* lambda */

int is_inspect_print(const char *n); /* p pp */
int is_then_alias(const char *n); /* then yield_self */
int is_intersection_alias(const char *n); /* & intersection */
int is_add_sub(const char *n); /* + - */
int is_store_alias(const char *n); /* []= store */
int is_hash_default_setter(const char *n); /* default= */
int is_pop_shift(const char *n); /* pop shift */
int is_prepend_alias(const char *n); /* prepend unshift */
int is_text_print(const char *n); /* print puts */
int is_printf_name(const char *n); /* printf: formats its operands, then writes them */
int is_union_alias(const char *n); /* union | */
int is_eq_or_ne(const char *n); /* != == */
int is_size_or_count(const char *n); /* count size */
int is_bounded_int_step(const char *n); /* downto upto */
int is_upto_name(const char *n);      /* upto */

int is_indexed_each(const char *n); /* each_index each_with_index */
int is_to_array_alias(const char *n); /* entries to_a */
int is_match_p_name(const char *n);   /* match? */
int is_record_class_builder(const char *recv, const char *meth); /* Struct.new, Data.define */
int is_class_new(const char *recv, const char *meth); /* Class.new */
int is_module_new(const char *recv, const char *meth); /* Module.new */
int is_class_builder(const char *recv, const char *meth); /* Struct.new, Data.define, Class.new, Module.new */
int is_value_constructor(const char *recv, const char *meth); /* Hash.new, Array.new, String.new, Object.new */
int is_array_new(const char *recv, const char *meth); /* Array.new */
int is_array_class_name(const char *n); /* Array */
int is_string_index(const char *n); /* index rindex */
int is_modulo_alias(const char *n); /* % modulo */
int is_append_concat(const char *n); /* << concat */
int is_socket_address(const char *n); /* addr peeraddr */
int is_attr_writer_family(const char *n); /* attr_accessor attr_writer */
int is_take_drop(const char *n); /* drop take */
int is_byte_codepoint_each(const char *n); /* each_byte each_codepoint */
int is_with_index_alias(const char *n); /* each_with_index with_index */
int is_freeze_family(const char *n); /* freeze frozen? */
int is_bivar_access(const char *n);  /* __bivar_get __bivar_set __bivar_defined */
int is_ivar_reflection_name(const char *n); /* instance_variable_get/set/defined?, instance_variables, remove_instance_variable */
int is_object_copy(const char *n);   /* dup clone */
int is_ivar_set_name(const char *n); /* instance_variable_set */
int is_marshal_dump(const char *recv, const char *meth); /* Marshal.dump */
int is_marshal_load(const char *recv, const char *meth); /* Marshal.load */
int is_object_loader(const char *recv, const char *meth, int argc); /* Marshal.load/restore, YAML/Psych.load family, JSON.load, JSON.parse with options, ObjectSpace._id2ref */
int is_method_bind_name(const char *n); /* bind bind_call */
int is_timeout_call(const char *recv, const char *meth); /* Timeout.timeout(sec, klass) */
int is_exception_ctor_name(const char *n); /* exception */
int is_enumerator_maker(const char *n); /* to_enum enum_for */
int is_allocate_name(const char *n); /* allocate */
int is_ivar_remove_name(const char *n); /* remove_instance_variable */
int is_ivar_presence_read(const char *n); /* instance_variables instance_variable_defined? remove_instance_variable inspect p pp */
int is_bivar_keyed_class(const char *n);  /* Array Hash Random */
int is_string_class_name(const char *n);   /* String */
int is_frozen_value_class(const char *n); /* Integer Float Symbol NilClass TrueClass FalseClass Range */
int is_nonblock_io(const char *n); /* read_nonblock write_nonblock */
int is_io_read(const char *n); /* read */

int is_mul_or_pow(const char *n); /* * ** */
int is_unary_sign(const char *n); /* +@ -@ */
int is_unary_minus(const char *n); /* -@ */
int is_casecmp_family(const char *n); /* casecmp casecmp? */
/* String#to_i([base]), #hex, #oct: the parses promote mode reads as a
   Bignum past sp_int */
int str_int_promotes(const char *n, int argc);
int is_hash_key_value_each(const char *n); /* each_key each_value */
int is_encoding_mutator(const char *n); /* encode! force_encoding */
/* the most arguments the arms serve: force_encoding's one (two read as a
   refusal), encode!'s two encodings and its option hash */
int encoding_mutator_max_argc(const char *n);
int is_range_end_reader(const char *n); /* end last */
int is_raise_alias(const char *n); /* fail raise */
int is_unary_plus(const char *n); /* +@ */
int is_loop_name(const char *n); /* loop */
int is_first_or_take(const char *n); /* first take */
int is_lazy_force(const char *n); /* force to_a */
int is_local_time(const char *n); /* getlocal localtime */
int is_line_read(const char *n); /* gets readline */
int is_open_constructor(const char *n); /* new open */

int is_succ_alias(const char *n); /* next succ */
int is_path_reader(const char *n); /* path to_path */
int is_io_position(const char *n); /* pos tell */
int is_rewind_name(const char *n); /* rewind: an Enumerator's restart, or a stream's seek to its start */
int is_io_offset_move(const char *n); /* pos= sysseek: the descriptor-control calls (boxed_desc_control_arity) whose first argument is an offset, NUM2OFFT-converted */
int is_sort_family(const char *n); /* sort sort! */
int is_hash_transform(const char *n); /* transform_values transform_keys */
int is_fallback_block_call(const char *n); /* fetch delete fetch_values: the block is the fallback */
int is_delete_name(const char *n);       /* delete: removal by key (ENV's missing-key block answers) */
int is_io_write(const char *n); /* syswrite write */
int is_to_integer(const char *n); /* to_i to_int */
int is_match_operator(const char *n); /* !~ =~ */
int is_object_receiver_handoff(const char *n); /* to_enum enum_for instance_eval instance_exec method public_method */
int is_div_or_mod(const char *n); /* % / */
int is_div_or_modulo(const char *n); /* div modulo: the named floored quotient and remainder */
int is_div_name(const char *n); /* div: the named floored quotient */
int is_divmod_name(const char *n); /* divmod: the [quotient, modulo] pair */
int is_modulo_name(const char *n); /* modulo: the named method, not the % operator */
int is_mod_operator(const char *n); /* %: the operator */
int is_add_or_mul(const char *n); /* * + */
int is_push_operator(const char *n); /* << push */
int is_eq_or_eql(const char *n); /* == eql? */
int is_element_access(const char *n); /* [] []= */
int is_index_assign(const char *n); /* []= */
int is_fill_name(const char *n);        /* fill */

int is_current_method(const char *n); /* __callee__ __method__ */
int is_hash_constructor(const char *n); /* __hash_new_default new */
int is_struct_constructor(const char *n); /* new [] */
int is_attr_reader_family(const char *n); /* attr_accessor attr_reader */
int is_range_bound_reader(const char *n); /* begin end */
int is_directory_entries(const char *n); /* children entries */
int is_exception_full_message(const char *n); /* detailed_message full_message */
int is_each_or_index(const char *n); /* each each_with_index */
int is_with_object_alias(const char *n); /* each_with_object with_object */
int is_exist_alias(const char *n); /* exist? exists? */
int is_select_alias(const char *n); /* filter select */
int is_format_alias(const char *n); /* format sprintf */
int is_initialize_name(const char *n); /* initialize */
int is_initialize_family(const char *n); /* initialize initialize_copy */

int is_iso8601_alias(const char *n); /* iso8601 xmlschema */
int is_rfc2822_alias(const char *n);   /* rfc2822 rfc822 */
int is_time_native_format(const char *n);   /* iso8601 xmlschema httpdate rfc2822 rfc822: Time#x and Time.x the runtime answers once "time" is required */
int is_time_parser_name(const char *n);   /* parse strptime and the native formats: Time's class-method parsers */
int sock_const_always(const char *n); /* Socket constants every platform defines */
int is_exception_message(const char *n); /* message to_s */
int is_socket_pair_alias(const char *n); /* pair socketpair */
int is_partition_family(const char *n); /* partition rpartition */
int is_to_rational(const char *n); /* rationalize to_r */
int is_rectangular_alias(const char *n); /* rect rectangular */
int is_numeric_conversion(const char *n); /* to_f to_i */
int is_remainder_family(const char *n); /* % modulo remainder */
int is_select_reject(const char *n); /* filter reject select */
int is_bit_set_operator(const char *n); /* & - | */
int is_find_or_take_while(const char *n); /* detect find take_while */
int is_named_set_operator(const char *n); /* difference intersection union */

int is_select_reject_bang(const char *n); /* filter! reject! select! */
int is_element_pick(const char *n); /* first last sample */
int is_extrema_family(const char *n); /* max min minmax */
int is_io_wait(const char *n); /* wait_priority wait_readable wait_writable */
int is_match_family(const char *n); /* !~ =~ match match? */
int is_integer_iteration(const char *n); /* downto step times upto */
int is_mixin_call(const char *n); /* include extend prepend */
/* The calls on a class's constant or self that answer neither the class nor
   a block of its own (comp_class_may_be_value): new allocate name to_s
   inspect === == ... respond_to? attr_* visibility. Not include, extend,
   prepend, private_constant, private_class_method and public_class_method:
   those answer their receiver. */
int is_class_compare(const char *n);  /* == != equal? eql? */
int is_class_plain_query(const char *n);
int is_class_self_answer(const char *n); /* include extend prepend private_class_method public_class_method private_constant public_constant remove_method undef_method: answer their receiver */
int is_self_rebinding_call(const char *n); /* instance_/class_/module_ eval and exec, define_singleton_method */
int is_class_relatives_query(const char *n); /* ancestors superclass subclasses descendants included_modules */
int is_class_query(const char *n); /* class */
int is_method_owner_query(const char *n); /* owner */
int is_instance_method_name(const char *n); /* instance_method public_instance_method */
int is_method_object_maker(const char *n); /* method public_method singleton_method instance_method public_instance_method */
/* const_get const_source_location nesting attached_object each_object eval binding */
int is_constant_or_object_reflection(const char *n);
int is_const_set_name(const char *n); /* const_set */
int is_const_query_name(const char *n); /* const_get const_defined? const_source_location */
int is_constants_list_name(const char *n); /* constants */
int is_autoload_query_name(const char *n); /* autoload? */
int is_stmt_output_name(const char *n); /* puts print p */
int is_class_body_acting_name(const char *n); /* output, raise, exit and the like */
int is_definition_hook_name(const char *n); /* inherited included extended prepended method_added singleton_method_added const_added */
int is_const_added_name(const char *n); /* const_added */
int is_method_missing_name(const char *n); /* method_missing respond_to_missing?: what a call of a name an object has no method by runs */
int definition_form_hooks(const char *form, int sg, const char **out); /* the hooks a definition form fires: included and append_features for include, method_added for attr_accessor, ... (up to 2) */
int is_const_missing_name(const char *n); /* const_missing */
int is_const_table_def_name(const char *n); /* __const_get__ __const_defined__ */
int is_string_position_mutator(const char *n); /* []= clear insert setbyte slice! */
int is_array_push_family(const char *n); /* << append prepend push unshift */

int is_io_class_name(const char *n); /* File IO */
int is_immediate_class_name(const char *n); /* FalseClass NilClass TrueClass */
int is_object_base_name(const char *n); /* BasicObject Object */
/* The classes a class's singleton chain ends in, which the load record
   holds by the header that reopens each (builtin_core_class_slot), are among
   these builtin rows. The list generates the builtin table and core-class
   slots in the same order; non-row slots follow in the enum. */
#define SP_BUILTIN_CLASSES(X) \
  X(INTEGER, "Integer", -100, BC_CLASS) \
  X(FLOAT, "Float", -101, BC_CLASS) \
  X(STRING, "String", -102, BC_CLASS) \
  X(SYMBOL, "Symbol", -103, BC_CLASS) \
  X(ARRAY, "Array", -104, BC_CLASS) \
  X(HASH, "Hash", -105, BC_CLASS) \
  X(RANGE, "Range", -106, BC_CLASS) \
  X(TIME, "Time", -107, BC_CLASS) \
  X(MODULE, "Module", -108, BC_CLASS) \
  X(CLASS, "Class", -109, BC_CLASS) \
  X(NILCLASS, "NilClass", -110, BC_CLASS) \
  X(TRUECLASS, "TrueClass", -111, BC_CLASS) \
  X(FALSECLASS, "FalseClass", -112, BC_CLASS) \
  X(NUMERIC, "Numeric", -113, BC_CLASS) \
  X(COMPARABLE, "Comparable", -114, BC_CLASS | BC_MODULE) \
  X(ENUMERABLE, "Enumerable", -115, BC_CLASS | BC_MODULE) \
  X(OBJECT, "Object", -116, BC_CLASS | BC_PLAIN) \
  X(BASIC_OBJECT, "BasicObject", -117, BC_CLASS | BC_PLAIN) \
  X(PROC, "Proc", -118, BC_CLASS) \
  X(KERNEL, "Kernel", -119, BC_CLASS | BC_MODULE) \
  X(IO, "IO", -120, BC_CLASS) \
  X(FILE, "File", -121, BC_CLASS) \
  X(EXCEPTION, "Exception", -122, BC_CLASS | BC_EXCEPTION) \
  X(STANDARDERROR, "StandardError", -123, BC_CLASS | BC_EXCEPTION) \
  X(RUNTIMEERROR, "RuntimeError", -124, BC_CLASS | BC_EXCEPTION) \
  X(TYPEERROR, "TypeError", -125, BC_CLASS | BC_EXCEPTION) \
  X(ARGUMENTERROR, "ArgumentError", -126, BC_CLASS | BC_EXCEPTION) \
  X(NAMEERROR, "NameError", -127, BC_CLASS | BC_EXCEPTION) \
  X(NOMETHODERROR, "NoMethodError", -128, BC_CLASS | BC_EXCEPTION) \
  X(STOPITERATION, "StopIteration", -129, BC_CLASS | BC_EXCEPTION) \
  X(MATH, "Math", -130, BC_CLASS | BC_MODULE) \
  X(COMPLEX, "Complex", -131, BC_CLASS) \
  X(INDEXERROR, "IndexError", -132, BC_CLASS | BC_EXCEPTION) \
  X(KEYERROR, "KeyError", -133, BC_CLASS | BC_EXCEPTION) \
  X(RANGEERROR, "RangeError", -134, BC_CLASS | BC_EXCEPTION) \
  X(FLOATDOMAINERROR, "FloatDomainError", -135, BC_CLASS | BC_EXCEPTION) \
  X(ZERODIVISIONERROR, "ZeroDivisionError", -136, BC_CLASS | BC_EXCEPTION) \
  X(FROZENERROR, "FrozenError", -137, BC_CLASS | BC_EXCEPTION) \
  X(IOERROR, "IOError", -138, BC_CLASS | BC_EXCEPTION) \
  X(LOCALJUMPERROR, "LocalJumpError", -139, BC_CLASS | BC_EXCEPTION) \
  X(NOTIMPLEMENTEDERROR, "NotImplementedError", -140, BC_CLASS | BC_EXCEPTION) \
  X(SCRIPTERROR, "ScriptError", -141, BC_CLASS | BC_EXCEPTION) \
  X(RATIONAL, "Rational", -142, BC_CLASS) \
  X(REGEXP, "Regexp", -143, BC_CLASS) \
  X(ENUMERATOR, "Enumerator", -144, BC_CLASS) \
  X(STRUCT, "Struct", -145, BC_CLASS) \
  X(DATA, "Data", -146, BC_CLASS | BC_PLAIN) \
  X(SYNTAXERROR, "SyntaxError", -147, BC_CLASS | BC_EXCEPTION) \
  X(SECURITYERROR, "SecurityError", -148, BC_CLASS | BC_EXCEPTION) \
  X(REGEXPERROR, "RegexpError", -149, BC_CLASS | BC_EXCEPTION) \
  X(ENCODINGERROR, "EncodingError", -150, BC_CLASS | BC_EXCEPTION) \
  X(SIGNALEXCEPTION, "SignalException", -151, BC_CLASS | BC_EXCEPTION) \
  X(INTERRUPT, "Interrupt", -152, BC_CLASS | BC_EXCEPTION) \
  X(THREADERROR, "ThreadError", -153, BC_CLASS | BC_EXCEPTION) \
  X(FIBERERROR, "FiberError", -154, BC_CLASS | BC_EXCEPTION) \
  X(CLOSEDQUEUEERROR, "ClosedQueueError", -155, BC_CLASS | BC_EXCEPTION) \
  X(UNCAUGHTTHROWERROR, "UncaughtThrowError", -156, BC_CLASS | BC_EXCEPTION) \
  X(NOMATCHINGPATTERNERROR, "NoMatchingPatternError", -157, BC_CLASS | BC_EXCEPTION) \
  X(NOMATCHINGPATTERNKEYERROR, "NoMatchingPatternKeyError", -158, BC_CLASS | BC_EXCEPTION) \
  X(EOFERROR, "EOFError", -159, BC_CLASS | BC_EXCEPTION) \
  X(MATH_DOMAINERROR, "Math::DomainError", -160, BC_EXCEPTION) \
  X(SYSTEMEXIT, "SystemExit", -161, BC_CLASS | BC_EXCEPTION) \
  X(SIGNAL, "Signal", -162, BC_CLASS | BC_MODULE) \
  X(PROCESS_STATUS, "Process::Status", -163, BC_CLASS) \
  X(PROCESS_TMS, "Process::Tms", -164, 0) \
  X(DIR, "Dir", -165, BC_CLASS) \
  X(BASICSOCKET, "BasicSocket", -166, BC_CLASS | BC_SOCKET) \
  X(IPSOCKET, "IPSocket", -167, BC_CLASS | BC_SOCKET) \
  X(TCPSOCKET, "TCPSocket", -168, BC_CLASS | BC_SOCKET) \
  X(TCPSERVER, "TCPServer", -169, BC_CLASS | BC_SOCKET) \
  X(UDPSOCKET, "UDPSocket", -170, BC_CLASS | BC_SOCKET) \
  X(UNIXSOCKET, "UNIXSocket", -171, BC_CLASS | BC_SOCKET) \
  X(UNIXSERVER, "UNIXServer", -172, BC_CLASS | BC_SOCKET) \
  X(SOCKET, "Socket", -173, BC_CLASS | BC_SOCKET) \
  X(THREAD, "Thread", -174, BC_CLASS) \
  X(MUTEX, "Mutex", -175, BC_CLASS) \
  X(THREAD_MUTEX, "Thread::Mutex", -175, 0) \
  X(QUEUE, "Queue", -176, BC_CLASS) \
  X(THREAD_QUEUE, "Thread::Queue", -176, 0) \
  X(SIZEDQUEUE, "SizedQueue", -177, BC_CLASS) \
  X(THREAD_SIZEDQUEUE, "Thread::SizedQueue", -177, 0) \
  X(CONDITIONVARIABLE, "ConditionVariable", -178, BC_CLASS) \
  X(THREAD_CONDITIONVARIABLE, "Thread::ConditionVariable", -178, 0) \
  X(FIBER, "Fiber", -179, BC_CLASS) \
  X(MATCHDATA, "MatchData", -180, BC_CLASS) \
  X(ENCODING, "Encoding", 0, BC_CLASS) \
  X(ENUMERATOR_CHAIN, "Enumerator::Chain", 0, BC_CLASS) \
  X(ENUMERATOR_LAZY, "Enumerator::Lazy", 0, BC_CLASS) \
  X(ENUMERATOR_PRODUCT, "Enumerator::Product", 0, BC_CLASS) \
  /* the json package's errors, which lib/sp_exc.c gives their parents (JSONError < StandardError): \
     a known name makes `rescue JSON::JSONError` climb that hierarchy, not compare a name (#7797) */ \
  X(JSON_JSONERROR, "JSON::JSONError", 0, BC_EXCEPTION) \
  X(JSON_PARSERERROR, "JSON::ParserError", 0, BC_EXCEPTION) \
  X(JSON_NESTINGERROR, "JSON::NestingError", 0, BC_EXCEPTION) \
  X(JSON_GENERATORERROR, "JSON::GeneratorError", 0, BC_EXCEPTION) \
  X(ENCODING_COMPATIBILITYERROR, "Encoding::CompatibilityError", 0, BC_EXCEPTION) \
  X(ENCODING_CONVERTERNOTFOUNDERROR, "Encoding::ConverterNotFoundError", 0, BC_EXCEPTION) \
  X(ENCODING_INVALIDBYTESEQUENCEERROR, "Encoding::InvalidByteSequenceError", 0, BC_EXCEPTION) \
  X(ENCODING_UNDEFINEDCONVERSIONERROR, "Encoding::UndefinedConversionError", 0, BC_EXCEPTION) \
  X(ERRNO_ENOENT, "Errno::ENOENT", 0, BC_EXCEPTION) \
  X(GC, "GC", 0, BC_CLASS | BC_MODULE) \
  X(IO_EAGAINWAITREADABLE, "IO::EAGAINWaitReadable", 0, BC_EXCEPTION) \
  X(IO_TIMEOUTERROR, "IO::TimeoutError", 0, BC_EXCEPTION) \
  /* the socket library's: lib/sp_exc.c gives them their parents, so \
     `rescue SocketError` catches a failed lookup's Socket::ResolutionError */ \
  X(SOCKETERROR, "SocketError", 0, BC_EXCEPTION) \
  X(SOCKET_RESOLUTIONERROR, "Socket::ResolutionError", 0, BC_EXCEPTION) \
  X(IO_EAGAINWAITWRITABLE, "IO::EAGAINWaitWritable", 0, BC_EXCEPTION) \
  X(IO_EINPROGRESSWAITREADABLE, "IO::EINPROGRESSWaitReadable", 0, BC_EXCEPTION) \
  X(IO_EINPROGRESSWAITWRITABLE, "IO::EINPROGRESSWaitWritable", 0, BC_EXCEPTION) \
  X(IO_EWOULDBLOCKWAITREADABLE, "IO::EWOULDBLOCKWaitReadable", 0, BC_EXCEPTION) \
  X(IO_EWOULDBLOCKWAITWRITABLE, "IO::EWOULDBLOCKWaitWritable", 0, BC_EXCEPTION) \
  X(IO_WAITREADABLE, "IO::WaitReadable", 0, BC_EXCEPTION) \
  X(IO_WAITWRITABLE, "IO::WaitWritable", 0, BC_EXCEPTION) \
  X(LOADERROR, "LoadError", 0, BC_EXCEPTION) \
  X(MARSHAL, "Marshal", 0, BC_CLASS | BC_MODULE) \
  X(METHOD, "Method", 0, BC_CLASS) \
  X(NOMEMORYERROR, "NoMemoryError", 0, BC_EXCEPTION) \
  X(OBJECTSPACE, "ObjectSpace", 0, BC_CLASS | BC_MODULE) \
  X(PROCESS, "Process", 0, BC_CLASS | BC_MODULE) \
  X(RANDOM, "Random", 0, BC_CLASS) \
  X(STRINGSCANNER_ERROR, "StringScanner::Error", 0, BC_EXCEPTION) \
  X(SYSTEMCALLERROR, "SystemCallError", 0, BC_EXCEPTION) \
  X(SYSTEMSTACKERROR, "SystemStackError", 0, BC_EXCEPTION) \
  X(UNBOUNDMETHOD, "UnboundMethod", 0, BC_CLASS)

/* A method added to Class is a class method of every class: the top-level
   `class Class` / `Class.class_eval { }` body becomes this module, and
   register_extends extends it into every root class, whose subclasses inherit. */
#define CLASS_REOPEN_NAME "Class__reopen"

enum {
#define X(k, n, id, flags) CORE_##k,
  SP_BUILTIN_CLASSES(X)
#undef X
  CORE_STRINGIO, CORE_SET, CORE_CLASS_REOPEN, CORE_FILETEST, CORE_ERRNO, CORE_WARNING,
  CORE_N
};
/* CORE_* for any builtin row or non-row slot above, -1 for any other name. */
int builtin_core_class_slot(const char *n);
int core_of_type(TyKind t);
const char *core_class_name(int k);
int is_builtin_method_host_name(const char *n); /* Object BasicObject Kernel Comparable Enumerable Module */
int is_boolean_class_name(const char *n); /* FalseClass TrueClass */
int is_numeric_literal_tag(const char *n); /* Float Int */
int is_standard_output_global(const char *n); /* $stderr $stdout */
int is_program_name_global(const char *n); /* $0 $PROGRAM_NAME */
int is_loaded_features_global(const char *n); /* $" $LOADED_FEATURES */
int is_load_path_global(const char *n); /* $: $-I $LOAD_PATH */
int is_range_or_time_class(const char *n); /* Range Time */
int is_numeric_class_name(const char *n); /* Float Integer */
int is_queue_class_name(const char *n); /* Queue SizedQueue */
int is_thread_class_name(const char *n); /* Queue SizedQueue Mutex ConditionVariable */
int is_thread_const_name(const char *n); /* Thread */
int is_threading_name(const char *n); /* Thread Monitor and is_thread_class_name's */
int is_array_or_object_class(const char *n); /* Array Object */
int is_array_hash_or_object_class(const char *n); /* Array Hash Object */

int is_integer_class_name(const char *n); /* Fixnum Integer */

int is_ivar_access(const char *n);   /* instance_variable_get instance_variable_set */
int is_plus_op(const char *n);       /* +: the operator `+=` writes through */
int is_ivar_set(const char *n);      /* instance_variable_set */

int is_string_append_or_prepend(const char *n); /* << concat prepend */

int is_string_append(const char *n); /* << concat: appends answering the receiver */
int is_string_byte_mutator(const char *n); /* bytesplice append_as_bytes: byte-level mutators answering the receiver */
int is_replace_name(const char *n); /* replace: a String's, Array's or Hash's contents swapped for another's, which ignores a block */

int is_string_rebind_mutator(const char *n); /* mutators needing argument-rebind snapshots */
/* String methods that only read the receiver's bytes and never retain the
   pointer: a shared-mutable receiver hands them its live buffer. */
int is_string_read_only_method(const char *name);
int str_mutator_str_args(const char *n, int argc, int *int_ok); /* the arguments a String mutator takes as Strings */

int builtin_module_owns(const char *cls, const char *name); /* included ahead of Object */
int builtin_mixin_module_id(const char *n);   /* Comparable Enumerable Kernel Math: its builtin id */
int is_include_name(const char *n);
int is_prepend_name(const char *n);
int is_load_name(const char *n);
int is_require_name(const char *n); /* require, require_relative */
int is_require_relative_name(const char *n);
int is_extend_name(const char *n);
int is_stdlib_const_name(const char *n);       /* a standard library's top-level constant */
int is_toplevel_const_name(const char *n);     /* a constant CRuby, or a library spinel provides, answers at the top level */
int is_constless_builtin_name(const char *n);  /* a builtin class or module with no constants of its own */
int is_autoload_name(const char *n);
int is_file_load_name(const char *n);    /* require require_relative load autoload: a call that runs a file's text */
int is_load_how_missing(const char *how);
int is_load_how_set(const char *how);
int is_load_how_core(const char *how);
int is_load_how_native(const char *how);
int is_native_lib_at_boot(const char *path);
int is_comparable_module_name(const char *n);
int is_enumerable_module_name(const char *n);
/* the index of n among the methods `extend Enumerable` sends to an
   instance-side twin (desugar_extend_enumerable), or -1 */
enum { EXTEND_ENUMERABLE_NNAMES = 63 };
int extend_enumerable_name_index(const char *n);
int is_comparable_builtin_name(const char *n);
typedef enum { BDC_BUILTIN, BDC_IDENTITY, BDC_EQUAL } BuiltinDefaultCmp;
BuiltinDefaultCmp builtin_default_cmp(int family);   /* asks builtin_class_id */
int builtin_class_id(const char *name);   /* analyze_util.c: a builtin class's id, 0 for none */
int builtin_comparable_owns(const char *cls, const char *name); /* Comparable's, on a class including it */
int builtin_comparable_arity(const char *name);
int builtin_numeric_owns(const char *cls, const char *name); /* Numeric's, on Integer or Float */
int builtin_numeric_arity(const char *name, int *out);

int is_gated_exception_accessor(const char *n); /* accessors owned by specific exception classes */
int is_symbol_exception_accessor(const char *n); /* exception accessors that can return a Symbol */

int is_builtin_reopen_name(const char *name);
/* A reopened class whose instance methods take self boxed (`sp_RbVal self`,
   emit_method_signature): Object, Numeric, Array, Hash. */
int is_boxed_self_class_name(const char *name);

int is_nil_method(const char *n); /* NilClass's public methods, its own and Object's: what nil answers */
int is_positional_io(const char *n); /* pread / pwrite: IO at an offset */

/* Array subclasses (#7449) */
int is_arysub_object_name(const char *n);        /* class is_a? dup ...: the object, not its elements */
int is_arysub_kernel_name(const char *n);        /* to_enum frozen? != ...: answered as the Array */

/* The Array adders the element store (strbuf_container_store_values) does
   not take: concat (of a literal), insert and prepend. */
enum { ARRAY_ADD_NONE, ARRAY_ADD_CONCAT, ARRAY_ADD_INSERT, ARRAY_ADD_PREPEND };
int array_unseen_add_kind(const char *n);

int is_scan_name(const char *n); /* scan: a String's match iterator */
int is_insert_name(const char *n); /* insert */
int is_arguments_node_type(const char *t); /* ArgumentsNode */
int is_when_node_type(const char *t);      /* WhenNode */
int is_clear_name(const char *n); /* clear */
int is_enumerator_with(const char *n); /* with_index with_object: an enumerator link */
int is_lazy_name(const char *n);       /* lazy */
int is_lazy_chain_name(const char *n);   /* a Lazy's call that answers a Lazy: map, select, take, ... */
int is_concat_name(const char *n);     /* concat */
int is_array_constructor(const char *recv, const char *meth); /* Array.new */
int is_pow_name(const char *n); /* pow: Integer power, with an optional modulus */

int is_lazy_name(const char *n);         /* lazy: makes a Lazy of its receiver */
int is_new_name(const char *n);          /* new: a class's constructor */
int is_native_share_decl(const char *n); /* native_share: a package's share declaration */
int is_proc_new(const char *recv, const char *meth); /* Proc.new */
int is_method_ref_name(const char *n);   /* method: Kernel#method */
int is_method_object_name(const char *n); /* method public_method instance_method: a Method of the name handed */
int is_marshal_const(const char *n);     /* Marshal, whose load makes objects with any ivars */
int is_env_const(const char *n);         /* ENV */
int is_argv_const(const char *n);        /* ARGV */
int is_engine_string_const(const char *n); /* RUBY_DESCRIPTION RUBY_VERSION ...: a frozen String */
int is_file_string_const(const char *n); /* File::SEPARATOR PATH_SEPARATOR NULL */
int is_proc_conversion_name(const char *n); /* to_proc curry: makes a proc */
int is_aref_name(const char *n);         /* []: an element read */
int is_shovel_name(const char *n);       /* <<: an append, a chain's link */

const char *dir_surface_alias(const char *n, int blockless_iter);
int is_dir_blockless_iter(const char *n);  /* foreach each_child: an Enumerator without a block */

int is_kernel_module_name(const char *n);
int is_kernel_module_function(const char *n);
int builtin_object_method_known(const char *m);   /* one of Object's own methods every object answers */
int is_method_presence_query(const char *n); /* 1: respond_to? *method_defined? (public_)instance_method (public_)method; 2: (public_)instance_methods (public_)methods, answered for the whole program; 3: private_/protected_instance_methods, private_/protected_methods, singleton_methods, answered by the receiver's chain */
int is_instance_method_list(const char *n); /* (public_|private_|protected_)instance_methods */
int is_singleton_method_list(const char *n); /* singleton_methods */
int builtin_every_object_answers(const char *m, int cmeth); /* Object's own, Kernel's, Object's; with cmeth Module's too */
int builtin_class_id_answers(int id, const char *m, int cmeth); /* the builtin class of cls_id id, or one above it, has m (cmeth: a class method) */
/* Does a builtin answer `name` on a self of the kind: main (Kernel's,
   Object's and main's own methods), a module (also Module's) or a class
   (also Class's)? */
enum { BSELF_MAIN, BSELF_MODULE, BSELF_CLASS, BSELF_N };
int builtin_self_method_known(const char *name, int kind);
int is_builtin_module_const_name(const char *n);
int is_untabled_native_class_name(const char *n);

int is_define_method_name(const char *n); /* define_method */
int is_define_singleton_method_name(const char *n); /* define_singleton_method */
int is_singleton_class_name(const char *n); /* singleton_class */
int is_ffi_decl_name(const char *n);        /* ffi_lib attach_function attach_variable callback native_func ...: declarations made methods before the walk */
int is_method_defined_query(const char *n); /* method_defined? public_/private_/protected_method_defined? */
int is_undef_method_name(const char *n);  /* undef_method */
int is_remove_method_name(const char *n); /* remove_method */
int is_check_funcall_name(const char *n);   /* respond_to? respond_to_missing? method_missing: what rb_check_funcall asks */
int is_implicitly_private_name(const char *n); /* initialize initialize_copy/_clone/_dup respond_to_missing? */
int is_alias_method_name(const char *n);  /* alias_method */
/* The operations a String Array (whose elements are handles) answers
   itself, by name, argument count and block (0 none, 1 a literal block, 2
   a block argument). */
typedef enum {
  STRH_NONE,
  STRH_ELEM,     /* [](i), at(i), first, last: the element, a String */
  STRH_STORE,    /* []=(i, v): stores v's String */
  STRH_PUSH,     /* <<, push, append: stores each argument's String */
  STRH_TAKE,     /* pop, shift: removes and answers the element */
  STRH_LEN,      /* size, length */
  STRH_EMPTY,    /* empty? */
  STRH_EACH,     /* each { }: the receiver */
  STRH_DUP,      /* dup: a new Array of the same Strings */
  STRH_SELF,     /* to_a: the receiver */
  STRH_JOIN,     /* join, join(sep) */
  STRH_INCLUDE,  /* include?(v) */
  STRH_EQ,       /* ==(other) */
  STRH_INSPECT   /* inspect, to_s */
} StrhOp;
int strh_op(const char *n, int argc, int block);
/* the receiver kind a block-taking iterator name belongs to */
enum { BITER_NONE, BITER_ARRAY, BITER_INTEGER, BITER_STRING };
int is_pop_name(const char *n);               /* pop */
int is_first_name(const char *n);             /* first */


const char *encoding_const_name(const char *nm);  /* Encoding::NM's encoding name, or NULL */
const char *builtin_operator_delegate(const char *name);
int is_mul_op(const char *n); /* * */
int is_pow_op(const char *n); /* ** */
int is_minus_op(const char *n); /* - */
int is_clone_name(const char *n); /* clone */
int is_cover_name(const char *n); /* cover? */
int is_flatten_name(const char *n); /* flatten */
int is_hash_name(const char *n); /* hash */
int is_puts_name(const char *n); /* puts */
int is_to_f_name(const char *n); /* to_f */
int is_to_i_name(const char *n); /* to_i */
int is_intersect_name(const char *n); /* intersect? */
int is_compare_by_identity_name(const char *n); /* compare_by_identity */
/* File::Stat's names, as the TY_IO arms answer them for a stat's handle */
int boxed_stat_pred(const char *n);   /* a mode predicate's sp_stat_pred kind, a type predicate's sp_stat_type_pred kind + 100, else -1 */
int stat_field_slot(const char *n);   /* a numeric field's sp_stat_field slot, else -1 */
int boxed_stat_name(const char *n);   /* mode or a numeric field */
int boxed_stat_reader(const char *n); /* mtime, atime, ftype */
/* ---- Lowerings onto a library's method: the shared shape rows ----
   A call of one of these shapes is lowered by the analysis onto a method or
   constant a library defines (builtins/*.rb, Set): `system(cmd, out: ...)`
   onto builtins/system_spawn.rb's __sp_system_spawn, and so on. Each row is
   the shape, the name it lowers to and the library that defines that name.
   The desugar a row's family names reads its shape from here; conditions on
   a value, a type or the rest of the program (the receiver is an
   Enumerator, the mode is a read, the program defines no `system`) stay in
   the desugar. A shape is only what the parsed call shows, so graph mode
   can ask the same rows which libraries a program's trees reach. A
   rewrite that falls back to a native method when the library is absent
   (sort_by.with_index, rfind) has no row. */
enum {
  LR_SYSTEM_OPTIONS,   /* desugar_system_options */
  LR_POPEN_BLOCK,      /* desugar_io_popen_block */
  LR_FOREACH_FIRST,    /* desugar_dir_surface's File.foreach(..).first(n) */
  LR_ENUM_WALK,        /* desugar_enum_walk_calls */
  LR_STR_WALK,         /* the String walk with an index (desugar_iter_receivers) */
  LR_ENUM_NAME,        /* desugar_builtin_enum_calls: a name for a builtins/enumerable.rb definition */
  LR_INDEX_BLOCK,      /* desugar_builtin_enum_calls: Array#index with a block */
  LR_WITH_OBJECT,      /* a stored Enumerator's with_object */
  LR_TO_SET            /* to_set -> Set.new(recv.to_a) */
};
/* the receiver: no condition, none, one, the constant recv_name, or a call
   named recv_name (on the constant recv_const when that is not NULL) */
enum { LR_RECV_ANY, LR_RECV_NONE, LR_RECV_SOME, LR_RECV_CONST, LR_RECV_CALL };
enum {
  LR_BLOCK = 1,          /* a block, literal or `&arg` */
  LR_BLOCK_LITERAL = 2,  /* a literal block */
  LR_NO_BLOCK = 4,
  LR_BREAKS = 8,         /* the literal block's body breaks out of it */
  LR_KWHASH = 16,        /* an argument is a keyword hash */
  LR_NO_SPLAT = 32,      /* no argument is a splat */
  LR_PLAIN_ARGS = 64,    /* no argument is a splat, a keyword hash or a block argument */
  LR_HAS_RECV = 128,     /* the call has a receiver (for the receiver call: of its own) */
  LR_NO_ARGS = 256       /* no ArgumentsNode at all (a synthesized call may carry an empty one) */
};
typedef struct {
  int family;                      /* LR_*: the desugar that reads it */
  const char *name;                /* the call's name */
  int recv;                        /* LR_RECV_* */
  const char *recv_name;           /* LR_RECV_CONST: the constant; LR_RECV_CALL: the call */
  const char *recv_const;          /* LR_RECV_CALL: that call's receiver constant, or NULL */
  const char *const *recv_calls;   /* when the receiver is a blockless call, its name is one of these (NULL: any) */
  signed char argc_min, argc_max;  /* the call's argument count (-1: no bound) */
  unsigned short flags;            /* LR_* bits for the call */
  signed char rargc_min, rargc_max;   /* LR_RECV_CALL: the receiver call's */
  unsigned short rflags;
  const char *target;              /* the method (or, for LR_TO_SET, the constant) it lowers to */
  const char *lib;                 /* the library that defines target, as a require names it */
} LowerRow;
const LowerRow *lower_row_at(int i);              /* the i-th row, NULL past the last */
/* SPINEL_LOWER_ROWS_CHECK: the rows a call's parse matched travel on its node
   as a mask of row indices (`lr_rows`), so only this many rows are checked */
#define LOWER_ROWS_MASK_BITS 63
const LowerRow *lower_row_of(int family);         /* the family's first row */
const LowerRow *lower_row_named(int family, const char *name);   /* the family's row for a call name */

/* ---- The pre-installed packages' facts ----
   Generated from their manifests (packages/<package>/spin.toml) into
   src/package_facts.inc by tools/gen_package_facts.rb: the feature a package
   is required by, whether CRuby has it loaded at boot, and the require it
   rides as an add-on. */
typedef struct {
  const char *package;   /* "set" */
  const char *feature;   /* its file, packages/<package>/<feature>.rb: "set", "io/buffer" */
  int boot;              /* CRuby has it loaded before a program runs */
  const char *rides;     /* the require that makes the add-on available ("time"), or NULL */
} PackageFact;
const PackageFact *package_fact_at(int i);   /* the i-th row, NULL past the last */

/* ---- Spinel's native classes, modules and exceptions (BUILTIN_CLASSES) ----
   One row per name: which predicate accepts it (BC_*) and its negative
   cls_id (0 when codegen has none). BC_PLAIN marks Object, BasicObject and
   Data, below which a program's class is an object of its own. */
enum { BC_CLASS = 1, BC_MODULE = 2, BC_EXCEPTION = 4, BC_SOCKET = 8, BC_PLAIN = 16 };
typedef struct { const char *name; int id; unsigned flags; } BuiltinClass;
const BuiltinClass *builtin_class_at(int i);   /* the i-th row, NULL past the last */
int is_native_class_name(const char *n);       /* any row: a class Spinel has without a library */
int is_plain_builtin_superclass(int id);       /* BC_PLAIN or an exception: gives a program's subclass no method of an Enumerable walk */
/* names the compile-time evaluation and its questions ask about (ct_eval.c,
   analyze_ct.c) */
int is_class_const_name(const char *n);    /* Class: the constant Class.new is called on */
int is_module_const_name(const char *n);   /* Module: the constant Module.new is called on */
int is_hash_const_name(const char *n);     /* Hash: the class a program's `class Hash` reopens */
int is_erb_const_name(const char *n);      /* ERB: a class of the program's own by that name */
int is_ffi_registry_name(const char *n);   /* FFI__Registry: the module the bundled ffi package defines */
int is_hash_read_only(const char *n);      /* the Hash calls that read a table and leave it as it is */
int is_to_a_name(const char *n);           /* to_a: the receiver's own Array, for an Array */
int is_hash_self_answer(const char *n);    /* each each_pair each_key each_value freeze to_h: answer the Hash itself */
int is_core_change_call(const char *n);    /* class_eval .. module_exec include prepend define_method send family alias_method remove_method undef_method: may change a class's methods */
int is_singleton_reach_call(const char *n);/* define_singleton_method extend singleton_class instance_eval instance_exec: give an object methods of its own */
int is_method_naming_call(const char *n);  /* define_method define_singleton_method alias_method remove_method undef_method attr*: give or take a method its argument names */
int is_attr_decl_reader(const char *n);    /* attr attr_reader attr_accessor, and Spinel's class-body attribute attributes: declare a reader of each name */
int is_attr_decl_writer(const char *n);    /* attr_writer attr_accessor attribute attributes: declare a writer of each name */
int class_method_vis_of_name(const char *n); /* private_class_method public_class_method: BVIS_PRIVATE / BVIS_PUBLIC, else 0 */
int is_refine_name(const char *n);         /* refine */
int is_enum_helper_name(const char *n);   /* __enum_pairs __enum_count_int: the compiler's helpers a receiverless call names */
int is_load_effect_call(const char *n);    /* a method body's call that defines, writes, loads or runs code the load record cannot follow */
int is_using_name(const char *n);          /* using */
int is_remove_const_name(const char *n);   /* remove_const */
int is_const_visibility_name(const char *n);   /* private_constant public_constant deprecate_constant */
int is_cvar_write_name(const char *n);     /* class_variable_set remove_class_variable */
int is_hook_protocol_name(const char *n);  /* append_features extend_object prepend_features: what include, extend and prepend send before their hooks */
int is_port_gvar_name(const char *n);      /* $stdout $stderr $>: assigning an object makes it the port output goes to */

/* A name the compiler synthesizes for a protocol method (the no-argument
   display wrappers `__noarg to_s` and `__noarg inspect`, the Enumerable
   materializer `__enum_to_a`): 1, and in *proto the i-th protocol name it
   answers for, else 0. The materializer is what Enumerable#to_a runs (it
   calls each), what a Struct's to_a and deconstruct read (its members; CRuby
   calls nothing), so it answers for each, to_a and deconstruct. */
int synth_stands_for(const char *name, int i, const char **proto);
int is_method_list_call(const char *n);    /* attr attr_reader attr_writer attr_accessor remove_method undef_method: every argument names a method */
#endif
