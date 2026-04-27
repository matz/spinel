# Poly dispatch where user classes disagree on the method's return
# type. `Foo#name` returns int, `Bar#name` returns string. The result
# of `a.name` is genuinely polymorphic — the dispatcher must use
# sp_RbVal as the result type and box each branch's return value.

class Foo
  def name
    42
  end
end

class Bar
  def name
    "bar"
  end
end

def show(a)
  a.name
end

puts show(Foo.new)
puts show(Bar.new)
