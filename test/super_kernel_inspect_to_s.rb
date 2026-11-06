# spinel: gc-stress
# `def inspect = super` and `def to_s = super` in a plain class reach
# Kernel's: the default rendering of self's class (its ivars for inspect),
# a new String each call. The super found no method of the program and
# raised NoMethodError, and its value, typed nothing, met what the share
# walk does not follow, so appending to it was refused (#9258).
class P
  def initialize(n) = @n = n
  def inspect = super
  def to_s = super
end

class Q < P
  def inspect = "Q(" + super + ")"
end

class V
  def initialize(x) = @x = x
  def inspect = super
end

def norm(s) = s.gsub(/0x\h+/, "X")

a = P.new(+"a")
s = a.inspect
s << "!"
t = a.to_s
t << "?"
q = Q.new(1).inspect
q << "."
p norm(s), norm(t), norm(q), P.new(1).inspect.size > 0
o = [a, Q.new(2), 3][ARGV.size]
u = o.inspect
u << "#"
p norm(u), norm(V.new(5).inspect), norm("#{Q.new(3)}")
