# `a <=> b` on a boxed receiver answers what the program's <=> returns,
# a Bignum too, which the Integer answer could not hold.
class A
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = (n - o.n) * (2**70)
end
class C < A
  def <=>(o) = (puts "C<=>"; n - o.n)
end
class W
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = o.n < 0 ? nil : (n - o.n) * (2**70)
end
xs = [A.new(3), C.new(3), 1, "a", nil]
p(xs[0] <=> A.new(4))
p(xs[1] <=> C.new(5))
p(xs[2] <=> 2)
p(xs[3] <=> "b")
p(xs[4] <=> nil)
p(xs[2] <=> "x")
r = xs[0] <=> A.new(3)
p r.class
ws = [W.new(1), 2]
p(ws[0] <=> W.new(5))
p(ws[0] <=> W.new(-1))
p [A.new(2), A.new(1), C.new(0)].sort.map(&:n)
