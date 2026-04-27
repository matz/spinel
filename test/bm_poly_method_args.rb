# Polymorphic method dispatch must pass arguments through to the
# selected class's method. Before this fix, `compile_poly_method_call`
# emitted `sp_<Class>_<method>((sp_<Class> *)recv.v.p)` with no
# argument list — the call-site arguments were dropped on the floor.

class Doubler
  def call(x)
    x * 2
  end
end

class Tripler
  def call(x)
    x * 3
  end
end

def apply(a, x)
  a.call(x)
end

puts apply(Doubler.new, 5)
puts apply(Tripler.new, 5)
