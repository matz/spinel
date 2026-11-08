# An alias of p in a module that the program mixes into String by a
# computed argument (`send(:include, Foo.ancestors[1])`): CRuby prints M1
# from the Comparable method. The class table does not show where such a
# module goes, so an alias in any module counts, and the copy is refused.
module M
  def original(x) = "M#{x}"
  alias p original
end
class Foo
  include M
end
class String
  send(:include, Foo.ancestors[1])
end
module Comparable
  def qa = p(1)
end
puts "x".qa
