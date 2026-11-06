# `o.method(:sym)` on a boxed receiver: an instance of a class with a
# `method` of the program's own calls that method (CRuby runs it, and it
# may answer anything), any other value builds the builtin Method. The
# Method a boxed receiver's wrapper makes is called where no site names it:
# merged with the other arm's value, held in an ivar, or built by a case.
$n = 0
class W
  def keep(x) = x * 2
  def method(n) = ($n += 1; super)
end
class U
  def keep(x) = x * 3
  def pair(a, b = 10) = a + b
end
[W.new, U.new].each do |o|
  m = o.method(:keep)
  p m.call(5)
end
p $n

class V
  def method(n) = :mine
end
[V.new, U.new].each { |o| p o.method(:keep) == :mine }

# held in an ivar, called elsewhere
class Holder
  def initialize(o) = @m = o.method(:keep)
  def go(x) = @m.call(x)
end
[W.new, U.new].each { |o| p Holder.new(o).go(4) }

# built by a case, called with one or two arguments
[U.new, W.new].each do |o|
  m = (case o when U then o.method(:pair) else o.method(:keep) end)
  p(o.is_a?(U) ? [m.call(1), m.call(1, 2)] : m.call(7))
end
p $n
