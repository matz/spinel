# A Set built from a program's Enumerable holds what its #each yields, which
# its own #to_a does not have to answer.
require "set"

class Twofold
  include Enumerable
  def each = (yield 1; yield 2; self)
  def to_a = [9]
end
Pt = Struct.new(:x, :y)
class Plain
  def each = (yield 7; self)
  def to_a = [7]
end

t = Twofold.new
p Set.new(t).to_a
p Set.new(t) { |x| x * 10 }.to_a
s = Set[1, 5]
p s.dup.merge(t).to_a
p s.dup.subtract(t).to_a
p s.dup.replace(t).to_a
p (s & t).to_a
p (s | t).to_a
p (s - t).to_a
p (s ^ t).to_a

# to_set, on a receiver of a known class and on a boxed one
p t.to_set.to_a
p t.to_set { |v| v + 1 }.to_a
x = [t, [3, 3]]
p x[0].to_set.to_a
p x[1].to_set.to_a

# the operands that are not a program's Enumerable read as before
p Set.new(1..3).to_a
p Set.new({a: 1}).to_a
p Set.new("ab".each_char).to_a
p Set.new(Pt.new(5, 6)).to_a
p Pt.new(1, 2).to_set.to_a
y = [Pt.new(4, 4), 1][0]
p y.to_set.to_a
p (Set[1] | Pt.new(5, 6)).to_a
p Set.new(Plain.new).to_a
p Set[1].to_set.to_a
