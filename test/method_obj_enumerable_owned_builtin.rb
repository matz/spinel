# A Method of one of Enumerable's own methods on an Array or a Hash: the
# builtin facts list each class's own methods only, so `[1, 1].method(:to_set)`
# and `[1, 2].method(:inject)` found no method and raised NameError, where
# Comparable's and Numeric's methods were already found on the classes that
# have them. The Method calls the builtin and answers Enumerable's arity,
# name and owner.
require "set"

m = [1, 1, 2].method(:to_set)
p m.call.size
p m.call.class
p m.arity
p m.name
p m.owner
p m.receiver
p m.to_proc.call.size
p m.().size
p %w[a a b].method(:to_set).call.size
p [1, 1].public_method(:to_set).call.size

h = { a: 1, b: 1 }.method(:to_set)
p h.call.size
p h.arity
p h.owner

p [1, 2, 3].method(:inject).call(:+)
p [1, 2, 3].method(:inject).call(10, :+)
p [1, 2, 3].method(:reduce).arity
p [1, 2, 3].method(:each_slice).call(2).to_a
p [1, 2, 3].method(:each_cons).call(2).to_a
p [1, 2].method(:entries).call
p [1, 2].method(:member?).call(2)
p [1, 2].method(:group_by).owner
p [3, 1].method(:max_by).arity

p({ a: 1, b: 2 }.method(:count).call)
p({ a: 1, b: 2 }.method(:first).call)
p({ b: 1, a: 2 }.method(:sort).call)
p({ b: 1, a: 2 }.method(:min).call)
p({ a: 1 }.method(:map).arity)
p({ a: 1 }.method(:map).owner)

a = [1, 2]
p a.method(:inject) == a.method(:inject)
p [1].method(:to_set) == [1].method(:to_set)

# a name neither the class nor Enumerable has is still CRuby's NameError
begin
  [1].method(:nope_zz)
rescue NameError => e
  puts e.message
end
