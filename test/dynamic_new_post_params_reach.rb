# Which classes an unpinnable `new` can reach is decided by how many positional
# arguments each class's initialize accepts, and Ruby counts the required
# parameters AFTER the optionals as well: `initialize(n = 0, b)` takes 1..2, not
# 0..1. Counting only the leading requireds put this class out of reach of a
# two-argument `k.new`, so its string parameter was promoted to the shared
# handle -- and the arm codegen then wanted no longer matched, so the dynamic
# call fell through to the switch default and raised NoMethodError where it had
# merely been reading a stale copy before.
#
# The shape needs all three parts: a post-required parameter, a STATIC call of
# that arity (which is what promotes), and a DYNAMIC one (which is what the
# promotion then breaks). It lives in its own file because any OTHER unpinnable
# `new` in the program -- a no-argument one is enough -- puts the class back in
# reach by a different count and hides the miscount.
class Holder
  def initialize(n = 0, b)
    @n = n
    @b = b
  end
  def at(i)
    @b.getbyte(i)
  end
  def n
    @n
  end
end

class Mutator
  def initialize(b)
    @b = b
  end
  def poke(i, v)
    @b.setbyte(i, v)
  end
end

def constantly(k)
  k
end

s = +"abcd"
kept = Holder.new(0, s)        # static, two positionals: this is what promotes
m = Mutator.new(s)
m.poke(2, 7)

# Built AFTER the write, so what it reads does not depend on the aliasing this
# class is (correctly) refused -- only on its constructor still resolving at
# all, which is what the miscount broke.
p constantly(Holder).new(0, s).at(2)
p kept.n
