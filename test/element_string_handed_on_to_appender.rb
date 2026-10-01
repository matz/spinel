# A String read out of a container and handed to a method that hands it on
# to one that appends to it: CRuby appends to the String the container
# holds. The element read went over as a copy, and the append was lost.

def bang(s) = (s << "!"; nil)
def via(s) = bang(s)
def via2(s) = via(s)
def up(s) = (s.upcase!; nil)
def via_up(s) = up(s)

# mixed values (a POLY hash), depth 2 and 3
h = {a: +"x", b: 1}
via(h[:a])
p h
via2(h[:a])
p h

# String-only containers
hs = {a: +"x", b: +"y"}
via(hs[:a])
p hs
arr = [+"p", +"q"]
via(arr[0])
via_up(arr[1])
p arr

# other mutators through the hand-on
def cat(s) = (s.concat("+"); nil)
def via_cat(s) = cat(s)
def rep(s) = (s.replace("zz"); nil)
def via_rep(s) = rep(s)
def ins(s) = (s.insert(0, ">"); nil)
def via_ins(s) = ins(s)
def set0(s) = (s[0] = "Z"; nil)
def via_set0(s) = set0(s)
a2 = [+"a", +"b", +"c", +"d", 5]
via_cat(a2[0]); via_rep(a2[1]); via_ins(a2[2]); via_set0(a2[3])
p a2

# a String-keyed hash, a nested container, first/last
sk = {"k" => +"v"}
via2(sk["k"])
p sk
nest = [[+"n"]]
via(nest[0][0])
hn = {a: {b: +"z"}}
via(hn[:a][:b])
p nest, hn
fl = [+"f", +"l"]
via(fl.first); via(fl.last)
p fl

# a local named from the element
lh = {a: +"x", b: 1}
s = lh[:a]
via(s)
p lh, s

# an ivar's container, a method with an object receiver, super, a rest
class Holder
  def initialize; @h = {a: +"x", b: 1}; @a = [+"y"]; end
  def add(s, t) = (s << t; nil)
  def wrap(s) = add(s, "*")
  def run
    wrap(@h[:a]); wrap(@a[0])
    p @h, @a
  end
end
Holder.new.run

class Base; def m(s) = (s << "!"; nil); end
class Bare < Base; def m(s) = super; end
class Named < Base; def m(s) = super(s); end
hb = {a: +"x", b: 1}
Bare.new.m(hb[:a]); Named.new.m(hb[:a])
p hb

def gather(*r) = bang(*r)
def gather_idx(*r) = (r[0] << "?"; nil)
hr = {a: +"x", b: 1}
gather(hr[:a]); gather_idx(hr[:a])
p hr

# a keyword beside it, and the result used
def app2(s, t) = (s << t; s.size)
def via_kw(s, t:) = app2(s, t) + 1
hk = {a: +"x", b: 1}
p via_kw(hk[:a], t: "?"), hk

# a chain longer than the analysis follows still reaches the container
def d10(s) = (s << "!"; nil)
def d9(s) = d10(s)
def d8(s) = d9(s)
def d7(s) = d8(s)
def d6(s) = d7(s)
def d5(s) = d6(s)
def d4(s) = d5(s)
def d3(s) = d4(s)
def d2(s) = d3(s)
def d1(s) = d2(s)
dh = {a: +"deep", n: 1}
d1(dh[:a])
p dh
