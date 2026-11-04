# `m::Nest::Foo` with m a value: Nest is looked up in m at run time, and
# then Foo in that Nest. Classes in more than one scope share the leaf Foo,
# so the path is not the leaf-named class: it is the Foo the module that
# `m::Nest` reads holds. Each arm reads the parent path again, as it reads
# m. A class or module that includes Ext reaches Ext::Nest too.
class Foo; def hi = "top"; end
module Ext
  module Nest
    class Foo; def hi = "nested"; end
    module Deep
      class Foo; def hi = "deep"; end
    end
  end
  class Foo; def hi = "ext"; end
end
module Sub
  include Ext
end
class Kls
  include Ext
end
m = Ext
puts m::Nest::Foo.new.hi
puts m::Nest::Deep::Foo.new.hi
def via(v) = v::Nest::Foo.new.hi
puts via(Ext)
puts via(Sub)
puts via(Kls)
k = Kls.new
puts k.class::Nest::Foo.new.hi
[Ext, Sub].each { |x| puts x::Nest::Foo.new.hi }
x = Ext::Nest::Foo.new
p x.is_a?(m::Nest::Foo)
case x
when m::Nest::Deep::Foo then puts "deep arm"
when m::Nest::Foo then puts "nested arm"
else puts "other arm"
end
puts m::Foo.new.hi
n = Ext::Nest
puts n::Foo.new.hi
puts n::Deep::Foo.new.hi
# A subclass's own Foo, and a class's own beside an included module's,
# come before the Foo of the class or module it reaches.
class Base; class Foo; def hi = "base"; end; end
class Kid < Base; class Foo; def hi = "kid"; end; end
module Wrap; K = Kid; end
kk = Kid
puts kk::Foo.new.hi
w = Wrap
puts w::K::Foo.new.hi
module Mix; class Foo; def hi = "mix"; end; end
class Own; include Mix; class Foo; def hi = "own"; end; end
ow = Own
puts ow::Foo.new.hi
bb = Base
puts bb::Foo.new.hi
