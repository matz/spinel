# A constant path names the constant its scope holds, by its own bodies or
# its ancestors' (a superclass, an included or prepended module). A
# constant another class or module holds under the same name is no member
# of it: `Holder::X`, with X only in M, is CRuby's NameError, where the
# lookup by the leaf's name alone read M's X.
module M
  X = 5
  S = "s"
end
class Holder; end
module Other; end
p M::X
begin
  p Holder::X
rescue NameError => e
  puts e.message
end
begin
  p Other::S
rescue NameError => e
  puts e.message
end

# what the scope holds through its ancestors still reads
class Base
  B = 1
end
class Sub < Base; end
class Sub2 < Sub; end
p Sub::B, Sub2::B
module Mx
  Y = 2
end
class Cy
  include Mx
end
p Cy::Y
module Pm
  R = 8
end
class Pre
  prepend Pm
end
p Pre::R
class Pt < Struct.new(:x)
  V = 6
end
p Pt::V

# a constant a multiple assignment writes in the body
class Pair2
  P2, Q2 = [1, 2]
end
p Pair2::P2, Pair2::Q2

# a constant written through the path, and a top-level one through Object
Holder::Z = 3
p Holder::Z
X0 = 4
p Object::X0

# an enclosing module's constant is no member of the module nested in it,
# and an extended module's is no member of the class extending it
module Outer
  W = 5
  module Inner; end
end
begin
  p Outer::Inner::W
rescue NameError => e
  puts e.message
end
module Ext
  Q = 7
end
class Host
  extend Ext
end
begin
  p Host::Q
rescue NameError => e
  puts e.message
end

# a constructor called through such a path is named as written, a class
# alias another module holds among them
class Top; end
module M
  Top2 = Top
end
begin
  Other::S.new
rescue NameError => e
  puts e.message
end
begin
  Other::Top2.new
rescue NameError => e
  puts e.message
end
p M::Top2.new.class

# a class reaches the modules Object includes (one the top level includes,
# Kernel), though not Object's own constants; a module and a class below
# BasicObject do not
module TopInc
  TI = 1
end
include TopInc
module Kernel
  KC = 2
end
class K0; end
module M0; end
class Bs < BasicObject; end
[-> { K0::TI }, -> { K0::KC }, -> { M0::TI }, -> { M0::KC }, -> { Bs::TI }].each do |f|
  begin
    p f.call
  rescue NameError => e
    puts e.message
  end
end

# a builtin class reaches them too; a builtin module does not
[-> { String::TI }, -> { Integer::KC }, -> { Comparable::TI }, -> { Kernel::KC }].each do |f|
  begin
    p f.call
  rescue NameError => e
    puts e.message
  end
end
