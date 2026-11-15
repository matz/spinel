# A String operand is read when the call takes it, after every sibling
# operand has run: a read-only method's receiver (center, ljust, gsub, *,
# %, include?, +) sees an append that a later argument makes through a
# proc, a method, a block or an interpolation's to_s, whatever holds the
# String (a local, a captured local, an ivar, a global, a class variable),
# and a receiver that may be nil, tested after the arguments. The receiver
# held its bytes ahead of the arguments, so the call read the String as it
# was before the append (and, past capacity, a moved payload).

s = +"ab"; g = proc { s << "zz"; "q" }
p s.center(10, g.call)
t = +"ab"; h = -> { t << "zz"; 3 }
p t.center(h.call + 5)
u = +"ab"; k = -> { u << "zz"; "-" }
p u.ljust(8, k.call)
v = +"ab"; big = proc { v << ("y" * 40); "-" }
p v.center(50, big.call).size
w = +"ab"; gw = proc { w << "a"; "X" }
p w.gsub("a", gw.call)
x = +"ab"; gx = proc { x << "c"; 2 }
p x * gx.call
y = +"%s-"; gy = proc { y << "%s"; ["a", "b"] }
p y % gy.call
z = +"ab"; gz = proc { z.replace("zzzz"); "zz" }
p z.include?(gz.call)

class Holder
  @@c = +"ab"
  def initialize = @s = +"ab"
  def bump = (@s << "zz"; "-")
  def cbump = (@@c << "zz"; "-")
  def run
    p @s.center(10, bump)
    @s = +"ab"
    p @s.ljust(8, bump)
    @s = +"ab"
    p @s + bump
    p @@c.center(10, cbump)
  end
end
Holder.new.run

$g = +"ab"
def gbump = ($g << "zz"; "-")
p $g.center(10, gbump)
$g = +"ab"
p $g.rjust(8, gbump)

r = +"ab"
p r.center(12, [1].map { r << "zz"; "q" }.first)
p r

# an interpolation's implicit to_s runs code too
class Bang; def initialize(s) = @s = s; def to_s = (@s << "!"; "o"); end
$t = +"ab"
p $t.center(8, "#{Bang.new($t)}")
p({ $t => "#{Bang.new($t)}" })

# a receiver that may be nil, tested after an argument of no one type ran
# (the argument changes the receiver's String in place)
def grow_n = (@gs << "z"; $n)
$n = "x"
$n = 2
def run_grow
  @gs = +"ab"
  p @gs.center(grow_n, "qrs"), @gs
  @gs = +"ab"
  p @gs[grow_n], @gs
end
run_grow
