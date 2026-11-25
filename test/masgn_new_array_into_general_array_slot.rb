# A new Array a builtin answers, stored through a multiple assignment's
# target into a slot that a store through a parameter makes the general
# Array, is converted at the store, as a plain assignment converts it.

def y1(a) = yield(a)
def m1(x) = x << "q"

# a local, the store in a block
w, u = "ab".bytes, 2
y1(w) { |x| x << "q" }
p w, u

# a local, the store in a method
w2, u2 = "cd".bytes, 3
m1(w2)
p w2, u2

# a target after a splat
h3, *r3, w3 = 0, 1, "ef".bytes
m1(w3)
p h3, r3, w3

# an ivar of a class, the store in a block and in a method
class K
  attr_reader :w, :v
  def initialize = (@w, @n, @v = "gh".bytes, 1, "ij".bytes)
  def go = y1(@w) { |x| x << "q" }
  def go2 = m(@v)
  def y1(a) = yield(a)
  def m(a) = a << "r"
end
k = K.new
k.go
k.go2
p k.w, k.v

# the main object's ivar
@w4, @u4 = "kl".bytes, 4
y1(@w4) { |x| x << "q" }
p @w4, @u4
