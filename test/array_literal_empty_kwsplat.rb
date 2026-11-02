# An array literal's keyword hash made only of `**h` splats is an element only
# when it has an entry, as CRuby's: `[1, **{}]` is `[1]`, `[1, **h]` with h
# empty `[1]`, and `[1, a: 1, **h]` always holds the hash.

h = {}
p [1, **h]

h = {k: 1}
p [1, **h]

p [1, **{}]
p [1, "a", **{}]

x = [2, 3]
h = {}
p [*x, **h]
g = {z: 1}
p [*x, **g]

e = {}
f = {}
a = {a: 1}
b = {b: 2}
p [1, **e, **f]
p [1, **a, **f]
p [1, **e, **b]
p [1, **a, **b]
p [1, **a, **a]

h = {}
g = {z: 1}
p [1, a: 1, **h]
p [1, **h, a: 1]
p [1, a: 1, **g]
p [1, a: 1]

h = {}
g = {z: 1}
p [**h]
p [**g]
p [**{}]
p [**h, **h].size

h = {}
p [1, 2, **h]
p [1, 2, **h].size
g = {a: 1}
p [1, 2, **g]
p ["a", "b", **h]
p [1.5, 2.5, **h]
p [:a, **h]
p [[1], **h]

h = {}
50.times { |i| h[:"k#{i}"] = "v#{i}" * 3 }
r = []
200.times { |i| r << [i, "s#{i}", **h] }
p r.size, r.last.size, r.last.last.size, r[7][2][:k3]
e = {}
p 100.times.map { |i| [i.to_s, **e] }.last

class T
  def to_hash = {}
end
class U
  def to_hash = {u: 1}
end
p [1, **T.new]
p [1, **U.new]

n = nil
p [1, **n]
p [1, **nil]

begin
  x = 5
  p [1, **x]
rescue TypeError => e
  p e.class, e.message
end

$t = []
def tick(x) = ($t << x; x)
def hh(x, v) = ($t << x; v)
p [tick(1), tick(2), **hh(3, {})]
p $t
$t.clear
p [tick(1), **hh(3, {k: 1})]
p $t

def f(*r, **k) = [*r, **k]
p f(2)
p f(2, k: 1)
p f
p f(**{})
def g(**k) = [0, **k]
p g, g(a: 1)

def fsz(a) = a.size
def g(h) = [:r, **h]
e = {}
s = {"a" => 1}
p fsz([1, **e]), fsz([1, **s])
p g(e), g({x: 2}), g(s)
p({v: [1, **e]})
p [[1, **e], [2, **s]]
p [1, **e].frozen?
x = [3, **e]
x << 4
p x

# a method ending in a literal with `**h`, and a multiple assignment from
# one, have no fixed tuple shape: the hash is left out when it is empty
def kw_pair(h) = [1, "s", **h]
e = {}
p kw_pair(e)[-2]
p kw_pair({k: 1})[-2]
a1, b1, c1 = kw_pair(e)
p [a1, b1, c1]
a2, b2, c2 = kw_pair({k: 1})
p [a2, b2, c2]
a3, b3, c3 = [1, "s", **e]
p [a3, b3, c3]
a4, *r4 = [1, **e]
p [a4, r4]
a5, b5 = [1, **{k: 2}]
p [a5, b5]
