# A top-level def, an Object reopening's def and a class method defined after
# a respond_to? are not there yet when it asks, for a literal name and for a
# name only the run time knows, with include_all, and for a class value.
class Foo; end
o = Foo.new
lit = [:later_top, :zz][ARGV.size]
obj = [:later_obj, :zz][ARGV.size]
p o.respond_to?(:later_top, true), o.respond_to?(:later_top)
p o.respond_to?(lit, true), 5.respond_to?(:later_top, true), nil.respond_to?(:later_top, true)
p o.respond_to?(:later_obj), o.respond_to?(obj), 5.respond_to?(obj), nil.respond_to?(obj)
p Foo.respond_to?(:later_cm)
p(o.respond_to?(:later_top, true) ? :yes : :no)
p(Foo.respond_to?(:later_cm) ? Foo.later_cm : :guarded)

def later_top = 1
class Object; def later_obj = 2; end
class Foo; def self.later_cm = 3; end

p o.respond_to?(:later_top, true), o.respond_to?(:later_top)
p o.respond_to?(lit, true), 5.respond_to?(:later_top, true), nil.respond_to?(:later_top, true)
p o.respond_to?(:later_obj), o.respond_to?(obj), 5.respond_to?(obj), nil.respond_to?(obj)
p Foo.respond_to?(:later_cm)
p(Foo.respond_to?(:later_cm) ? Foo.later_cm : :guarded)
