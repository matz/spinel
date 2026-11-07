# Module#const_set with a literal name sets the receiver's own constant, as
# `Mod::N = v` does, and leaves a top-level constant of that name alone. A
# module's, a class's, a nested path's and Object's are each their own
# constant; `self.const_set` and a bare `const_set` in a class body set the
# body's class's, and a local holding one module, `self.class` and self in a
# class method that module's. Presence is asked before and after each write.
module Values; end
class Box; end
module Outer; module Inner; end; end
def Object.present(n) = const_defined?(n)
def Object.read(n) = const_get(n)

Values.const_set(:G, 13)
p Object.present(:G)
begin
  p Object.read(:G)
rescue NameError => e
  puts e.message
end
Box.const_set(:G, 14)
Outer::Inner.const_set(:G, 15)
p Object.present(:G)
G = 1
p Object.present(:G), Object.read(:G)
p G, Values::G, Box::G, Outer::Inner::G

# the call answers its value, and the constant holds that same object
s = Values.const_set(:S, +"ab")
s << "c"
p Values::S
p Box.const_set(:G, 24), Box::G, G

# Object's is the top-level constant
Object.const_set(:H, 5)
p H, Object::H
p Object.present(:H)
Object.const_set(:G, 2)
p G, Values::G

# in a class body, self is the class
class Klass
  self.const_set(:G, 30)
  const_set(:J, 31)
  p G, J
end
p Klass::G, Klass::J, G
p Object.present(:J)

# a String name, and a write from a method body
def reset_values = Values.const_set("G", 40)
reset_values
p Values::G, G

# a local holding one module, `self.class` and self in a class method set
# that module's own constant
v = Values
v.const_set(:G, 50)
p Values::G, G
class Klass
  def self.reset = self.const_set(:G, 60)
  def bump = self.class.const_set(:J, 61)
end
Klass.reset
Klass.new.bump
p Klass::G, Klass::J, G

# a receiver known only at run time sets the one constant of its name
module Crate
  U = 1
end
def set_u(m) = m.const_set(:U, 2)
set_u(Crate)
p Crate::U
