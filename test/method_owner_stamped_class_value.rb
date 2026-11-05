# A Method read back out of a container, or out of a local written two
# Methods, answers its stamped owner as the class object: a class method's
# singleton has no name and equals the class's singleton_class, not the
# class. An accessor's, a Struct member's and a builtin's Method is owned by
# the class or module that declares the method: Kernel's frozen? and puts,
# BasicObject's equal?.
class C
  def self.a = 1
  def b = 2
end
ms = [C.method(:a), C.new.method(:b)]
p ms.map { |m| m.owner.name }, ms.map { |m| m.owner == C }, ms[0].owner == C.singleton_class
m = C.new.method(:b)
m = C.method(:a) if ARGV.empty?
p m.owner, m.owner.name, m.owner == C, m.owner.to_s
class L
  attr_reader :r, :t
  def initialize = (@r = 1; @t = 2)
end
class L2 < L; end
x = L.new.method(:r)
x = L2.new.method(:t) if ARGV.empty?
p x.owner, x.call
S = Struct.new(:a, :b)
y = S.new(1, 2).method(:a)
y = S.new(1, 2).method(:b) if ARGV.empty?
p y.owner, y.call
z = "ab".method(:upcase)
z = "ab".method(:between?) if ARGV.empty?
p z.owner
w = "ab".method(:upcase)
w = "ab".method(:frozen?) if ARGV.empty?
p w.owner
p L.new.method(:r).owner, S.new(1, 2).method(:a).owner, [1].method(:push).owner
p "ab".method(:frozen?).owner, 1.method(:equal?).owner, method(:puts).owner, 1.method(:abs2).owner
