# Where a module's `def check(...) = super` is mixed in decides the end of the
# chain its super walks. An instance's chain ends in Object's methods, and the
# program's reopening of Object is one of them (K has no superclass written); a
# class's singleton chain goes through its superclass's singleton. In both the
# keyword the caller passes stays a keyword on the way to the def.

class Object
  def check(a, b = 2, k: 3) = [:obj, a, b, k]
end
module Wrap
  def check(...) = super
end
class K
  include Wrap
end
p K.new.check(1, k: 5)
p K.new.check(1, 9)

module E2
  class Base
    def self.zork(a, b = 2, k: 3) = [:base, a, b, k]
  end
  module CM
    def zork(...) = super
  end
  class K < Base
    extend CM
  end
end
p E2::K.zork(1, k: 5)
p E2::K.zork(1, 9)

# A class's singleton chain reaches Class's and Module's own methods before the
# program's reopening of Object: Class#superclass answers, not the Object def.
class Object
  def superclass(a = 1, k: 2) = [:obj, a, k]
end
module W
  def superclass(...) = super
end
class K2
  extend W
end
p K2.superclass

# A top-level def and the program's reopening of Object define into one method
# table, the later def replacing the earlier: the forwarder takes the later
# one's shape, whichever of the two it is, and a Kernel reopening's def ahead
# of Object's is replaced the same way.
def zork(a, b = 7) = [:top, a, b]
class Object
  def zork(a = 1, k: 2) = [:obj, a, k]
end
module W3
  def zork(...) = super
end
class A3
  include W3
end
p A3.new.zork
p A3.new.zork(5, k: 6)

module Kernel
  def zork2(a, b = 7) = [:kern, a, b]
end
class Object
  def zork2(a = 1, k: 2) = [:obj, a, k]
end
module W4
  def zork2(...) = super
end
class A4
  include W4
end
p A4.new.zork2
p A4.new.zork2(5, k: 6)

# Object's prepends come ahead of its own table, which holds the program's
# reopening's defs and the top-level ones: the forwarder takes the prepended
# module's shape.
module P5
  def zork5(a = 1, k: 2) = [:p, a, k]
end
class Object
  prepend P5
  def zork5(a, b = 7) = [:obj, a, b]
end
module W5
  def zork5(...) = super
end
class A5
  include W5
end
p A5.new.zork5
p A5.new.zork5(5, k: 6)

module P6
  def zork6(a = 1, k: 2) = [:p, a, k]
end
class Object
  prepend P6
end
def zork6(a, b = 7) = [:top, a, b]
module W6
  def zork6(...) = super
end
class A6
  include W6
end
p A6.new.zork6
p A6.new.zork6(5, k: 6)

# A top-level include is an include into Object, and the later include comes
# first: Object's reopening includes B after the top-level include of A.
module B7
  def zork7(a = 1, k: 2) = [:b, a, k]
end
module A7
  def zork7(a, b = 7) = [:a, a, b]
end
include A7
class Object
  include B7
end
module W7
  def zork7(...) = super
end
class C7
  include W7
end
p C7.new.zork7
p C7.new.zork7(5, k: 6)

# Including a module that is already among the ancestors does nothing, so the
# second include of A8 leaves the order B8, A8: in a class, and across the
# reopening of Object and a top-level include. A module is prepended even when
# the class includes it.
module A8
  def zork8(a, b = 7) = [:a, a, b]
end
module B8
  def zork8(a = 1, k: 2) = [:b, a, k]
end
module W8
  def zork8(...) = super
end
class D8
  include A8
  include B8
  include A8
end
class C8 < D8
  include W8
end
p C8.new.zork8
p C8.new.zork8(5, k: 6)

module A9
  def zork9(a, b = 7) = [:a, a, b]
end
module B9
  def zork9(a = 1, k: 2) = [:b, a, k]
end
module W9
  def zork9(...) = super
end
include A9
class Object
  include B9
  include A9
end
class C9; include W9; end
p C9.new.zork9
p C9.new.zork9(5, k: 6)

module A10
  def zork10(a = 1, b = 7) = [:a, a, b]
end
module B10
  def zork10(a = 1, k: 2) = [:b, a, k]
end
module W10
  def zork10(...) = super
end
class D10
  include A10
  include B10
  prepend A10
end
class C10 < D10
  include W10
end
p C10.new.zork10
p C10.new.zork10(5, k: 6)
