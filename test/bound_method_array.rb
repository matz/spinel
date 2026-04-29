# An array of Methods naturally infers as a ptr_array of
# obj_Method, since Method is just a regular user class as
# far as the type system is concerned. Each entry survives the
# round-trip through sp_PtrArray_get and dispatches to its captured
# (self, fn) pair.

class C
  def double(x); x * 2; end
  def triple(x); x * 3; end
  def quad(x);   x * 4; end

  def fns
    [method(:double), method(:triple), method(:quad)]
  end
end

c = C.new
fns = c.fns
puts fns.length
puts fns[0].call(5)
puts fns[1].call(5)
puts fns[2].call(5)
