# spinel: gc-minor
# spinel: gc-stress
# append_as_bytes with several arguments takes every argument before it
# appends any, as CRuby measures each one before it copies: an argument that
# is the receiver itself appends the String as it was, each time, and an
# argument that changes the receiver has run before the first byte goes in.
#
# Sections: the receiver as an argument (a local, beside an Integer, a
# global, an instance variable, an attribute, an Array element, a boxed
# value, a parameter, a conditional with both arms); arguments that run code;
# the errors (a frozen receiver, an argument of the wrong type); a loop
# whose arguments allocate.

def eff(x) = ($e += 1; x)
$e = 0

class Box
  attr_accessor :a
  def initialize(a) = @a = a
end

def grow(s) = s.append_as_bytes(s, s)

def both_arms(c)
  j = +"ja"
  r = (c ? j : nil).append_as_bytes(j, "-", j)
  [r, j, r.equal?(j)]
end

def looped
  s = +"hh"
  1000.times { s.append_as_bytes(s.size.to_s, s[0, 2] + "x", 7) }
  [s.size, s[0, 12]]
end

puts "== the receiver as an argument"
m = +"la"
m.append_as_bytes(m, m)
p m
m = +"la"
r = m.append_as_bytes(m, 33, m)
p [r, m, r.equal?(m)]
m = +"la"
m.append_as_bytes("é", m)
p [m.bytesize, m.encoding]
$g = +"gg"
$g.append_as_bytes($g, $g)
p $g
@i = +"ii"
@i.append_as_bytes(@i, "z", @i)
p @i
o = Box.new(+"oo")
o.a.append_as_bytes(o.a, o.a)
p o.a
ar = [+"ar"]
ar[0].append_as_bytes(ar[0], ar[0])
p ar
x = [1, +"po"].last
x.append_as_bytes(x, x)
p x
y = [2, +"py"][1]
m = +"mm"
m.append_as_bytes(y, y, m)
p m
p grow(+"pp")
p both_arms(true)

puts "== arguments that run code"
m = +"la"
m.append_as_bytes((m << "x"; "a"), m)
p m
m = +"la"
m.append_as_bytes(m, (m << "x"; "a"))
p m
m = +"la"
m.append_as_bytes(1, (m << "x"; 2))
p m
m = +"la"
m.append_as_bytes(eff(m), eff("b"), 256, -1)
p [m.bytes, $e]

puts "== errors"
f = "fz".freeze
begin
  f.append_as_bytes(f, f)
rescue => e
  p e.class
end
m = +"la"
begin
  m.append_as_bytes(m, nil)
rescue => e
  p [e.class, e.message, m]
end

puts "== allocating arguments"
p looped
