# A frozen String literal stored where a String handle is demanded (a
# gathered rest argument, an initialize's, an Array element, a Hash value,
# a multiple assignment's element) is that literal itself: mutation raises
# FrozenError, frozen? is true, and every read answers the one object. A
# new String stored beside it is still the caller's object, changed in
# place through the store.
class Nums
  attr_reader :xs
  def initialize(*xs); @xs = xs; end
  def self.make(*xs) = xs
end
s = +"ab"
t = +"cd"
a = Nums.make(s, t)
a[1] << "!"
a[0] << "?"
p [s, t]
b = Nums.new(s, t)
b.xs[1] << "#"
p t
d = Nums.make("lit", "lit2")
begin
  d[0] << "x"
rescue => e
  p e.class
end
p d[0].frozen?, d[1].frozen?
e2 = Nums.new("lit", "lit2")
begin
  e2.xs[1] << "x"
rescue => e
  p e.class
end
p d[0].equal?(e2.xs[0]), e2.xs
# an element, a value, a multiple assignment
v = [+"q"]
v[0], v[1] = "lit", +"cd"
v[1] << "z"
p v, v[0].frozen?, v[0].equal?(d[0])
h = {}
h[:k] = "lit"
p h[:k].equal?(d[0]), h[:k].frozen?
w = []
w.unshift((w << 1; "lit"), "lit2")
p w, w[0].equal?(d[0])
