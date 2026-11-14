# A push or an append of several arguments pushes each one before the next
# is read, and an argument that reads the Array being pushed onto -- an
# element, its size, its last -- is a pure read the operand order left in
# place, so it read the Array the earlier pushes had grown: `k.push(k[-2],
# k[-2])` pushed the element it had just pushed, and a later argument that
# runs code (`a.push(5, last(a))`) ran on it too. CRuby reads every argument
# first. Each argument is now read before the first push.

k = [0, 1]
k.push(k[-2], k[-2])
p k

f = [0.5, 1.5]
f.append(f[-2], f[-2])
p f

s = ["a", "b"]
s.push(s[-2], s.first, s[-2])
p s

m = [1, "b"]
m.push(m[-2], m[-2])
p m

n = [[1], [2]]
n.push(n[-2], n[-2])
p n

# a count and a last element
c = [7, 8]
c.push(c.size, c.length, c.last, c.empty?)
p c

# through another name for the same Array
x = [0, 1]
y = x
x.push(y[-2], y[-2])
p x

# in value position, and spread from a literal
v = [0, 1]
p v.push(v[-2], v[-2])
w = [0, 1]
w.push(*[w[-2], w[-2]])
p w

# a Hash read beside the pushes
h = { a: 3 }
z = [0]
z.push(h.size, h[:a], z.size)
p z

# a lambda among the arguments, and a Struct member's Array
l = [0, 1]
l.push(-> { l.size }, l.size)
p l.size, l[2].call, l[3]
Pair = Struct.new(:items)
pr = Pair.new([1, 2])
pr.items.push(pr.items[-2], pr.items[-2])
p pr.items

# an ivar
class Stack
  def initialize = (@items = [5, 6])
  def dup_second = (@items.push(@items[-2], @items[-2]); @items)
end
p Stack.new.dup_second

# a later argument that runs code reaching the Array: a method handed it,
# a method reading the ivar, a lambda over the local
def last_of(xs) = xs[-1]
e = [0, 1]
e.push(5, last_of(e))
p e
class Queue2
  def initialize = (@items = [0, 1])
  def peek = @items[-1]
  def go = (@items.push(5, peek); @items)
end
p Queue2.new.go
lc = [0, 1]
tail = -> { lc[-1] }
lc.append(9, tail.call)
p lc

# pushes that read no container keep their order too
q = [1]
q.push(2, 3)
t = 4
q.push(t, t + 1)
p q

# Strings and Arrays pushed, collected between: each read is held
g = ["x", "y"]
300.times { |i| g.push(g[-2] + i.to_s, g[-2]); g.shift(2) }
p g.size, g[0].size > 0
