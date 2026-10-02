# A call through an alias that captured a builtin before the class reopened
# that name runs the builtin, on a yield receiver too: `yield.orig_abs` with
# an Integer block at one site and a Float block at another answers 3 and
# 2.5, though Integer#abs is reopened to answer a String.
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
def reopened_abs = yield.abs
p reopened_abs { -3 }
p reopened_abs { -2.5 }
