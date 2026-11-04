# A super in ==, equal?, eql?, !, nil?, frozen?, hash or itself that no
# program ancestor defines reaches BasicObject's or Kernel's method: the
# same object, false, the frozen bit, the object's hash or the object.
# A class under BasicObject has BasicObject's ==, equal? and ! only.
class Pt
  attr_reader :x
  def initialize(x) = @x = x
  def ==(o) = super
  def equal?(o) = super
  def eql?(o) = super
  def hash = super
  def !() = super
  def nil? = super
  def frozen? = super
  def itself = super
end
a = Pt.new(1)
b = Pt.new(1)
p a == a, a == b, a == 1, a.equal?(a), a.equal?(b)
p a.eql?(a), a.eql?(b), a.hash == a.hash, !a, a.nil?
p a.frozen?, a.freeze.frozen?, a.itself.equal?(a)
h = { a => :a }
p h[a], h[b]
p [a, a, b].uniq.size

class Sub < Pt; end
s = Sub.new(2)
p s == s, s == a

class Bare < BasicObject
  def ==(o) = super
  def equal?(o) = super
  def !() = super
  def hash = super
end
z = Bare.new
p z == z, z == 1, z.equal?(z), !z
begin
  z.hash
rescue NoMethodError
  puts "NoMethodError"
end
