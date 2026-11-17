# Literal keys beside a `**h`, given to a prepended forward with keyword
# parameters and no keyword rest: CRuby's binding raises `unknown keyword` for
# every key it does not name, the literal ones and the `**h`'s alike, before
# the forward's own defaults run; where it names them all, it fills its
# defaults and passes them on.
$log = []
module Sized
  def initialize(d = ($log << :d; 3), capacity: 1) = super
end
Hash.prepend(Sized)
h = { bar: 1 }
e = {}
p((Hash.new(foo: 1, **h) rescue $!.message), $log)
p((Hash.new(foo: 1, **e) rescue $!.message), $log)
p((Hash.new(**h, foo: 1) rescue $!.message), $log)
p((Hash.new(capacity: 2, **h) rescue $!.message), $log)
g = { capacity: 4 }
p Hash.new(capacity: 2, **g)[:x], $log
p Hash.new(capacity: 2, **e)[:x], $log
def mk(x) = Hash.new(capacity: 2, **x)
p((mk(h) rescue $!.message), mk(g)[:y], $log)
