# A Range of program objects: find, detect, take_while, find_index and
# each_with_index with a block walk by succ and stop at the block's answer,
# so succ runs only as far as CRuby runs it and an endless Range works.
# first(n) runs the nth member's succ (range.c first_i); take(n) does not.
class V
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ
    puts "succ #{n}"
    V.new(n + 1)
  end
  def to_s = "V#{n}"
end
r = (V.new(1)..V.new(5))
p r.find { |v| v.n == 2 }.to_s
p r.detect { |v| v.n == 9 }
p r.take_while { |v| v.n < 3 }.map(&:to_s)
p r.find_index { |v| v.n == 3 }
r.each_with_index { |v, i| puts "#{v} #{i}"; break if i == 1 }
e = (V.new(1)..)
p e.find { |v| v.n == 3 }.to_s
p e.take_while { |v| v.n < 3 }.size
p e.find_index { |v| v.n == 2 }
x = (V.new(1)...V.new(3))
p x.find { |v| v.n == 3 }
p x.first(2).map(&:to_s), x.take(1).map(&:to_s)
p (V.new(1)..V.new(3)).first(3).map(&:to_s)
p e.first(2).map(&:to_s), e.take(2).map(&:to_s)
def f(rr) = rr.find { |v| v.n > 1 }.to_s
p f(r)
class Holder
  def initialize = @r = (V.new(1)..V.new(3))
  def big = @r.find { |v| v.n > 2 }
end
p Holder.new.big.to_s
p r.each_with_index { |v, i| next if i.odd? }.to_s
p r.find { |v| next false if v.n == 1; true }.to_s
p r.take_while { |v| break [] if v.n == 2; true }
p (V.new(3)..V.new(1)).find { |v| true }
# take_while's Array chained into a String map and a sum block
class W
  include Comparable
  attr_reader :n, :s
  def initialize(n) = (@n = n; @s = "x" * n)
  def <=>(o) = n <=> o.n
  def succ = W.new(n + 1)
end
w = (W.new(1)..W.new(5))
p w.take_while { |v| v.n < 3 }.map { |v| v.s }.sum { |y| y.size }
p w.take_while { |v| v.n < 4 }.map(&:s).sum(&:size)
