# frozen_string_literal: true
# String#scan with a block answers its receiver, at a method's tail too,
# where the method answered nil.
def f(s) = s.scan(/\d/) { print _1 }
p f("a1b2")
def g(s)
  s.scan(/(\w)(\d)/) { |a, b| print a, b }
end
p g("a1b2")
def h(s)
  @s = s
  @s.scan(/\d/) { print _1 }
end
p h("a3")
def k(s) = s.scan(/\d/)
p k("a1b2")
class S
  def scan(x)
    yield 1
    :mine
  end
end
def m(o) = o.scan(1) { }
p m(S.new)

# A handle result preserves the walked String, including through a rescue
# result slot; callers can mutate that same receiver after the walk.
def kept(s)
  s.scan(/\d/) { }
end
def kept_with_ensure(s)
  s.scan(/\d/) { }
ensure
  puts :ensured
end
s = +"a1"
r = kept(s)
p r.equal?(s)
r << "b2"
p s
r = kept_with_ensure(s)
p r.equal?(s)
r << "c3"
p s
p kept("").frozen?
