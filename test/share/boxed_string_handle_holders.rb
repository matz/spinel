# spinel: share
# A new String boxed into a mixed container is boxed as its handle, so
# every holder the box is copied into -- a global, a class variable, a
# Struct member, a Hash value, an element, a parameter -- names the one
# String, and a change in place through one of them shows through the
# others. A plain box of its bytes was copied by value.
$h = [+"cd", 1].first
u = $h
u.upcase!
p $h, u

class K
  @@c = [+"ef", 1].first
  def self.go
    u = @@c
    u.upcase!
    p @@c
  end
end
K.go
$g = [+"ab", 1].first
def y(s) = s.upcase!
y($g)
p $g

S = Struct.new(:v)
o = S.new([+"ab", 1].first)
s = o.v
s.upcase!
p o.v, s

h = { a: [+"ab", 1].first }
s = h[:a]
t = s.upcase!
t << "!" if t
p h, s
a = [[+"cd", 1].first, 2]
u = a[0]
u.downcase!; u.upcase!
p a, u

$b = [+"ab", 1].first
t = $b.upcase!
t << "!" if t
p $b

s = [+"ab", 1].first
t = s << "c"; t << "!"
p s
s = [+"ab", 1].first
t = s.insert(1, "-"); t << "!"
p s
s = [+"ab", 1].first
t = s.replace(+"xy"); t << "!"
p s
s = [+"ab", 1].first
t = s.reverse!; t << "!"
p s

def k(s)
  t = s.upcase!
  t << "!"
end
a = [+"ab", 1]
k(a[0])
p a
h = { k: [+"cd", 1].first }
k(h[:k])
p h

def z(o)
  s = o.v
  t = s.upcase!
  t << "!"
  p o.v
end
z(S.new([+"ab", 1].first))

x = [+"ab", 1].first
s = x.to_s
p s.equal?(x)
t = x.itself
t << "!"
p x
