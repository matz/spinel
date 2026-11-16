# A store with two indices (`c[y, x] = v`, `c.[]=(y, x, v)`) through a box
# that can hold an instance of a program class whose `[]=` takes them calls
# that `[]=`; a real Array or String in the same box still splices.
# spinel: gc-stress

class Grid
  def initialize = @h = {}
  def [](y, x) = @h[[y, x]]
  def []=(y, x, v)
    @h[[y, x]] = v
  end
end

class Cell
  def [](y) = 0
  def []=(y, v)
    v
  end
end

boxes = [Grid.new, Cell.new, [1, 2, 3, 4], +"abcdef"]
g = boxes[0]
g[3, 3] = 9
p g[3, 3]
p(g[1, 1] = 5)
g.[]=(4, 4, 7)
p g[4, 4], g[1, 1]

# the box's Array and String take the builtin splice
a = boxes[2]
a[1, 2] = [9]
p a
s = boxes[3]
s[0, 3] = "Z"
p s

# through an element read, and as a counter in a method
held = [g]
held[0][2, 2] = "two"
p held[0][2, 2]

def bump(m, y, x)
  m[y, x] = (m[y, x] || 0) + 1
end
3.times { bump(boxes[0], 0, 0) }
p g[0, 0]

# a store whose class takes one index fewer raises, as in Ruby
c = boxes[1]
begin
  c[1, 2] = 3
rescue ArgumentError => e
  p e.class
end
