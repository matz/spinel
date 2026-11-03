# The class of a value-type instance is known statically, but the receiver
# expression still runs: its assignments and calls happen.
class Pt
  def initialize(x, y) = (@x = x; @y = y)
  attr_reader :x, :y
end

$log = []
def make(x, y)
  $log << [x, y]
  Pt.new(x, y)
end

p((n = 5; Pt.new(1, 2)).class)
p n
p make(3, 4).class
p $log
p((q = Pt.new(6, 7)).class)
p q.x + q.y
k = Pt.new(8, 9)
p k.class
p [k.x, k.y]
