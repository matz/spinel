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
