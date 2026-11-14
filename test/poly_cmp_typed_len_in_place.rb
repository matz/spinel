# spinel: gc-stress
# A boxed comparison whose argument is a typed Array's or Hash's count
# (`pc < cmds.size`) held its receiver in the frame first, as for an
# argument that may run the program's code: a loop's condition stored and
# read back its counter on every pass. The count runs no code where the
# program leaves the method alone, so the comparison reads both in place.
# An argument that may write the receiver is still ordered after it.

def jumps_for(n) = n > 0 ? Array.new(n, 1) : nil

# a counter boxed by a nil-able table, against a String Array's size
def walk(cmds, jumps)
  pc = 0
  steps = 0
  while pc < cmds.size
    pc = jumps[pc] if cmds[pc] == "x"
    pc += 1
    steps += 1
  end
  steps
end
p walk(%w[a x b c], jumps_for(4))

Item = Struct.new(:w)
# a Hash's size and length, an object Array's, empty?, ==, <=, >=, <=>
def counts(h, items, k)
  [k < h.size, k <= items.length, k == items.size, k >= h.length, (k <=> items.size), k == items.empty?]
end
p counts({ a: 1, b: 2 }, [Item.new(1), Item.new(2), Item.new(3)], jumps_for(0) || 2)
p counts({}, [], jumps_for(1) ? 0 : nil)

# an argument that writes the receiver is still evaluated after it is read
def ordered(a)
  x = jumps_for(0) || 1
  r1 = x == (x = 5; a.size)
  y = jumps_for(0) || 3
  r2 = y < (y = 0; a.length)
  [r1, x, r2, y]
end
p ordered([1, 2, 3, 4, 5])

# a count read beside an operand that changes the container is read first
def show(*xs) = xs
a = [1, 2, 3]
p [a.size, a.push(4).size, a.length]
h = { a: 1 }
p [h.size, (h[:b] = 2), h.size]
b = [1, 2]
p show(b.size, b.pop, b.size, b.empty?)
d = [1, 2, 3]
p d.size + (d << 7).size
def grow(z) = (z << 0; z.size)
f = [1, 2]
p [f.size, grow(f), f.size]
class Bag
  def initialize = (@items = [1, 2])
  def pair = [@items.size, add, @items.size]
  def add = (@items << 3; @items.size)
end
p Bag.new.pair
