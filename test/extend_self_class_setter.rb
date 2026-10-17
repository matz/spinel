# A module that does `extend self` gets a `def self.x=` clone of an instance
# setter `def x=`, which took the place of the module's own class setter of
# the name: `def self.x=`, `def M.x=`, a `def x=` in `class << self`, or an
# `alias` / `alias_method` of the name there. `M.x = 1` ran the instance
# setter. The module's own setter wins whatever the order, and an operator
# like `[]=` is matched by its full name too.
module SelfDef
  extend self
  def self.x=(v)
    @cls = v
  end
  def x=(v)
    @inst = v
  end
  def self.cls = @cls
  def self.inst = @inst
end
SelfDef.x = 1
p SelfDef.cls, SelfDef.inst

module SingletonDef
  extend self
  class << self
    def x=(v)
      @cls = v
    end
    def cls = @cls
    def inst = @inst
  end
  def x=(v)
    @inst = v
  end
end
SingletonDef.x = 2
p SingletonDef.cls, SingletonDef.inst

module ConstDef
  extend self
  def ConstDef.x=(v)
    @cls = v
  end
  def x=(v)
    @inst = v
  end
  def self.cls = @cls
  def self.inst = @inst
end
ConstDef.x = 3
p ConstDef.cls, ConstDef.inst

module AliasMethod
  extend self
  class << self
    def set_cls(v)
      @cls = v
    end
    alias_method :x=, :set_cls
    def cls = @cls
    def inst = @inst
  end
  def x=(v)
    @inst = v
  end
end
AliasMethod.x = 4
p AliasMethod.cls, AliasMethod.inst

module AliasKeyword
  extend self
  class << self
    def set_cls(v)
      @cls = v
    end
    alias x= set_cls
    def cls = @cls
    def inst = @inst
  end
  def x=(v)
    @inst = v
  end
end
AliasKeyword.x = 5
p AliasKeyword.cls, AliasKeyword.inst

module InstanceFirst
  extend self
  def x=(v)
    @inst = v
  end
  def self.x=(v)
    @cls = v
  end
  def self.cls = @cls
  def self.inst = @inst
end
InstanceFirst.x = 6
p InstanceFirst.cls, InstanceFirst.inst

module Operator
  extend self
  def []=(k, v)
    @inst = v
  end
  class << self
    def []=(k, v)
      @cls = v
    end
    def cls = @cls
    def inst = @inst
  end
end
Operator[0] = 7
p Operator.cls, Operator.inst

class Includer
  include SelfDef
end
i = Includer.new
i.x = 8
p i.instance_variable_get(:@inst)
