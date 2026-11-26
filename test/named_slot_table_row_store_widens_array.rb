# A named slot's table of new Arrays -- a constant's, a global's, an
# ivar's, a class variable's -- written by `||=`, `&&=`, through a path or
# as a multiple assignment's target: a store of another kind through a row
# widens the row, as it does for the plain write's box.

def y1(a) = yield(a)
def m(a) = a << "r"

N1 ||= ["ab".bytes]
y1(N1[0]) { |x| x << "q" }
p N1

module M2; end
M2::N = ["cd".bytes]
y1(M2::N[0]) { |x| x << "q" }
p M2::N

module M3; N = ["ef".bytes]; end
y1(M3::N[0]) { |x| x << "q" }
p M3::N

N4, z4 = ["gh".bytes], 0
y1(N4[0]) { |x| x << "q" }
p N4, z4

N5 ||= ["ij".bytes].freeze
y1(N5[0]) { |x| x << "q" }
p N5

module M6; end
M6::N ||= ["kl".bytes]
m(M6::N.first)
p M6::N

# the store in a method's tail, and in a subclass reading its parent's
module M7; end
def put7(v) = y1(M7::N[0]) { |x| x << v }
M7::N ||= ["mn".bytes]
p put7("q"), M7::N
class A8; end
A8::N ||= ["op".bytes]
class B8 < A8; def go = y1(N[0]) { |x| x << "q" }; end
B8.new.go
p A8::N

# two rows in one statement, and a store that never runs
N9 ||= ["qr".bytes, "st".bytes]
p(y1(N9[0]) { |x| x << "q" }, y1(N9[1]) { |x| x << 2.5 })
p N9
N10 ||= ["uv".bytes]
y1(N10[0]) { |x| x << "q" } if ARGV.size > 5
p N10

# a global's, an ivar's and a class variable's table
$g11 ||= ["wx".bytes]
y1($g11[0]) { |x| x << "q" }
p $g11
$g12, z12 = ["yz".bytes], 0
y1($g12[0]) { |x| x << "q" }
p $g12, z12
$g13 = [[0]]
$g13 &&= ["ab".bytes]
y1($g13[0]) { |x| x << "q" }
p $g13
class K14
  def initialize = (@t ||= ["cd".bytes])
  def go = y1(@t[0]) { |x| x << "q" }
  attr_reader :t
end
k14 = K14.new; k14.go; p k14.t
class K15
  def initialize = (@t = [[0]]; @t &&= ["ef".bytes])
  def go = y1(@t[0]) { |x| x << "q" }
  attr_reader :t
end
class K16 < K15; def go2 = y1(@t[0]) { |x| x << 2.5 }; end
k16 = K16.new; k16.go; k16.go2; p k16.t
class K17
  @@t ||= ["gh".bytes]
  def self.go = y1(@@t[0]) { |x| x << "q" }
  def self.t = @@t
end
K17.go; p K17.t
@t18 ||= ["ij".bytes]
y1(@t18[0]) { |x| x << "q" }
p @t18

# a class builder's block writes the enclosing scope's constant
S19 = Struct.new(:a) do
  T19 ||= ["kl".bytes]
end
y1(T19[0]) { |x| x << "q" }
p T19
K20 = Class.new do
  T20 ||= ["mn".bytes]
end
y1(T20[0]) { |x| x << "q" }
p T20
