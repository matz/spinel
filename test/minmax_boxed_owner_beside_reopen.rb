# minmax on a boxed value beside a program's reopening of Enumerable#minmax:
# an Array and a Range answer by their own minmax, which CRuby looks up
# before the module's; a Hash, which has none, and a class that only
# includes Enumerable answer by the reopening. A Float Range or a String
# Range answers its endpoints, and a class with its own minmax answers by
# it, whether or not such a class is there.

module Enumerable
  def minmax = :enum
end

class Span
  def minmax = [:lo, :hi]
end

class Bag
  include Enumerable

  def each = yield(1)
end

def pick(i) = [[3, 1, 2], (1..4), (1.5..2.5), ("a".."c"), { a: 1 }, Span.new, Bag.new][i]

0.upto(6) { |i| p pick(i).minmax }

def pick_plain(i) = [[5, 4], (2..3), ("x".."z"), Bag.new][i]

0.upto(3) { |i| p pick_plain(i).minmax }
