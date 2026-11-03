# frozen_string_literal: true
# slice!(regexp, capture_index) where regexp is held in a value (a parameter,
# not a /.../ literal at the call site) used to fall past the only
# two-argument regex-capture arm in emit_op_string_slice, since it gated on
# re_lit_index alone. A mutable-variable receiver then ran the generic
# "slice!(start, len)" arm with the Regexp coerced as the start index, raising
# "no implicit conversion of Regexp into Integer"; a non-variable receiver hit
# the same wrong arm through span2 (which only excluded a *literal* regex
# first argument, not a Regexp-typed one held at runtime).
def var_recv(re)
  s = +"ab12cd34"
  r = s.slice!(re, 1)
  [r, s]
end
p var_recv(/\d(\d)/)

def nonvar_recv(re)
  (+"ab12cd34").slice!(re, 0)
end
p nonvar_recv(/\d+/)

def no_match(re)
  s = +"abcdef"
  r = s.slice!(re, 1)
  [r, s]
end
p no_match(/\d+/)
