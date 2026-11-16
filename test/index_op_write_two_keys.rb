# An index operator-write with more than one index on a program class whose
# `[]` and `[]=` take them (`m[y, x] += 1`) calls those methods as Ruby
# defines it: the receiver and each index evaluated once, in order, then
# `[]`, the value, the operator, and `[]=` with every index and the result.
# The expression answers the value assigned, whatever `[]=` returns.
# spinel: gc-stress

$log = []
class Grid
  def initialize = @h = {}
  def [](y, x)
    $log << [:get, y, x]
    @h[[y, x]]
  end
  def []=(y, x, v)
    $log << [:set, y, x, v]
    @h[[y, x]] = v
  end
end

class Cube
  def initialize = @h = Hash.new(0)
  def [](a, b, c) = @h[[a, b, c]]
  def []=(a, b, c, v)
    @h[[a, b, c]] = v
    :ignored
  end
end

def k(n) = ($log << [:key, n]; n)
def g(o) = ($log << :recv; o)

m = Grid.new
r = (g(m)[k(1), k(2)] ||= ($log << :val; "a"))
p r
g(m)[k(1), k(2)] += ($log << :val; "b")
g(m)[k(1), k(2)] &&= nil
x = (m[0, 0] &&= 5)
p x, m[1, 2]
p $log

# three indices, and the value of the expression
c = Cube.new
p(c[1, 2, 3] += 4)
c[1, 2, 3] *= 10
p c[1, 2, 3]

# the shape of a matrix counter, in a method and a block
class Matrix
  def initialize(h, w) = (@w = w; @a = Array.new(h * w, 0))
  def [](y, x) = @a[y * @w + x]
  def []=(y, x, v)
    @a[y * @w + x] = v
  end
end
def lay(depth, points)
  points.each { |(y, x)| depth[y, x] += 1 }
end
depth = Matrix.new(3, 3)
lay(depth, [[0, 1], [2, 2], [0, 1]])
depth[1, 1] ||= 7
p depth[0, 1], depth[2, 2], depth[1, 1]

# a `[]` that takes fewer indices raises ArgumentError, as in Ruby
class One
  def [](i) = 1
  def []=(i, v)
    v
  end
end
begin
  One.new[1, 2] += 1
rescue ArgumentError => e
  p e.class
end
