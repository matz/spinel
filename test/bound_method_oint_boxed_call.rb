# A bound Method read out of an Array and called there, whose target answers
# a nil-capable Integer or takes one as its parameter: nil comes back as nil,
# and a passed Integer arrives as itself.
class Latch
  def initialize = @l = nil
  def peek(a) = @l
  def poke(v) = @l = v
end
x = Latch.new
f = [x.method(:peek)] * 4
p f[0][1]
x.poke(5)
p f[1][2]
x.poke(nil)
p f[2][3]
p f[2].call(3)
x.poke(7)
p f[3][4] + 1
p f[3].call(4)

def g(a) = a.nil? ? -1 : a + 1
p g(nil)
p g(3)
ms = [method(:g)] * 2
p ms[0][2]
p ms[1][40]
p ms[0].call(nil)
p ms[1].call(8)
