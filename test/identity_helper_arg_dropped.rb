# A table handed to a helper that only answers it back, where the answer is
# thrown away, goes nowhere: it keeps its kind. A helper that keeps,
# captures, yields or hands on its argument, or a call whose answer is read,
# leaves it boxed.
# spinel: gc-stress

def share(obj, copy: false) = obj

def build(n, v)
  t = Array.new(n) { Array.new(n, v) }
  t[n - 1][n - 1] = v * 3
  t
end

# a constant, a local and a Float table, each shared and dropped
SH_TBL = build(3, 2)
share(SH_TBL)
SH_NAMES = share(%w[a b])
sh_loc = build(2, 5)
share(sh_loc, copy: true)
sh_flt = Array.new(2) { Array.new(2, 0.5) }
share(sh_flt)

def total(t)
  s = 0
  t.each { |row| row.each { |v| s += v } }
  s
end
p total(SH_TBL), total(sh_loc), sh_flt[1][1] * 4, SH_NAMES

# shared inside a block, before its last statement
lp_t = build(2, 1)
3.times { |i| share(lp_t); lp_t[0][0] += i }
p lp_t

# shared under a condition, and as the last statement of an each block,
# whose value each ignores
cd_t = build(2, 4)
share(cd_t) if cd_t.size > 1
eb_t = build(2, 6)
[1, 2].each { |i| eb_t[0][0] += i; share(eb_t) }
p cd_t[1][1], eb_t[0][0]

# (each table below is built where it is used: one built by `build` would
# join the tables above, and its escape would keep them all boxed)

# the helper keeps the argument
def keep(o)
  @kept = o
  o
end
kp_t = Array.new(2) { Array.new(2, 0) }
keep(kp_t)
@kept[0][0] = "s"
p kp_t[0]

# another parameter's default reads it
def keep_default(o, x = ($kd = o)) = o
kd_t = Array.new(2) { Array.new(2, 0) }
keep_default(kd_t)
$kd[0][0] = "d"
p kd_t[0]

# the call's answer is read
an_t = Array.new(2) { Array.new(2, 0) }
an_x = share(an_t)
an_x[0][0] = "a"
p an_t[0]

# a lambda captures it
def hold(o)
  $held = -> { o }
  o
end
ld_t = Array.new(2) { Array.new(2, 0) }
hold(ld_t)
$held.call[1][1] = "l"
p ld_t[1]

# the helper yields it
def lend(o)
  yield o
  o
end
yl_t = Array.new(2) { Array.new(2, 0) }
lend(yl_t) { |v| v[1][0] = "y" }
p yl_t[1]

# a subclass overrides the helper with define_method
class DmA
  def ident(o) = o

  def run
    dm_t = Array.new(2) { Array.new(2, 0) }
    ident(dm_t)
    $dm[0][0] = "m" if $dm
    p dm_t[0]
  end
end

class DmB < DmA
  define_method(:ident) { |o| $dm = o; o }
end
DmA.new.run
DmB.new.run

# a parameter an RBS signature types as an Array takes the table converted,
# not boxed
class Pinned
  #: (Array[untyped]) -> void
  def take(obj)
    obj
  end

  def run
    rb_t = Array.new(2) { Array.new(2, 3) }
    take(rb_t)
    p rb_t[1][1]
  end
end
Pinned.new.run

# each helper above but `take` also answers other values, so its parameter
# is untyped and the rule, not a slot of the parameter's own, decides each table
p keep("k"), keep_default("d"), hold(1).class, lend(:y) { }, DmA.new.ident("i")

# a helper that hands its argument on to another method
def pass_on(o)
  stash(o)
  o
end

def stash(v) = ($stashed = v)
ho_t = Array.new(2) { Array.new(2, 0) }
pass_on(ho_t)
$stashed[0][0] = "h"
p ho_t[0], pass_on(:x)

# the program's last statement shares a table: nothing reads its value
ls_t = Array.new(2) { Array.new(2, 7) }
p ls_t[1][1]
share(ls_t)
