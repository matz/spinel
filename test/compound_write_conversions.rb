# The `||=` and `&&=` writes of every kind of variable, and a multiple
# assignment's targets, store their value through the same conversion as a
# plain write. `x.b ||= raise(...)` put the raise, a call with no value,
# into the attribute's Integer field and the C did not build. A bare
# Array.new written by `&&=` is built at the slot's kind, not as the general
# Array its own emission is.

class Box
  attr_accessor :b, :w
  def initialize; @b = 10; @w = nil; end
end
x = Box.new
x.b ||= raise("should not be executed")
p x.b
x.w ||= 5
p x.w
def big = 2**64
$g = nil
$g ||= 0
$g = big if $g > 5
p $g
@t = nil
@t ||= 0
@t = big if @t > 5
p @t
class K
  @@c = nil
  def self.run; @@c ||= 0; @@c = 2**64 if @@c > 5; @@c; end
end
p K.run
l = nil
l ||= 0
l = big if l > 5
p l
y = 5
y &&= raise("no") if false
p y
a, $h = 1, 2
p a, $h

class K
  @@a = [1.5]
  def self.go; @@a &&= Array.new; @@a << 2.5; @@a; end
end
p K.go
$a = [1.5]
$a &&= Array.new
$a << 3.5
p $a
z = [1.5]
z &&= Array.new
z << 4.5
p z
