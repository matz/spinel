# A NilClass method of the program, called on a String, Integer or Float
# that may be nil, runs for nil. The other values raise NoMethodError, or
# run their own class's method of the name.
class NilClass
  def t = "nil!"
  def add(a, b = 2) = a + b
end

def mk(f) = f ? 7 : nil
def mf(f) = f ? 1.5 : nil
def ms(f) = f ? "s" : nil

p mk(false).t, mf(false).t, ms(false).t
p mk(false).add(1), mk(false).add(1, 5), ms(false).add(3)
[true, false].each do |f|
  p((mk(f).t rescue $!.message))
  p((mf(f).t rescue $!.message))
  p((ms(f).t rescue $!.message))
end

class C
  def initialize(f) = (@v = f ? 3 : nil)
  def go = @v.t
end
p((C.new(false).go rescue $!.message), (C.new(true).go rescue $!.message))

x = mk(false)
y = x.t
p y.upcase, y.class
p mk(false)&.t

# Object has the method too: nil runs NilClass's, the others Object's.
class Object
  def u = "obj"
end
class NilClass
  def u = "niu"
end
p mk(false).u, mk(true).u, ms(false).u, ms(true).u, mf(false).u, mf(true).u

# String has its own: nil runs NilClass's.
class String
  def w = "str"
end
class NilClass
  def w = "nilw"
end
p ms(false).w, ms(true).w
