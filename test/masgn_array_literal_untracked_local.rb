# An Array literal a multiple assignment writes into a local that may hold
# an untracked container (the receiver of an `@v >= o` an object's method
# reads, which no call reaches): the local is in no alias group with the
# literal, so the literal is built as the local's Array, an Array of any
# value, where it was built as an Array of Integers and stored bare.
class Holder
  def initialize(v) = @v = v
  def eq(other) = @v == other
  def >=(o) = @v >= o
end
class Object
  def held = Holder.new(self)
end
(a, *b), c, (*d, (e, *f, g)) = [1, [2, 3]], [4, 5], [6, 7, 8]
p [a, b, c, d, e, f, g].held.eq([1, [[2, 3]], [4, 5], [6, 7], 8, [], nil])
p c, d, f
c << :x
p c, c.size
