# A `new` site runs the initialize that is current where it runs, not the
# last one the program defines: its arguments, defaults, keywords and block
# bind to that version, and the ivars only the later version sets stay nil.
class K
  attr_reader :a, :b
  def initialize(a, d = 2)
    @a = a + d
  end
end
k1 = K.new(1)
class K
  def initialize(a, k: 10)
    @a = a * k
    @b = 0.5
  end
end
k2 = K.new(1, k: 3)
k3 = K.new(4)
p [k1.a, k1.b, k2.a, k2.b, k3.a, k3.b]
p k1.b.nil?, k2.b + k3.b

class Pt < Struct.new(:x, :y)
  def initialize(x, y) = super(x, y)
end
p1 = Pt.new(1, 2)
class Pt
  def initialize(x, y) = super(x + 100, y + 100)
end
p2 = Pt.new(1, 2)
p p1.to_a, p2.to_a

class E < StandardError
  def initialize(msg = "one") = super(msg)
end
begin; raise E; rescue E => e; p e.message; end
class E
  def initialize(msg = "two") = super(msg + "!")
end
begin; raise E; rescue E => e; p e.message; end
p E.new("x").message

class Y
  def initialize = yield(1)
end
Y.new { |x| p x + 1 }
class Y
  def initialize = yield("s")
end
Y.new { |x| p x + "t" }
