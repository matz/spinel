# Polymorphic dispatch over a parameter typed `poly` should pick the
# right `[]` based on the runtime type — a user class's `def [](x)`
# for Foo.new, sp_IntArray_get for an Array literal.

class Foo
  def [](x)
    42
  end
end

def read0(a)
  a[0]
end

puts read0(Foo.new)
puts read0([100, 200, 300])
