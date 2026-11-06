# Comparable#== through a <=> answering a Bignum (or nil beside one): equal
# only when that answer is 0, typed and read out of a container alike.
class V
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); (n - o.n) * (2**70); end
end
class W
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); o.n < 0 ? nil : (n - o.n) * (2**70); end
end
a = V.new(2)
p a == V.new(2)
p a == V.new(3)
p a != V.new(2)
x = [a, 1]
p x[0] == V.new(2)
p x[0] == V.new(3)
w = W.new(2)
p w == W.new(2)
p w == W.new(-1)
y = [w, 1]
p y[0] == W.new(2)
p y[0] == W.new(-1)
p y[0] == W.new(5)

# a <=> reading an unset Bignum ivar answers nil: unequal, not a crash
class B
  include Comparable
  def initialize(set) = (@x = 2**70 if set)
  def <=>(o) = @x
end
p B.new(false) == B.new(false)
p B.new(false) != B.new(false)
p B.new(true) == B.new(true)
c = B.new(false)
p c == B.new(false)
p c != B.new(false)
