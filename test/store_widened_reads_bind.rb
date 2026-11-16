# A value read from an Array that a store has widened (`w = t[1]` after
# `t[1] = 3` made a String Array the general Array) is a box wherever the
# program passes it: a Struct member, a constructor's parameter and the
# instance variable it sets, a method's parameter. The same holds for a
# value a multiple assignment takes from such an Array, for a value
# computed from the read, and for a copy of either. Each case has its own
# Struct, class and method, so nothing else gives them a box.
# spinel: gc-stress

# a read after a store
B1 = Struct.new(:width, :height)
class K1
  def initialize(v) = @v = v
  def v = @v
end
def twice1(z) = z * 2
t = ["a", "b"]
t[1] = 3
w = t[1]
p B1.new(w, 1)
p K1.new(w).v + 1, twice1(w)

# targets of a multiple assignment, and a copy of one
B2 = Struct.new(:width, :height)
class K2
  def initialize(v) = @v = v
  def v = @v
end
def twice2(z) = z * 2
u = ["x", "y", "z"]
u[1..-1] = u[1..-1].map(&:size)
_, a, b = u
p B2.new(a, b), K2.new(b).v + 1
c = a
p twice2(c)

# a value computed from such a read, through a chain of calls on the Array
B3 = Struct.new(:width, :height)
def twice3(z) = z * 2
s3 = ["a", "b", "c"]
s3[1] = 4
v1 = s3[1] + 1
v2 = s3[1].succ
v3 = s3.first(2).last
p B3.new(v1, v2), twice3(v3)

# an alias of the Array, and a copy of the read
B4 = Struct.new(:width, :height)
def twice4(z) = z * 2
al = ["p", "q"]
ar = al
al[1] = 8
w2 = ar[1]
w3 = w2
p B4.new(w3, 1), twice4(w3)

# an Integer Array given a Float, and one given nil
B5 = Struct.new(:width, :height)
class K5
  def initialize(v) = @v = v
  def v = @v
end
f = [1, 2]
f[1] = 2.5
fv = f[1]
p B5.new(fv, 1), K5.new(fv).v
B6 = Struct.new(:width, :height)
def show6(z) = z.inspect
nl = [1, 2]
nl[1] = nil
nv = nl[1]
p B6.new(nv, 1), show6(nv)

# the shape of a parser: the locals a block destructures into, passed on
B7 = Struct.new(:width, :height)
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
  B7.new(width, height)
end
bd = read(["B 75 74", "P 1 2"])
p bd, bd.width * bd.height

# a String Array no store widens keeps its Strings
B8 = Struct.new(:width, :height)
names = ["ann", "bob"]
first = names[0]
p B8.new(first, names[1]).width.upcase
