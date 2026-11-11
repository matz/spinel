# A String's object_id and its equal? are one question, asked of the same
# identity. equal? read the identity the String plan renders for a value
# (the handle a producer holds or publishes, a box, the bytes), while
# object_id knew only a list of producers that had a handle and took the
# bytes' address for anything else, so a chain that hands its receiver on
# through a method (`id(s).concat("5")`, `s.concat("6")&.object_id`, a frozen
# `id(s).itself`) answered another id than `s.object_id`, as did a String read
# back from a Hash. object_id now renders the identity face and asks the
# box of it, so the two agree. Freezing a String does not change its id.
def id(x) = x
def same(a, b) = [a.equal?(b), a.object_id == b.object_id]

s = +"ab"
p id(s).concat("5").object_id == s.object_id
p s.concat("6")&.object_id == s.object_id
p s.upcase!&.object_id == s.object_id
p id(s).upcase!.object_id == s.object_id
p id(s).itself.object_id == s.object_id
p id(s).object_id == s.object_id
p [s].first.concat("x").object_id == s.object_id
p s.then { _1 }.concat("x").object_id == s.object_id
p s.tap { |t| t << "y" }.object_id == s.object_id
p s.freeze.object_id == s.object_id
p id(s).itself.object_id == s.object_id
p id(s).itself.equal?(s)
p s

# copies are other objects, a literal is one object
t = +"ab"
p t.dup.object_id == t.object_id, t.upcase.object_id == t.object_id
p (+t).object_id == t.object_id, (-t).object_id == t.object_id
p "lit".object_id == "lit".object_id
p same("lit", "lit")
u = "frozen"
p same(u, u), same(u.dup, u), same(+u, u), same(-u, u)
p :sy.to_s.object_id == :sy.to_s.object_id, :sy.to_s.equal?(:sy.to_s)

# kept in an Array, a Hash, an ivar, and read back
v = +"kept"
a = [v]
h = { k: v }
p same(a[0], v), same(a.first, v), same(h[:k], v), same(h.fetch(:k), v)
a.freeze
w = "z".freeze
p same([w][0], w), same({ k: w }[:k], w)
class Holder
  def initialize(s) = @s = s
  def get = @s
  attr_reader :s
end
x = +"held"
o = Holder.new(x)
p same(o.get, x), same(o.s, x)
y = "held".freeze
oy = Holder.new(y)
p same(oy.get, y), same(oy.s, y)

# nil is nil's id, through a nil-able String
n = [nil, v][0]
p n.object_id == nil.object_id, n&.object_id
m = [nil, v][1]
p m&.object_id == v.object_id

# a conditional and a method's answer
c = ARGV.empty? ? v : +"other"
p same(c, v)
d = id(ARGV.empty? ? v : +"other")
p same(d, v)
e = v.dup << "!"
p same(e, v), v
GC.start
p same(a[0], v), same(h[:k], v), same(o.get, x)

# freezing a String does not change its identity: an id taken before the
# freeze is the one after it, through every name of the object
f = +"ab"
fi = f.object_id
f.freeze
p f.object_id == fi, f.equal?(f)
g = +"ab"
g << "cd"
gi = g.object_id
g.replace("zz")
g.freeze
p g.object_id == gi
q = +"ab"
qi = q.object_id
r = q.freeze
p r.object_id == qi, q.object_id == qi, r.equal?(q)
class Fz
  def initialize = (@s = +"q")
  def fr = @s.freeze
  def oid = @s.object_id
end
fz = Fz.new
fzi = fz.oid
fz.fr
p fz.oid == fzi
$gv = +"g"
gvi = $gv.object_id
$gv.freeze
p $gv.object_id == gvi
ar = [+"x"]
ari = ar[0].object_id
ar[0].freeze
p ar[0].object_id == ari
dd = "lit".dup
ddi = dd.object_id
dd.freeze
p dd.object_id == ddi
k = 3
ip = "v#{k}"
ipi = ip.object_id
ip.freeze
p ip.object_id == ipi
ipd = "w#{k}".dup
ipdi = ipd.object_id
ipd << "z"
ipd.freeze
p ipd.object_id == ipdi
kept = +"kept"
ka = [kept]
kh = { k: kept }
kid = kept.object_id
kept.freeze
p ka[0].equal?(kept), kh[:k].equal?(kept), ka[0].object_id == kid, kh[:k].object_id == kid

# a String a builtin produced (its handle adopts the bytes) keeps its id too
up = "ab".upcase
upi = up.object_id
up.freeze
p up.object_id == upi
jn = [1, 2].join(",")
jni = jn.object_id
jb = [jn]
jn.freeze
p jn.object_id == jni, jb[0].equal?(jn), jb[0].object_id == jni
ce = "q".center(5)
cei = ce.object_id
ce2 = ce
ce.freeze
p ce2.object_id == cei, ce2.equal?(ce)
