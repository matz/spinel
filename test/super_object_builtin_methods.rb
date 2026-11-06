# A super in ==, equal?, eql?, !, nil?, frozen? or itself that no program
# ancestor defines reaches BasicObject's or Kernel's method: the same
# object, false, the frozen bit or the object.
# A class under BasicObject has BasicObject's ==, equal? and ! only.
class Pt
  attr_reader :x
  def initialize(x) = @x = x
  def ==(o) = super
  def equal?(o) = super
  def eql?(o) = super
  def !() = super
  def nil? = super
  def frozen? = super
  def itself = super
end
a = Pt.new(1)
b = Pt.new(1)
p a == a, a == b, a == 1, a.equal?(a), a.equal?(b)
p a.eql?(a), a.eql?(b), !a, a.nil?
p a.frozen?, a.freeze.frozen?, a.itself.equal?(a)
p [a, a, b].uniq.size, [b].include?(a)

class Sub < Pt; end
s = Sub.new(2)
p s == s, s == a

class Bare < BasicObject
  def ==(o) = super
  def equal?(o) = super
  def !() = super
  def nil? = super
end
z = Bare.new
p z == z, z == 1, z.equal?(z), !z
begin
  z.nil?
rescue NoMethodError
  puts "NoMethodError"
end


# A forwarded parameter that takes an Integer and nil is compared too.
class Opt
  def ==(o) = super
end
q = Opt.new
p q == 1, q == nil

# The is_a? fold boxes such a parameter the same way.
class Ok
  def is_a?(k) = super
end
def kk(f) = f ? 1 : nil
begin
  p Ok.new.is_a?(kk(false))
rescue TypeError
  puts "TypeError"
end
p Ok.new.is_a?(Ok)

# A block argument to such a super runs, after the arguments, though the
# method it reaches takes no block.
$n = 0
def side
  $n += 1
  nil
end
class Blk
  def ==(o) = super(o, &side)
  def ! = super(&side)
  def nil? = super(&side)
  def frozen? = super(&side)
  def itself = super(&side)
  def is_a?(k) = super(k, &side)
end
g = Blk.new
p g == g, !g, g.nil?, g.frozen?, g.itself.class, g.is_a?(Blk), $n

# The method's own block, forwarded, builds too.
class Fwd
  def kind_of?(k, &b) = super(k, &b)
  def equal?(o, &) = super(o, &)
  def nil?(&b) = super(&b)
end
f = Fwd.new
p f.kind_of?(Fwd) { }, f.equal?(f), f.nil?
