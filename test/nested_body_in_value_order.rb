# A class or module body in a value (an array element, an argument, a multiple assignment's value, a hash value,
# a parenthesised value, a written value) runs where its statement does, in the order it has against the calls the
# statement holds and the statements around it: a call before the body's statement answers what it did before the
# body, a call after it answers the body's definition, and a def placed after the body wins from its own point.
class K
  def m = 0
  def self.cm = 0
end

# left and right of the body, in each value position
p [K.new.m, (class K; def m = 1; end; 0), K.new.m]
p(K.cm, (class K; def self.cm = 1; end; 0), K.cm)
a, z, d = K.new.m, (class K; def m = 2; end; 0), K.new.m
p [a, d]
p({ a: K.cm, b: (class K; def self.cm = 2; end; 0), c: K.cm })
x = (K.new.m; class K; def m = 3; end; K.new.m)
p x
x = class K; def self.cm = 3; end
p K.cm

# a def placed after the body's statement wins from its own point
p K.new.m
class K; def m = 4; end
p K.new.m

# two bodies in one statement
p [K.new.m, (class K; def m = 5; end; 0), K.new.m, (class K; def m = 6; end; 0), K.new.m]

# a class that the body makes, its constant, a superclass and super
class P0; def m = 1; end
z = [(class K2 < P0; def m = super + 10; end; 0)]
p K2.new.m, K2.superclass
class K2; def m = super + 100; end
p K2.new.m

# a module body reaching an includer
module Mx; end
class K4; include Mx; end
z = [(module Mx; def m = 2; end; 0)]
p K4.new.m, K4.new.respond_to?(:m)
module Mx; def m = 3; end
p K4.new.m

# the singleton `new` of a body, with a def after it
class K5
  attr_reader :v
  def initialize(x) = @v = x
end
p K5.new(1).v
z = [(class K5; def self.new(x) = super(x * 3); end; 0)]
p K5.new(1).v
class K5
  def self.new(x) = super(x * 7)
end
p K5.new(2).v

# the statement under a condition and in a loop
class K7; def m = 0; end
i = 0
while i < 2
  p K7.new.m
  z = [(class K7; def m = 2; end; 0)]
  i += 1
end
if i > 5
  z = [(class K7; def m = 9; end; 0)]
end
p K7.new.m
