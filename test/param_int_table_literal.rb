# A literal of Integer (or Float) rows passed straight to a method narrows
# the parameter, and what is written from it, to a table of rows, as the
# same literal written to a local does. It was a literal of objects only:
# any other literal argument kept the parameter an Array of boxed values.
# A literal or caller the table cannot take keeps it boxed.

def row_sum(src) = src[1].sum
p row_sum([[5, 6], [7, 8]])

# into a constructor's ivar, read in a loop
class Grid
  def initialize(rows)
    @rows = rows
  end

  def total
    t = 0
    @rows.each { |r| r.each { |v| t += v } }
    t
  end

  def at(i, j) = @rows[i][j]
end

g = Grid.new([[1, 2, 3], [4, 5, 6]])
p g.total
p g.at(1, 2)

# through a setter
class Holder
  def set(rows) = (@rows = rows)
  def first_row = @rows[0]
end

h = Holder.new
h.set([[9, 8], [7, 6]])
p h.first_row

# Float rows
def float_sum(rows) = rows.map(&:sum).sum
p float_sum([[1.5, 2.5], [3.0]])

# two callers, a literal and a local holding the same kind of table
def corner(t) = t[0][0] + t[-1][-1]
p corner([[1, 2], [3, 4]])
local = [[10, 20], [30, 40]]
p corner(local)

# a String row: the table stays boxed
def describe(t) = t.map { |r| r.inspect }.join(" ")
p describe([[1, 2], "row"])
p describe([[3, 4], [5, 6]])

# Integer rows beside Float rows
def mixed_rows(t) = t.map { |r| r.sum }
p mixed_rows([[1, 2], [1.5, 2.5]])

# the callee stores something other than a row into the table
def push_odd(t)
  t << "odd"
  t.size
end
p push_odd([[1], [2]])

# another caller hands over a table with a String in it
def last_of(t) = t[-1]
p last_of([[1, 2], [3, 4]])
p last_of([[5, 6], "seven"].dup)

# rows that are modified in the callee
def bump(t)
  t.each { |r| r[0] += 100 }
  t
end
p bump([[1, 2], [3, 4]])

# a literal that spreads its rows from elsewhere, and one a level deeper:
# neither is a literal of rows, and both stay boxed
def flat_size(t) = t.sum { |r| r.size }
extra = [[7, 8]]
p flat_size([[1], *extra])
p flat_size([[[1, 2]], [[3]]])
