# `singleton_class.attr_reader :x` in a class or module body defines the
# accessors on the body's singleton class at that point, as `class << self;
# attr_reader :x; end` does: respond_to? before it answers false, a bare call
# after it in the body reads the class-level ivar, and a subclass reads its
# own. It was rewritten to `def self.x` before the walk, and the class's own
# reader was not emitted (a C compile error), or its value reached a C
# function as the wrong type.

class Foo
  @x = 1
  p respond_to?(:x)
  self.singleton_class.attr_reader "x"
  p respond_to?(:x)
  p x
end

module M
  singleton_class.attr_accessor :conf
  self.conf = {a: 1}
end
p M.conf

class Sub < Foo
  @x = 9
end
p Sub.x, Foo.x
p Foo.singleton_methods.include?(:x)

class Bare
  @y = 3
  singleton_class.attr_reader :y
  p y
end

class BareSub < Bare
end
p Bare.y, BareSub.y
