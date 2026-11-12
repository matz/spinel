# A subclass's initialize runs `super` into a parent initialize that is
# replaced later. An object built before the replacement ran the first
# version, which leaves some ivars unset: they read nil, not 0 or 0.0.
# Which ivars a construction leaves set holds only where it holds for every
# initialize the construction may run.

class P
  def initialize(x)
    @x = x
    @f = 1.5
  end
end

class C < P
  attr_reader :x, :y, :f, :g, :c
  def initialize(x)
    super(x)
    @c = 7
  end

  def total
    y.nil? ? x : x + y
  end

  def scaled
    g.nil? ? f : f * g
  end
end

c1 = C.new(4)

class P
  def initialize(x)
    @x = x * 2
    @y = x + 1
    @f = 2.5
    @g = 0.5
  end
end

c2 = C.new(4)

p [c1.x, c1.y, c1.f, c1.g, c1.c]
p [c2.x, c2.y, c2.f, c2.g, c2.c]
p [c1.y.nil?, c1.g.nil?]
p [c1.total, c2.total]
p [c1.scaled, c2.scaled]
