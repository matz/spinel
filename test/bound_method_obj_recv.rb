# `<obj>.method(:bar)` — bind to a receiver that's not the current
# `self`. The Method's iv_self_obj must end up holding the
# receiver, not the *enclosing* class's `self`.
#
# Note: Method#call(x) lowers to a function pointer cast to
# `(void *, mrb_int...) -> mrb_int`, so the bound underlying method
# must return an int. Non-int returns are out of scope for this PR.

class Worker
  def initialize(base)
    @base = base
  end
  def shout(x)
    @base + x
  end
end

# (a) bound on a local variable
def via_local
  w = Worker.new(100)
  w.method(:shout)
end
puts via_local.call(42)   # 142

# (b) bound on an ivar from another class
class Boss
  def initialize
    @worker = Worker.new(1000)
  end
  def via_ivar
    @worker.method(:shout)
  end
end
puts Boss.new.via_ivar.call(99)   # 1099

# (c) array of Methods on a non-self receiver
def fns
  w = Worker.new(10)
  [w.method(:shout), w.method(:shout)]
end
fns.each {|bm| puts bm.call(7) }   # 17, 17
