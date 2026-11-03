# A boxed value a multiple assignment or a block of several parameters
# takes apart converts through its class's to_ary, as CRuby's does.
class Pair
  def initialize(a, b) = (@a = a; @b = b)
  def to_ary = ($calls << :to_ary; [@a, @b])
end
class Nope
  def to_ary = nil
end
class Bad
  def to_ary = 7
end
module Ary
  def to_ary = [:m, :n]
end
class Mixed
  include Ary
end
$calls = []

[Pair.new(5, 6)].each { |a, b| p [a, b] }
xs = [[1, 2], Pair.new(3, 4), 5, nil, Nope.new, Mixed.new]
xs.each { |a, b| p [a.class, b] }
p xs.map { |a, b| b }
p xs.select { |a, b| b }.map(&:class)
p [Pair.new(1, 2)].map { |pr| x, y = pr; x * y }

def give(x) = yield(x)
give(xs[1]) { |a, b| p [a, b] }
give(xs[2]) { |a, b| p [a, b] }

v = xs[1]
r = (a, b = v)
p r.class, a, b
a, *rest = v
p [a, rest]
v = xs[4]
a, b = v
p [a.class, b]

begin
  [Bad.new, 1].each { |a, b| p [a, b] }
rescue TypeError => e
  p e.message
end
begin
  v = [Bad.new, 1][0]
  a, b = v
rescue TypeError => e
  p e.message
end
p $calls.size
# a nested target: the boxed value converts, and so does the element the
# nested target takes apart, each at its own level
v = ARGV.size > 5 ? 1 : Pair.new(Pair.new(1, 2), 3)
(a, b), c = v
p [a, b, c]
v = ARGV.size > 5 ? 1 : Pair.new(Pair.new(Pair.new(4, 5), 6), 7)
((a, b), c), d = v
p [a, b, c, d]
v = ARGV.size > 5 ? 1 : Pair.new(Nope.new, 8)
(a, b), c = v
p [a.class, b, c]
