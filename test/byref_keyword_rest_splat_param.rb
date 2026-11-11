# A String a method appends to is the caller's String: CRuby passes the object,
# so the growth shows through every name for it. Spinel hands a plain
# positional parameter the caller's own handle, but a keyword
# parameter, a positional beside a rest, a keyword or a post, and a value that
# arrived through a splat or a `**` all took a copy, and the appends stayed in
# the callee once the String outgrew its first allocation. Each probe appends
# LONG, which always reallocates, and prints what the caller's name sees.
# spinel: gc-minor
LONG = "!" * 100

def seen(s) = [s[0], s.size]

# keyword parameters: required, optional (and its own default), beside **kw
def kreq(k:) = (k << LONG; nil)
def kopt(k: +"d") = (k << LONG; k.size)
def kkw(k:, **o) = (k << LONG; o)
s = +"A"; kreq(k: s); p seen(s)
s = +"B"; kopt(k: s); p seen(s); p kopt
s = +"C"; p kkw(k: s, z: 1); p seen(s)

# a positional beside a rest, a keyword or a default reading it, and a post
def prest(a, *r) = (a << LONG; r)
def pkw(a, k: 0) = (a << LONG; k)
def pdef(a, b = a.size, k: b) = (a << LONG; [b, k])
def ppost(*r, a) = (a << LONG; r)
s = +"D"; p prest(s); p prest(s, 1, 2); p seen(s)
s = +"E"; p pkw(s); p pkw(s, k: 1); p seen(s)
s = +"F"; p pdef(s); p seen(s)
s = +"G"; p ppost(1, s); p seen(s)

# passed on into another lent keyword, and an ivar lent to one
def kfwd(k:) = kreq(k: k)
s = +"H"; kfwd(k: s); p seen(s)
class Box
  def initialize = (@s = +"I")
  def grow = (kbox(k: @s); seen(@s))
  def kbox(k:) = (k << LONG; nil)
end
p Box.new.grow

# instance and class methods with a rest and keywords
class Pad
  def m(a, *r, k: 0) = (a << LONG; [r, k])
  def self.c(k:, **o) = (k << LONG; o)
end
s = +"J"; p Pad.new.m(s, 1, k: 2); p seen(s)
s = +"K"; p Pad.c(k: s); p seen(s)

# through a splat: of a literal, of an Array holding the String, after an
# empty one; and through a `**` of a Hash holding it, boxed or not
def one(a) = (a << LONG; nil)
def one_kw(a, k: 1) = (a << LONG; k)
s = +"L"; one(*[s]); p seen(s)
s = +"M"; arr = [s]; one(*arr); p seen(s); p seen(arr[0])
s = +"N"; one(*[], s); p seen(s)
s = +"O"; p one_kw(*[s], k: 3); p seen(s)
def dkw(k:) = (k << LONG; nil)
def dkwo(k: 70) = (k << LONG; nil)
s = +"P"; h = { k: s }; dkw(**h); p seen(s); p seen(h[:k])
s = +"Q"; h = [{ k: s }, 0][0]; dkwo(**h); p seen(s)
s = +"R"; h = { k: s }; dkw(k: h[:k]); p seen(s)

# a parameter widened past String by another caller still shares the String
def wide(x) = (x << LONG; x.size)
s = +"S"; wide(s); p seen(s); p wide([])

# bare and explicit super, with keywords, a rest, and into a module's method
class Base
  def g(a, k: 1) = (a << LONG; k)
  def r(a, *rest) = (a << LONG; rest)
end
class Kid < Base
  def g(a, k: 2) = super
  def r(a, *rest) = super
end
class Kid2 < Base
  def g(a, k: 2) = super(a, k: k + 1)
end
module Grow
  def g(a) = (a << LONG; nil)
end
class Mixed
  include Grow
  def g(a) = super
end
# (a String of its own: the one above is also stored in an Array, which makes
# it a shared handle, and a handle reaches a name defined twice as a copy)
def supers
  s = +"T"; p Kid.new.g(s); p seen(s)
  s = +"U"; p Kid.new.r(s, 9); p seen(s)
  s = +"V"; p Kid2.new.g(s); p seen(s)
  s = +"W"; Mixed.new.g(s); p seen(s)
end
supers

# a bare super from a method with a rest into a parent's post: the gather ends
# with this method's posts, whatever the parent's count -- a matching post, one
# of two, the last of a fixed count, a post after an optional
class Tail
  def last(a, *r, z) = (z << LONG; r)
  def gain(*rest, value) = (value << LONG; rest.size)
  def two(*r, y, z) = (z << LONG; [r, y.size])
  def fix(a, b, c) = (c << LONG; b.size)
  def opt(a, b = +"o", c) = (c << LONG; b)
end
class Kin < Tail
  def last(a, *r, z) = super
  def gain(*rest, value) = super
  def two(a, *r, z) = super
  def fix(x, *r, z) = super
  def opt(x, *r, z) = super
end
def tails
  k = Kin.new
  s = +"X"; p k.last(+"a", s); p k.last(+"a", +"b", +"c", s); p seen(s)
  s = +"Y"; p k.gain(s); p k.gain(+"a", s); p seen(s)
  s = +"Z"; u = +"u"; p k.two(u, s); p [seen(s), seen(u)]
  s = +"a"; u = +"u"; p k.fix(u, +"v", s); p [seen(s), seen(u)]
  s = +"b"; p k.opt(+"u", s); p k.opt(+"u", +"v", s); p seen(s)
end
tails
