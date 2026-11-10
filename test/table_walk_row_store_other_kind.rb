# A walk over a table of Integer or Float rows binds each row to its block's
# parameter. When the block stores a value of another kind into that row
# (unshift, insert, fill, concat), or hands the row on to a local
# that does, the table keeps its rows boxed. The typed row cut
# the value to an Integer or lost it.

t = [[1, 2], [3, 4]]
t.each { |r| r.unshift(1.5) }
p t

u = [[1, 2], [3, 4]]
u.each { |r| r.insert(1, 2.5) }
p u

v = [[1, 2], [3, 4]]
v.each { |r| r.fill(2.5) }
p v

w = [[1, 2], [3, 4]]
w.each { |r| r.concat([1.5]) }
p w

def walk(rows) = rows.each { |r| r.prepend("s") }
y = [[1, 2], [3, 4]]
walk(y)
p y

a1 = [[1, 2], [3, 4]]
a1.reverse_each { |r| r.unshift(1.5) }
p a1

a2 = [[1, 2], [3, 4]]
a2.each { |r| c = r; c << 2.5 }
p a2

n = [[1, 2], [3, 4]]
n.each_with_index { |r, i| r.fill(i * 0.5) }
p n

# a Float table: an Integer stays an Integer
f = [[1.5], [2.5]]
f.each { |r| r.unshift(7) }
p f

# stores of the row's own kind keep working
g = [[1, 2], [3, 4]]
g.each { |r| r << r.sum; r.unshift(0); r[1] = 9 }
p g
h = [[1.5], [2.5]]
h.each { |r| r.push(r.first * 2) }
p h
