# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# A real -2^63 stored into an Integer slot that can also hold nil (a local
# written nil on another path, an ivar seeded nil, a parameter also passed
# nil). The slot's nil is kept beside the value, so the Integer is held and
# read back, as CRuby does.
n = ARGV.size
m = -9223372036854775807 - (n + 1)

x = 5
x = nil if m == 0
x = m
p x
p x.nil?

y = nil
y = m if n == 0
p y
p y.nil?
p(y ? "set" : "unset")

class Box
  def initialize = @v = nil
  def set(v) = @v = v
  attr_reader :v
end
b = Box.new
b.set(m)
p b.v
p b.v.nil?

def f(v) = v
f(nil)
p f(m)
p f(m).nil?
