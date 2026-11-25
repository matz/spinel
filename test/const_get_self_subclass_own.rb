# A run-time-name const_get or const_defined? on `self` in an inherited
# class method reaches a constant only a subclass holds when self is that
# subclass, or one below it, and not when self is the class that defines
# the method. A constant a subclass holds again answers by the subclass.
class Reg
  LIMIT = 7
  def self.look(n) = const_get(n)
  def self.has?(n) = const_defined?(n)
end

class Sub < Reg
  OWN = :sub_own
  class Box; WIDTH = 3; end
end

class SubSub < Sub; end

class Deep < Sub
  LIMIT = 70
end

module Mixin
  MIX = :mixed
end

class Other < Reg
  include Mixin
end

def miss
  yield
rescue NameError => e
  p e.message
end

p Sub.look("OWN" + "")
p SubSub.look("OWN" + "")
p Sub.look("Box" + "")
p SubSub.look("Box" + "")
p Other.look("MIX" + "")
p Deep.look("LIMIT" + "")
p Deep.look("OWN" + "")
p Sub.look("LIMIT" + "")
p Reg.look("LIMIT" + "")
p Sub.has?("OWN" + "")
p Reg.has?("OWN" + "")
p Other.has?("OWN" + "")
p Other.has?("MIX" + "")
p Sub.has?("MIX" + "")
p Deep.has?("LIMIT" + "")
miss { Reg.look("OWN" + "") }
miss { Other.look("OWN" + "") }
miss { Reg.look("Box" + "") }
miss { Sub.look("MIX" + "") }
