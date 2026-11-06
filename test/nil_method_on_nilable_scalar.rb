# A NilClass method of the program, called on a String, Integer or Float
# that may be nil, runs for nil. The other values raise NoMethodError.
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

# a name that ends in `?` gives nil's result, not a bool
class NilClass; def ready? = :nope; end
r = mk(false).ready?
p r, ms(false).ready?, mf(false).ready?
