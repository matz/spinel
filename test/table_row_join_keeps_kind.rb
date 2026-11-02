# A row of a table joined into another local is the row's Array
# A row read straight into a local was typed (`a = rows[0]` an Integer
# Array), but a local that joined rows, or a row and another Array, was
# boxed: `c ? rows[0] : rows[1]`, `c ? a : [5]`, and a second write to a
# row's local. The narrowed table and row locals were read at their
# narrowed kinds only once the round's writes were all joined; the writes
# joined before that read them boxed. They are now read at those kinds
# from their own write on.

c = ARGV.empty?

rows = [[1, 2], [3, 4]]
a = rows[0]
r1 = c ? a : [5]
r2 = c ? rows[0] : rows[1]
r3 = rows[1]
r3 = [] if ARGV.size > 3
p r1.sum, r2, r3.sum

# a row past the end is nil
r4 = c ? rows[9] : rows[1]
p r4

# a store through a joined row is a store into the table
grid = [[1, 2], [3, 4]]
g = c ? grid[0] : grid[1]
g << 9
g[0] += 10
p grid

floats = [[1.5], [2.5, 0.25]]
f1 = c ? floats[1] : floats[0]
p f1.sum

def pick(table, c)
  row = c ? table[0] : table[-1]
  row.sum + row.size
end
p pick([[1, 2, 3], [4]], true), pick([[1, 2, 3], [4]], false)

Item = Struct.new(:w)
items = [Item.new(1), Item.new(2)]
it = c ? items[1] : items[0]
p it.w
