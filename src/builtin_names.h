/* builtin_names.h -- the families of builtin method names the compiler asks
   about by name in many places: `[] () call`, `is_a? kind_of? instance_of?`,
   ... Each predicate answers whether a name is one of its family; the
   family's names are listed once, in builtin_names.c, where every site that
   spelled the chain out now asks. A predicate compares the name with each of
   its family in turn, as the chains did (sp_streq: the work counter counts
   the same compares). Near-families (a set one name larger or smaller) are
   different questions and keep their own spelling. */
#ifndef SPINEL_BUILTIN_NAMES_H
#define SPINEL_BUILTIN_NAMES_H

int is_zip_name(const char *n);       /* zip: tuple-yielding iteration */
int is_call_alias(const char *n);     /* call () []: a Proc/Method's invocation */
int is_kind_query(const char *n);     /* is_a? kind_of? instance_of? */
int is_round_family(const char *n);   /* round ceil floor truncate */
int is_push_alias(const char *n);     /* push << append */
int is_bit_op(const char *n);         /* & | ^ */
int is_basic_arith(const char *n);    /* + - * / (is_arith_op adds % and **) */
int is_add_sub_mul(const char *n);    /* + - * */
int is_int_bit_op(const char *n);     /* & | ^ << >>: Integer's bitwise operators */
int is_object_root(const char *n);    /* Object Kernel BasicObject: the classes every object has */
int is_send_family(const char *n);    /* send __send__ public_send */
int is_name_reader(const char *n);    /* name to_s inspect: a Class's or Module's name */
int is_tap_alias(const char *n);      /* tap then yield_self */
int is_quantifier(const char *n);     /* all? any? none? one? */
int is_set_op(const char *n);         /* & intersection | union - difference */
int is_combination_family(const char *n);  /* combination permutation repeated_combination repeated_permutation */
int is_visibility_name(const char *n);     /* private protected public */
int is_select_bang(const char *n);    /* select! filter! keep_if reject! delete_if: the in-place filters */
int is_each_walk(const char *n);      /* each each_entry reverse_each */
int is_index_query(const char *n);    /* find_index index rindex */
int is_self_copy(const char *n);      /* freeze dup clone itself: the receiver, or a copy of it */
int is_int_step(const char *n);       /* times upto downto: Integer's counting iterators */
int is_equality_name(const char *n);  /* equal? eql? == */
int is_bits_query(const char *n);     /* allbits? anybits? nobits? */
int is_count_alias(const char *n);    /* length size count */
int is_class_eval_family(const char *n);  /* class_eval module_eval class_exec module_exec */
int is_call_or_yield(const char *n);  /* call () [] yield: is_call_alias's names and yield */
int is_quantifier_or_count(const char *n);  /* all? any? none? one? count: is_quantifier's names and count */
int is_push_unshift(const char *n);   /* << push append unshift: is_push_alias's names and unshift */
int is_len_alias(const char *n);      /* length size */

#endif
