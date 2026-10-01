# An Integer written into a variable the program widened to Bignum is that
# Integer in the wide representation, in every kind of variable and in both
# the statement and the value form of the write. A global took the Integer
# as it was and stored NULL, which read back as nil; the value forms of an
# ivar, a class variable and an attribute writer did not build. A top-level
# ivar's value form now takes its slot's kind too: `y = (@iv = [])` built an
# Integer array into a slot a later write had made a poly array. An
# attribute writer builds a bare Array.new or Hash.new at its field's kind,
# as `@x = Array.new` does, where it stored the general Array.

def big
  2**64
end
class Box
  attr_accessor :v
  @@c = nil
  def initialize; @v = big; @w = big; end
  def reset; p(@w = 0); @w; end
  def self.cv; @@c = big; @@c = 0; p(@@c = 1); @@c; end
end
def bump
  $b = 0
  3
end
$b = big
bump
p $b
x = Box.new
x.v = 0
p x.v
p(x.v = 1)
p x.reset
p Box.cv
X = [big]
l = big
l = 0
p l, (l = 1), l

y = (@iv = []); @iv = [1, :b]
p y, @iv

class Fld; attr_accessor :h, :a; end
f = Fld.new
f.h = {a: 1}
f.h = Hash.new
p f.h
f.a = [1.5]
f.a = Array.new
f.a << 2.5
p f.a
