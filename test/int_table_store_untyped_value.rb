# Each value stored into a table must be an Integer row for the table's reads
# to answer rows. `@rows[0] = @other[i]` stores an element of another, mixed
# Array: a String here. The check counts a value not typed yet as neither
# kind, and in the passes after the fixpoint the element read had no type
# yet when `@rows[0]` was typed, so that read answered an Integer row and
# printed the String's bytes as Integers. The value is now inferred when it
# has no type yet.
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

m = Mixed.new([[7, 8], "row"])
m.take(1)
p m.first
m.take(0)
p m.first
r = Rows.new([[7, 8], [9, 10]])
r.take(1)
p r.first.sum
