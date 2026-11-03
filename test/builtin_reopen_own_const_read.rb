# A method of a reopened builtin reads the constant the class's body
# writes, not a top-level one of the same name.
LIM = 1
class Helper; def to_s = "top"; end

class Integer
  LIM = 3
  class Helper; def to_s = "int"; end
  def lim = LIM
  def lim_plus(x = LIM) = x + LIM
  def helper = Helper.new.to_s
  def self.lim = LIM
  def in_block = [1, 2].map { |v| v * LIM }
end
p 5.lim, 5.lim_plus, 5.lim_plus(10), 5.helper, Integer.lim, 5.in_block

class Proc; LIM = 4; def lim = LIM; end
p proc { 1 }.lim

class Rational; LIM = 5; def lim = LIM; end
p 2r.lim

class Complex; LIM = 6; def lim = LIM; end
p Complex(1, 2).lim

class Regexp; LIM = 7; def lim = LIM; end
p(/a/.lim)

module Comparable; LIM = 8; def clim = LIM; end
p 5.clim

module Kernel; LIM = 9; def klim = LIM; end
p 5.klim

module Enumerable; LIM = 10; def elim = LIM; end
p [1].elim

class String; LIM = 11; def lim = LIM; end
p "s".lim

class Hash; LIM = 12; def lim = LIM; end
p({}.lim)

# a name the class does not write reads the top-level constant
TOP = :top
class Integer; def top = TOP; end
class Proc; def top = TOP; end
p 5.top, proc { 1 }.top

# a later body of the same class writes the constant
class Float; def flim = FLIM; end
FLIM = :top
class Float; FLIM = :float; end
p 1.5.flim

# a boxed receiver
def g(v) = v.lim
p g(5), g(proc { 1 }), g(2r), g("s")

# a body opened later writes the constant: until then the read is the
# top-level one (this method's own body writes none)
BLIM = :top
class Integer; def blim = BLIM; end
p 5.blim
