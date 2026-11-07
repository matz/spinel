# A table of Integer (or Float) rows written with an empty row among its
# rows. The empty `[]` had been given the general Array's kind before the
# table narrowed, so it was built as one and read back as an Integer row:
# a length of 8 and the general Array's storage as its elements, a push
# into it lost, and a row taken apart over two block parameters bound to
# 0 and 0. Such a table now stays a general Array and answers as CRuby.

x = [[1, 2], [], [3, 4]]
x.each { |a, b| p [a, b] }
x[1] << 7
p x.size, x[0], x[1], x[2]

# an empty row first and last, read by index and by size
y = [[], [5], []]
p y[0], y[0].size, y[0].empty?, y[2].size
y[2].push(8, 9)
p y[0], y[1], y[2]

# Float rows
f = [[1.5], []]
f[1] << 2.5
p f[0], f[1], f.sum { |r| r.sum }

# Float rows whose empty row is only read: built as a Float row
w = [[1.5, 2.5], []]
p w[0].sum, w[1].size, w[1].empty?

# each_with_index and map over the rows
z = [[1], [], [2, 3]]
z.each_with_index { |r, i| p [i, r.size] }
p z.map(&:size)

# an instance variable
class Grid
  def initialize
    @rows = [[1, 2], []]
  end

  def grow
    @rows[1] << 7
    @rows[1].size
  end

  def pairs = @rows.map { |a, b| [a, b] }
end
g = Grid.new
p g.grow, g.pairs

# a table without an empty row still narrows and answers as before
t = [[1, 2], [3, 4]]
t[0] << 5
p t[0], t[1], t[0].sum
