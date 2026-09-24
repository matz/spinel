# A user `self.new` reached through every `new` dispatch on a Class value:
# positional, zero-argument, splat, keywords, a boxed receiver read out of a
# Hash, and an inherited `self.new` that reads the receiving class. Classes
# without one still construct. (A is built statically once as well: a class
# built only through a Class value leaves its own to_s out of puts, a
# separate gap.)
class A
  def initialize(x = 0) = @x = x
  def to_s = "A(#{@x})"
end
class C
  def self.new(x = 1) = "C.new(#{x})"
end
class K
  def self.new(a:, b: 2) = "K.new(#{a}, #{b})"
end
class P
  def self.new(x) = "#{name}.new(#{x})"
end
class Q < P; end
class V
  def self.new(a, *rest, z) = "V.new(#{a}, #{rest}, #{z})"
end
class W
  def self.new(x, **kw) = "W.new(#{x}, #{kw.size})"
end
def pick(i) = [A, C, K, P, Q, V, W][i]
REG = {0 => A, 1 => C}
A.new(-1)
puts pick(0).new(5)
puts pick(1).new(6)
puts pick(1).new
puts pick(0).new
puts pick(1).new(*[8])
puts pick(2).new(a: 3)
puts pick(3).new(4)
puts pick(4).new(4)
puts pick(5).new(1, 2)
puts pick(5).new(1, 2, 3, 4)
puts pick(6).new(1)
puts REG.fetch(1).new(9)
puts REG.fetch(0).new(9)
begin
  pick(1).new(1, 2)
rescue ArgumentError => e
  p e.class
end
