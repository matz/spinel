# A builtin on a yield, reached through an alias that captured it before the
# class reopened the name: the call runs the builtin, so each site keeps the
# builtin's kind. `class Integer; alias orig_abs abs; def abs = "int-abs";
# end` with Integer and Float blocks answers 3 and 2.5, as in CRuby; typing
# the Integer site from the reopen's String stopped the build.
class Integer
  alias orig_abs abs
  def abs = "int-abs"
end
class Float
  alias orig_abs abs
end
def plain_abs = yield.orig_abs
p plain_abs { -3 }
p plain_abs { -2.5 }
