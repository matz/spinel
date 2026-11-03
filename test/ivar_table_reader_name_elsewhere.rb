# An ivar table is not boxed by a name only another class answers
# A table in an ivar narrows unless a dynamic call could hand it out: a call
# on an untyped receiver by its reader's name, or a Symbol spelling that name
# (send, method). Both used to box every class's table of the name, whether
# or not the class had such a reader. A Struct member :b, a `.b` on some other
# class's value: Grid has no `b`, so its @b stays a Float table. Ledger has
# readers, and the tables they hand out stay boxed: @c reached by a dynamic
# call alone (no Symbol spells :c) on a value that may be a Ledger or a Pt,
# whose own `c` answers something else, @d through send and method as well.

class Grid
  def initialize(n)
    @b = Array.new(n) { |i| Array.new(n) { |j| (i * j + 0.5) } }
    @r = Array.new(n) { |i| Array.new(n) { |j| i - j } }
  end

  def total
    s = 0.0
    @b.each { |row| row.each { |x| s += x } }
    s
  end

  def diag
    s = 0
    @r.each_with_index { |row, i| s += row[i] + row[0] }
    s
  end
end

class Ledger
  def initialize(n)
    @c = Array.new(n) { |i| Array.new(2) { |j| i * 10 + j } }
    @d = Array.new(n) { |i| Array.new(2) { |j| i * 100 + j } }
  end

  attr_reader :c, :d
end

Color = Struct.new(:r, :g, :b)
col = Color.new(1, 2, 3)
p [col.r, col.b]

class Pt
  def initialize(v) = @v = v
  def r = @v
  def c = [@v]
end

g = Grid.new(4)
p g.total, g.diag

q = [Pt.new(7), Ledger.new(3)][ARGV.size]
p q.r
l = [Ledger.new(3), Pt.new(5)][ARGV.size]
p l.c.size, l.c[2]
p l.send(:d)[1][1], l.method(:d).call.size
p Grid.new(2).total
