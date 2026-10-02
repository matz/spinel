/*
** shim/sp_re_names.h - link names for the mrb_ functions the engine defines
**
** The engine calls mruby's C API. re_spinel.c implements that API with
** the C library, and its source spells each function with mruby's name.
** A program that links real mruby as well as the spinel runtime must not
** see two definitions of one name.
** The engine files define more mrb_ functions of their own. This header
** gives each mrb_ function that the engine objects define an sp_re_ prefix.
** The copied mruby files stay unchanged, because every engine file
** includes shim/mruby.h, and so this header, before it uses an mrb_ name.
** Types (mrb_int, mrb_value, mrb_state, mrb_regexp_pattern) are not link
** symbols, so they keep their names.
*/
#ifndef SP_RE_SHIM_NAMES_H
#define SP_RE_SHIM_NAMES_H

#define mrb_calloc sp_re_mrb_calloc
#define mrb_exc_get_id sp_re_mrb_exc_get_id
#define mrb_exc_new_str sp_re_mrb_exc_new_str
#define mrb_exc_raise sp_re_mrb_exc_raise
#define mrb_format sp_re_mrb_format
#define mrb_free sp_re_mrb_free
#define mrb_malloc sp_re_mrb_malloc
#define mrb_malloc_simple sp_re_mrb_malloc_simple
#define mrb_re_case_fold sp_re_mrb_re_case_fold
#define mrb_re_class_ctype_match sp_re_mrb_re_class_ctype_match
#define mrb_re_compile sp_re_mrb_re_compile
#define mrb_re_ctype sp_re_mrb_re_ctype
#define mrb_re_ctype_span sp_re_mrb_re_ctype_span
#define mrb_re_exec sp_re_mrb_re_exec
#define mrb_re_flags_cat sp_re_mrb_re_flags_cat
#define mrb_re_free sp_re_mrb_re_free
#define mrb_re_is_word_char sp_re_mrb_re_is_word_char
/* This name is a function only in the RE_NO_UNICODE_* build. */
#define mrb_re_needs_case_data sp_re_mrb_re_needs_case_data
#define mrb_re_rexec sp_re_mrb_re_rexec
#define mrb_realloc sp_re_mrb_realloc
#define mrb_realloc_simple sp_re_mrb_realloc_simple
#define mrb_str_cat sp_re_mrb_str_cat
#define mrb_str_new sp_re_mrb_str_new
#define mrb_str_new_cstr sp_re_mrb_str_new_cstr
#define mrb_str_resize sp_re_mrb_str_resize
#define mrb_temp_alloc sp_re_mrb_temp_alloc
#define mrb_uni_case_fold sp_re_mrb_uni_case_fold
#define mrb_uni_case_fold_range sp_re_mrb_uni_case_fold_range
#define mrb_uni_case_map sp_re_mrb_uni_case_map
#define mrb_uni_case_unfold sp_re_mrb_uni_case_unfold
#define mrb_uni_case_unfold_range sp_re_mrb_uni_case_unfold_range
#define mrb_utf8_char_head sp_re_mrb_utf8_char_head
#define mrb_utf8_decode sp_re_mrb_utf8_decode
#define mrb_utf8_to_buf sp_re_mrb_utf8_to_buf
#define mrb_utf8len sp_re_mrb_utf8len

#endif
