# The same through `self.new` and `allocate` in an inherited class method: a
# descendant the program reaches only by calling the method is built, and
# rendered, as itself. So is one built by a method whose value is a `new` in
# parentheses, alone or after another statement, with the descendant's own
# method called on it.
class Shape
  def initialize(name = "shape")
    @name = name
  end

  def self.fresh = self.new
  def self.blank = allocate
  def to_s = "#{self.class}(#{@name})"
end

class Round < Shape
end

class Oval < Round
  def inspect = "oval:#{@name}"
end

class Flat < Round
end

def show(o) = puts [o, 1].inspect.gsub(/0x\h+/, "XX")

o = Oval.fresh
f = Flat.blank
puts o.class, f.class
puts o.to_s
show(o)
puts [o, f].map(&:class).inspect

class Box
  def initialize(x = 1) = @x = x
  def self.mk = (new)
  def self.mk2(n) = (n += 1; new(n))
  def self.mk3
    (new(7))
  end
end
class Crate < Box
  def crate = @x * 2
end
p Crate.mk.crate, Crate.mk2(2).crate, Crate.mk3.crate
p Crate.mk.class, Crate.mk2(1).class, Box.mk3.class
