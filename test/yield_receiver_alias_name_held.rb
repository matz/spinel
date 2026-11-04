# A call through an alias of a builtin on a yield, `yield.orig_abs`, is
# renamed to the builtin while its own receiver is inferred: the yield's
# per-site answer infers the call again. The name the outer inference read
# before the rename stays readable after it, and the call answers 3 and 2.5.
class Integer
  alias orig_abs abs
end
class Float
  alias orig_abs abs
end
def f
  r = yield.orig_abs
  r
end
p f { -3 }
p f { -2.5 }
