# A Hash return that widens late (see hash_return_follows_late_widened_ivar.rb) reaches
# what holds it. A call the fixpoint had bound to the narrower Hash binds again:
# through a local the parameter stayed Hash[String, Integer] and read the
# sp_StrPolyHash through that layout (`use(x)` printed 1), and given the call
# directly the program was refused.
class T
  attr_accessor :a
  def initialize
    @a = 7
  end
  def h
    { "a" => @a }
  end
end
def widen(t)
  n = DB.with { |c| c.changes }
  t.a = t.a + n
end
def use(h)
  h["a"]
end
def plus(h)
  h["a"] + 1
end
class Holder
  attr_accessor :v
  def initialize
    @v = { "q" => 1 }
  end
end
t = T.new
widen(t) if ARGV.size > 5
x = t.h
p use(x)
p plus(x)
y = x
p use(y)
p use(t.h)
o = Holder.new
o.v = t.h
p o.v["a"]
p use({ "a" => 2 })
