# A builtin `new` in a method that runs only before a top-level prepend takes the chain without it; one after it takes it.
$l = []
module Sized; def initialize(d = 3, **kw) = super; end
module Outer; def initialize(d = ($l << :o; nil), **kw) = super; end
module Pair; def initialize(n = 2, f = :v) = super; end
def mk_hash = Hash.new
def mk_raise = (Hash.new(foo: 1) rescue $!.message)
def mk_array = Array.new
def mk_two = Array.new(2, 0)
p mk_hash[:x]
p mk_raise, $l
p mk_array
p mk_two
Hash.prepend(Sized); Hash.prepend(Outer)
Array.prepend(Pair)
p Hash.new[:x], $l
p Array.new
