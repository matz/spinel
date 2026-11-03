# Each value stored into a table must be an Integer row for the table's reads
# to answer rows. `@rows[0] = @other[i]` stores an element of another, mixed
# Array: a String here. The check counts a value not typed yet as neither
# kind, and in the passes after the fixpoint the element read had no type
# yet when `@rows[0]` was typed, so that read answered an Integer row and
# printed the String's bytes as Integers. The value is now inferred when it
# has no type yet. That inference can read a table whose check is running:
# Two's tables store each other's rows (a row read out of a table being
# checked says nothing of the rows), and Grid's store index reads Grid's own
# table, which recursed without end.
class Mixed
  def initialize(other)
    @rows = [[1, 2]]
    @other = other
  end

  def take(i)
    @rows[0] = @other[i]
  end

  def first = @rows[0]
end

class Rows
  def initialize(other)
    @rows = [[1, 2]]
    @other = other
  end

  def take(i)
    @rows[0] = @other[i]
  end

  def first = @rows[0]
end

class Two
  def initialize
    @a = [[1, 2], [3, 4]]
    @b = [[5, 6]]
  end

  def ab = (@a[0] = @b[0])
  def ba = (@b[0] = @a[1])
  def sums = [@a[0].sum, @b[0].sum]
end

class Grid
  def initialize = @t = [[0, 1], [3, 4]]
  def put = (@t[@t[0][0]] = [2])
  def at(i) = @t[i]
end

m = Mixed.new([[7, 8], "row"])
m.take(1)
p m.first
m.take(0)
p m.first
r = Rows.new([[7, 8], [9, 10]])
r.take(1)
p r.first.sum
t = Two.new
t.ba
t.ab
p t.sums
g = Grid.new
g.put
p g.at(0), g.at(1).sum
