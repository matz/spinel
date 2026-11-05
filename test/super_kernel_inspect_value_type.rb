# `def inspect = super` in a class held by value (scalar or frozen-literal
# ivars, no subclass) reaches Kernel's inspect with the address of self's
# struct on the caller's stack. The hook rooted that address as an object,
# and the collection that marked it wrote into the stack: called often
# enough for collections to run, the program crashed.
class V
  def initialize(x) = @x = x
  def inspect = super
end

class W
  def initialize(x, y) = (@x = x; @y = y; @tag = "w")
  def inspect = "W:" + super
  def to_s = super
end

class H
  def initialize(s) = @s = s
  def inspect = super
end

def norm(s) = s.gsub(/0x\h+/, "X")

keep = []
n = 0
20_000.times do |i|
  keep << H.new(+"h#{i}")
  keep.shift if keep.size > 50
  n += V.new(i).inspect.size + W.new(i, 0.5).inspect.size
end
p n
p norm(V.new(7).inspect), norm(W.new(1, 2.5).inspect), norm(W.new(3, 0.0).to_s)
p norm(keep.last.inspect)
