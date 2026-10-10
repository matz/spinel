# `a || b` over an Integer or Float that can be nil answers a only when a
# is not nil, so it is nil only where b can be: `@ram[i] || 0` and a local
# written from it hold no nil. A nil right side keeps the nil.
a = [1, 2][ARGV.size + 3]
b = a || 0
p b, b + 1
c = a || nil
p c
d = [3][ARGV.size] || a
p d
e = (a || 7) * 2
p e
class R
  def initialize; @ram = [0] * 4; end
  def poke(i, v) = @ram[i % 8] = v
  def peek(i) = @ram[i] || 0
  attr_reader :ram
end
r = R.new
r.poke(6, 5)
p r.peek(6), r.peek(5), r.peek(9), r.peek(5) + 1
f = 1.5 if ARGV.size > 0
g = f || 2.5
p g, g * 2
h = false || nil
p h
