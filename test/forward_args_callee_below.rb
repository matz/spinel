# A method forwarding `...` to another forwarder takes that one's shape
# wherever the two stand. Written above the forwarder it calls, it kept one
# parameter per argument while its callee took a rest, and handed it an
# Integer where the callee takes an Array: C that did not build. Chains of
# two and four, a block, keywords into a `**nil` target and an argument into
# one taking none, `new(...)` into a forwarding initialize, `super(...)` into
# a parent method a later reopening defines, and a name two classes define,
# one of them a forwarder, where the keyword arrived as one more positional.
def try
  p yield
rescue ArgumentError => e
  puts "ArgumentError: #{e.message}"
end

def top(...) = mid(...)
def mid(...) = leaf(...)
def leaf(a) = a * 2
puts top(21)

def c3(...) = c2(...)
def c2(...) = c1(...)
def c1(...) = c0(...)
def c0(a, b = 5, *r, k: 0) = [a, b, r, k]
p c3(1)
p c3(1, 2, 3, k: 4)

def b2(...) = b1(...)
def b1(...) = b0(...)
def b0(a) = yield(a * 10)
puts(b2(5) { |x| x + 1 })

def n2(...) = n1(...)
def n1(...) = n0(...)
def n0(a, **nil) = a
try { n2(1, z: 3) }
try { n2(3) }

def z2(...) = z1(...)
def z1(...) = z0(...)
def z0() = [:z0]
none = []
try { z2(*none, 1) }
try { z2 }

def make(...) = Built.new(...)
class Built
  def initialize(...) = setup(...)
  def setup(a, b = 5, k: 0)
    @v = [a, b, k]
  end
  attr_reader :v
end
p make(1, k: 2).v
p make(1, 2, k: 3).v

class Par
  def pt(*r, **k) = [:Par, r, k.to_a]
end
class Chi < Par
  def pm(...) = super(...)
end
class Par
  def pm(...) = pt(...)
end
p Chi.new.pm(1, z: 2)

def either(o, ...) = o.em(...)
class Plain
  def em(*r) = [:Plain, r]
end
class Fwd
  def em(...) = et(...)
  def et(*r, **k) = [:Fwd, r, k.to_a]
end
p either(Plain.new, 1)
p either(Fwd.new, 1, z: 2)
