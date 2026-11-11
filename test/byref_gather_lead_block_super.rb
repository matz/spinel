# A String a method appends to is the caller's String, and Spinel hands the
# parameter that takes it the caller's own handle. Two binders handed a copy
# instead. A call whose count the run time decides (a splat with an argument
# after it) binds every positional out of one gathered Array, so a String
# written ahead of the splat went into a temp; and a bare `super` inside a
# block passed the method's captured parameter as a temp of its value. Each
# probe appends LONG, which always reallocates, and prints what the caller's
# name sees.
# spinel: gc-minor
LONG = "!" * 100

def seen(s) = [s[0], s.size]

# ahead of the first splat of a gathering call: beside a post, three leading
# ones, a keyword whose default reads the first, a class method, an ivar
def grow(a, b, *r, z) = (a << LONG; b << "?" if b.is_a?(String); r.size)
def trio(a, b, c, *r) = (a << LONG; b << LONG; c.size)
def gkw(a, b, *r, z, k: a.size) = (a << LONG; k)
class Gath
  def self.cg(a, b, *r, z) = (a << LONG; z)
  def initialize = (@buf = +"I")
  def run(xs) = (grow(@buf, *xs, 9); seen(@buf))
end
xs = [+"y", +"w"]
s = +"A"; p grow(s, *xs, 9); p seen(s), xs
s = +"B"; p grow(s, *[], +"q", 9); p seen(s)
s = +"C"; t = +"t"; p trio(s, t, *xs, 1); p seen(s), seen(t)
s = +"D"; p gkw(s, *xs, 9); p seen(s)
s = +"E"; p Gath.cg(s, *xs, 7); p seen(s)
p Gath.new.run([1])

# ... but not one a later argument can give another value: the gather ran
# that argument first, so the parameter is the String read before it, and
# the caller's name holds the new one -- an assignment, one in a block, one
# behind `&&` or `||`, a lambda assigning its capture, a method assigning the
# ivar, and a keyword's default reading the parameter; a later read is no
# rebinding
def bind(a, b, *r, z) = (a << LONG; [seen(a), z])
def cond = true
s = +"L"; p bind(s, *xs, (s = +"q"; 9)), seen(s)
s = +"M"; p bind(s, *xs, [1].map { s = +"b"; 1 }.size), seen(s)
s = +"N"; p bind(s, *xs, (cond && (s = +"n"; 5))), seen(s)
s = +"O"; p bind(s, *xs, (!cond || (s = +"o"; 6))), seen(s)
s = +"P"; p gkw(s, *xs, (s = +"q"; 9)), seen(s)
s = +"R"; p bind(s, *xs, s.size), seen(s)
t = +"S"; f = -> { t = +"l"; 7 }; p bind(t, *xs, f.call), seen(t)
class Gath
  def bump = (@buf = +"m"; 3)
  def rebind(xs) = [bind(@buf, *xs, (@buf = +"j"; 8)), seen(@buf)]
  def rebump(xs) = [bind(@buf, *xs, bump), seen(@buf)]
end
p Gath.new.rebind([1]), Gath.new.rebump([1])

# the same with a second name for the String, which makes the local a shared
# handle the later assignment would overwrite, and with no splat at all: the
# call ran the variable first, so it binds the value read then, never the
# slot or the handle read late (names of their own: an aliased String
# reaches a method as a handle, and the method's other callers with it)
def bind2(a, b, *r, z) = (a << LONG; [seen(a), z])
def pair(a, z) = (a << LONG; [seen(a), z])
u = +"T"; o = u; p bind2(u, *xs, (u = +"q"; 9)), seen(u)
v = +"U"; o = v; arr = [v]; p pair(v, (v = +"q"; 1)), seen(v)
w = +"V"; p pair(w, (w = +"q"; 1)), seen(w)
class Twin
  def initialize = (@buf = +"W")
  def rebind(xs) = (w = @buf; [bind2(@buf, *xs, (@buf = +"j"; 8)), seen(@buf)])
end
class Solo
  def initialize = (@buf = +"X")
  def rebind = [pair(@buf, (@buf = +"j"; 2)), seen(@buf)]
end
p Twin.new.rebind([1]), Solo.new.rebind

# a bare super inside a block the method hands on: the parameter is the
# block's capture, and the parent's appends reach it through the capture's
# cell -- a plain parameter, a keyword, the one ahead of a rest, a post, and
# into an included module's copy
class Runner
  def self.run(n, &b) = n == 0 ? b.call : run(n - 1, &b)
end
module Mod
  def mm(s) = (s << LONG; nil)
end
class Par
  def pl(s) = (s << LONG; nil)
  def kw(s, k:) = (k << LONG; nil)
  def le(a, *r) = (a << LONG; r.size)
  def po(a, *r, z) = (z << LONG; r.size)
end
class Chi < Par
  include Mod
  def pl(s) = (Runner.run(1) { super }; seen(s))
  def kw(s, k:) = (Runner.run(1) { super }; seen(k))
  def le(a, *r) = (Runner.run(1) { super }; seen(a))
  def po(a, *r, z) = (Runner.run(1) { super }; seen(z))
  def mm(s) = (Runner.run(1) { super }; seen(s))
end
c = Chi.new
s = +"F"; p c.pl(s), seen(s)
s = +"G"; p c.kw(1, k: s), seen(s)
s = +"H"; p c.le(s, 2, 3), seen(s)
s = +"J"; p c.po(1, 2, s), seen(s)
s = +"K"; p c.mm(s), seen(s)

# The same super from procs that outlive the call. The slot there would be
# the capture's cell, a heap object the parent stores young Strings into
# (#4391), so the parameter is the shared handle instead: the parent appends
# to it in place, and the buffers come back whole under a minor collection.
FRAG = "-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
PROCS = []
module Keep
  def keep(s) = (s << "k#{FRAG}"; nil)
end
class Keeper
  include Keep
  def keep(s) = (PROCS << proc { super; s.size }; nil)
end
kp = Keeper.new
8.times { kp.keep(String.new) }
junk = []
r = 0
last = 0
while r < 3000
  junk << "j#{r}#{FRAG}"
  junk.shift while junk.size > 20
  last = PROCS[r % 8].call
  r += 1
end
p last
