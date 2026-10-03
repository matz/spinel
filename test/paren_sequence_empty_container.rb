# A parenthesized sequence that ends in an empty Array or Hash is that
# collection: `x = (1; [])` is [], as `x = ([])` and `x = begin; 1; []; end`
# are. The empty literal carries no element type, so the sequence read as
# having no value: the slot that took it was boxed, and what went into the
# box was nil. `x = (1; [])` printed nil, `p((4; {}))` printed nil, and
# `(1; []).size` raised NoMethodError.

x = (1; [])
p x
w = (3; {})
p w
p((4; []))
p((4; {}))
p [(5; []), (6; {})]
p((7; Array.new))
p((8; Hash.new))

# a method whose body is the sequence, and a block's value
def r = (1; [])
def h = (1; {})
p r, h
p r.size, r.empty?, r == [], h.size, h.empty?, h == {}
p [1, 2].map { |v| (v; []) }
l = -> { (1; {}) }
p l.call

# as a receiver, an argument, a default and an interpolation
p((1; []).size, (1; {}).size, (1; []).empty?, (1; []).class, (1; {}).class)
def one(a) = a
def sized(a) = a.size
def dflt(a = (1; []), b: (2; {})) = [a, b]
p one((1; [])), sized((1; [])), dflt
puts "#{(1; [])} #{(2; {})}"

# it is a collection to write into, of whatever the program then puts there
ints = (1; [])
ints << 3
strs = (1; [])
strs << "a"
flts = (1; [])
flts << 1.5
mixed = (1; [])
mixed << 3 << "s" << :t
p ints, strs, flts, mixed
by_sym = (1; {})
by_sym[:a] = 1
by_str = (1; {})
by_str["a"] = 2
by_int = (1; {})
by_int[1] = 3
p by_sym[:a], by_str["a"], by_int[1], by_sym.size, by_str.keys, by_int.keys

# each evaluation is a fresh collection
a = (1; [])
b = (1; [])
a << 1
p a, b
i = 0
while i < 2
  f = (i += 1; [])
  f << i
  p f
end

# an instance variable, a global, a constant and a Struct member
class Box
  def initialize = @items = (1; [])
  def add(v) = @items << v
  def items = @items
end
box = Box.new
box.add(3)
p box.items
$g = (1; [])
p $g
C = (1; {})
p C
S = Struct.new(:a, :b)
s = S.new((1; []), (2; {}))
p s.a, s.b

# nested, behind single parentheses, and as an arm
p((1; (2; [])), (1; ([])), ((1; 2); {}))
p(true ? (1; []) : nil)
def arm(c)
  if c
    (1; [])
  else
    (2; {})
  end
end
p arm(true), arm(false)
case (1; {})
when Array then puts "array"
when Hash then puts "hash"
else puts "neither"
end

# the statements before it run once, in order, among the other operands
def lg(v) = (puts "lg #{v}"; v)
p [lg(1), (lg(:seq); []), lg(2)]
p(lg(3), (lg(:seq); {}), lg(4))
d = (lg(:before); Hash.new(lg(7)))
p d.size, d[:missing]

# a sequence ending in anything else is what it was
p((2; [1, 2]), (6; nil.to_a), (1; "s"), (1; nil))
e = begin; 8; []; end
p e
