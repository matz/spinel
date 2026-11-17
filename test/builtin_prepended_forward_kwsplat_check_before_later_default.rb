# A prepended forward with keyword parameters and no keyword rest checks the
# keys a `**h` passes as it binds, before its own defaults and every later
# forward's run, though it fills none itself.
$l = []
module Later; def initialize(d = ($l << :later; 3), **kw) = super; end
module Check; def initialize(*a, capacity: 1) = super; end
Hash.prepend(Later); Hash.prepend(Check)
h = { bar: 1 }
e = {}
p((Hash.new(capacity: 2, **h) rescue $!.message), $l)
p Hash.new(capacity: 2, **e)[:x], $l
p((Hash.new(5, **h) rescue $!.message), Hash.new(5, **{ capacity: 2 })[:x], $l)
def mk(x) = Hash.new(capacity: 2, **x)
p((mk(h) rescue $!.message), mk(e)[:y], $l)

