# A private constant is read lexically and by const_get; a read through a
# scope (`P::A`, `::P::A`, `Sub::A` for one a superclass holds) raises
# NameError, in the class's own methods too, and so does a call on a private
# class read that way. defined? of such a path is nil. A constant made
# public again reads as any other.
class P
  A = 1
  B = 2
  class Inner; end
  module Mod; end
  private_constant :A, :Inner
  private_constant "Mod"
  C = 3
  private_constant :C
  public_constant :C
  def self.scoped = P::A
  def self.lexical = A
  def self.inner = Inner.new.class
end
class Sub < P
  def self.own = Sub::A
end
class Own < P
  A = 5
end
module Ns
  class K
    X = 9
    private_constant :X
  end
  def self.inner = K::X
end
def try(label)
  print label, ": "
  p yield
rescue NameError => e
  puts "#{e.class}: #{e.message} (#{e.name.inspect})"
end
try("P::A") { P::A }
try("::P::A") { ::P::A }
try("P::B") { P::B }
try("P::C") { P::C }
try("P::Inner") { P::Inner }
try("P::Inner.new") { P::Inner.new.class }
try("P::Mod") { P::Mod }
try("P.scoped") { P.scoped }
try("P.lexical") { P.lexical }
try("P.inner") { P.inner }
try("Sub::A") { Sub::A }
try("Sub.own") { Sub.own }
try("Own::A") { Own::A }
try("Ns::K::X") { Ns::K::X }
try("Ns.inner") { Ns.inner }
try("P.const_get(:A)") { P.const_get(:A) }
try("Object.const_get") { Object.const_get("P::A") }
try("defined?(P::A)") { defined?(P::A) }
try("defined?(P::B)") { defined?(P::B) }
v = (P::A rescue 7)
p v
p [P::B, (P::A rescue :priv), P::C]
