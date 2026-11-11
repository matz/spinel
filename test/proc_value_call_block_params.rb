# spinel: gc-minor
# A literal block given at a call on a Proc or a Method value is the
# callee's block: its parameters take what the callee calls it with, not
# what the block's own body would make of them. Typed from the body,
# `y << "?"` made `y` an Array and the String the proc passed crashed
# the program; `y << 2` did the same for an Integer. A Proc, a lambda and
# a Method are the receivers, called with call, .(), [] and yield;
# the block is called, passed on, kept and called later with two values,
# and a boxed value that may be a Proc (made by proc, lambda or Kernel.proc)
# or a user object binds it boxed.

s = +"a"
pr = proc { |x, &b| b.call(x) }
r = pr.call(s) { |y| y << "1"; y }
p [s, r.equal?(s)]
pr.(s) { |y| y << "2" }
pr.yield(s) { |y| y << "3" }
p pr.call(1) { |y| y << 2 }
p pr.call([1]) { |y| y << 2 }
p pr.call(5) { |y| y + 1 }

l = ->(x, &b) { b.call(x) }
l[s] { |y| y << "4" }
l.(s) { |y| y << "5" }
p s

def m(x, &b) = b.call(x)
mo = method(:m)
r = mo.call(s) { |y| y << "6"; y }
p [s, r.equal?(s)]
p method(:m).call(3) { |y| y << 1 }

def via(pr, v) = pr.call(v) { |y| y << "7"; y }
via(proc { |x, &b| b.call(x) }, s)
p s
slot = [proc { |x, &b| b.call(x) }, method(:m)]
slot[0].call(s) { |y| y << "s" }
slot[1].call(s) { |y| y << "t" }
p s

on = proc { |x, &b| [x].map(&b) }
p on.call(s) { |y| y << "8"; y.size }

$kept = nil
keep = proc { |&b| $kept = b; nil }
keep.call { |y, z| y << z; y }
$kept.call(s, "9")
p s
a = [0]
$kept.call(a, 1)
p a

# a boxed value that is a Proc, or an object of the one class whose call
# takes a block: the Proc hands the block the box, so it binds boxed
class Doubler
  def call(x, &b) = b.call(x * 2)
end
[Doubler.new, proc { |x, &b| b.call(x.to_s) }].each { |it| p it.call(3) { |y| y.inspect } }
def run(it) = p(it.call(3) { |y| y.inspect })
[Doubler.new, Kernel.proc { |x, &b| b.call(x.to_s) }, Kernel.lambda { |x, &b| b.call(x.to_s) }].each { |w| run(w) }

# Kernel's proc and lambda boxed beside an Integer, called by a name no
# user class defines (yield), so no class's method is adopted
v = [Kernel.proc { |x, &b| b.call(x) }, 5][ARGV.size]
t = +"k"
p v.yield(t) { |y| y << "5" }
p t
w = [Kernel.lambda { |x, &b| b.call(x) }, 5][ARGV.size]
a = [0]
w.yield(a) { |y| y << 1 }
p a
