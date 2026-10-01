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

# a private_constant in a later reopening counts too
class ReopenP
  RK = 1
end
class ReopenP
  private_constant :RK
end
class ReopenC < ReopenP
  def self.read = self::RK
end
p((ReopenC.read rescue $!.class))

# a superclass written with its namespace
module Outer
  class QParent
    QK = 5
  end
end
class QChild < Outer::QParent
  def self.read = self::QK
end
p QChild.read

# the nearest definition decides: a subclass's private one raises for
# self::NAME, while const_get reads a private constant
class VBase
  def self.read = self::VK
  def self.cg = const_get(:VK)
end
class VPriv < VBase
  VK = 3
  private_constant :VK
end
class VPub < VBase
  VK = 4
end
p((VPriv.read rescue $!.class), VPriv.cg, VPub.read, VPub.cg)
class GA
  GK = 1
  private_constant :GK
end
class GB < GA
  def self.cg = const_get(:GK)
  def self.read = self::GK
end
p(GB.cg, (GB.read rescue $!.class))
