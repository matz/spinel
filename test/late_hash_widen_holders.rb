# A Hash that takes another kind in the late ivar re-run reaches everything the
# fixpoint settled on the old one (see ivar_widened_late_hash_return_bound.rb
# for the calls): a store, a multiple assignment, a pattern binding, and calls
# given a Hash that no ordinary return carried, a literal, a seeded return or
# the branch a method takes without a block. Each stayed narrow: the program was
# refused, the C did not build, or a read answered 0 through the wrong layout.
class T
  attr_accessor :a
  def initialize
    @a = 7
  end

  def h
    { "a" => @a }
  end

  #: () -> Hash[String, Integer]
  def seeded
    { "a" => @a }
  end

  def blockless
    if block_given?
      yield
    else
      { "a" => @a }
    end
  end
end

# only this literal holds @b: no method returns it
class L
  attr_accessor :b
  def initialize
    @b = 8
  end

  def literal
    other({ "a" => @b })
  end
end

def widen(t, l)
  n = DB.with { |c| c.changes }
  t.a = t.a + n
  l.b = l.b + n
end

def use(h)
  h["a"]
end

def other(h)
  h["a"]
end

t = T.new
l = L.new
far = ARGV.size > 5
widen(t, l) if far

$cache = { "q" => 1 }
$cache = t.h
p $cache["a"]
K = t.h
p K["a"]

class Box
  def initialize
    @v = { "q" => 1 }
  end
  def set(x) = @v = x
  def get = @v
end
b = Box.new
b.set(t.h)
p b.get["a"]

d = { "q" => 1 }
d, e = t.h, 1
p d["a"], e
f = { "q" => 1 }
f, _g = [t.h, 2]
p f["a"]

m = { "q" => 1 }
case t.h
in Hash => m
  p m["a"]
end
case [t.h, 1]
in [z, 1]
  p z["a"]
end

p other({ "b" => 1 })
p l.literal

x = t.blockless
p use(x)
p t.blockless { 5 }
y = t.seeded
p use(y)
p use({ "a" => 2 })
