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

int is_zip_name(const char *n);       /* zip: tuple-yielding iteration */
int is_call_alias(const char *n);     /* call () []: a Proc/Method's invocation */
int is_method_invoke(const char *n);  /* call () [] ===: Method invocation */
int is_bind_call(const char *n);  /* bind_call: an UnboundMethod bound and called in one */
int is_kind_query(const char *n);     /* is_a? kind_of? instance_of? */
int is_member_blind_query(const char *n); /* class object_id __id__ nil? frozen? equal? respond_to? is_a? ... */
int is_round_family(const char *n);   /* round ceil floor truncate */
int is_push_alias(const char *n);     /* push << append */
int is_bit_op(const char *n);         /* & | ^ */
int is_basic_arith(const char *n);    /* + - * / (is_arith_op adds % and **) */
int is_int_arith_op(const char *n);   /* + - * / %: an Integer's arithmetic that answers an Integer */
int is_add_sub_mul(const char *n);    /* + - * */
int is_int_bit_op(const char *n);     /* & | ^ << >>: Integer's bitwise operators */
int is_embedding_builtin(const char *nm); /* Array Hash: subclass instances embed the builtin */
int is_class_name_name(const char *n);   /* name */
int is_inspect_name(const char *n);       /* inspect */
int is_object_root(const char *n);    /* Object Kernel BasicObject: the classes every object has */
int is_send_family(const char *n);    /* send __send__ public_send */
int is_opaque_reaching_call(const char *n); /* send family, call, new, lambda/proc, freeze, eval, instance_/class_/module_*, *method* */
int is_async_code_entry(const char *recv, const char *n); /* Thread.new/start/fork, Fiber.new, trap, Signal.trap */
int is_name_reader(const char *n);    /* name to_s inspect: a Class's or Module's name */
int is_tap_alias(const char *n);      /* tap then yield_self */
int is_tap_name(const char *n);       /* tap */
int is_quantifier(const char *n);     /* all? any? none? one? */
int is_set_op(const char *n);         /* & intersection | union - difference */
int is_combination_family(const char *n);  /* combination permutation repeated_combination repeated_permutation */
int is_visibility_name(const char *n);     /* private protected public */
int is_select_bang(const char *n);    /* select! filter! keep_if reject! delete_if: the in-place filters */
int is_retaining_filter(const char *n); /* select filter find_all reject, and the in-place filters: the iterators answering some of their elements */
int is_enum_partition_def(const char *n); /* __enum_partition__N: partition's builtin definition, per call site */
int is_each_walk(const char *n);      /* each each_entry reverse_each */
int is_index_query(const char *n);    /* find_index index rindex */
int is_self_copy(const char *n);      /* freeze dup clone itself: the receiver, or a copy of it */
int is_int_step(const char *n);       /* times upto downto: Integer's counting iterators */
int is_equality_name(const char *n);  /* equal? eql? == */
int is_bits_query(const char *n);     /* allbits? anybits? nobits? */
int is_count_alias(const char *n);    /* length size count */
int is_class_eval_family(const char *n);  /* class_eval module_eval class_exec module_exec */
int is_eval_exec_family(const char *n);   /* class/module/instance eval and exec */
int is_key_query(const char *n);      /* key? has_key? include? member?: Hash/ENV membership aliases */
int is_hash_key_lookup(const char *n); /* [] fetch delete and is_key_query: a Hash call that only compares its key */
int is_receiver_conversion(const char *n); /* to_s to_str itself: conversions a String answers with itself */
int is_to_s_name(const char *n);
int is_range_membership(const char *n); /* cover? include? member? ===: Range membership predicates */
int is_each_walk_or_with_index(const char *n); /* each each_entry reverse_each each_with_index */
int is_call_or_yield(const char *n);  /* call () [] yield: is_call_alias's names and yield */
int is_proc_invoke(const char *n);    /* call () [] yield ===: every name that invokes a Proc */
int is_quantifier_or_count(const char *n);  /* all? any? none? one? count: is_quantifier's names and count */
int is_push_unshift(const char *n);   /* << push append unshift: is_push_alias's names and unshift */
int is_identity_query(const char *n); /* equal? object_id __id__ frozen?: tells an object from its copy */
int is_len_alias(const char *n);      /* length size */
int is_str_each_iter(const char *n);  /* each_char each_line each_byte each_codepoint: String's element iterators */
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
int is_endpoint_query(const char *n); /* first last: collection or Range endpoints */
int is_map_alias(const char *n); /* map collect: Enumerable transformation aliases */
int is_instance_eval_family(const char *n); /* instance_eval instance_exec */
int is_find_alias(const char *n); /* find detect: Enumerable search aliases */

int is_text_conversion(const char *n); /* inspect to_s */
int is_slice_alias(const char *n); /* [] slice */
int is_shift_op(const char *n); /* << >> */
int is_copy_alias(const char *n); /* clone dup */
int is_hash_merge_bang(const char *n); /* merge! update */
int is_membership_alias(const char *n); /* include? member? */
int is_eql_or_equal(const char *n); /* eql? equal? */
int is_equal_name(const char *n);   /* equal? */
int is_substitution(const char *n); /* gsub sub */
int is_element_at_alias(const char *n); /* [] at */
int is_map_bang_alias(const char *n); /* collect! map! */
int is_each_or_pair(const char *n); /* each each_pair */
int is_proc_constructor(const char *n); /* lambda proc */

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
int is_object_copy(const char *n);   /* dup clone */
int is_ivar_set_name(const char *n); /* instance_variable_set */
int is_marshal_dump(const char *recv, const char *meth); /* Marshal.dump */
int is_marshal_load(const char *recv, const char *meth); /* Marshal.load */
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
int is_hash_key_value_each(const char *n); /* each_key each_value */
int is_encoding_mutator(const char *n); /* encode! force_encoding */
int is_range_end_reader(const char *n); /* end last */
int is_raise_alias(const char *n); /* fail raise */
int is_exc_message_name(const char *n); /* message full_message detailed_message */
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
int is_visibility_or_module_function(const char *n); /* module_function private protected public */
int is_mixin_call(const char *n); /* include extend prepend */
int is_const_set_name(const char *n); /* const_set */
int is_const_query_name(const char *n); /* const_get const_defined? const_source_location */
int is_constants_list_name(const char *n); /* constants */
int is_definition_hook_name(const char *n); /* inherited included extended prepended method_added singleton_method_added const_added */
int is_const_missing_name(const char *n); /* const_missing */
int is_const_table_def_name(const char *n); /* __const_get__ __const_defined__ */
int is_string_position_mutator(const char *n); /* []= clear insert setbyte slice! */
int is_array_push_family(const char *n); /* << append prepend push unshift */

int is_io_class_name(const char *n); /* File IO */
int is_file_class_name(const char *n); /* File */
int is_filetest_module_name(const char *n); /* FileTest */
int is_immediate_class_name(const char *n); /* FalseClass NilClass TrueClass */
int is_object_base_name(const char *n); /* BasicObject Object */
int is_boolean_class_name(const char *n); /* FalseClass TrueClass */
int is_numeric_literal_tag(const char *n); /* Float Int */
int is_standard_output_global(const char *n); /* $stderr $stdout */
int is_program_name_global(const char *n); /* $0 $PROGRAM_NAME */
int is_range_or_time_class(const char *n); /* Range Time */
int is_numeric_class_name(const char *n); /* Float Integer */
int is_queue_class_name(const char *n); /* Queue SizedQueue */
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
int builtin_comparable_owns(const char *cls, const char *name); /* Comparable's, on a class including it */
int builtin_comparable_arity(const char *name);
int builtin_numeric_owns(const char *cls, const char *name); /* Numeric's, on Integer or Float */
int builtin_numeric_arity(const char *name, int *out);

int is_gated_exception_accessor(const char *n); /* accessors owned by specific exception classes */
int is_symbol_exception_accessor(const char *n); /* exception accessors that can return a Symbol */

int is_builtin_reopen_name(const char *name);

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
int is_enumerator_with(const char *n); /* with_index with_object: an enumerator link */
int is_lazy_name(const char *n);       /* lazy */
int is_concat_name(const char *n);     /* concat */
int is_array_constructor(const char *recv, const char *meth); /* Array.new */
int is_pow_name(const char *n); /* pow: Integer power, with an optional modulus */

int is_lazy_name(const char *n);         /* lazy: makes a Lazy of its receiver */
int is_new_name(const char *n);          /* new: a class's constructor */
int is_native_share_decl(const char *n); /* native_share: a package's share declaration */
int is_proc_new(const char *recv, const char *meth); /* Proc.new */
int is_method_ref_name(const char *n);   /* method: Kernel#method */
int is_env_const(const char *n);         /* ENV */
int is_argv_const(const char *n);        /* ARGV */
int is_proc_conversion_name(const char *n); /* to_proc curry: makes a proc */
int is_aref_name(const char *n);         /* []: an element read */
int is_shovel_name(const char *n);       /* <<: an append, a chain's link */

const char *dir_surface_alias(const char *n, int blockless_iter);

int is_kernel_module_name(const char *n);
int is_kernel_module_function(const char *n);
int is_builtin_module_const_name(const char *n);

int is_define_method_name(const char *n); /* define_method */
int is_alias_method_name(const char *n);  /* alias_method */

#endif
