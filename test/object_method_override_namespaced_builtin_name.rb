# A program's override of an Object method in a builtin class (Range, Proc,
# Object) is folded into Object's method. A class of the same name inside
# a module (`module Geo; class Range`, `class Geo::Range`) is another class:
# it keeps its own method and its own parameter default.
class Object; def o(x = 1) = [:obj, x]; end
module Geo
  class Range
    def o(x = 2) = [:georange, x]
  end
  class Object
    def o(x = 5) = [:geoobject, x]
  end
end
class Box; end
class Box::Proc
  def o(x = 3) = [:boxproc, x]
end
module M
  class ::Range
    def o(x = 4) = [:range, x]
  end
end
p Geo::Range.new.o
p Geo::Object.new.o
p Box::Proc.new.o
p (1..2).o
p 7.o
p [Geo::Range.new, 1..2, 7, Box::Proc.new, proc {}].map { |v| v.o }
# A class written in a body of Object is Object's constant: the builtin.
class Object
  class Rational
    def o(x = 6) = [:rational, x]
  end
end
p 3r.o
# A Date inside a module with its own initialize stays its own class.
module Geo
  class Date
    def initialize = @d = 1
    def o(x = 7) = [:geodate, x, @d]
  end
end
p Geo::Date.new.o
