# A body that runs after the load by a path the record does not follow
# step by step still meets the versions an exit handler makes: a method a
# closure calls that a handler runs, a handler given by `&`, a Method object
# called in a handler, and blocks kept in a global, a table or an ivar and
# run at exit. The load, which runs first, meets the old methods.
class A; def baz = "baz"; end
class B; def baz = "baz"; end
class C; def baz = "baz"; end
class D; def baz = "baz"; end

# a method called from a closure a handler runs
def rd = A.new.baz
la = -> { rd }
at_exit { p([:a_last, la.call]) }
at_exit { class A; def baz = "new"; end; nil }
p([:a_load, rd])

# handlers given by & and a Method object called in a handler
def chk_b = p([:b_chk, B.new.baz, (B.new.nw rescue $!.class), B.new.respond_to?(:nw)])
def chk_b2 = p([:b_chk2, B.new.baz])
mm = method(:chk_b2)
pr = proc { p([:b_pr, B.new.baz]) }
at_exit(&method(:chk_b))
at_exit(&pr)
at_exit { mm.call }
at_exit { class B; def baz = "new"; def nw = "nw"; end; nil }
chk_b

# a block kept in a global, and blocks a table keeps and runs
$kept = proc { [C.new.baz, (C.new.nw rescue $!.class)] }
at_exit { p([:c_last, $kept.call]) }
at_exit { class C; def baz = "new"; def nw = "nw"; end; nil }
p([:c_load, $kept.call])

class Reg
  def initialize = @hs = []
  def add(&b) = (@hs << b; self)
  def run = @hs.map(&:call)
end
R = Reg.new
R.add { D.new.baz }
at_exit { p([:d_last, R.run]) }
at_exit { class D; def baz = "new"; end; nil }
p([:d_load, R.run])
