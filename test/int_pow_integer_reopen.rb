# The facts that show an exponent non-negative (a size, an abs, the order of
# two locals) hold only while the program does not redefine the operator they
# stand on. Here Integer#abs answers the negation and a prepended module makes
# `<=` always true, so each power below has a negative exponent and answers a
# Rational. The rest of Integer is untouched: the other powers stay Integers.

class Integer
  def abs = -self
end
e = ARGV.size + 2
p 2 ** e.abs
p 2 ** e

module Loose
  def <=(other) = true
end
class Integer
  prepend Loose
end
d = ARGV.size + 5
m = 2
if d <= m
  p 2 ** (m - d)
end
d = ARGV.size
if d <= m
  p 2 ** (m - d)
end
