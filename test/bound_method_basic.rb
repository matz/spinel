# `method(:foo)` inside a class body must yield a value that survives
# being stored in an ivar / passed across a method-call boundary, then
# called with `bm.call(x)` or `bm[x]`. Spinel's pre-fix path treated
# `method(:foo)` as a compile-time alias only — the variable held a
# placeholder int and the call body silently miscompiled to `return 0`.

class C
  def initialize
    @bm = method(:double)
  end

  def double(x)
    x * 2
  end

  def via_call(x)
    @bm.call(x)
  end

  def via_bracket(x)
    @bm[x]
  end
end

c = C.new
puts c.via_call(7)
puts c.via_bracket(20)
