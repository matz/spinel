# A `new` on a class whose `self.new` is defined again runs the def in
# force where the call runs: a call from a method answers the first def
# before the second is made and the second after it, through an explicit
# `new` on the class and on a subclass that inherits it. Two defs that both
# build the instance, and two in the arms of an if, answer the instance at
# every call.
class Base
  def self.new(*a) = super
  def initialize(x) = @x = x
  def x = @x
end
class Kid < Base
  def kid = @x * 2
end
def mk = Kid.new(1)
def mb = Base.new(3)
p mk.kid, mb.x
class Base
  def self.new(*a) = 42
end
p mk, mb

class Sym
  def self.new(*a) = super
  def initialize(x) = @x = x
  def x = @x
end
def ms = Sym.new(5)
p ms.x
class Sym
  def self.new(*a) = :sym
end
p ms

class Two
  def initialize(x) = @x = x
  def x = @x
end
class TwoKid < Two
  def kid = @x * 2
end
class Two
  def self.new(*a) = super
end
def mt = TwoKid.new(1)
p mt.kid
class Two
  def self.new(*a) = super(*a)
end
p mt.kid, mt.class

class Arm
  def initialize(x) = @x = x
end
class ArmKid < Arm
  def kid = @x * 2
end
if ARGV.empty?
  class Arm
    def self.new(*a) = super
  end
else
  class Arm
    def self.new(*a) = super(*a)
  end
end
def ma = ArmKid.new(4)
p ma.kid
