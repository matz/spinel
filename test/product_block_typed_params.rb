# `a.product(b) { |x, y| }` takes x from the receiver and y from b each step,
# typed as their elements are (as zip's block does), rather than building a
# boxed [x, y] pair and taking it apart: Integer, Float and String arrays,
# the values the block can see and what the call answers, and the block
# shapes that still take the pair whole. zip's and product's String params
# held as handles (a block that stores them) build.
# spinel: gc-stress

a = [1, 2, 3]
f = [0.5, 1.5]
# the value: the receiver itself
r = a.product(f) { |x, y| }
p r.equal?(a), r
# break, with and without a value; next
p a.product(a) { |x, y| break x * 10 + y if x + y == 4 }
p a.product(a) { |x, y| break if y == 2 }
s = 0
a.product(a) { |x, y| next if x == y; s += x * y }
p s
# Integer and Float elements mixed in the arithmetic
t = 0.0
a.product(f) { |x, y| t += x * y }
p t
# a param reassigned in the block, to another class
o = []
a.product(f) { |x, y| x = x.to_s if x == 2; o << [x, y] }
p o.size, o[2]
# the same array on both sides, an empty one, one element
c = 0
a.product(a) { |x, y| c += 1 }
p c, [].product(a) { |x, y| raise "no" }, a.product([]) { |x, y| raise "no" }
p [7].product([8]) { |x, y| p [x, y] }
# a nested product, and one inside a method over its parameters
def pairs(xs, ys) = (n = 0; xs.product(ys) { |x, y| n += x * y if x < y }; n)
p pairs((1..30).to_a, (1..30).to_a)
q = 0
a.product(a) { |x, y| [x].product([y]) { |u, v| q += u + v } }
p q
# a lone param, a destructuring one, a trailing comma, a rest: the pair as before
p a.product(f).size
w = []
a.product(f) { |pr| w << pr }
a.product(f) { |(x, y)| w << x }
a.product(f) { |x, | w << x }
a.product(f) { |x, *rs| w << rs }
# Strings, the block storing and changing them
ss = []
%w[a b].map(&:dup).product(%w[c d].map(&:dup)) { |x, y| ss << x << y; y << "!" if x == "a" }
p ss
def strs(a, b) = (r = []; a.product(b) { |x, y| r << x + y }; r)
def zstrs(a, b) = (r = []; a.zip(b) { |x, y| r << x << y }; r)
p strs(%w[a b], %w[c d]), zstrs(%w[a b], %w[c d])
# typed through a method, so its answer keeps its class
def ip(a, b) = (s = 0; a.product(b) { |x, y| s += x * y }; s)
def fp(a, b) = (s = 0.0; a.product(b) { |x, y| s += x * y }; s)
p ip(Array.new(50) { |i| i }, Array.new(40) { |i| i * 2 }), fp([1.5, 2.5], [2.0, 4.0]).class
# the operands and blocks that keep the pair: a Range (CRuby's TypeError),
# an object converted through to_ary, a keyword parameter; and unused params
k = []
a.product(3..4) { |x, y| k << x + y } rescue k << $!.class
class Src; def to_ary = [7, 8]; end
a.product(Src.new) { |x, y| k << x * y }
a.product([5]) { |x, y, kw: 1| k << [x, y, kw] }
u = 0
a.product(a) { |x, y| u += 1 }
a.product(a) { |x, _y| u += x }
p k, u
