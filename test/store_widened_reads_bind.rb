# A value read from an Array that a store has widened (`w = t[1]` after
# `t[1] = 3` made a String Array the general Array) is a box wherever the
# program passes it: a Struct member, a constructor's parameter and the
# instance variable it sets, a method's parameter. The same holds for a
# value a multiple assignment takes from such an Array, and for a copy of
# either.
# spinel: gc-stress

Board = Struct.new(:width, :height)

class Keep
  def initialize(v) = @v = v
  def v = @v
end

def twice(z) = z * 2

t = ["a", "b"]
t[1] = 3
w = t[1]
p Board.new(w, 1)
p Keep.new(w).v + 1, twice(w)

u = ["x", "y", "z"]
u[1..-1] = u[1..-1].map(&:size)
_, a, b = u
p Board.new(a, b), Keep.new(b).v + 1
c = a
p twice(c)

# a value computed from such a read, through a chain of calls on the Array
v1 = t[1] + 1
v2 = t[1].succ
v3 = t.first(2).last
p Board.new(v1, v2), twice(v3)

# the shape of a parser: the locals a block destructures into, passed on
def read(lines)
  width = nil
  height = nil
  lines.each do |line|
    tokens = line.split(" ")
    tokens[1..-1] = tokens[1..-1].map(&:to_i)
    case tokens[0]
    when "B"
      _, width, height = tokens
    end
  end
  Board.new(width, height)
end
bd = read(["B 75 74", "P 1 2"])
p bd, bd.width * bd.height

# a String Array no store widens keeps its Strings
names = ["ann", "bob"]
first = names[0]
p Board.new(first, names[1]).width.upcase
