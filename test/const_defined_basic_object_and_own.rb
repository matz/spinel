# const_defined? on a class below BasicObject: it reaches no Object, so a
# builtin's name, a top-level constant of the program and the class's own
# name are not defined there. Its own constants, its nested classes and a
# module it includes count. BasicObject's own name counts too.
module Mk; MK = 1; end
class Bare < BasicObject
  include ::Mk
  IN = 2
  class Nest; end
  def self.q(n) = const_defined?(n)
  def self.r = const_defined?(:File)
  def self.s = self.const_defined?(:IN)
end
class Bare2 < Bare; end
X = 1
module Mo; end
p Bare.const_defined?(:X), Bare.const_defined?(:Mo), Bare.const_defined?(:Bare)
p Bare.const_defined?(:String), Bare.const_defined?(:File), Bare.const_defined?(:Comparable)
p Bare.const_defined?(:BasicObject), Bare.const_defined?(:Kernel)
k = Bare
p k.const_defined?(:File), Bare.const_defined?("File")
p Bare.const_defined?(:MK), Bare.const_defined?(:IN), Bare.const_defined?(:Nest)
p Bare2.const_defined?(:IN), Bare2.const_defined?(:X), Bare2.const_defined?(:Bare)
p Bare.q(:File), Bare.q(:IN), Bare.q(:X)
p Bare.r, Bare.s

# const_defined?(N, false) on Object: the builtin names CRuby defines at
# boot are Object's own constants
p Object.const_defined?(:Struct, false), Object.const_defined?(:Thread, false)
p Object.const_defined?(:Errno, false), Object.const_defined?(:String, false)
p Object.const_defined?(:X, false), Object.const_defined?(:NoSuch, false)
class Foo; end
p Foo.const_defined?(:Struct, false), Foo.const_defined?(:Struct)
p Kernel.const_defined?(:Struct, false)

# BasicObject itself, also reopened, holds only its own constants
class BasicObject; BB = 1; end
def pass(x) = x
p BasicObject.const_defined?(:X), BasicObject.const_defined?(:String), BasicObject.const_defined?(:BB)
p pass(BasicObject).const_defined?(:X), pass(BasicObject).const_defined?(:Thread)
p Bare.q(:BasicObject)

