# A multiple assignment whose right side is a local table of Integer or
# Float rows hands each target one row, nil past the end. The table is an
# sp_PtrArray of row pointers once a class is in the program, and the
# destructure read it with the flat Integer Array's getter: the C did not
# compile. Element, attribute and nested targets each take a row.
class Box
  attr_accessor :l, :r
end

t1 = [[1, 2], [3]]
g = [0, "x", 1]
g[0], g[1], g[2] = t1
p g
p g[0].sum + g[1].sum

t2 = [[4, 5], [6]]
bx = Box.new
bx.l, bx.r = t2
p [bx.l, bx.r]

t3 = [[7], [8, 9], [10]]
h = {}
h[:l], (h[:m], h[:r]) = t3
p h

f = [[1.5, 2.5], [3.5]]
k = [0, "x"]
k[0], k[1] = f
p k
