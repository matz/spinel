#include "types.h"
#include "builtin_ops.h"
#include "builtin_names.h"
#include <stddef.h>
#include <string.h>

/* ---- The boxed-receiver face table (see types.h) ---- */
static const PolyFace ty_poly_face_tbl[] = {
  /* Random stored in a mixed container keeps its instance surface. The
     typed call also checks invalid counts; bytes without arguments belongs
     to String and keeps its existing dispatch. */
  {"rand", PF_RANDOM, 0, -1, -1},
  {"bytes", PF_RANDOM, 1, -1, -1},
  {"seed", PF_RANDOM, 0, -1, -1},
  /* String value-form mutators: the non-bang transform runs against the
     unboxed contents and the result is written back through the box. */
  {"gsub!", PF_STRING | PF_STR_BANG, 0, -1, -1}, {"sub!", PF_STRING | PF_STR_BANG, 0, -1, -1},
  {"upcase!", PF_STRING | PF_STR_BANG, 0, -1, -1}, {"downcase!", PF_STRING | PF_STR_BANG, 0, -1, -1},
  {"capitalize!", PF_STRING | PF_STR_BANG, 0, -1, -1}, {"swapcase!", PF_STRING | PF_STR_BANG, 0, -1, -1},
  {"strip!", PF_STRING | PF_STR_BANG, 0, -1, -1}, {"lstrip!", PF_STRING | PF_STR_BANG, 0, -1, -1},
  {"rstrip!", PF_STRING | PF_STR_BANG, 0, -1, -1}, {"chomp!", PF_STRING | PF_STR_BANG, 0, -1, -1},
  {"chop!", PF_STRING | PF_STR_BANG, 0, -1, -1}, {"squeeze!", PF_STRING | PF_STR_BANG, 0, -1, -1},
  {"tr!", PF_STRING | PF_STR_BANG, 0, -1, -1}, {"delete!", PF_STRING | PF_STR_BANG, 0, -1, -1},
  {"tr_s!", PF_STRING | PF_STR_BANG, 0, -1, -1}, {"delete_prefix!", PF_STRING | PF_STR_BANG, 0, -1, -1},
  {"delete_suffix!", PF_STRING | PF_STR_BANG, 0, -1, -1},
  {"succ!", PF_STRING | PF_STR_BANG | PF_STR_SELF, 0, -1, -1},
  {"next!", PF_STRING | PF_STR_BANG | PF_STR_SELF, 0, -1, -1},
  /* The rest of the String surface. `partition` qualifies only in its
     one-argument form: Enumerable#partition takes none. */
  {"squeeze", PF_STRING, 0, 0, 0}, {"byteindex", PF_STRING, 1, 2, 0}, {"byterindex", PF_STRING, 1, 2, 0},
  {"partition", PF_STRING, 1, 1, 0}, {"rpartition", PF_STRING, 1, 1, 0},
  {"hex", PF_STRING, 0, 0, 0}, {"oct", PF_STRING, 0, 0, 0}, {"tr_s", PF_STRING, 2, 2, 0},
  {"crypt", PF_STRING, 1, 1, 0}, {"casecmp", PF_STRING, 1, 1, 0}, {"casecmp?", PF_STRING, 1, 1, 0},
  /* The names Integer alone owns -- and step, which a Float receiver owns
     too: two owners, dispatched on the box's tag at run time, so a boxed
     `2.5.step(9, 3)` reaches the Float emitter rather than being truncated
     to an sp_int (#4763). The block form only: the blockless form answers an
     Enumerator neither typed emitter builds from a box. */
  /* digits, pred, bit_length, ceildiv, pow and gcdlcm were rows here: the
     narrowing to sp_int truncated a Bignum; they answer by the box's tag now
     (sp_poly_int_*, #4665) */
  {"times", PF_INT, 0, 0, 1}, {"upto", PF_INT, 1, 1, 1}, {"downto", PF_INT, 1, 1, 1},
  {"step", PF_INT | PF_FLOAT, 0, 2, 1},   /* no limit: the endless Integer walk (the Float arm declines it) */
  /* the blockless form materializes the sequence the way the typed
     emitters do (an Integer or a Float array), boxed since the arms
     disagree, so `.to_a` / `.map` read it as the Enumerator's answer (#4779);
     an Integer or Float Range's step(n) materializes the same way */
  {"step", PF_INT | PF_FLOAT | PF_RANGE | PF_FRANGE, 1, 2, 0},
  /* with no argument at all, a number's endless walk by 1 */
  {"step", PF_INT | PF_FLOAT, 0, 0, 0},
  /* A Range of each kind owns step and bsearch with a block: unboxed to its
     own by-value struct, the typed emitter walks it. A blockless step(n)
     takes the row above; blockless bsearch answers an Enumerator no typed
     emitter builds.
     An Array owns bsearch too: a row for the name decides every receiver
     kind, so leaving it out made a boxed Array's bsearch a NoMethodError.
     A String Range's bsearch is its TypeError, raised by its own row. */
  {"step", PF_RANGE | PF_FRANGE | PF_SRANGE | PF_TRANGE, 0, 1, 1},
  {"bsearch", PF_ARRAY | PF_RANGE | PF_FRANGE | PF_SRANGE, 0, 0, 1},
  /* bsearch_index with a block is Array's alone (a Range has none) */
  {"bsearch_index", PF_ARRAY, 0, 0, 1},
  /* minmax is Range's own, read off the endpoints (a Float Range cannot be
     walked), beside the Enumerable row below that walks a collection */
  {"minmax", PF_RANGE | PF_FRANGE | PF_SRANGE, 0, 0, -1},
  /* The Enumerable names a boxed receiver shares with Array: its elements
     (a hash's [key, value] pairs) materialize into a poly array once. */
  {"minmax", PF_ENUM, 0, -1, -1}, {"tally", PF_ENUM, 0, -1, -1}, {"product", PF_ENUM, 0, -1, -1},
  {"combination", PF_ENUM, 0, -1, -1}, {"permutation", PF_ENUM, 0, -1, -1},
  {"group_by", PF_ENUM, 0, -1, 1}, {"partition", PF_ENUM, 0, -1, 1},
  {"each_with_object", PF_ENUM, 0, -1, 1}, {"chunk_while", PF_ENUM, 0, -1, 1},
  {"slice_when", PF_ENUM, 0, -1, 1},
  /* Names served by unboxing to an Array, on an Array at run time; a mutator
     writes its result back into a typed original. */
  {"sort!", PF_ARRAY | PF_MUT | PF_VAL_SELF, 0, 0, -1}, {"sort_by!", PF_ARRAY | PF_MUT | PF_VAL_SELF, 0, 0, 1},
  {"rotate!", PF_ARRAY | PF_MUT | PF_VAL_SELF, 0, 1, 0}, {"uniq!", PF_ARRAY | PF_MUT | PF_VAL_SELF, 0, 0, -1},
  {"shuffle!", PF_ARRAY | PF_MUT | PF_VAL_SELF, 0, 0, 0},
  {"flatten!", PF_ARRAY | PF_MUT | PF_VAL_SELF, 0, 1, 0}, {"fill", PF_ARRAY | PF_MUT | PF_VAL_SELF, 1, 3, 0},
  {"fill", PF_ARRAY | PF_MUT | PF_VAL_SELF, 0, 2, 1},
  {"to_ary", PF_ARRAY, 0, 0, 0}, {"transpose", PF_ARRAY, 0, 0, 0},
  /* and the Enumerable names that had no arm at all */
  {"grep", PF_ENUM, 1, 1, -1}, {"minmax_by", PF_ENUM, 0, 0, 1},
  /* The String mutators String alone owns. */
  {"setbyte", PF_STRING | PF_MUT, 2, 2, 0}, {"scrub!", PF_STRING | PF_MUT | PF_VAL_SELF | PF_SAME_OK | PF_STR_TYPED | PF_STR_SELF, 0, 1, -1},
  {"bytesplice", PF_STRING | PF_MUT | PF_VAL_SELF | PF_ENC, 2, 5, 0}, {"append_as_bytes", PF_STRING | PF_MUT | PF_VAL_SELF, 0, -1, 0},
  /* The names String and Array share: the receiver's run-time kind picks
     the arm. concat's arguments are of the receiver's own kind in CRuby
     (a String concatenates Strings, an Array Arrays), so an argument the
     inference has typed as the other kind rules that arm out. */
  {"concat", PF_STRING | PF_MUT | PF_ARGS_OWN | PF_VAL_SELF, 0, -1, 0}, {"concat", PF_ARRAY | PF_MUT | PF_VAL_SELF | PF_ARGS_OWN, 0, -1, 0},
  {"prepend", PF_STRING | PF_MUT | PF_ARGS_OWN | PF_VAL_SELF, 0, -1, 0}, {"prepend", PF_ARRAY | PF_MUT | PF_VAL_SELF, 0, -1, 0},
  {"reverse!", PF_STRING | PF_MUT | PF_VAL_SELF | PF_STR_TYPED | PF_STR_SELF, 0, 0, 0}, {"reverse!", PF_ARRAY | PF_MUT | PF_VAL_SELF, 0, 0, 0},
  {"slice!", PF_STRING | PF_MUT, 1, 2, 0}, {"slice!", PF_ARRAY | PF_MUT, 1, 2, 0},
  /* The Hash mutators, on a Hash at run time; a typed variant takes the
     result back from the general copy it was normalized to, and the value is
     the box, since the copy is detached once written back. Their arguments
     are Hashes. */
  {"merge!", PF_HASH | PF_MUT | PF_VAL_SELF, 0, 0, -1}, {"merge!", PF_HASH | PF_MUT | PF_VAL_SELF | PF_ARGS_OWN, 1, -1, 0},
  {"update", PF_HASH | PF_MUT | PF_VAL_SELF, 0, 0, -1}, {"update", PF_HASH | PF_MUT | PF_VAL_SELF | PF_ARGS_OWN, 1, -1, 0},
  /* The names Array and Hash share: the receiver's run-time kind picks the
     arm. The in-place filters take their block; of the blockless names,
     assoc, rassoc and fetch_values keep their last-resort Hash rows below, so
     a splat or a block still reaches the read-only face; compact! never had
     one. */
  {"select!", PF_ARRAY | PF_MUT | PF_VAL_SELF, 0, 0, 1}, {"select!", PF_HASH | PF_MUT | PF_VAL_SELF, 0, 0, 1},
  {"filter!", PF_ARRAY | PF_MUT | PF_VAL_SELF, 0, 0, 1}, {"filter!", PF_HASH | PF_MUT | PF_VAL_SELF, 0, 0, 1},
  {"reject!", PF_ARRAY | PF_MUT | PF_VAL_SELF, 0, 0, 1}, {"reject!", PF_HASH | PF_MUT | PF_VAL_SELF, 0, 0, 1},
  {"keep_if", PF_ARRAY | PF_MUT | PF_VAL_SELF, 0, 0, 1}, {"keep_if", PF_HASH | PF_MUT | PF_VAL_SELF, 0, 0, 1},
  {"delete_if", PF_ARRAY | PF_MUT | PF_VAL_SELF, 0, 0, 1}, {"delete_if", PF_HASH | PF_MUT | PF_VAL_SELF, 0, 0, 1},
  {"compact!", PF_ARRAY | PF_MUT | PF_VAL_SELF, 0, 0, 0}, {"compact!", PF_HASH | PF_MUT | PF_VAL_SELF, 0, 0, 0},
  /* The Hash mutators that were not rows and raised NoMethodError through
     a box. The setters answer their argument the way the typed emitter
     renders them, `default_proc=` the receiver. `shift` and `replace` are
     names an Array (and a String) own too, and stay with the runtime
     helpers that dispatch on the box's kind (sp_poly_shift,
     sp_poly_replace_any); the bang transforms are `replace` calls by the
     time codegen sees them (analyze.c), so they reach it too. */
  {"rehash", PF_HASH | PF_MUT | PF_VAL_SELF, 0, 0, 0},
  {"default=", PF_HASH | PF_MUT, 1, 1, 0}, {"default_proc=", PF_HASH | PF_MUT | PF_VAL_SELF, 1, 1, 0},
  {"assoc", PF_ARRAY, 1, 1, 0}, {"assoc", PF_HASH, 1, 1, 0},
  {"rassoc", PF_ARRAY, 1, 1, 0}, {"rassoc", PF_HASH, 1, 1, 0},
  {"fetch_values", PF_ARRAY, 0, 0, -1}, {"fetch_values", PF_ARRAY, 1, -1, 0},
  {"fetch_values", PF_HASH, 0, 0, -1}, {"fetch_values", PF_HASH, 1, -1, 0},
  /* fetch's block form: the dispatch that serves the argument forms has no
     block to call on a miss */
  {"fetch", PF_ARRAY, 1, 2, 1}, {"fetch", PF_HASH, 1, 2, 1},
  /* The read-only Hash/Enumerable face. */
  {"dig", PF_HASH | PF_LAST, 0, -1, -1}, {"value?", PF_HASH | PF_LAST, 0, -1, -1}, {"has_value?", PF_HASH | PF_LAST, 0, -1, -1},
  {"invert", PF_HASH | PF_LAST, 0, -1, -1}, {"assoc", PF_HASH | PF_LAST, 0, -1, -1}, {"rassoc", PF_HASH | PF_LAST, 0, -1, -1}, {"key", PF_HASH | PF_LAST, 0, -1, -1},
  {"filter_map", PF_HASH | PF_LAST, 0, -1, -1}, {"each_with_object", PF_HASH | PF_LAST, 0, -1, -1},
  {"group_by", PF_HASH | PF_LAST, 0, -1, -1}, {"partition", PF_HASH | PF_LAST, 0, -1, -1}, {"zip", PF_HASH | PF_LAST, 0, -1, -1},
  {"reduce", PF_HASH | PF_LAST, 0, -1, -1}, {"inject", PF_HASH | PF_LAST, 0, -1, -1}, {"find_all", PF_HASH | PF_LAST, 0, -1, -1},
  {"take", PF_HASH | PF_LAST, 0, -1, -1}, {"drop", PF_HASH | PF_LAST, 0, -1, -1}, {"select", PF_HASH | PF_LAST, 0, -1, -1},
  {"filter", PF_HASH | PF_LAST, 0, -1, -1}, {"reject", PF_HASH | PF_LAST, 0, -1, -1}, {"compact", PF_HASH | PF_LAST, 0, -1, -1},
  {"slice", PF_HASH | PF_LAST, 0, -1, -1}, {"except", PF_HASH | PF_LAST, 0, -1, -1}, {"values_at", PF_HASH | PF_LAST, 0, -1, -1},
  {"fetch_values", PF_HASH | PF_LAST, 0, -1, -1}, {"entries", PF_HASH | PF_LAST, 0, -1, -1}, {"flatten", PF_HASH | PF_LAST, 0, -1, -1},
  {"first", PF_HASH | PF_LAST, 0, -1, -1}, {"each_pair", PF_HASH | PF_LAST, 0, -1, -1}, {"each_key", PF_HASH | PF_LAST, 0, -1, -1},
  {"each_value", PF_HASH | PF_LAST, 0, -1, -1}, {"transform_values", PF_HASH | PF_LAST, 0, -1, -1},
  {"transform_keys", PF_HASH | PF_LAST, 0, -1, -1}, {"tally", PF_HASH | PF_LAST, 0, -1, -1},
  {"chunk_while", PF_HASH | PF_LAST, 0, -1, -1}, {"flat_map", PF_HASH | PF_LAST, 0, -1, -1},
  {"collect_concat", PF_HASH | PF_LAST, 0, -1, -1}, {"none?", PF_HASH | PF_LAST, 0, -1, -1}, {"one?", PF_HASH | PF_LAST, 0, -1, -1},
  /* the readers of a hash's default, and deconstruct_keys */
  {"deconstruct_keys", PF_HASH | PF_LAST, 0, -1, -1}, {"default", PF_HASH | PF_LAST, 0, -1, -1}, {"default_proc", PF_HASH | PF_LAST, 0, -1, -1},
  /* The zero-argument forms of names whose boxed arms were written for one
     argument or more: the typed emitter of each owner answers them, so a
     row sends the box there (a Hash's merge, an Array's values_at / union /
     difference / intersection / zip, the blockless index family and
     sort_by, slice, the String's start_with? / end_with? / dedup / encode!). */
  {"merge", PF_HASH, 0, 0, -1},
  {"values_at", PF_ARRAY, 0, 0, 0}, {"values_at", PF_HASH, 0, 0, 0},
  {"union", PF_ARRAY, 0, 0, 0}, {"difference", PF_ARRAY, 0, 0, 0}, {"intersection", PF_ARRAY, 0, 0, 0},
  {"zip", PF_ARRAY | PF_HASH | PF_RANGE, 0, 0, 0},
  {"index", PF_ARRAY | PF_STRING, 0, 0, 0}, {"rindex", PF_ARRAY | PF_STRING, 0, 0, 0},
  {"find_index", PF_ARRAY | PF_HASH, 0, 0, 0}, {"bsearch", PF_ARRAY, 0, 0, 0},
  {"sort_by", PF_ARRAY | PF_HASH | PF_RANGE, 0, 0, 0},
  {"slice", PF_ARRAY | PF_STRING | PF_HASH, 0, 0, 0},
  {"start_with?", PF_STRING, 0, 0, 0}, {"end_with?", PF_STRING, 0, 0, 0},
  {"dedup", PF_STRING, 0, 0, 0}, {"encode!", PF_STRING | PF_MUT | PF_VAL_SELF, 0, 0, 0},
  {0, 0, 0, 0, 0}
};
static int face_row_matches(const PolyFace *r, int argc, int has_blk, int plain) {
  if (!plain) return r->argc_min == 0 && r->argc_max < 0 && r->blk != 0;
  if (argc < r->argc_min || (r->argc_max >= 0 && argc > r->argc_max)) return 0;
  return r->blk < 0 || r->blk == (has_blk != 0);
}

unsigned ty_poly_face_owners(const char *name, int argc, int has_blk, int plain, int with_last) {
  unsigned own = 0;
  if (!name) return 0;
  for (const PolyFace *r = ty_poly_face_tbl; r->name; r++) {
    if (!sp_streq(name, r->name)) continue;
    if (!with_last && (r->flags & PF_LAST)) continue;
    if (!face_row_matches(r, argc, has_blk, plain)) continue;
    own |= r->flags;
  }
  return own;
}
unsigned ty_poly_face_owner_flags(const char *name, int argc, int has_blk, int plain, unsigned owner) {
  unsigned fl = 0;
  if (!name) return 0;
  for (const PolyFace *r = ty_poly_face_tbl; r->name; r++) {
    if (!(r->flags & owner) || !sp_streq(name, r->name)) continue;
    if (!face_row_matches(r, argc, has_blk, plain)) continue;
    fl |= r->flags;
  }
  return fl;
}
unsigned ty_str_bang_flags(const char *name) {
  if (!name) return 0;
  for (const PolyFace *r = ty_poly_face_tbl; r->name; r++)
    if ((r->flags & PF_STR_BANG) && sp_streq(name, r->name))
      return r->flags & (PF_STR_BANG | PF_STR_SELF);
  return 0;
}
unsigned ty_str_typed_bang_flags(const char *name) {
  if (!name) return 0;
  /* reverse! and scrub! are String bangs too, answering self, but their
     face rows are the mutator kind's (PF_MUT | PF_VAL_SELF): Array has
     reverse! as well, so a boxed receiver's is either, and answers poly */
  for (const PolyFace *r = ty_poly_face_tbl; r->name; r++)
    if ((r->flags & (PF_STR_BANG | PF_STR_TYPED)) && sp_streq(name, r->name))
      return PF_STR_BANG | (r->flags & (PF_STR_SELF | PF_SAME_OK));
  return 0;
}
void str_bang_plain(const char *bang, char *out, int n) {
  size_t len = bang ? strlen(bang) : 0;
  if (len && bang[len - 1] == '!') len--;
  if (n <= 0) return;
  if (len >= (size_t)n) len = (size_t)n - 1;
  if (len) memcpy(out, bang, len);
  out[len] = 0;
}
int ty_poly_hash_face_name(const char *nm) {
  if (!nm) return 0;
  for (const PolyFace *r = ty_poly_face_tbl; r->name; r++)
    if ((r->flags & PF_HASH) && (r->flags & PF_LAST) && sp_streq(nm, r->name)) return 1;
  return 0;
}

const char *ty_name(TyKind t) {
  switch (t) {
    case TY_UNKNOWN: return "unknown";
    case TY_VOID:    return "void";
    case TY_NIL:     return "nil";
    case TY_INT:     return "int";
    case TY_BIGINT:  return "bigint";
    case TY_FLOAT:   return "float";
    case TY_STRING:  return "string";
    case TY_STRBUF:  return "strbuf";
    case TY_SYMBOL:  return "symbol";
    case TY_BOOL:    return "bool";
    case TY_RANGE:   return "range";
    case TY_FLOAT_RANGE: return "float_range";
    case TY_TIME:    return "time";
    case TY_COMPLEX: return "complex";
    case TY_RATIONAL: return "rational";
    case TY_MATCHDATA: return "matchdata";
    case TY_REGEX:     return "regex";
    case TY_EXCEPTION: return "exception";
    case TY_INT_ARRAY:   return "int_array";
    case TY_FLOAT_ARRAY: return "float_array";
    case TY_STR_ARRAY:   return "str_array";
    case TY_POLY_ARRAY:  return "poly_array";
    case TY_INT_ARRAY_ARRAY: return "int_array_array";
    case TY_FLOAT_ARRAY_ARRAY: return "float_array_array";
    case TY_STR_INT_HASH: return "str_int_hash";
    case TY_STR_STR_HASH: return "str_str_hash";
    case TY_INT_INT_HASH: return "int_int_hash";
    case TY_INT_STR_HASH: return "int_str_hash";
    case TY_SYM_POLY_HASH:  return "sym_poly_hash";
    case TY_STR_POLY_HASH:  return "str_poly_hash";
    case TY_POLY_POLY_HASH: return "poly_poly_hash";
    case TY_PROC:    return "proc";
    case TY_CURRY:   return "curry";
    case TY_FIBER:   return "fiber";
    case TY_THREAD:  return "thread";
    case TY_QUEUE:   return "queue";
    case TY_MUTEX:   return "mutex";
    case TY_CONDVAR: return "condvar";
    case TY_RANDOM:  return "random";
    case TY_DIR:     return "dir";
    case TY_ADDRINFO: return "addrinfo";
    case TY_SOCKOPT: return "sockopt";
    case TY_STR_RANGE: return "str_range";
    case TY_TIME_RANGE: return "time_range";
    case TY_OBJ_RANGE: return "obj_range";
    case TY_TMS:     return "tms";
    case TY_PROCESS_STATUS: return "process_status";
    case TY_OPENSTRUCT: return "openstruct";
    case TY_METHOD:  return "method";
    case TY_IO:      return "io";
    case TY_ARGF:    return "argf";
    case TY_ENUMERATOR: return "enumerator";
    case TY_CLASS:   return "class";
    case TY_POLY:    return "poly";
  }
  if (ty_is_obj_array(t)) return "obj_array";
  return "?";
}

static const struct { TyKind kind, key, val; const char *cname; } hash_tbl[] = {
  {TY_STR_INT_HASH, TY_STRING, TY_INT,    "StrInt"},
  {TY_STR_STR_HASH, TY_STRING, TY_STRING, "StrStr"},
  {TY_INT_INT_HASH, TY_INT,    TY_INT,    "IntInt"},
  {TY_INT_STR_HASH, TY_INT,    TY_STRING, "IntStr"},
  {TY_SYM_POLY_HASH,  TY_SYMBOL, TY_POLY, "SymPoly"},
  {TY_STR_POLY_HASH,  TY_STRING, TY_POLY, "StrPoly"},
  {TY_POLY_POLY_HASH, TY_POLY,   TY_POLY, "PolyPoly"},
};

int ty_is_hash(TyKind t) {
  for (unsigned i = 0; i < sizeof hash_tbl / sizeof hash_tbl[0]; i++)
    if (hash_tbl[i].kind == t) return 1;
  return 0;
}
TyKind ty_hash_of(TyKind key, TyKind val) {
  /* Under the one-String mode a String handle keys and fills the same Hash
     kinds as the bytes: the typed containers hold the handle
     (string-one-repr-design.md). */
  if (key == TY_STRBUF) key = TY_STRING;
  if (val == TY_STRBUF) val = TY_STRING;
  for (unsigned i = 0; i < sizeof hash_tbl / sizeof hash_tbl[0]; i++)
    if (hash_tbl[i].key == key && hash_tbl[i].val == val) return hash_tbl[i].kind;
  return TY_UNKNOWN;
}
TyKind ty_hash_key(TyKind h) {
  for (unsigned i = 0; i < sizeof hash_tbl / sizeof hash_tbl[0]; i++)
    if (hash_tbl[i].kind == h) return hash_tbl[i].key;
  return TY_UNKNOWN;
}
TyKind ty_hash_val(TyKind h) {
  for (unsigned i = 0; i < sizeof hash_tbl / sizeof hash_tbl[0]; i++)
    if (hash_tbl[i].kind == h) return hash_tbl[i].val;
  return TY_UNKNOWN;
}
const char *ty_hash_cname(TyKind h) {
  for (unsigned i = 0; i < sizeof hash_tbl / sizeof hash_tbl[0]; i++)
    if (hash_tbl[i].kind == h) return hash_tbl[i].cname;
  return NULL;
}

int ty_is_numeric(TyKind t) { return t == TY_INT || t == TY_BIGINT || t == TY_FLOAT; }
/* Kinds that can never answer #call: composing one is CRuby's TypeError
   (callable object is expected). A user object stays out -- it may define
   call -- as do poly/unknown, whose value is decided at run time. */
int ty_never_callable(TyKind t) {
  return t == TY_INT || t == TY_FLOAT || t == TY_BIGINT || t == TY_RATIONAL ||
         t == TY_COMPLEX || t == TY_STRING || t == TY_STRBUF ||
         t == TY_SYMBOL || t == TY_BOOL ||
         t == TY_NIL || t == TY_RANGE || t == TY_FLOAT_RANGE ||
         t == TY_STR_RANGE || t == TY_TIME_RANGE || t == TY_OBJ_RANGE || t == TY_REGEX || t == TY_TIME ||
         t == TY_MATCHDATA || t == TY_EXCEPTION || t == TY_IO ||
         t == TY_ENUMERATOR || ty_is_array(t) || ty_is_obj_array(t) ||
         ty_is_hash(t);
}
int ty_is_array(TyKind t) {
  return t == TY_INT_ARRAY || t == TY_FLOAT_ARRAY ||
         t == TY_STR_ARRAY || t == TY_POLY_ARRAY || t == TY_INT_ARRAY_ARRAY ||
         t == TY_FLOAT_ARRAY_ARRAY;
}
int array_new_copies(TyKind t) {
  return t == TY_INT_ARRAY || t == TY_FLOAT_ARRAY || t == TY_STR_ARRAY || t == TY_POLY_ARRAY;
}
TyKind ty_array_of(TyKind elem) {
  switch (elem) {
    case TY_INT:    return TY_INT_ARRAY;
    case TY_FLOAT:  return TY_FLOAT_ARRAY;
    case TY_STRING: return TY_STR_ARRAY;
    /* under the one-String mode the String Array holds the handle */
    case TY_STRBUF: return TY_STR_ARRAY;
    /* An element type that is not yet known is not the same as an element
       type that is anything: while the fixpoint runs, keep the array at
       bottom so the slots it flows into wait for the real element type
       instead of locking onto the poly array. Producers reach this from
       every direction -- a literal whose elements are calls not yet typed,
       a `map` whose block return is not yet typed -- so the rule belongs
       here rather than at each of them. See g_infer_optimistic. */
    case TY_UNKNOWN: return g_infer_optimistic ? TY_UNKNOWN : TY_POLY_ARRAY;
    default:        return TY_POLY_ARRAY;
  }
}
TyKind ty_array_elem(TyKind arr) {
  switch (arr) {
    case TY_INT_ARRAY:       return TY_INT;
    case TY_FLOAT_ARRAY:     return TY_FLOAT;
    case TY_STR_ARRAY:       return TY_STRING;
    case TY_INT_ARRAY_ARRAY: return TY_INT_ARRAY;
    case TY_FLOAT_ARRAY_ARRAY: return TY_FLOAT_ARRAY;
    default:
      /* a narrowed object array holds one class, unboxed (#4846) */
      if (ty_is_obj_array(arr)) return ty_object(ty_obj_array_class(arr));
      return TY_POLY;
  }
}

/* A builtin whose result follows its receiver's kind, called on a yield
   whose block answers different kinds at different call sites
   (`def w = yield.first` with an Integer Array block at one site and a
   String Array one at another). The inlined call is lowered per site from
   that site's receiver, so it emits an sp_int at one and a const char * at
   the other, while the slot it lands in was typed from one site only: the C
   did not compile, or a Float site's 1.5 was stored into an sp_int as 1.
   This names, per receiver kind, what that lowering produces -- the same
   type the analyzer answers for the call on a receiver of that kind alone
   (`[1.5].first` is a Float, `["a"].sort` a String Array) -- for the kinds
   whose lowering is a plain value of one kind. Two families: the element
   readers answer the array's element, and the copies and reorderings answer
   the receiver's own kind. A pair left out here keeps the yield's single
   typing, as before. `dig` with one index answers the array's element --
   same as `[]` -- since `builtin_ops.c`'s own `BOP_ANY_ARRAY "dig" 1 1` row
   lowers it to the same nilable-aware getter (`sp_$AArray_get$O`) `[]`
   uses, confirmed with `--emit-types` on a direct (non-yield) `arr.dig(0)`:
   `int`, `nullable: true`, not poly. Only `dig` with two or more indices
   (that row's `2 127` sibling) is poly, since a second level of navigation
   is not guaranteed to be a plain scalar.
   `promote` (the `--int-overflow=promote` flag) answers a different type
   for `succ`'s Integer site, and an Integer Array's `sum`, than the
   default (raise) mode: a receiver that is not a known constant promotes
   past the word to a boxed Bignum, which the non-yield path already does
   (`infer_numeric_call`'s `succ`/`next`/`abs` arm) and a yield's receiver,
   never a compile-time constant, always takes. `succ` on a String or
   Symbol site, and `sum` on a Float or String Array site, are unaffected
   by `promote` -- confirmed each with `--emit-types` on a direct call
   before writing its arm, not assumed from the pattern the Integer cases
   follow. `sum` itself is answered by `fold_sum_type` (below), already
   shaped for this: a Float or String Array's `sum` answers poly in BOTH
   modes (the default Integer(0) seed's mismatch with a non-Integer
   element, not a promote-mode rule -- a String Array's boxed path raises
   CRuby's TypeError at runtime, same as the direct, non-yield call). */
static int ty_recv_is_plain_array(TyKind t) {
  return t == TY_INT_ARRAY || t == TY_FLOAT_ARRAY || t == TY_STR_ARRAY;
}
/* bop_find_arg's BopArgKind callback: ud points at a two-element array
   holding arg0 and arg1, so a row guarded on either argument's kind
   (bop_find_stage checks both) can still be matched -- a lookup that
   only ever supplied arg0 found the first unguarded row instead of a
   row guarded on arg1, e.g. a two-argument IO#set_encoding row. */
static TyKind bop_arg01_kind(const void *ud, int i) {
  return i == 0 || i == 1 ? ((const TyKind *)ud)[i] : TY_UNKNOWN;
}
/* g_promote_mode gates a scattered, open-ended set of arms across
   analyze_infer.c/analyze.c for a numeric scalar receiver -- pred, the
   div/divmod/modulo family (Integer with a Float or poly argument, and
   Float's own div), pow, to_i, <<, and others, found piecemeal rather
   than from one table. An earlier version of this guard denylisted
   names by hand and missed pred and the div family outright, a fresh
   review found the miss by running those exact calls through a yield
   receiver under --int-overflow=promote and comparing to CRuby directly
   (four programs that matched CRuby on the unfixed compiler started
   raising RangeError once this generic path started answering them with
   the row's plain, non-promoting result). A name denylist cannot be
   trusted to enumerate every such arm as new ones are added elsewhere;
   gating by receiver kind instead is structural and can't miss one:
   none of the names this generic path was written to answer
   (sort!/reverse!/rotate/shuffle/sample/take/drop/first(n)/last(n)/fetch)
   ever operate on a scalar numeric receiver, and clamp -- the only
   numeric-receiver name reaching this function at all -- is answered by
   its own hand-written arm above, which always returns before this
   check runs. Excluding every numeric scalar receiver under promote
   mode here therefore loses nothing this generic path exists for. */
static int bop_result_excluded_for_promote(TyKind recv, int promote) {
  return promote && (recv == TY_INT || recv == TY_FLOAT || recv == TY_BIGINT);
}
/* The names the generic bop_rows fallback below may answer for -- each
   verified with its own RED (compile failure on differing-kind yield
   sites) then GREEN test, not merely "the table has a concrete row for
   it". A name not on this list never reaches the fallback, however
   concrete its own bop_rows row looks; see the fallback's own comment
   for why that distinction matters (inspect has a concrete row too, and
   broke a real program once let through unverified). */
static int bop_result_fallback_allowed(const char *name) {
  return !strcmp(name, "sort!") || !strcmp(name, "reverse!") ||
         !strcmp(name, "rotate") || !strcmp(name, "shuffle") ||
         !strcmp(name, "first") || !strcmp(name, "last") ||
         !strcmp(name, "take") || !strcmp(name, "drop") ||
         !strcmp(name, "fetch") || !strcmp(name, "sample") ||
         /* K-033: each verified with its own RED/GREEN test, same as above.
            dig and fetch(1,2) stay OFF this list on purpose -- Hash's own
            bop_rows row for either is a placeholder TY_UNKNOWN, not yet a
            real answer, so adding them here would change nothing (bop_result
            below declines a TY_UNKNOWN row) and falsely suggest they were
            verified. */
         !strcmp(name, "[]") || !strcmp(name, "slice") ||
         !strcmp(name, "delete") || !strcmp(name, "delete_at") ||
         !strcmp(name, "assoc") || !strcmp(name, "rassoc") ||
         !strcmp(name, "values_at") || !strcmp(name, "pop") ||
         !strcmp(name, "shift") ||
         /* K-034: the one-argument forms of names whose 0-arg form is
            already answered above (by yield_site_listed, not this
            allowlist) -- flatten(n)/max(n)/min(n) each have their own
            concrete row for argc 0..1 (BOPR_SELF/BOPE_ARRAY_NMIN), and
            insert(n) (one index, no values to insert) is BOPR_SELF, "self,
            or FrozenError" -- none of the four was ever on this allowlist,
            so the argument form fell through unanswered even though the
            0-arg form already worked. A Hash receiver's own row for
            max(n)/min(n) stays a TY_UNKNOWN placeholder (same as the 0-arg
            form before the top-of-function redirect answers it) and Hash
            has no #insert at all, so this allowlist addition only ever
            resolves a plain-Array site for these two; a Hash site among
            them is left exactly as unanswered as before. */
         !strcmp(name, "flatten") || !strcmp(name, "max") || !strcmp(name, "min") ||
         !strcmp(name, "insert") || !strcmp(name, "prepend") ||
         /* K-035: slice!(start, length)'s two-argument form -- String and
            Array both have a concrete row for it (BOPE_STRING_SLICE /
            BOPR_SELF), same shape as "slice" above, just never added
            alongside it. A String site mixed with Array ones compiles now,
            but on a literal still hits #2332's frozen-literal limitation at
            runtime, same as every other mutator this family has fixed. */
         !strcmp(name, "slice!") ||
         /* K-036: rotate!(n) -- its own row is BOPR_SELF for a plain Array
            at every argc (0..127, same row as the 0-arg form yield_site_
            listed now answers), but the 0-arg form's success does not
            reach this argc>=1 allowlist on its own; verified with its own
            RED (differing-kind sites, raw C pointer-type error) then
            GREEN test, same as every other name here. */
         !strcmp(name, "rotate!");
}
/* Whether a yield site answers `name` on a receiver of kind recv from the
   builtin-op rows rather than from a rule of its own: 1 for a listed pair,
   0 for a listed name whose receiver is not one of its kinds (the site keeps
   the yield's single typing), -1 for a name not listed. This list, not the
   rows, is the set of pairs a yield may widen to poly: the rows answer
   kinds beyond these (a poly Array's `first`, Complex#abs), and widening
   those sites would change what is emitted. Every listed pair has a row
   that states its kind. */
static int yield_site_listed(const char *name, TyKind recv) {
  int num = recv == TY_INT || recv == TY_FLOAT;
  if (!strcmp(name, "abs") || !strcmp(name, "magnitude")) return num;
  /* String#-@/#+@ are the deduplicated-frozen and unfrozen-copy String
     respectively, so a String site belongs to both too; a String has no
     abs or magnitude. +@ was never listed here at all before K-035 (its
     own bop_rows row was also a TY_UNKNOWN placeholder, fixed alongside
     this), unlike -@ which already was. */
  if (!strcmp(name, "-@") || !strcmp(name, "+@")) return num || recv == TY_STRING;
  /* itself alone also takes a Hash site (K-033) -- dup/clone already have
     a concrete Hash row too, but widening them here is untested outside a
     direct call and out of this fix's verified scope; freeze's Hash row is
     a mutation (sp_gc_freeze), not a pure answer, and was never part of
     K-033's finding either, so neither joins ty_is_hash(recv) here. */
  if (!strcmp(name, "itself"))
    return num || recv == TY_STRING || recv == TY_SYMBOL || ty_recv_is_plain_array(recv) ||
           ty_is_hash(recv);
  if (!strcmp(name, "dup") || !strcmp(name, "clone") || !strcmp(name, "freeze"))
    return num || recv == TY_STRING || recv == TY_SYMBOL || ty_recv_is_plain_array(recv);
  /* capitalize/downcase/upcase/swapcase: String and Symbol both have a
     concrete own-kind row (K-033). Before this entry, neither site was
     ever answered here, so the whole yield stayed unwidened and the
     second site's value got forced through the first site's own concrete
     representation (SP_STR_B on a freshly re-interned Symbol, reading as
     NULL) -- a silently wrong value, not a compile failure, because
     String's and Symbol's C representations both happen to coexist inside
     the same narrow (non-poly) slot without the compiler ever noticing
     the clash. Listing both here lets yield_recv_builtin_every_site see
     every site answered and widen properly, the same fix shape as itself
     above, this time for two kinds that were never touched by this table
     at all rather than one kind (Hash) missing from an existing list. */
  if (!strcmp(name, "capitalize") || !strcmp(name, "downcase") ||
      !strcmp(name, "upcase") || !strcmp(name, "swapcase"))
    return recv == TY_STRING || recv == TY_SYMBOL;
  if (!strcmp(name, "sort") || !strcmp(name, "reverse") || !strcmp(name, "uniq") ||
      !strcmp(name, "last") || !strcmp(name, "min") || !strcmp(name, "max") ||
      !strcmp(name, "pop"))
    return ty_recv_is_plain_array(recv);
  /* rotate!: its own row is already BOPR_SELF for a plain Array (every
     argc, unlike push/append/unshift below) -- never gated here before
     (kilau K-036), so a bare `yield.rotate!` across differing Array-kind
     sites failed to compile with a raw C pointer-type error, the same
     shape minmax had in K-033. push/append/unshift are NOT listed here:
     their row is TY_UNKNOWN on purpose (a direct, non-yield multi-arg
     push of a mismatched-kind element, e.g. an IntArray's push("s"),
     relies on that placeholder to answer poly rather than the receiver's
     narrow kind -- confirmed by a real regression, test/
     array_push_lift_splat.rb, when this fix first tried widening their
     row to BOPR_SELF instead of adding a dedicated 0-arg arm below). */
  if (!strcmp(name, "rotate!")) return ty_recv_is_plain_array(recv);
  /* first/shift's own row states a Hash kind too (the [key, value] pair --
     K-033/kilau), unlike min/max/uniq/minmax above, whose Hash row is a
     placeholder TY_UNKNOWN (bop_result below declines it, so listing Hash
     here for those would still answer nothing -- only a name whose row is
     concrete for Hash belongs on this side of the list). */
  if (!strcmp(name, "first") || !strcmp(name, "shift"))
    return ty_recv_is_plain_array(recv) || ty_is_hash(recv);
  /* clear: BOPR_SELF for a plain Array (K-033) AND for a String (#2332),
     AND BOPR_SELF for a Hash (K-033) -- the only name in this family whose
     row is concrete for all three. K-033's own fix added the Hash case but
     missed that String answers #clear too (kilau K-034): a site mix
     including a String failed to compile the same way a Hash-less mix did
     before K-033. flatten's own row is TY_POLY_ARRAY, concrete for a plain
     Array and a Hash (K-033), but String has no #flatten. */
  if (!strcmp(name, "clear"))
    return ty_recv_is_plain_array(recv) || ty_is_hash(recv) || recv == TY_STRING;
  if (!strcmp(name, "flatten")) return ty_recv_is_plain_array(recv) || ty_is_hash(recv);
  /* compact/to_a: each has its own concrete Hash row (BOPE_HASH_COMPACT,
     BOPE_HASH_TO_A), same shape as clear/flatten above, just never gated
     for a Hash receiver until now (kilau K-034). */
  if (!strcmp(name, "compact") || !strcmp(name, "to_a"))
    return ty_recv_is_plain_array(recv) || ty_is_hash(recv);
  /* entries/fetch_values: concrete for both a plain Array and a Hash (the
     only two receiver kinds either name answers on at all); never gated
     here before K-034. */
  if (!strcmp(name, "entries") || !strcmp(name, "fetch_values"))
    return ty_recv_is_plain_array(recv) || ty_is_hash(recv);
  /* concat: BOPR_SELF for a plain Array, and (#2309) for a String too --
     neither was ever gated here before K-034, so even two plain Array
     sites of differing element kind failed to compile. */
  if (!strcmp(name, "concat")) return ty_recv_is_plain_array(recv) || recv == TY_STRING;
  /* deconstruct/difference/intersection/to_ary/union: each exists only on
     Array among this table's kinds (no String, Symbol or Hash row) --
     never gated here before K-034, same "nobody tried the base case" gap
     minmax turned out to have in K-033. */
  if (!strcmp(name, "deconstruct") || !strcmp(name, "difference") ||
      !strcmp(name, "intersection") || !strcmp(name, "to_ary") || !strcmp(name, "union"))
    return ty_recv_is_plain_array(recv);
  /* minmax was never listed here at all (K-033) -- an Int Array site beside
     a String Array one failed to compile even with no Hash anywhere in the
     picture, a plainer bug than the Hash-specific one below. */
  if (!strcmp(name, "minmax")) return ty_recv_is_plain_array(recv);
  return -1;
}
int ty_recv_builtin_result(const char *name, int argc, TyKind arg0, TyKind arg1,
                            int range_has_float_bound, TyKind recv, int promote, TyKind *out,
                            const struct BuiltinOp **row) {
  if (!name) return 0;
  if (row) *row = NULL;
  if (argc == 0) {
    /* min/max/uniq on a Hash site: the normal (non-yield) call path already
       answers these correctly (hash_enum_redispatch, analyze_infer.c) by
       treating the Hash's materialized [key, value] pairs as a Poly Array
       -- min/max answer one pair (poly), uniq answers an array of pairs
       (poly array). This table is only ever consulted for a BLOCKLESS call
       (yield_builtin_method_site_type's own guard), so the blockless-
       always-redispatches condition hash_enum_redispatch checks for these
       names is unconditionally true here; it is reproduced inline rather
       than called (it also takes a node id for the block-form names, which
       do not reach this table). The row is looked up in TY_POLY_ARRAY's own
       family, and `recv` passed to bop_result is TY_POLY_ARRAY too (not the
       real Hash kind) -- BOPR_ELEM/BOPR_SELF read it as "the poly array's
       own element/self", exactly the answer a materialized pair array would
       give; min/max/uniq's own BOP_ANY_HASH row stays a TY_UNKNOWN
       placeholder on purpose; it isn't reached here at all, so no row's
       result is misread (K-033).

       minmax deliberately excluded (confirmed empirically, not assumed from
       the pattern the other three follow): a Hash site beside an Array site
       compiles and matches CRuby for min/max/uniq, each verified with their
       own inlined-per-call-site RED/GREEN test, but minmax hits a separate,
       pre-existing boxing gap in the method's own (non-inlined) compiled
       body -- "incompatible pointer types assigning to 'sp_PolyArray *'
       from 'sp_IntArray *'" at the method's own `yield.minmax` line, not at
       either call site. Left as a documented limitation rather than papered
       over; the root cause is in the per-site boxing emitter this fix does
       not touch, not in this table. */
    if (ty_is_hash(recv) &&
        (!strcmp(name, "min") || !strcmp(name, "max") || !strcmp(name, "uniq") ||
         !strcmp(name, "minmax"))) {
      const BuiltinOp *op = bop_find(bop_family_of(TY_POLY_ARRAY), name, 0, 0);
      TyKind r = op ? bop_result(op, TY_POLY_ARRAY) : TY_UNKNOWN;
      if (r == TY_UNKNOWN) return 0;
      *out = r; if (row) *row = op;
      return 1;
    }
    /* A listed name answers from its row: the receiver's own kind for the
       copies, the identities and the reorderings, the element for the
       element readers. */
    int listed = yield_site_listed(name, recv);
    if (listed == 0) return 0;
    if (listed > 0) {
      const BuiltinOp *op = bop_find(bop_family_of(recv), name, 0, 0);
      TyKind r = op ? bop_result(op, recv) : TY_UNKNOWN;
      if (r == TY_UNKNOWN) return 0;
      *out = r; if (row) *row = op;
      return 1;
    }
    /* 0-arg push/append/unshift: each answers self (CRuby's own no-op
       push/append/unshift of nothing). Answered here by name, bypassing
       the row entirely -- their shared bop_rows row states TY_UNKNOWN on
       purpose, read by a direct (non-yield) multi-arg call of a
       mismatched-kind element (an IntArray's push("s")) to fall through
       to a poly answer there; widening that row to BOPR_SELF for this
       fix instead regressed test/array_push_lift_splat.rb, a real,
       unrelated direct call, so this arm leaves the row untouched and
       only ever answers the plain-Array 0-arg yield-receiver case
       (kilau K-036). */
    if (ty_recv_is_plain_array(recv) &&
        (!strcmp(name, "push") || !strcmp(name, "append") || !strcmp(name, "unshift"))) {
      *out = recv;
      return 1;
    }
    /* succ: a String or Symbol site always answers its own kind (String's
       and Symbol's own `succ` bop rows are a fixed result type, no
       receiver-value dependence). An Integer site answers Integer in raise
       (default) mode, poly under --int-overflow=promote -- a yield's
       receiver is never a known constant, so it always takes that mode's
       non-constant path (confirmed with --emit-types on a direct, non-yield
       call: an Integer local read back from an array answers poly under
       promote). next is succ's alias (Integer/String/Symbol each have their
       own separate bop_rows row for the name "next", not "succ" -- the
       `bop_find(recv, name, 0, 0)` below already looks up by `name`, so no
       change needed there); K-024/#9419 fixed 0-arg succ, including a
       Symbol arm, but never added the alias name itself (kilau K-034). */
    if (!strcmp(name, "succ") || !strcmp(name, "next")) {
      if (recv == TY_STRING) *out = TY_STRING;
      else if (recv == TY_SYMBOL) *out = TY_SYMBOL;
      else if (recv == TY_INT) *out = promote ? TY_POLY : TY_INT;
      else return 0;
      /* the kind's row, where it states that answer (not an Integer's under
         promote, which the row does not know) */
      const BuiltinOp *op = bop_find(recv, name, 0, 0);
      if (row && op && bop_result(op, recv) == *out) *row = op;
      return 1;
    }
    /* sum (no seed, no block -- the only shape a yield receiver's inlined
       call takes): fold_sum_type already names this exactly, with the
       default Integer(0) seed every plain kind folds from. An Integer Array
       answers Integer in raise mode and poly under promote (the same
       non-constant-receiver rule succ above follows); a Float Array answers
       poly in BOTH modes (0 + 1.5's seed/element mismatch, not a
       receiver-follows rule -- confirmed with --emit-types on a direct
       [1.5].sum, which is poly whether or not --int-overflow=promote is
       given); a String Array also answers poly, and its boxed path raises
       CRuby's TypeError at runtime (confirmed by running the compiled
       binary), so it is answered here rather than declined like a truly
       unresolvable pair. */
    if (!strcmp(name, "sum") && ty_recv_is_plain_array(recv)) {
      *out = fold_sum_type(TY_INT, ty_array_elem(recv), promote);
      /* the kind's row is the call's: it states every kind's answer but an
         Integer Array's, which it leaves to the mode (bop_result) */
      const BuiltinOp *op = bop_find(bop_family_of(recv), name, 0, 0);
      if (row && op && op->result == BOPR_ARRAY_SUM) *row = op;
      return 1;
    }
    /* No hand-written arm above matched this argc=0 name: fall through
       (instead of returning 0 here) to the generic bop_rows-backed check
       below, which also answers argc=0 names (sort!, reverse!, rotate,
       shuffle, sample). The dig/[] checks right after this block
       self-guard on argc>=1, so they simply no-op for an argc=0 name that
       reaches here. */
  }
  /* min(n)/max(n) on a Hash site: the one-argument form of the redirect
     above, needed separately because that block only runs for argc==0 and
     bop_result_fallback_allowed's own addition for this argument form
     (kilau K-034) only ever resolves a plain-Array site -- a Hash site in
     the same mix was left exactly as unanswered as the 0-arg form was
     before K-033's redirect, and an unanswered site keeps the whole
     method unwidened even though the Array sites alone would have been
     fine. Confirmed CRuby answers {a: 1}.max(2) as [[:a, 1]] (an array of
     up to n pairs), the same "Hash's pairs as a Poly Array" rule as the
     0-arg form, so the row is looked up the same way: in TY_POLY_ARRAY's
     own family at this argc, with `recv` passed to bop_result as
     TY_POLY_ARRAY rather than the real Hash kind. */
  if (argc == 1 && ty_is_hash(recv) && (!strcmp(name, "min") || !strcmp(name, "max"))) {
    const BuiltinOp *op = bop_find(bop_family_of(TY_POLY_ARRAY), name, 1, 0);
    TyKind r = op ? bop_result(op, TY_POLY_ARRAY) : TY_UNKNOWN;
    if (r == TY_UNKNOWN) return 0;
    *out = r; if (row) *row = op;
    return 1;
  }
  /* Unlike succ/sum above, dig ignores `promote`: a one-index dig answers
     the same nilable element `[]` does (below), and `[]` is itself
     promote-independent -- an Integer Array's indexed read stays Integer
     under promote (only arithmetic results that could overflow become
     poly), so dig at one index follows it rather than succ/sum's rule. */
  /* fetch(i, default) (the 2-argument, with-default form): the element
     kind when the default's own type is unknown or already agrees with
     it, poly when it disagrees -- mirrors analyze_infer_recv.c's fetch
     arm exactly (the 1-arg form above, in the generic fallback via
     bop_rows, has no such dependence; this is why the two forms are
     split into separate rows there). A fresh review found this form was
     never actually exercised by the probing that found the rest of this
     family broken, and an earlier ledger ruling wrongly concluded it was
     untouched by this change -- it was broken on both sides, just never
     tested; this arm closes it now that arg1 exists. */
  if (!strcmp(name, "fetch") && argc == 2 && ty_recv_is_plain_array(recv)) {
    TyKind et = ty_array_elem(recv);
    *out = (arg1 == TY_UNKNOWN || arg1 == et) ? et : TY_POLY;
    return 1;
  }
  /* fetch(key, default) on a Hash: the same rule as the Array arm above,
     against the Hash's own value kind (ty_hash_val) rather than an
     element kind -- the row for this exact shape (argc==2) is still a
     TY_UNKNOWN placeholder in bop_rows (K-033), so it is answered here by
     hand instead, mirroring analyze_infer_recv.c's own Hash#fetch arm. */
  if (!strcmp(name, "fetch") && argc == 2 && ty_is_hash(recv)) {
    TyKind vt = ty_hash_val(recv);
    *out = (arg1 == TY_UNKNOWN || arg1 == vt) ? vt : TY_POLY;
    return 1;
  }
  /* dig(key) on a Hash, one key only: the same answer as `[]` (BOPR_HASH_VAL,
     the value kind, nilable on a miss) -- confirmed against CRuby, which
     raises TypeError for a second key unless the first key's value itself
     supports dig, something this per-receiver-kind table has no way to
     check, so a Hash dig with 2+ keys is deliberately left unanswered here
     (falls through, same as before this arm existed). */
  if (!strcmp(name, "dig") && argc == 1 && ty_is_hash(recv)) {
    const BuiltinOp *op = bop_find(bop_family_of(recv), "[]", 1, 0);
    TyKind r = op ? bop_result(op, recv) : TY_UNKNOWN;
    if (r != TY_UNKNOWN) { *out = r; if (row) *row = op; return 1; }
  }
  if (!strcmp(name, "dig") && argc >= 1 && ty_recv_is_plain_array(recv)) {
    /* one index is the element, two or more are poly: the Array rows state
       both, for any count of indexes */
    const BuiltinOp *op = bop_find(bop_family_of(recv), name, argc, 0);
    if (op) {
      *out = bop_result(op, recv);
      if (row) *row = op;
      return 1;
    }
  }
  /* sample(n): tells a count (arg0 == TY_INT, answers a subarray -- the
     receiver's own kind) apart from a random: keyword hash (arg0 is a
     Hash kind, answers one element) purely from arg0's own TyKind --
     confirmed empirically (--emit-types on a direct sample(random: r)):
     a KeywordHashNode literal's inferred type is a Hash kind, not TY_INT,
     so no AST-shape inspection is needed, unlike it first looked. */
  if (!strcmp(name, "sample") && argc == 1 && ty_recv_is_plain_array(recv)) {
    *out = ty_is_hash(arg0) ? ty_array_elem(recv) : recv; return 1;
  }
  /* clamp(lo, hi): has no bop_rows row at all for Integer/Float -- its
     answer is 100% hand-written, split across two files and two separate
     receiver-specific rules (found by a fresh review after an earlier,
     wrong version of this arm claimed to mirror them "line by line" but
     missed both: an Integer receiver with a Float OR poly bound
     (analyze_infer.c's clamp arm, inside its `rt == TY_INT` block) is
     poly, not the Integer receiver this arm used to answer; a Float
     receiver with a poly bound (confirmed empirically via --emit-types,
     since analyze_infer_recv.c's own Float-receiver rule explicitly
     excludes a poly bound from its own condition and falls through
     elsewhere) is also poly, not the Float this arm used to answer).
     Either bound TY_NIL keeps the receiver's own class boxed regardless
     of receiver kind (analyze_infer_recv.c, checked for both TY_INT and
     TY_FLOAT together).

     Written as an allowlist ("both bounds match the receiver's own
     kind"), not a blocklist of the bound kinds known to force poly: a
     second review round (CodeRabbit, upstream) found the earlier
     blocklist version (poly only for FLOAT or POLY bounds) left a
     Bignum or Rational bound answering the receiver's own kind, where
     a non-constant receiver (confirmed via --emit-types -- a yield's
     receiver is never a compile-time constant, so it always takes this
     general path, never the constant-folded one a literal receiver
     can take) answers poly for ANY bound kind other than its own. */
  if (!strcmp(name, "clamp") && argc == 2 && (recv == TY_INT || recv == TY_FLOAT)) {
    if (arg0 == TY_NIL || arg1 == TY_NIL) { *out = TY_POLY; return 1; }
    *out = (arg0 == recv && arg1 == recv) ? recv : TY_POLY;
    return 1;
  }
  /* clamp(range): confirmed empirically (--emit-types on direct calls)
     that the only case answering the receiver's own kind is an Integer
     receiver with a plain (non-mixed-bound) Range; every other
     combination -- a Float receiver with ANY Range, or either receiver
     with a TY_FLOAT_RANGE or a mixed-bound Range literal -- is poly. The
     mixed-bound case needs range_has_float_bound (the Range literal's own
     left/right child types), since TY_RANGE alone does not distinguish a
     plain Int range from a mixed one. */
  if (!strcmp(name, "clamp") && argc == 1 && (recv == TY_INT || recv == TY_FLOAT) &&
      (arg0 == TY_RANGE || arg0 == TY_FLOAT_RANGE)) {
    *out = (recv == TY_INT && arg0 == TY_RANGE && !range_has_float_bound) ? TY_INT : TY_POLY;
    return 1;
  }
  /* [] with an Integer index: the element, from the row that guards the
     Integer. A Range or any other index kind is not answered here. */
  if (argc == 1 && !strcmp(name, "[]") && arg0 == TY_INT && ty_recv_is_plain_array(recv)) {
    TyKind ak[2] = { arg0, arg1 };
    const BuiltinOp *op = bop_find_arg(bop_family_of(recv), name, argc, 0, bop_arg01_kind, ak);
    TyKind r = op ? bop_result(op, recv) : TY_UNKNOWN;
    if (r == TY_UNKNOWN) return 0;
    *out = r; if (row) *row = op;
    return 1;
  }
  /* flatten's generic-fallback row below is BOPR_SELF (the receiver's own
     kind) -- right for a plain or Hash receiver (K-034), but WRONG for a
     nested array receiver (TY_INT_ARRAY_ARRAY/TY_FLOAT_ARRAY_ARRAY):
     flattening one level produces the ELEMENT kind (ty_array_elem), not
     the still-nested receiver kind. Confirmed against spinel's own direct
     (non-yield) call, which already answers mk.flatten(1) correctly where
     mk returns a nested array -- this table must not disagree with it.
     A yield's own forward inference never widens a literal
     array-of-arrays to this concrete nested kind on its own (confirmed
     empirically via --emit-types: it answers poly_array there instead),
     but a value flowing in from a method's own post-fixpoint narrowed
     return can still carry it to a yield site, so this is answered
     explicitly ahead of the generic fallback rather than trusted to it. */
  if (!strcmp(name, "flatten") && (recv == TY_INT_ARRAY_ARRAY || recv == TY_FLOAT_ARRAY_ARRAY)) {
    *out = ty_array_elem(recv);
    return 1;
  }
  /* Every name above answers from its own hand-written rule, several of
     them mode-gated (succ/sum) or needing more than the receiver's kind
     to answer (clamp). Below is a generic fallback, delegating to
     builtin_ops.c's bop_rows table (a fixed TyKind or a BOPR_* result
     derived purely from the receiver's kind, via the existing pure
     bop_find_arg/bop_result) instead of hand-writing another arm --
     closes sort!, reverse!, rotate, shuffle, first(n), last(n), take,
     drop, fetch(1-arg) and sample(0-arg) with no per-name code.

     Deliberately an ALLOWLIST (bop_result_fallback_allowed), not every
     name the table happens to answer concretely: a first version of
     this let any name through and answered `inspect` too (its own row
     has a concrete TY_STRING result), which a fresh review found
     breaking test/string_encode_arguments.rb -- the wider bop_rows
     table's "concrete result" promise is about what a DIRECT call
     answers, not about what's safe to answer per SITE across a
     yield's diverging call sites, and only the ten names below were
     ever verified for that second, stricter property. A future name
     needs the same verification (a RED test with differing-kind sites,
     confirmed GREEN) before joining the list, not just a concrete row. */
  if (bop_result_fallback_allowed(name) &&
      !bop_result_excluded_for_promote(recv, promote)) {
    /* bop_find_arg looks rows up by their exact key, and plain-array rows
       are keyed BOP_ANY_ARRAY (a family, not a real TyKind) -- a concrete
       recv (TY_INT_ARRAY, ...) must be mapped to its family first, the
       same way every other bop_rows caller (infer_array_call,
       an_bop_find's own callers) already does, or the lookup finds
       nothing for every plain-array name despite the row existing. */
    TyKind ak[2] = { arg0, arg1 };
    const BuiltinOp *op = bop_find_arg(bop_family_of(recv), name, argc,
                                        /*has_block=*/0, bop_arg01_kind, ak);
    if (op && op->result != TY_UNKNOWN) { *out = bop_result(op, recv); if (row) *row = op; return 1; }
  }
  return 0;
}
/* ty_array_of deliberately does NOT map TY_INT_ARRAY -> TY_INT_ARRAY_ARRAY, nor
   TY_FLOAT_ARRAY -> TY_FLOAT_ARRAY_ARRAY: like TY_OBJ_ARRAY, the nested types are
   produced only by the post-fixpoint narrow pass, never by forward inference (a
   forward mapping would cascade the nested type through the fixpoint and
   destabilize it). */

TyKind ty_unify(TyKind a, TyKind b) {
  if (a == b) return a;
  if (a == TY_UNKNOWN) return b;
  if (b == TY_UNKNOWN) return a;
  /* int values flow into bigint slots losslessly (the emitters insert
     sp_bigint_new_int at the boundary), so a bigint-promoted local that
     also sees int writes stays bigint instead of widening to poly. */
  if ((a == TY_BIGINT && b == TY_INT) || (a == TY_INT && b == TY_BIGINT)) return TY_BIGINT;
  /* a plain string value flows into a mutable-string (shared handle) slot
     losslessly: the write re-wraps it in the sp_String (#3227 phase 3) */
  if ((a == TY_STRBUF && b == TY_STRING) || (a == TY_STRING && b == TY_STRBUF)) return TY_STRBUF;
  /* A heap object reference that also sees nil stays the object type: the
     object pointer's NULL encodes nil (legacy's `ClassName?`), so a nullable
     single-class reference need not widen to poly. */
  if (a == TY_NIL && ty_is_object(b)) return b;
  if (b == TY_NIL && ty_is_object(a)) return a;
  /* A String that also sees nil stays a (nullable) String: the const char *
     NULL is nil, the same slot `&.`, an ivar and an --rbs `String?` seed
     already hand out, and every String consumer reads it as nil (#nil?,
     truth, to_s, boxing, and NoMethodError from the rest). The join used to
     widen to poly, so `v.nil? ? nil : v.upcase`, `return nil if v.nil?` and
     `t = nil; t = "y" if ...` all took the boxed slow path where `v&.upcase`
     did not, and that one rule was the largest single source of untyped
     slots in a real tree (#4567). Integer and Float hold their nil beside
     the value and follow below; bool and Symbol have no spare inhabitant and
     stay poly. */
  if (a == TY_NIL && b == TY_STRING) return b;
  if (b == TY_NIL && a == TY_STRING) return a;
  /* (one-String mode: the handle every String slot holds is nil as NULL,
     as the bytes are) */
  if (a == TY_NIL && b == TY_STRBUF) return b;
  if (b == TY_NIL && a == TY_STRBUF) return a;
  /* An Integer or a Float that also sees nil stays a (nullable) scalar: the
     slot's nil rides beside the value (sp_oint / sp_ofloat), which the
     search misses already leave there, and every consumer that can carry
     it is guarded by the #3505 marking (nullable_int on the local, the
     ivar, the return). `best = nil` followed by `best = x` in a search
     loop used to box both, and every arithmetic on the way with it; the
     Ruby definitions of the builtins wrote exactly that (matz, 2026-09-21:
     the join follows the String's). A literal element still keeps its
     container boxed, as with a String. */
  if (a == TY_NIL && (b == TY_INT || b == TY_FLOAT)) return b;
  if (b == TY_NIL && (a == TY_INT || a == TY_FLOAT)) return a;
  /* A poly array that also sees nil stays a (nullable) poly array: the
     sp_PolyArray* NULL encodes nil, and the poly-array method paths already
     NULL-guard, so a method returning `array | nil` need not widen to poly
     (which would strip every array method from the result). */
  if (a == TY_NIL && b == TY_POLY_ARRAY) return b;
  if (b == TY_NIL && a == TY_POLY_ARRAY) return a;
  /* An exception reference that also sees nil stays TY_EXCEPTION: the
     sp_Exception* NULL encodes nil ($! outside a rescue is already nil), and
     the exception method arms NULL-guard (#2739). */
  if (a == TY_NIL && b == TY_EXCEPTION) return b;
  if (b == TY_NIL && a == TY_EXCEPTION) return a;
  /* An IO handle that also sees nil stays TY_IO: sp_File* NULL is nil (the
     readiness family answers nil on timeout, and #nil? reads it), and widening
     to poly stripped every IO method from an `h = nil; h = <accept>` slot. */
  if (a == TY_NIL && b == TY_IO) return b;
  if (b == TY_NIL && a == TY_IO) return a;
  return TY_POLY;
}

/* Numeric accumulator promotion: a fold accumulator seeded with one numeric
   type but reassigned to another (e.g. `[1.5].reduce(0){|a,x| a+x}` -- an int
   seed folded over floats) takes the wider numeric type (float > bigint > int)
   rather than widening to poly. Non-numeric mixes fall back to ty_unify. */
TyKind ty_promote_numeric(TyKind a, TyKind b) {
  if (a == b) return a;
  if (a == TY_UNKNOWN) return b;
  if (b == TY_UNKNOWN) return a;
  if (ty_is_numeric(a) && ty_is_numeric(b)) {
    if (a == TY_FLOAT || b == TY_FLOAT) return TY_FLOAT;
    return TY_BIGINT;  /* int + bigint */
  }
  return ty_unify(a, b);
}

TyKind fold_seed_kind(TyKind resolved, const char *node_type) {
  if (resolved != TY_UNKNOWN) return resolved;
  if (node_type && sp_streq(node_type, "ArrayNode")) return TY_POLY_ARRAY;
  if (node_type && sp_streq(node_type, "HashNode")) return TY_POLY_POLY_HASH;
  return resolved;
}

int fold_seed_typed(TyKind seed, TyKind elem) {
  /* A seed whose class is not settled yet says nothing about the accumulator,
     and the boxed fold would be no more right than the typed one: keep what
     the typed emitters already do for it. */
  if (seed == TY_UNKNOWN) return 1;
  if (seed == elem) return 1;
  return elem == TY_FLOAT && seed == TY_INT;
}

/* A sum can return the untouched seed when empty. Keep that class as well as
   the block's values; nonnumeric accumulators use the boxed Ruby operator. */
TyKind fold_sum_type(TyKind seed, TyKind elem, int promote) {
  if (elem != TY_INT && elem != TY_FLOAT) return TY_POLY;
  if (seed == TY_POLY || seed == TY_STRING ||
      (seed != TY_INT && seed != TY_FLOAT && seed != TY_UNKNOWN &&
       !fold_seed_typed(seed, elem))) return TY_POLY;
  if (elem == TY_FLOAT && seed != TY_FLOAT) return TY_POLY;
  if (promote && elem == TY_INT && seed != TY_FLOAT) return TY_POLY;
  return seed == TY_FLOAT ? TY_FLOAT : elem;
}

TyIterShape ty_iter_shape(const char *name) {
  if (!name) return TY_ITER_NONE;
  if (is_map_alias(name)) return TY_ITER_MAP;
  if (is_select_alias(name)) return TY_ITER_SELECT;
  if (sp_streq(name, "reject")) return TY_ITER_REJECT;
  return TY_ITER_NONE;
}

/* Read off the iterator rows (builtin_ops.c iter_rows). A Hash's pair is
   handed on as its key and its value: reporting 0 for the Hash iterators
   that yield it did not mean "no block". The forwarded-callable desugar
   reads 0 as "not a context-free iterator" and declines, and declining
   left `h.map(&f)` in its &-form with no emitter to claim it: a
   NoMethodError at run time from a program that compiled. The 1-param and
   2-param callable cases are separated by the wrap_pair rule at the call
   site. The boxed-element widening (analyze_pass.c
   widen_boxed_elem_sources) asks it too, whether an Array iterator's
   first parameter is an element. A run (each_slice, each_cons) is left
   untyped: the block's parameter is typed from it as a literal block's
   is. */
int ty_block_yield(TyKind recv, const char *name, TyKind *out, int max) {
  if (!name || max < 1) return 0;
  TyKind fam = ty_is_array(recv) ? BOP_ANY_ARRAY
             : ty_is_hash(recv) ? BOP_ANY_HASH
             : recv == TY_RANGE || recv == TY_INT || recv == TY_STRING ? recv
             : TY_UNKNOWN;
  const IterRow *r = fam == TY_UNKNOWN ? NULL : iter_row(fam, name, -1, IRF_GAP_FWD);
  if (!r) return 0;
  if (r->nyield == 1 && r->yield[0] == YS_PAIR) {
    out[0] = ty_hash_key(recv);
    if (max > 1) out[1] = ty_hash_val(recv);
    return 2;
  }
  for (int k = 0; k < r->nyield && k < max; k++)
    out[k] = r->yield[k] == YS_SUB ? TY_UNKNOWN : iter_yield_kind(r, k, recv);
  return r->nyield;
}

int ty_object_protocol_kind(TyKind t) {
  switch (t) {
    case TY_FIBER: case TY_THREAD: case TY_QUEUE: case TY_MUTEX: case TY_CONDVAR:
    case TY_DIR: case TY_ADDRINFO: case TY_IO: case TY_ENUMERATOR: case TY_CURRY:
    case TY_RANDOM: case TY_OPENSTRUCT: case TY_METHOD: case TY_EXCEPTION:
    case TY_MATCHDATA:
      return 1;
    case TY_TIME: case TY_TMS: case TY_STR_RANGE:
      return 2;
    default:
      return 0;
  }
}

/* Kinds whose members may compare equal across storage (1 == 1.0, an
   IntArray against a PolyArray) share a family; a receiver against an operand
   of another family is unequal without looking. Every other kind is its own
   family. This is a coarser partition than eq_family (codegen_util.c), which
   splits ranges by element type and reports 0 for "undecidable" -- here an
   unknown kind is decidable: it is simply not equal to anything else. */
int ty_object_protocol_family(TyKind t) {
  if (ty_is_numeric(t) || t == TY_RATIONAL || t == TY_COMPLEX) return 1;
  if (t == TY_STRING || t == TY_STRBUF) return 2;
  if (ty_is_array(t) || ty_is_obj_array(t)) return 3;
  if (ty_is_hash(t)) return 4;
  if (t == TY_RANGE || t == TY_FLOAT_RANGE || t == TY_STR_RANGE || t == TY_TIME_RANGE || t == TY_OBJ_RANGE) return 5;
  return 100 + (int)t;
}

/* The IO family -- a File, IO or File::Stat handle and a Dir handle -- also
   takes Object#to_s and Object#<=> from the arm. Nothing else claimed them:
   the generic fallbacks emitted a "" for to_s and an int-or-nil sentinel for
   <=>, which on a File/IO handle met a slot the inference had typed poly, so
   `p f.to_s` and `f <=> f` refused to compile, and on a Dir, whose slot
   happened to be typed, answered "" and nil. The other heap handles keep
   their own to_s (Thread, Exception, Method) or are not audited here. */
static int ty_object_protocol_io(TyKind t) { return t == TY_IO || t == TY_DIR; }

int ty_object_protocol_answers(TyKind rt, TyKind at, const char *name, int argc) {
  if (!name) return 0;
  int kind = ty_object_protocol_kind(rt);
  if (argc == 0) {
    if (kind == 1)
      return is_freeze_family(name) ||
             (ty_object_protocol_io(rt) && sp_streq(name, "to_s"));
    /* a Range is always frozen; Time and Tms carry no frozen bit, so neither
       question is answered for them */
    if (kind == 2) return rt == TY_STR_RANGE && sp_streq(name, "frozen?");
    return 0;
  }
  if (argc != 1) return 0;
  if (ty_object_protocol_io(rt) && sp_streq(name, "<=>")) return 1;
  int is_equal = sp_streq(name, "equal?");
  int is_eql = sp_streq(name, "eql?");
  int is_case = sp_streq(name, "===");
  int is_eq = is_case || is_eq_or_ne(name);
  if (!is_eq && !is_eql && !is_equal) return 0;
  if (kind == 0) {
    /* the cross-family tier */
    if (!(rt == TY_RANGE || rt == TY_FLOAT_RANGE || rt == TY_BIGINT || ty_is_array(rt))) return 0;
    if (ty_is_array(rt) && ty_is_array(at) && at != rt) return !is_equal;
    if (at == TY_POLY || at == TY_UNKNOWN || at == TY_VOID) return 0;
    if (ty_is_object(at) || ty_is_obj_array(at)) return 0;
    return ty_object_protocol_family(at) != ty_object_protocol_family(rt);
  }
  /* Proc#=== (a curried one too), Method#=== and Range#=== call, invoke or
     cover: not identity */
  if (is_case && (rt == TY_METHOD || rt == TY_CURRY || rt == TY_STR_RANGE)) return 0;
  /* a by-value struct has no identity to compare; a Time is a reference */
  if (kind == 2 && is_equal && rt != TY_TIME) return 0;
  return 1;
}

/* ty_traits (types.h): generated from the functions each column names by
   spinel --dump-traits, and checked against them by --check-traits. */
const TyTraits ty_traits[TY_TRAITS_N] = {
#include "ty_traits.inc"
};
/* --int-overflow=promote holds a Rational boxed (infer_type), and a value
   rendered as one may be the box already, a big Rational's: boxed through
   sp_promote_rat_box, which boxes an sp_Rational and passes a box on. The
   rest of the row is the table's. */
int g_traits_promote = 0;
const TyTraits ty_traits_promote_rational = {
  "sp_Rational", "(sp_Rational){0}", "(sp_Rational){0}", NULL, "sp_promote_rat_box", NULL,
  "sp_promote_rat_box($e)", NULL, NULL, "(*(sp_Rational *)($e).v.p)", "(*(sp_Rational *)($e).v.p)",
  NULL, NULL, NULL, NULL, 0, 0, 1, 1, 3, 10 };

/* A builtin value's type, which lays out no instance variables: a String,
   a number, true, false, nil, a Symbol, a Range (of any endpoints), a Random,
   an Array or a Hash. */
int ty_builtin_ivar_less(TyKind t) {
  return t == TY_STRING || t == TY_STRBUF || t == TY_INT || t == TY_FLOAT || t == TY_BOOL || t == TY_NIL ||
         t == TY_SYMBOL || t == TY_BIGINT || t == TY_RANGE || t == TY_FLOAT_RANGE || t == TY_STR_RANGE ||
         t == TY_TIME_RANGE || t == TY_OBJ_RANGE || t == TY_RANDOM || ty_is_array(t) || ty_is_hash(t);
}

/* Of those, the values whose identity Spinel keeps, so the runtime's map can
   hold their ivars (sp_bivar_*): an Array, a Hash, a Random. An exception
   is keyed too, though it is no ivar-less builtin: one of the program's
   exception classes keeps its ivars in its own layout, so the reflection on
   it dispatches by the runtime class (emit_exc_ivar_call), and only a copy
   of it reads this predicate. */
int ty_bivar_keyed(TyKind t) {
  return t == TY_RANDOM || t == TY_EXCEPTION || t == TY_STRING || t == TY_STRBUF || ty_is_array(t) || ty_is_hash(t);
}

int is_scalar_ret(TyKind t) {
  /* a builtin kind's ty_traits row says (types.c); a user object and an
     object array are pointers a method can return */
  const TyTraits *tr = ty_traits_of(t);
  return tr ? tr->scalar_ret : (ty_is_object(t) || ty_is_obj_array(t));
}
