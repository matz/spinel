# `self::Name` in a class method (and `self.class::Name` in an instance one)
# finds a constant the class inherits, from a superclass or an included
# module. The class got a getter that raised NameError, kept for a class
# that leaves the name to its subclasses, as soon as another class defined it.

class P
  PC = 1
end
class C < P
  OC = 2
  def self.h = self::OC
  def self.i = self::PC
  def j = self.class::PC
end
class D < C; end
p C.h, C.i, C.new.j, D.i, D.h
module Mx
  MC = 3
end
class E < P
  include Mx
  def self.k = self::MC
  def self.l = self::PC
end
p E.k, E.l
class Abs
  def self.kb = self::KEYBYTES
end
class Impl < Abs
  KEYBYTES = 32
end
p Impl.kb

# a private constant is not read through a scope, an inherited one included
module PrivM
  PRIV = true
  private_constant :PRIV
end
module UsesPriv
  include PrivM
  def self.via_self = self::PRIV
  def self.bare = PRIV
end
p((UsesPriv.via_self rescue $!.class), UsesPriv.bare)
