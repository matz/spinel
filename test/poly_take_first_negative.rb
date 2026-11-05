# A negative count on a boxed receiver raises the message its own class
# does: take always "attempt to take negative size"; first by the receiver.
class V
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ = V.new(n + 1)
  def inspect = "V#{n}"
end
def t
  yield
rescue => e
  p [e.class, e.message]
end
b = [V.new(1)..V.new(3), [1, 2], 1..3, "a".."c", [1, 2].each, (1..)]
b.each do |x|
  t { x.first(-1) }
  t { x.take(-1) }
  p x.take(1)
  p x.first(1)
end

# a Float Range checks the count before it says it cannot iterate
f = [1.0..2.0, [1]]
t { f[0].take(-1) }
t { f[0].first(-1) }
t { f[0].take(1) }
t { f[0].first(1) }
t { (1.0..2.0).take(-1) }
t { (1.0..2.0).first(-1) }
t { (1.0..2.0).take(1) }
t { (1.0..2.0).first(1) }
# an endless Range of objects too, where take used to walk first
t { (V.new(1)..).take(-1) }
t { [V.new(1).., 1][0].take(-1) }
p [V.new(1).., 1][0].take(2)
