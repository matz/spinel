# A yield's value reaches a C slot typed for the value it carries.

# An empty Array literal yielded next to Integers, through the proc ABI that
# Enumerable's collector calls #each with: it was stored into an sp_int.
class YieldsMixed
  include Enumerable
  def each
    yield 0
    yield []
  end
end
p YieldsMixed.new.to_a
p YieldsMixed.new.map { |x| x }

# An empty literal handed back by `next`, next to an Integer from another
# block: `next []` went into the Integer slot the other block's value made,
# where a tail `[]` did not.
def r(val)
  a = yield
  p(val == a)
end
r(1) { next 1 }
r([]) { next [] }
r({}) { next({}) }

# A yield whose value is a block's, handed to another method the program
# defines, where the outer blocks answer an Integer and a String.
def run(x) = yield(x)
def run2(x) = run(x) { |u| yield u }
run2(4) { |w| p w }
p(run2(5) { |w| w.to_s + "!" })

# A super into a yielding parent, beside a dispatched call that runs the
# parent's proc form: the block's parameter took that form's boxed value, and
# the boxed result went into the Integer slot the call was typed for.
class P
  def k(a) = yield(a)
end
class Q < P
  def k(a) = super(a)
end
class R < P
  def k(...) = super(...)
end
p P.new.k(1) { |x| x + 1 }
p Q.new.k(1) { |x| x + 1 }
p R.new.k(2) { |x| x * 10 }
