# A String mutator whose argument assigns the variable its receiver reads:
# Ruby evaluates the receiver first, so the call mutates and answers the
# String the variable held then, and the argument's assignment stands.

# value and statement position, a local
v = +"a"; r = v << (v = +"b"); p [r, v]
w = +"a"; r = w.concat(w = +"b"); p [r, w]
x = +"a"; r = x.prepend(x = +"b"); p [r, x]
y = +"a"; y << (y = +"c"); p y
t = +"a"; r = t.insert(1, (t = +"b")); p [r, t]
u = +"ab"; u.insert(1, (u = +"q")); p u
s = +"ab"; r = s.slice!((s = +"zz"; 0)); p [r, s]
q = +"ab"; q.slice!((q = +"zz"; 0)); p q

# an alias of the old String sees the mutation
a = +"ab"; al = a; a[0] = (a = +"q"); p [a, al]
b = +"ab"; bl = b; r = b.insert(0, (b = +"q")); p [r, b, bl]
k = +"a"; kl = k; r = k << "x" << (k = +"b"); p [r, k, kl]

# a conditional, a block, a lambda, a multiple assignment in the argument
c1 = +"ab"; f = [true, false].first; r = c1 << (f ? (c1 = +"q") : "z"); p [r, c1]
c2 = +"ab"; f = [false, true].first; r = c2 << (f ? (c2 = +"q") : "z"); p [r, c2]
c3 = +"ab"; r = c3.sub!("a") { c3 = +"q"; "z" }; p [r, c3]
c4 = +"ab"; r = c4 << -> { c4 = +"q"; "z" }.call; p [r, c4]
c5 = +"ab"; r = c5 << ((c5, n5 = +"q", 1); "z"); p [r, c5, n5]
c6 = +"ab"; r = c6.concat("x", (c6 = +"q")); p [r, c6]

# a frozen receiver raises, after the argument ran
fz = "lit"
begin
  fz << (fz = +"q")
rescue FrozenError => e
  p e.class
end
p fz

# a captured local, a parameter, a loop
cap = +"a"; [1].each { r = cap << (cap = +"b"); p [r, cap] }
def rebind(s) = [s << (s = +"q"), s]
arg = +"ab"; p rebind(arg); p arg
i = 0; lp = +"a"
while i < 3
  lp << (lp = +"b#{i}")
  i += 1
end
p lp

# instance, class and global variables, and a top-level ivar
class Holder
  @@cv = +"ab"
  def initialize = (@iv = +"ab")
  def iv = [@iv << (@iv = +"q"), @iv]
  def self.cv = [@@cv << (@@cv = +"q"), @@cv]
end
p Holder.new.iv
p Holder.cv
$gv = +"ab"; r = $gv << ($gv = +"q"); p [r, $gv]
@top = +"a"; r = @top << (@top = +"b"); p [r, @top]
