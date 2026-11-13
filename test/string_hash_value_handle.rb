# spinel: gc-minor
# spinel: gc-stress
# A Hash of String values (String or Integer keys) holds each value as the
# String itself: reading a value hands that String back, and storing one
# hands the Hash the String, not a copy of its bytes. After each way of
# reading a value (h[k], fetch, dig, values, a block parameter of each,
# each_pair, each_value, select, map, min_by, merge's block), a change made
# through the String that came out shows through the Hash and every other
# holder; after each way of storing one (`[]=`, store, a literal, Hash.new's
# default, `||=`, the op-assigns, the copies merge, update, select, slice,
# transform_keys), the String in the Hash is the one that went in.
#
# Sections: identity of each read; block parameters; stores; Hash.new with
# a String variable as its default; op-assigns (`<<=` appends to the String
# the Hash holds, `*=`, `%=`, `+=`, `||=`), on a frozen Hash too; copies
# between Hashes; nil values, compact and compact! (and compact! on Integer
# values, which cannot be nil there); a String stored while
# its key and other operands allocate, which a collection under
# SPINEL_GC_STRESS must not free; every read and store inside a method.
def make(n) = +"made#{n}"
def maybe(s) = s.empty? ? nil : +s

def churn(n)
  x = +""
  n.times { |i| x = "g#{i}" * 3 }
  x
end

# --- identity of each read -------------------------------------------------
h = {"a" => +"x", "b" => +"y", "c" => +"z"}
v = h["a"]
h["a"] << "1"
p v, v.equal?(h["a"])
h.fetch("b") << "2"
p h["b"]
h.dig("c") << "3"
p h["c"]
h.values[0] << "4"
p h["a"], v
g = {1 => +"p", 2 => +"q"}
w = g[1]
g[1] << "!"
p w, g[2].equal?(g.fetch(2)), g.values.last.equal?(g[2])

# --- block parameters ------------------------------------------------------
h.each { |k, s| s << "e" }
p h
h.each_pair { |k, s| s << "p" if k == "a" }
h.each_value { |s| s << "v" }
p h, v
g.each { |k, s| s << k.to_s }
p g, w
m = h.min_by { |k, s| s.size }
m[1] << "m"
p h["b"]
sel = h.select { |k, s| k != "c" }
sel["a"] << "s"
p v, sel["a"].equal?(v)
mapped = h.map { |k, s| s }
mapped[0] << "M"
p v
mg = h.merge({"a" => +"o"}) { |k, mine, theirs| mine << theirs; mine }
p mg["a"].equal?(v), v

# --- stores ----------------------------------------------------------------
s1 = +"one"
t = {}
t["k"] = s1
s1 << "+"
p t
t.store("j", s1)
t["j"] << "#"
p s1, t
lit = {"l" => s1, "m" => +"mm"}
lit["l"] << "L"
p s1
x = (t["n"] = s1)
x << "N"
p t["n"], s1
it = {}
it[3] = s1
it[4] = it[3]
it[4] << "I"
p it, s1

# --- Hash.new with a String variable as its default ------------------------
d = +"dflt"
dh = Hash.new(d)
dh["x"] = +"X"
p dh["nope"], dh["nope"].equal?(d)
d << "!"
p dh["nope"]
di = Hash.new(d)
di[1] = +"one"
p di[2].equal?(d), di[1]

# --- op-assigns ------------------------------------------------------------
o = {"a" => +"x", "b" => +"y"}
y = o["b"]
o["b"] <<= "4"
p o["b"], y, o["b"].equal?(y)
z = o["a"]
o["a"] += "5"
p o["a"], z, o["a"].equal?(z)
o["a"] *= 2
p o
pc = {1 => +"%d-%s"}
pc[1] %= [5, "z"]
p pc
o["c"] ||= +"c0"
o["c"] << "1"
o["c"] ||= +"never"
p o["c"]
r = (o["d"] ||= +"d0")
r << "1"
p o["d"]
begin
  o["e"] <<= "q"
rescue NoMethodError => e
  p e.class
end
od = Hash.new(+"D")
od["k"] <<= "1"
p od, od["zz"]
o.freeze
begin
  o["a"] <<= "!"
rescue FrozenError => e
  p e.class
end
p o["a"]

# --- copies between Hashes -------------------------------------------------
src = {"a" => +"x", "b" => +"y"}
sa = src["a"]
cp = src.merge({"c" => +"z"})
cp["a"] << "1"
p sa
dst = {"q" => +"q"}
dst.update(src)
dst["b"] << "2"
p src["b"]
sl = src.slice("a")
sl["a"] << "3"
p sa
tk = src.transform_keys { |k| k.upcase }
tk["A"] << "4"
p sa, src

# --- nil values, compact and compact! -------------------------------------
n = {"a" => maybe("x"), "b" => maybe("")}
p n, n.compact, n.compact.size
na = n["a"]
n.compact["a"] << "c"
p na
p n.compact!, n, n.compact!
ni = {}
3.times { |i| ni[i] = maybe(i.odd? ? "v#{i}" : "") }
p ni.compact, ni.compact!, ni
nd = Hash.new(+"D")
nd["x"] = maybe("")
nd["y"] = +"y"
nc = nd.compact
p nc, nc["none"]
nn = {"a" => +"x"}
p nn.compact!, nn
ia = {1 => 2}
sa = {"s" => 2}
p ia.compact!, sa.compact!, ia.compact, sa.compact
nn.freeze
begin
  nn.compact!
rescue FrozenError => e
  p e.class
end

# --- stores while the key and other operands allocate ----------------------
big = {}
20.times do |i|
  big["key#{i}#{churn(2)}"] = make(i)
  big[churn(1) + i.to_s] = make(i + 100)
end
p big.size, big.values.count { |s| s.start_with?("made") }
held = big.values.first
churn(50)
held << "!"
p big.values.first, big.values.first.equal?(held)
bi = {}
30.times { |i| bi[i] = (i.even? ? make(i) : "lit") }
churn(40)
p bi[2], bi[3], bi.size

# --- inside a method -------------------------------------------------------
def inside(h, k)
  s = h[k]
  s << "+"
  h[k + "2"] = s
  h[k] <<= "-"
  h.each_value { |e| e << "." }
  [s, h]
end
a2 = {"m" => +"in"}
s2, h2 = inside(a2, "m")
p s2, h2, h2["m"].equal?(h2["m2"])
