# A method of a builtin reopening reads a constant from a builtin above
# it (Numeric for an Integer, Comparable or Enumerable, IO for a File)
# before the top-level one, as CRuby looks in the ancestors first. The
# ancestor's write can come after the def, or under a condition.
LIM = 1
CL = 2
BOTH = 3
class Integer
  def a = LIM
  def b = CL
  def c = BOTH
  def d = defined?(LIM)
end
class Float
  def a = LIM
end
p 5.a, 2.5.a
class Numeric; LIM = 6; BOTH = 8; end
module Comparable; CL = 7; BOTH = 9; end
p 5.a, 5.b, 5.c, 2.5.a, 5.d
class String
  def e = CL
end
p "s".e
class Integer; BOTH = 10; end
p 5.c

SEP = "top"
class Array
  def s = SEP
  def t = [1].map { SEP }.first
end
p [1].s
module Enumerable; SEP = "enum" if ARGV.size == 0; end
p [1].s, [1].t
W = "top"
class File
  def self.w = W
end
class IO; W = "io"; end
p File.w
Z = 1
class Rational
  def z = Z
end
class Numeric; Z = 4; end
p Rational(1, 2).z
