#ifndef SP_JSON_H
#define SP_JSON_H
/* JSON serialization, defined in packages/json/sp_json.c -- a carried-C
   spin package (Path B), linked on demand when `require "json"` appears.

   The serializer owns no container types. It walks a boxed value through the
   generic container and symbol reflection hooks installed by the generated TU
   at startup, so it lives in its own translation unit and allocates result
   strings on the shared string heap. It compiles against the stable package
   ABI rather than the compiler internal headers. */
#include "spinel/runtime.h"   /* sp_RbVal, SP_TAG_*, hooks, sp_str_alloc, ... */

const char *sp_json_str(const char *s);   /* quote + escape a string */
const char *sp_json_val(sp_RbVal v);      /* serialize any boxed value */
const char *sp_json_dump(sp_RbVal v, sp_int limit); /* JSON.dump: its limit, NaN allowed */
const char *sp_json_pretty(sp_RbVal v);   /* two-space-indented serialization */
sp_RbVal sp_json_parse(const char *s);    /* parse JSON text into a boxed value */
sp_RbVal sp_json_parse_limit(const char *s, sp_int max_nesting);
/* An object serializes as CRuby's json does, which is method dispatch: a class
   with its own #to_json answers through the sp_obj_to_json_fn hook (declared in
   sp_gc.h, installed by the generated program), and everything else is its
   #to_s as a JSON string -- which is what Object#to_json is over there. A
   Struct has no override of its own, so it reads "#<struct S x=1>" (#4387). */
#endif /* SP_JSON_H */
