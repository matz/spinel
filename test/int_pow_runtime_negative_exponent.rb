# An Integer power with a run-time exponent answers what CRuby answers: an
# Integer for an exponent >= 0, a Rational for a negative one (2 ** -1 is
# (1/2)), an Integer for a base of 1 or -1, ZeroDivisionError for 0 ** a
# negative. The exponent here is not a literal, so it was an sp_int power
# that raised RangeError "negative exponent". `b ** e`, `b.pow(e)` and
# `x **= e` (on a local, an instance variable, a global, a class variable and
# an attribute) are the same power. The values come from ARGV.size (0 at run
# time) and from parameters, so the compiler cannot fold them.

# a local, the three signs of the exponent
e = ARGV.size - 1
p 2 ** e
p 2 ** (e + 1)
p 2 ** (e + 4)

# bases 1 and -1 answer an Integer, 0 a ZeroDivisionError, a negative base
# keeps its sign
n = ARGV.size - 3
p 1 ** n, (-1) ** n, (-1) ** (n - 1)
begin
  p 0 ** e
rescue ZeroDivisionError => x
  p [x.class, x.message]
end
p 0 ** (e + 1)
p((-2) ** n)

# a method's parameters, called with both signs
def f(b, k) = b ** k
z = ARGV.size
p f(2, z - 2)
p f(2, z + 3)

# `**=` and `.pow`
x = 2
x **= ARGV.size - 1
p x
y = 3
y **= ARGV.size + 2
p y
p 2.pow(ARGV.size - 2)
p 2.pow(ARGV.size + 5)

# the exponent from a container's size, and a user class's length, which can
# be negative
s = "ab" * (ARGV.size + 1)
a = [1, 2]
p 10 ** s.length, 2 ** a.size, 2 ** (a.size - 3)
class L
  def length = ARGV.size - 2
end
p 10 ** L.new.length

# -n, a literal negative exponent with a run-time base (1 and -1 answer an
# Integer, the others a Rational)
m = ARGV.size + 2
p 2 ** -m
[1, -1, 2].each { |b| p b ** -2, b ** -3 }
def g(b) = b ** -1
p g(1), g(2)
w = ARGV.size + 1
p w ** -1, w.pow(-2)
u = ARGV.size + 3
u **= -1
p u

# `**=` on an instance variable, a global, a class variable and an attribute
class C
  def initialize = @x = 2
  def go(k) = (@x **= k; @x)
end
p C.new.go(ARGV.size - 1)
p C.new.go(ARGV.size + 3)
$x = 2
$x **= ARGV.size - 1
p $x
class Box
  attr_accessor :v
  def initialize = @v = 2
end
bx = Box.new
bx.v **= ARGV.size - 2
p bx.v
class K
  @@x = 2
  def self.go(k) = (@@x **= k; @@x)
end
p K.go(ARGV.size - 1)

# a power that is discarded raises nothing
2 ** e
2.pow(e)

# the result is a Rational where it is one: printing, arithmetic, comparison,
# containers, conversions
r = 2 ** (ARGV.size - 1)
puts r
p r
puts r.inspect
puts "#{r}"
p r.class
a4 = [10, 20, 30, 40]
p a4[2 ** (ARGV.size + 1)]
p r + 1, 1 + r, r * 4, r < 1, r == 0.5, r == Rational(1, 2), r <=> 1
h = {}
h[r] = 1
p h
p [r, 2 ** (ARGV.size + 2)]
v = 2 ** (ARGV.size - 2)
p v.to_f, v.to_i, v.numerator, v.denominator
sum = 0
(-2..2).each { |k| sum += 2 ** k }
p sum
p((2 ** e) ** 2, (2 ** e) ** e)
acc = 1
[1, -2, 3].each { |k| acc = acc * (2 ** k) }
p acc
def pw(k) = 2 ** k
p pw(ARGV.size - 1)
p pw(ARGV.size + 3) + 1
def half(q) = q * 2
p half(2 ** (ARGV.size - 1))
p half(3)

# an exponent from a conversion, a unary minus and a rand
p 5 ** Integer("-#{ARGV.size + 2}")
p 2 ** rand(0..0).pred
p 2.send(:**, e), 2.public_send(:pow, e), 2.method(:**).call(e)
q = ARGV.empty? ? 2 : nil
p q&.**(ARGV.size - 1)

# a power with an operand that is not an Integer keeps CRuby's error
def t(v, k) = (v ** k rescue $!.class)
p t(3, ARGV.size + 2)
p t(3, ARGV.size - 2)
p t(nil, 2)
p t(3, nil)

# a `when` value is a power
def k1(subject, k)
  case subject
  when 2 ** k then "int"
  else "none"
  end
end
p k1("x", ARGV.size - 1)
p k1(Rational(1, 2), ARGV.size - 1)
p k1(1, ARGV.size)
def k2(subject, k)
  case subject
  when 0 ** k then "int"
  else "none"
  end
rescue ZeroDivisionError => err
  err.class
end
p k2("x", ARGV.size - 1)
