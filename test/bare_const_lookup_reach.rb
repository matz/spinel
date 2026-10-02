# A bare constant resolves the way CRuby's lookup does: the lexical scopes
# the reference is written in, then the ancestors of the innermost one.
# Each of these names is defined only inside a namespace, and each reference
# reaches it.

module Shapes
  SIDES = 4
  class Square
    def sides = SIDES                    # lexical: Shapes is in scope
    def each_side = [1, 2].map { |i| SIDES * i }   # blocks keep the scope
  end
end
p Shapes::Square.new.sides
p Shapes::Square.new.each_side

module Colors
  RED = "red"
end
class Paint
  include Colors
  def color = RED                        # an included module's constant
end
p Paint.new.color

module Loud
  VOLUME = 11
end
class Speaker
  prepend Loud
  def volume = VOLUME                    # a prepended module's constant
end
p Speaker.new.volume

class Base
  LIMIT = 3
end
class Derived < Base
  def limit = LIMIT                      # a superclass constant
  def self.climit = LIMIT
end
p Derived.new.limit
p Derived.climit

module Settings
  DEPTH = 7
end
include Settings                         # Object includes it: visible everywhere
def depth = DEPTH
p depth
module Elsewhere
  def self.depth = DEPTH                 # a module's lookup falls back to Object
end
p Elsewhere.depth
class Other
  def depth = DEPTH
end
p Other.new.depth

module Kernel
  KERNEL_ANSWER = 42                     # Kernel is one of Object's ancestors
end
p KERNEL_ANSWER
module Outer
  class Inner
    def answer = KERNEL_ANSWER
  end
end
p Outer::Inner.new.answer

module Config
  LEVEL = 2
  class << self
    def level = LEVEL                    # class << self keeps Config in scope
  end
end
p Config.level

Anon = Class.new(Base) do
  def depth = DEPTH                      # the block's cref is the top level
end
p Anon.new.depth

module Colors
  BLUE = "blue"
end
class Paint
  def blue = BLUE
end
p Paint.new.blue

p defined?(SIDES)                        # top level: Shapes is not in scope
module Shapes
  p defined?(SIDES)
end

class Holder
  class << self
    class Nested < Base                  # a class written in `class << self`
      def depth = DEPTH                  # finds Base::DEPTH through its superclass
    end
    def nested = Nested
  end
end
p Holder.nested.new.depth

class FromAnon < Class.new(Base)         # superclass: the anonymous class, then Base
  def depth = DEPTH
end
p FromAnon.new.depth

class FromData < Data.define(:n)
  include Colors
  def blue = BLUE
end
p FromData.new(n: 1).blue

p defined?(SIDES::X)                     # nil, as the head SIDES is unreachable
