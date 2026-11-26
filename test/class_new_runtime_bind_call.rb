# `k.instance_method(:m).bind_call(o)` on a class a Class.new made at run
# time binds the site's method to o when o is an instance of that class,
# and raises TypeError naming it when o is an instance of another class
# the same call made; a method a static superclass owns binds an instance
# of any of them.

mk = -> { Class.new { def m(x = 1) = x * 7 } }
a = mk.call
b = mk.call
p a.instance_method(:m).bind_call(a.new), a.instance_method(:m).bind_call(a.new, 2)
p a.instance_method(:m).bind_call([a.new, 1].first)
p (begin; a.instance_method(:m).bind_call(b.new); rescue TypeError => e; e.message.sub(/0x\h+/, "X"); end)
p (begin; a.instance_method(:m).bind_call(nil); rescue TypeError => e; e.message.sub(/0x\h+/, "X"); end)
Named = b
p (begin; Named.instance_method(:m).bind_call(a.new); rescue TypeError => e; e.message; end)
def go(k, o) = k.instance_method(:m).bind_call(o)
p go(a, a.new)
p (begin; go(a, b.new); rescue TypeError => e; e.class; end)

class P
  def m = 1
end
mk2 = -> { Class.new(P) { def n = 2 } }
c = mk2.call
d = mk2.call
p c.instance_method(:m).bind_call(d.new), c.instance_method(:n).bind_call(c.new)
p (begin; c.instance_method(:n).bind_call(d.new); rescue TypeError => e; e.message.sub(/0x\h+/, "X"); end)
p (begin; c.instance_method(:n).bind_call(P.new); rescue TypeError => e; e.message.sub(/0x\h+/, "X"); end)
