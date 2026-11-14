# An accessor that is no member of its Struct or Data class gets the same
# per-call answers as any other; a member's own reader keeps its slot.
S = Struct.new(:a) do
  attr_reader :z
  def initialize(*); super; @z = :attr; end
end
s = S.new(1)
p s.z
class S; def z = :def; end
p s.z, s.a

class T < Struct.new(:a)
  attr_reader :z
  def initialize(a); super(a); @z = :attr; end
end
t = T.new(1)
p t.z
class T; def z = :def; end
p t.z, t.a

D = Data.define(:m) do
  def self.make = new(m: 7)
end
class D
  attr_reader :w
  def w = :def
end
p D.make.m, D.make.w
