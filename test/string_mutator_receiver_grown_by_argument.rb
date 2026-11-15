# A String method that changes its receiver reads the receiver once its
# arguments ran, as CRuby does: an argument that grows the String (here
# past its capacity, so the payload moves) is seen by the method, which
# then changes the grown String or leaves it as it is. The receiver is a
# local, a global, an attribute's assignment, `||=` and `&&=` on an
# attribute, and an instance variable, as a statement and as a value.
# Read ahead of such an argument, the bytes were the old payload, and the
# method wrote them back over the growth.

class K
  attr_accessor :a
  def initialize(a = +"abc") = (@a = a)
end

def grow(s, ch = "x") = (s << ch * 300; "Z")
def sm(x) = x.is_a?(String) ? [x.size, x[0, 6]] : x

s = +"abc"
s.delete_prefix!((s << "x" * 300; "Z"))
p sm(s)
s = +"abc"
s.delete_suffix!(grow(s))
p sm(s)
s = +"abc"
s.prepend(grow(s))
p sm(s)
$g = +"abc"
$g.delete_prefix!(($g << "x" * 300; "Z"))
p sm($g)

o = K.new(+"q"); t = +"abc"
(o.a = t).squeeze!((o.a << "x" * 300; "Z"))
p [sm(o.a), sm(t)]
o = K.new(+"q"); t = +"abc"
(o.a = t).sub!("a", (o.a << "x" * 300; "Z"))
p [sm(o.a), sm(t)]
o = K.new(nil); t = +"abc"
(o.a ||= t).delete!((o.a << "x" * 300; "Z"))
p [sm(o.a), sm(t)]
o = K.new(nil); t = +"abc"
(o.a ||= t).chomp!((o.a << "x" * 300; "Z"))
p [sm(o.a), sm(t)]
o = K.new(+"abc"); t = +"def"
(o.a &&= t).delete_suffix!((o.a << "x" * 300; "Z"))
p [sm(o.a), sm(t)]
o = K.new(nil); t = +"abc"
r = (o.a ||= t).squeeze!((o.a << "x" * 300; "x"))
p [sm(r), sm(o.a)]

class H
  def initialize = @s = +"abc"
  def run
    a = @s.delete_prefix!((@s << "x" * 300; "Z"))
    @s.delete_suffix!(grow(@s))
    [a, sm(@s)]
  end
end
p H.new.run
