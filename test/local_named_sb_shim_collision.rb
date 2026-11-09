# A shared-handle shim reads and writes a String through a shadow copy of it
# for the length of one call, and named that shadow `lv__sb<N>` (N the
# shim's temp), which is the C name of a program local `_sb<N>`. The shim's
# arm then read the shadow where the program meant its own local, so the C
# did not compile; a held chain's shadow, `lv__sh<N>`, had the same
# collision with `_sh<N>`. The shadows are now spelled with an uppercase
# letter, which no Ruby local can start with. Each shape below keeps a local
# of the name its shim's shadow had, in a method of its own so that the temp
# numbers stay small: a statement and a value `s[i] = v`, a reader's
# String (`o.name.insert`, as a value and as a statement), an ivar's, one
# reached through a method's answer, a Hash value and an Array element, and
# a held chain's shadow.
def id(x) = x

def aset
  s = +"abc"
  _sb1 = +"P"
  s[0] = _sb1
  s
end
p aset

class Named
  attr_reader :name
  def initialize; @name = +"bob"; end
end
def reader(o)
  _sb5 = +"-"
  _sb11 = +"+"
  x = o.name.insert(0, _sb5)
  o.name.insert(0, _sb11)
  [x, o.name]
end
p reader(Named.new)

class Held
  def initialize; @s = +"bob"; end
  def go
    _sb1 = +"-"
    _sb4 = +"+"
    @s.insert(0, _sb1)
    x = @s.insert(0, _sb4)
    [x, @s]
  end
end
p Held.new.go

def routed
  s = +"abcd"
  _sb3 = 0
  t = id(s).slice!(_sb3)
  [t, s]
end
p routed

def hash_value
  h = {k: +"abcd", n: 1}
  _sb4 = 0
  t = h[:k].slice!(_sb4)
  h[:k].insert(_sb4, "Z")
  [t, h]
end
p hash_value

def array_element
  a = [+"x", +"y"]
  _sb5 = +"P"
  a[0][0] = _sb5
  a[1].insert(1, _sb5)
  a
end
p array_element

def held_chain
  _sh1 = "W"
  u = +"wbcw"
  id(u).upcase!.sub!(_sh1) { |m| m + m }
  [u, _sh1]
end
p held_chain

# a local that only looks like a shadow is its own: no shim here, as before
_sb2 = +"P"
q = +"abc"
q[0] = _sb2
p q, _sb2
_sb1 = +"R"
p _sb1
