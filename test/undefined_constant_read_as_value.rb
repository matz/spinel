# A read of a constant defined nowhere raises NameError wherever its value
# would go. Its C value is a Class placeholder, which a slot of another kind
# cannot take: as a block's value beside a block that answers a Symbol, the
# read did not build, and a path on a Class value (`m::Nope`) did not build
# in a ternary, an if, a method's tail, an argument, an interpolation or a
# begin either.
def sym
  p yield
rescue NameError => e
  puts e.message
end

module A; end
m = String
sym { :ok }
sym { Nope }
sym { A::Nope }
sym { m::Nope }

def report
  yield
rescue NameError => e
  puts e.message
end

def ternary(x, m) = x ? :a : m::Nope
def branch(x, m)
  if x then 1 else m::Nope end
end
def early(x, m)
  return "s" if x
  m::Nope
end
def sq(n) = n * n

report { p ternary(true, String) }
report { p ternary(false, String) }
report { p branch(true, String) }
report { p branch(false, String) }
report { p early(true, String) }
report { p early(false, String) }
report { p sq(3) }
report { p sq(m::Nope) }
report { p "v=#{m::Nope}" }
v = begin
  m::Nope
rescue NameError => e
  e.message
end
p v

# Beside a block that answers an Integer or nil, the raise fills that slot.
def mapped = [1, 2].map { |x| yield(x) }
p mapped { |x| x if x > 1 }
report { p mapped { |x| Nope } }

# A path read on the yield's own value, where the block's value is read.
def inner
  p yield
end
def outer
  inner { (yield)::Nope }
end
def other
  inner { :ok }
end
other { 1 }
report { outer { String } }

# A path some class defines is still looked up on the value at run time.
class K
  NM = 7
end
def read(m) = m::NM
report { p read(K) }
report { p read(String) }
