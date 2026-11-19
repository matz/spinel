/* spinel/runtime.h -- the stable ABI surface for carried package C (Path B).
 *
 * A spin package that carries C (compiled and linked on demand, see the
 * native_* binding DSL) includes THIS header, not the compiler's internal
 * lib/sp_*.h. Only the symbols documented here are a stable contract; anything
 * reachable through the umbrella includes but not listed below is an internal
 * of the current runtime and may change without notice.
 *
 * Frozen surface (grown per Path B target; this is the json set):
 *
 *   Values / tags:
 *     sp_RbVal            boxed value       (sp_gc.h)
 *     SP_TAG_INT/FLT/BOOL/NIL/SYM/OBJ   value tags
 *
 *   Container reflection hooks (installed by the generated program at startup;
 *   a package reads a program's typed containers only through these):
 *     sp_json_kind_fn, sp_json_len_fn, sp_json_aref_fn, sp_json_hpair_fn
 *     sp_sym_name_fn
 *
 *   String payload allocation and scalar formatting (sp_alloc.h):
 *     sp_str_alloc, sp_str_alloc_raw, sp_str_set_len, sp_str_empty
 *     sp_int_to_s, sp_float_to_s
 *     sp_oom_die
 *     sp_hash_key_from_bytes  (a frozen String key, which a Hash stores as
 *                              is: a parser's object keys)
 *
 *   Boxing a new String (sp_string.h):
 *     sp_box_str_new  (the package's newly built String or the owner of a
 *                      live String payload, boxed as a handle. A change
 *                      through the box reaches all copies. Use it for a
 *                      parser's String values.
 *                      Object keys follow the frozen Hash key contract.)
 *     sp_poly_arg_string  (retain a String or the result of its #to_str)
 *
 *   Float digits (sp_dtoa.h):
 *     sp_float_shortest  (the shortest round-trip digits and exponent, for a
 *                         package with a layout of its own, as JSON's)
 *
 *   Errors:
 *     sp_raise_cls   (raise a named exception class)
 *     sp_raise_parse_error  (raise one with the line and column that its
 *                            #line and #column answer: JSON::ParserError)
 *
 *   GC roots (sp_gc.h):
 *     SP_GC_ROOT, SP_GC_ROOT_STR, SP_GC_ROOT_RBVAL
 *     A package function that allocates has to root whatever it was handed
 *     first. The collector finds a value only through a root, and a caller
 *     passing an expression straight in (`JSON.parse File.read path`) holds it
 *     in nothing but the C argument slot -- so the package's own first
 *     allocation collects it. Rooting inside the callee is early enough
 *     BECAUSE the argument was still live on entry; a package that instead
 *     keeps a borrowed pointer past its return (a scanner over its source)
 *     also needs a scan hook that marks it.
 *
 *   Write barrier (sp_gc.h):
 *     sp_gc_wb(obj)
 *     A package object whose scan hook marks a reference stored AFTER the
 *     object was made (a scanner's last match) calls it on the object before
 *     each such store, with nothing that allocates in between. By then the
 *     object may be old, and a minor collection walks only the young objects
 *     and the old ones the barrier recorded: without it, the new value the
 *     old object holds is freed.
 */
#ifndef SPINEL_RUNTIME_H
#define SPINEL_RUNTIME_H

#include "sp_gc.h"      /* sp_RbVal, SP_TAG_*, container + sym reflection hooks */
#include "sp_alloc.h"   /* sp_str_alloc, sp_str_set_len, sp_int_to_s, sp_float_to_s, sp_oom_die */
#include "sp_dtoa.h"    /* sp_float_shortest */
#include "sp_string.h"   /* sp_box_str_new */

sp_String *sp_poly_arg_string(sp_RbVal value);

#endif /* SPINEL_RUNTIME_H */
