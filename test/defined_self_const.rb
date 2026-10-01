# defined?(self::Name) in a class method, and defined?(self.class::Name) in an
# instance method, answer "constant" when the receiving class or an ancestor
# defines Name publicly, else nil. A self:: path was no constant defined?
# could fold, so it answered nil, and self.class:: answered "method".

class P
  PC = 1
end
class C < P
  OC = 2
  def self.a = defined? self::PC
  def self.b = defined? self::OC
  def self.c = defined? self::Nope
  def e = defined? self.class::PC
  def f = defined? self.class::Nope
end
class D < C
  DC = 4
  def self.g = defined? self::DC
end
p C.a, C.b, C.c, C.new.e, C.new.f, D.a, D.g
p C.a.frozen?
class Abs
  def self.has = defined? self::KEYBYTES
end
class Impl < Abs
  KEYBYTES = 32
end
p Impl.has, Abs.has
module PrivM
  PRIV = true
  private_constant :PRIV
end
module UsesPriv
  include PrivM
  def self.dp = defined? self::PRIV
end
p UsesPriv.dp
