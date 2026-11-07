# A walk over a table of Integer rows a method's parameter holds binds each
# row to its block's parameter, and `row.each(&h)` walks the row with a
# proc. The parameter was typed from the walk's binding alone: it first saw
# the table boxed, while the parameter's table was still to narrow, kept
# that through the rounds, and was retyped only after them, when the
# rewrite that turns `&h` into a block had passed it by. The call was then
# refused at compile time, though the same walk over a local's table
# compiled. Such a walk's parameter now takes the row type as the table
# narrows, as a walk over an Array of objects does.

def run(xs)
  acc = []
  xs.each do |row|
    h = proc { |v| acc << v }
    row.each(&h)
  end
  acc
end
t = [[1], [2, 3]]
p run(t)

# each_with_index and reverse_each
def indexed(xs)
  out = []
  xs.each_with_index do |row, i|
    add = proc { |v| out << v * 10 + i }
    row.each(&add)
  end
  xs.reverse_each do |row|
    neg = proc { |v| out << -v }
    row.each(&neg)
  end
  out
end
p indexed([[1, 2], [3]])

# Float rows
def floats(xs)
  sum = 0.0
  xs.each do |row|
    f = proc { |v| sum += v * 2 }
    row.each(&f)
  end
  sum
end
p floats([[1.5], [2.25, 0.5]])

# a row extended in the walk, and handed on to a method
def grow(xs)
  xs.each do |row|
    row << row.sum
  end
  xs
end
def total(row) = row.sum
def walk_total(xs)
  s = 0
  xs.each { |row| s += total(row) }
  s
end
g = grow([[1, 2], [3]])
p g
p walk_total(g)

# a table one caller fills with a String: the rows stay boxed
def describe(xs)
  out = []
  xs.each do |row|
    d = proc { |v| out << v.inspect }
    row.each(&d)
  end
  out
end
p describe([[1, 2]])
p describe([[1], "ab".chars])
