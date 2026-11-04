# Data#with on a Data with no initialize of its own applies every keyword
# and every `**hash` in order, the last one naming a member winning as in
# a Hash literal. It read only the last `**hash`, and a literal keyword
# won over a `**hash` after it.
D = Data.define(:a, :b, :c)
d = D.new(a: 1, b: 2, c: 3)
h1 = {a: 10}
h2 = {b: 20}
p d.with(**h1, **h2)
h = {a: 100}
p d.with(a: 1000, **h)
p d.with(**h, a: 1000)
x = 7
p d.with(c: x, **h1)
ab = {a: 5, b: 6}
b7 = {b: 7}
p d.with(**ab, **b7)
p d.with(b: 0, **h1, c: 9, **h2)
p d.with(**{}, b: 4, **{})
both = {a: -1, c: -3}
p d.with(a: 0, b: 0, c: 0, **h1, **both)

S = Data.define(:name, :tag)
s = S.new(name: "n", tag: :t)
nh = {name: "from-hash"}
p s.with(name: "lit", **nh)
tv = {tag: :v}
p s.with(**nh, tag: :u, **tv)
p s.with(**nh, **{name: "later"})

N = Data.define(:v, :w)
n = N.new(v: nil, w: 2)
p n.with(**{w: 5})
w6 = {w: 6}
p n.with(w: 1, **w6)

# Every element runs in source order, the receiver first, a hash literal's
# own values in its place, and an overridden keyword value still runs.
$log = []
def first_h = ($log << :first; {a: 1})
def mark_v = ($log << :mark; 2)
def recv_of(x) = ($log << :recv; x)
def side_v = ($log << :side; 5)
p recv_of(d).with(**first_h, **{b: mark_v})
b1 = {b: 1}
p d.with(a: side_v, **b1, a: 3)
p $log

# A keyword value read ahead of a later `**hash` keeps what it read.
class Upd
  def initialize = @v = 1
  def updates = (@v = 2; {})
  def go(x) = x.with(a: @v, **updates)
end
p Upd.new.go(d)

# A value a `**hash` gives keeps its own class, and a nil keeps its nil.
T = Data.define(:a, :b)
t = T.new(a: 0, b: 0)
p t.with(**{a: "text"}, **{})
U = Data.define(:a, :b, :c)
u = U.new(a: 1, b: 0, c: 0)
p u.with(**{a: nil}, **{})
un = U.new(a: nil, b: 0, c: 0)
p un.with(**{b: 3})
p un.with(**{}, a: 4, **{c: 1})
V = Data.define(:a, :b)
v = V.new(a: 0, b: 0)
sh = {a: "x"}
p v.with(b: 1, **sh)
p v.with(**sh, **{})

# With no `**hash` among them, a keyword a later one names again still runs.
$log = []
p d.with(a: side_v, b: 8, a: 3)
p $log
