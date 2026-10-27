# Kernel#Array takes an Array subclass instance as it is, but a Hash or
# String subclass instance is not an Array: a Hash one answers its pairs
# (or what its own #to_ary / #to_a answers), a String one is wrapped
# (or asked the same way).
class H < Hash
  def conv = Array(self)
end

class G < Hash
  def to_a = [:own]
end

class K < Hash
  def to_ary = [:ary]
end

class R < Hash
  def to_a = super.reverse
end

class N < Hash
  alias_method :to_ary, :keys
end

class P < Hash
  private def to_a = [:priv]
end

class J < Hash
  def to_ary = 5
end

class S < String; end

class L < String
  alias_method :to_ary, :lines
end

class T < Array; end

class Box
  def initialize(h) = @h = h
  def conv = Array(@h)
end

def via(x) = Array(x)

h = H.new
h[:a] = 1
h[:b] = 2
x = Array(h)
p x
p x.class
x << [:c, 3]
p h
p h.conv
p Box.new(h).conv
p via(h)
p Array(H.new)
p Array(G.new)
p Array(K.new)
r = R.new
r[1] = 2
r[3] = 4
p Array(r)
n = N.new
n[5] = 6
p Array(n)
p Array(P.new)
begin
  Array(J.new)
rescue TypeError => e
  p e.message
end

s = S.new("q")
p Array(s)
p Array(s)[0].class
p via(S.new("w"))
p Array(L.new("a\nb"))

t = T.new([1, 2])
p Array(t).equal?(t)
p Array(t).class
