# `m::Nest::Foo` with m a value, where modules in two scopes are named Nest
# and classes in two scopes are named Foo: the middle step `m::Nest` is
# resolved by its leaf name, so the Foo it reaches is a guess. The program
# is refused instead of reading the leaf-named class.
class Foo; def hi = "top"; end
module Ext; module Nest; class Foo; def hi = "ext"; end; end; end
module Oth; module Nest; class Foo; def hi = "oth"; end; end; end
m = Oth
puts m::Nest::Foo.new.hi
