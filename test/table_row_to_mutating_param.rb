# A row a walk binds, handed to a method that stores a value of another
# kind into its parameter. The table had narrowed to Integer rows, the
# parameter is a general Array, and the row would have been copied into
# it and the store lost, so the call was refused at compile time; through
# each_with_index, a printed table raised TypeError instead. The widening
# that builds such an argument as a general Array where it is made now
# follows a block parameter to the elements of the walk's receiver, as it
# does for a boxed one, so the table's rows are built as general Arrays.

def shl1(a) = a << "s"
t1 = [[1, 2], [3]]
t1.each { |r| shl1(r) }
p t1[0], t1[1]

# each_with_index (an iterator moved onto its Enumerable copy), and the
# same table printed
def shl2(a) = a << "s"
t2 = [[1, 2], [3]]
t2.each_with_index { |r, i| shl2(r) if i.zero? }
p t2
def shl3(a) = a << :sym
t3 = [[4], [5]]
t3.each_with_index { |r, _| shl3(r) }
p t3[0], t3[1]

# map, reverse_each and select
def shl4(a) = a.push("m")
t4 = [[1], [2]]
t4.map { |r| shl4(r) }
p t4[0], t4[1]
def shl5(a) = a.unshift(nil)
t5 = [[6, 7]]
t5.reverse_each { |r| shl5(r) }
p t5[0]
def shl6(a) = (a[0] = "x")
t6 = [[8], [9]]
t6.select { |r| shl6(r) }
p t6[0], t6[1]

# a Float store into a row of Integers, and into a row of Floats a String
def set7(a) = (a[0] = 1.5)
t7 = [[1, 2], [3]]
t7.each { |r| set7(r) }
p t7[0], t7[1]
def set8(a) = (a[1] = "f")
t8 = [[1.5, 2.5]]
t8.each { |r| set8(r) }
p t8[0]

# an instance variable's table, walked inside the class
class Grid
  def initialize = (@rows = [[1, 2], [3]])
  def mark = @rows.each { |r| tag(r) }
  def tag(a) = a << "t"
  def rows = [@rows[0], @rows[1]]
end
g = Grid.new
g.mark
p g.rows

# an element taken apart over several parameters: each binds the
# element's own entry, a row there among them
def put11(a) = a << "s"
t11 = [[0, [1]], [2, [3, 4]]]
t11.each { |_k, row| put11(row) }
p t11
def put12(a) = a << :y
t12 = [[[5], 0]]
t12.each { |row, _k| put12(row) }
p t12

# the same with the element assembled in a local first
def put13(a) = a << "s"
pair = [0, [1]]
t13 = [pair]
t13.each { |_k, row| put13(row) }
p t13, pair

# a store of the rows' own kind prints as before
def add9(a) = a << 9
t9 = [[1, 2], [3]]
t9.each { |r| add9(r) }
p t9[0], t9[1], t9[0].sum

# a walk whose rows go to no such method: the table still narrows
t10 = [[1, 2], [3]]
t10.each { |r| r << r.size }
p t10[0], t10[1]
