# A nested target over an element whose to_ary answers nil keeps the
# element as its one value, as a plain target does: for a program's class
# and for String, on the left and after a rest, with a splat inside, two
# nested targets in one assignment, a subclass's own to_ary, a value read
# back, in a method, and over an element that may be nil. The element is
# evaluated, and its to_ary called, once.
class Pair
  def initialize(n) = @n = n
  def to_ary
    puts :conv
    @n ? nil : [:x, :y]
  end
end
class Q < Pair; end
class R < Q
  def to_ary = @n ? [:r, :s] : nil
end
class String
  def to_ary = empty? ? nil : [self, :t]
end
(a, b), c = Pair.new(true), 1
p [a.class, b, c]
(a, b), c = Pair.new(false), 2
p [a, b, c]
(a, b), c = +"", 3
p [a, b, c]
(a, b), c = +"s", 4
p [a, b, c]
q, *r, (a, b) = 1, 2, Pair.new(true)
p [q, r, a.class, b]
(a, *b), c = Pair.new(true), 5
p [a.class, b, c]
(a, b), (c, d) = Pair.new(true), +"r"
p [a.class, b, c, d]
(a, b), c = Q.new(true), 6
p [a.class, b, c]
(a, b), c = R.new(false), 7
p [a.class, b, c]
r = ((a, b), c = +"", 8)
p [a, b, c, r]
$k = 0
def mk = ($k += 1; Pair.new(true))
r = ((a, b), c = mk, 9)
p [a.class, b, c, $k, r.map(&:class)]
def g(v) = ((a, b), c = v, 1; [a, b, c])
p g(+""), g(+"m"), g(Pair.new(true)).map(&:class)
def pick(k) = k == 0 ? nil : Pair.new(k == 1)
(a, b), c = pick(0), 1
p [a, b, c]
(a, b), c = pick(1), 2
p [a.class, b, c]
