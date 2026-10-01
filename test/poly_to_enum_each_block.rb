# `x.to_enum.each { }` / `x.enum_for.each { }` on a boxed receiver runs the
# receiver's own each with the block and answers what that each answers
# (the receiver), for a builtin and for a class with a yielding each. The
# chain was folded into a blockless Enumerator build that dropped the block.
class W
  def initialize(a) = (@a = a)
  def each
    @a.each { |x| yield x }
    self
  end
end
h = { k: [3, 1], w: W.new([5, 6]) }
p h[:k].to_enum.each { |q| q }
p h[:k].enum_for.each { |q| q + 1 }
r = h[:w].to_enum.each { |q| p q }
p r.class
p h[:w].enum_for.to_a
